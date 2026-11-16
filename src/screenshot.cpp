// Screenshots (Print keys) with grim, through the screencopy protocol Dusk
// offers, and running a command with something to do when it finishes.
#include "server.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <ctime>

#include "notify.hpp"
#include "shot.hpp"

namespace dusk {

namespace {

struct Pending {
	Server *server;
	pid_t pid;
	int fd;
	wl_event_source *source = nullptr;
	std::function<void(bool)> done;
};

int handle_pending(int fd, uint32_t, void *data) {
	auto *p = static_cast<Pending *>(data);
	char buf[256];
	if (read(fd, buf, sizeof buf) > 0) {
		return 0;   // output we don't need; wait for the end
	}
	int status = 0;
	waitpid(p->pid, &status, 0);
	wl_event_source_remove(p->source);
	close(p->fd);
	auto done = std::move(p->done);
	delete p;
	if (done) {
		done(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	return 0;
}

bool have(const char *program) {
	std::string cmd = std::string("command -v ") + program + " >/dev/null 2>&1";
	return std::system(cmd.c_str()) == 0;
}

std::string pictures_dir() {
	const char *home = std::getenv("HOME");
	std::string dir = std::string(home ? home : "") + "/Pictures";
	// The localised name from user-dirs.dirs, if there is one.
	std::string file = std::string(home ? home : "") + "/.config/user-dirs.dirs";
	if (FILE *f = std::fopen(file.c_str(), "r")) {
		char line[1024];
		while (std::fgets(line, sizeof line, f)) {
			std::string l = line;
			if (l.rfind("XDG_PICTURES_DIR=\"", 0) == 0) {
				std::string v = l.substr(18);
				v = v.substr(0, v.find('"'));
				if (v.rfind("$HOME", 0) == 0) v = std::string(home ? home : "") + v.substr(5);
				if (!v.empty()) dir = v;
			}
		}
		std::fclose(f);
	}
	return dir;
}

} // namespace

void Server::run_then(const std::string &command, std::function<void(bool ok)> done) {
	int fds[2];
	if (pipe(fds) != 0) {
		return;
	}
	const pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		signal(SIGPIPE, SIG_DFL);   // Dusk ignores it; the apps it starts must not
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);   // the pipe closes when it (and its children) finish
		close(fds[1]);
		execl("/bin/sh", "/bin/sh", "-c", command.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	auto *p = new Pending{this, pid, fds[0], nullptr, std::move(done)};
	p->source = wl_event_loop_add_fd(wl_display_get_event_loop(display), fds[0],
		WL_EVENT_READABLE | WL_EVENT_HANGUP, handle_pending, p);
}

void Server::screenshot(const std::string &what) {
	if (what == "tool") {
		if (shot_tool) {
			if (shot_tool->active()) shot_tool->close();
			else shot_tool->open();
		}
		return;
	}
	wlr_box box{};
	if (what == "window") {
		if (!focused) {
			return;
		}
		box = focused->frame_box();
	} else if (what == "screen") {
		if (Output *o = output_at(cursor->x, cursor->y)) {
			box = o->box();
		}
	}
	capture(box, config.screenshot_pointer, 0);
}

namespace {
std::string self_exe() {
	char buf[4096];
	const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (n <= 0) return "dusk";
	buf[n] = 0;
	return buf;
}
} // namespace

void Server::capture(wlr_box box, bool pointer, int delay, std::vector<InkStroke> ink) {
	if (!have("grim")) {
		if (notifications) {
			notifications->post("Screenshots need grim", "Install it with: sudo dnf install grim wl-clipboard",
				"dialog-information-symbolic");
		}
		return;
	}
	// Menus, the control centre and the overview stay open: they're in the
	// picture too.
	std::string geometry;
	if (box.width > 0 && box.height > 0) {
		geometry = "-g " + shell_quote(std::to_string(box.x) + "," + std::to_string(box.y) + " " +
			std::to_string(box.width) + "x" + std::to_string(box.height));
	}
	if (pointer) {
		geometry += " -c";
	}
	// Full sharpness: at the pixel density of the sharpest screen in the
	// picture (with fractional scaling too), saved losslessly.
	int hd_factor = 1;
	{
		double sharpest = 1.0;
		for (Output *o : outputs) {
			if (!o->enabled()) continue;
			const wlr_box ob = o->box();
			const bool in = box.width <= 0 || (box.x < ob.x + ob.width && ob.x < box.x + box.width &&
				box.y < ob.y + ob.height && ob.y < box.y + box.height);
			if (in) sharpest = std::max(sharpest, static_cast<double>(o->output->scale));
		}
		// Big enough to share: a small picture (a region, a 1366×768 laptop)
		// is enlarged 2× (3×, 4×) so its long side reaches 1920 px. WhatsApp
		// only offers HD above ~1600 px and otherwise squeezes it into a
		// blurry JPEG. Whole pixels made bigger (blocky, not smoothed: a
		// smoothed enlargement looked blurry itself), after it's taken.
		if (config.screenshot_hd) {
			int lw = box.width, lh = box.height;
			if (lw <= 0 || lh <= 0) {
				int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
				for (Output *o : outputs) {
					if (!o->enabled()) continue;
					const wlr_box ob = o->box();
					x0 = std::min(x0, ob.x);
					y0 = std::min(y0, ob.y);
					x1 = std::max(x1, ob.x + ob.width);
					y1 = std::max(y1, ob.y + ob.height);
				}
				lw = std::max(0, x1 - x0);
				lh = std::max(0, y1 - y0);
			}
			const double longest = std::max(lw, lh) * sharpest;
			if (longest > 0 && longest < 1920) hd_factor = std::min(4, static_cast<int>(std::ceil(1920.0 / longest)));
		}
		char s[32];
		std::snprintf(s, sizeof s, " -s %.4g -l 3", sharpest);
		geometry += s;
	}

	const bool save = config.screenshot_save;
	const std::string dir = config.screenshot_dir.empty() ? pictures_dir() + "/Screenshots"
		: config.screenshot_dir;
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char name[256];
	const std::string pattern = config.screenshot_name.empty() ? "Screenshot from %Y-%m-%d %H-%M-%S"
		: config.screenshot_name;
	if (std::strftime(name, sizeof name, pattern.c_str(), &local) == 0) {
		std::strftime(name, sizeof name, "Screenshot from %Y-%m-%d %H-%M-%S", &local);
	}
	std::string base = name;
	std::replace(base.begin(), base.end(), '/', '-');
	// Not saving: a file in the runtime folder, only for the clipboard (and the editor).
	const char *runtime = std::getenv("XDG_RUNTIME_DIR");
	const std::string folder = save ? dir : std::string(runtime ? runtime : "/tmp") + "/dusk";
	const std::string file = folder + "/" + base + ".png";

	// Waits a moment first, so the screenshot tool itself is gone from the screen.
	const std::string shot_cmd = "sleep " + std::string(delay > 0 ? std::to_string(delay) : "0.15") + "; mkdir -p " +
		shell_quote(folder) + " && grim " + geometry + " " + shell_quote(file + ".part") +
		(hd_factor > 1 ? " && " + shell_quote(self_exe()) + " --scale-png " + shell_quote(file + ".part") + " " +
			std::to_string(hd_factor) : std::string()) + " && mv " +
		shell_quote(file + ".part") + " " + shell_quote(file);
	std::string cmd = "true";
	const std::string &editor = config.screenshot_editor;
	if (editor == "satty") {
		cmd += " && { command -v satty >/dev/null && satty --filename " + shell_quote(file) +
			" --output-filename " + shell_quote(file) + " >/dev/null 2>&1; true; }";
	} else if (editor == "swappy") {
		cmd += " && { command -v swappy >/dev/null && swappy -f " + shell_quote(file) + " -o " +
			shell_quote(file) + " >/dev/null 2>&1; true; }";
	}
	if (config.screenshot_copy) {
		cmd += " && { command -v wl-copy >/dev/null && wl-copy --type image/png < " + shell_quote(file) +
			" >/dev/null 2>&1 || true; }";   // wl-copy stays running: not on our pipe
	}
	if (config.screenshot_sound) {
		cmd += "; command -v canberra-gtk-play >/dev/null && canberra-gtk-play -i camera-shutter "
			">/dev/null 2>&1 &";
	}
	auto finished = [this, file, dir, save](bool ok) {
		if (!notifications) {
			return;
		}
		if (!ok) {
			notifications->post("Couldn't take a screenshot", "grim didn't manage to save it.",
				"dialog-error-symbolic");
			return;
		}
		if (!config.screenshot_notify) {
			return;
		}
		if (!save) {
			notifications->post("Screenshot copied", "Paste it anywhere.", file, 1, 0, {}, {});
			return;
		}
		// Its size in pixels (from the PNG header), so you can see it's full resolution.
		std::string size;
		if (FILE *f = std::fopen(file.c_str(), "rb")) {
			unsigned char h[24];
			if (std::fread(h, 1, 24, f) == 24) {
				const unsigned w = (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19];
				const unsigned hh = (h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23];
				size = " · " + std::to_string(w) + "×" + std::to_string(hh);
			}
			std::fclose(f);
		}
		const std::string shown = file.substr(file.rfind('/') + 1) + size;
		notifications->post("Screenshot saved", config.screenshot_copy ? shown + "\nAlso copied." : shown,
			file, 1, 0, {{"default", "Open"}, {"folder", "Show in folder"}},
			[this, file, dir](const std::string &key) {
				spawn("xdg-open " + shell_quote(key == "folder" ? dir : file));
			});
	};
	if (ink.empty() || box.width <= 0 || box.height <= 0) {
		run_then(shot_cmd + " && " + cmd, finished);
		return;
	}
	// Lines drawn in the tool: onto the picture (at its own pixel size) before
	// it's copied, opened in the editor or announced.
	run_then(shot_cmd, [this, file, box, ink = std::move(ink), cmd, finished](bool ok) {
		if (ok) {
			cairo_surface_t *img = cairo_image_surface_create_from_png(file.c_str());
			if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
				const double k = static_cast<double>(cairo_image_surface_get_width(img)) / box.width;
				cairo_t *cr = cairo_create(img);
				cairo_scale(cr, k, k);
				cairo_translate(cr, -box.x, -box.y);
				cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
				cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
				for (const InkStroke &st : ink) {
					if (st.points.empty()) continue;
					cairo_set_source_rgba(cr, st.color.r, st.color.g, st.color.b, st.color.a);
					cairo_set_line_width(cr, st.width);
					cairo_move_to(cr, st.points[0].first, st.points[0].second);
					if (st.points.size() == 1) cairo_line_to(cr, st.points[0].first + 0.1, st.points[0].second);
					for (size_t i = 1; i < st.points.size(); i++) cairo_line_to(cr, st.points[i].first, st.points[i].second);
					cairo_stroke(cr);
				}
				cairo_destroy(cr);
				cairo_surface_write_to_png(img, file.c_str());
			}
			cairo_surface_destroy(img);
		}
		if (!ok) {
			finished(false);
			return;
		}
		run_then(cmd, finished);
	});
}

} // namespace dusk
