#include "recorder.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "anim.hpp"
#include "notify.hpp"
#include "server.hpp"

namespace dusk {

namespace {

std::string videos_dir() {
	const char *home = std::getenv("HOME");
	std::string dir = std::string(home ? home : "") + "/Videos";
	const std::string file = std::string(home ? home : "") + "/.config/user-dirs.dirs";
	if (FILE *f = std::fopen(file.c_str(), "r")) {
		char line[1024];
		while (std::fgets(line, sizeof line, f)) {
			std::string l = line;
			if (l.rfind("XDG_VIDEOS_DIR=\"", 0) == 0) {
				std::string v = l.substr(16);
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

Recorder::Recorder(Server *server) : server_(server) {
	tick_ = wl_event_loop_add_timer(server->loop, &Recorder::handle_tick, this);
}

Recorder::~Recorder() {
	stop();
	if (pid_ > 0) waitpid(pid_, nullptr, 0);
	if (source_) wl_event_source_remove(source_);
	if (fd_ >= 0) close(fd_);
	if (tick_) wl_event_source_remove(tick_);
	if (kill_timer_) wl_event_source_remove(kill_timer_);
}

double Recorder::elapsed() const {
	return started_ > 0 ? (now_ms() - started_) / 1000.0 : 0;
}

std::string Recorder::elapsed_text() const {
	const long s = static_cast<long>(elapsed());
	char buf[32];
	std::snprintf(buf, sizeof buf, "%ld:%02ld", s / 60, s % 60);
	return buf;
}

void Recorder::toggle() {
	if (active() || counting_down_) {
		stop();
		return;
	}
	Output *o = server_->output_at(server_->cursor->x, server_->cursor->y);
	if (o) start(wlr_box{}, o, 0);
}

void Recorder::start(wlr_box box, Output *output, int delay) {
	if (active() || counting_down_ || !output) return;
	// An area is recorded from one screen: the one most of it is on.
	const wlr_box ob = output->box();
	std::string name = output->name();
	if (box.width > 0 && box.height > 0) {
		Output *home = server_->output_at(box.x + box.width / 2.0, box.y + box.height / 2.0);
		if (home) {
			name = home->name();
			const wlr_box hb = home->box();
			const int x1 = std::max(box.x, hb.x), y1 = std::max(box.y, hb.y);
			const int x2 = std::min(box.x + box.width, hb.x + hb.width), y2 = std::min(box.y + box.height, hb.y + hb.height);
			box = wlr_box{x1, y1, std::max(2, x2 - x1), std::max(2, y2 - y1)};
		}
		// Encoders want even sizes.
		box.width -= box.width % 2;
		box.height -= box.height % 2;
		if (box.x == ob.x && box.y == ob.y && box.width >= ob.width - 1 && box.height >= ob.height - 1) {
			box = wlr_box{};
		}
	}
	if (delay > 0) {
		counting_down_ = true;
		server_->redraw_panels();
		server_->run_then("sleep " + std::to_string(delay), [this, box, name](bool) {
			if (!counting_down_) return;   // stopped meanwhile
			counting_down_ = false;
			begin(box, name);
		});
		return;
	}
	begin(box, name);
}

void Recorder::begin(wlr_box box, std::string output_name) {
	const Config &c = server_->config;
	const std::string dir = c.record_dir.empty() ? videos_dir() + "/Screencasts" : c.record_dir;
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char name[96];
	std::strftime(name, sizeof name, "Screencast from %Y-%m-%d %H-%M-%S", &local);
	std::string cmd = "exec " + shell_quote(server_->helper_path("dusk-record")) + " --file " +
		shell_quote(dir + "/" + name) + " --output " + shell_quote(output_name) + " --fps " +
		std::to_string(c.record_fps) + " --quality " + shell_quote(c.record_quality) + " --audio " +
		shell_quote(c.record_audio);
	if (box.width > 0) {
		cmd += " --geometry " + shell_quote(std::to_string(box.x) + "," + std::to_string(box.y) + " " +
			std::to_string(box.width) + "x" + std::to_string(box.height));
	}
	int fds[2];
	if (pipe(fds) != 0) return;
	const pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	pid_ = pid;
	fd_ = fds[0];
	fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
	fcntl(fd_, F_SETFD, FD_CLOEXEC);
	out_buf_.clear();
	file_.clear();
	error_.clear();
	started_ = 0;
	stop_requested_ = false;
	source_ = wl_event_loop_add_fd(server_->loop, fd_, WL_EVENT_READABLE | WL_EVENT_HANGUP,
		&Recorder::handle_output, this);
	server_->redraw_panels();
}

void Recorder::stop() {
	if (counting_down_) {
		counting_down_ = false;
		server_->redraw_panels();
		return;
	}
	if (pid_ > 0) {
		stop_requested_ = true;
		kill(pid_, SIGTERM);   // it finishes the file, then ends
		// A hung recorder must not keep the red pill forever: three seconds
		// later it ends the hard way (the file keeps whatever it got).
		if (!kill_timer_) {
			kill_timer_ = wl_event_loop_add_timer(server_->loop, [](void *d) {
				auto *self = static_cast<Recorder *>(d);
				if (self->pid_ > 0) {
					kill(self->pid_, SIGKILL);
				}
				return 0;
			}, this);
		}
		wl_event_source_timer_update(kill_timer_, 3000);
	}
}

int Recorder::handle_output(int fd, uint32_t mask, void *data) {
	auto *self = static_cast<Recorder *>(data);
	char buf[512];
	for (;;) {
		const ssize_t n = read(fd, buf, sizeof buf);
		if (n > 0) {
			self->out_buf_.append(buf, static_cast<size_t>(n));
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR) && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) break;
		// The recorder ended.
		int status = 0;
		waitpid(self->pid_, &status, 0);
		wl_event_source_remove(self->source_);
		self->source_ = nullptr;
		close(self->fd_);
		self->fd_ = -1;
		self->pid_ = -1;
		if (self->kill_timer_) wl_event_source_timer_update(self->kill_timer_, 0);   // stop pending, recorder is gone
		self->finished(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		return 0;
	}
	size_t nl;
	while ((nl = self->out_buf_.find('\n')) != std::string::npos) {
		const std::string line = self->out_buf_.substr(0, nl);
		self->out_buf_.erase(0, nl + 1);
		if (line.rfind("FILE:", 0) == 0) {
			self->file_ = line.substr(5);
			self->started_ = now_ms();
			wl_event_source_timer_update(self->tick_, 1000);
			self->server_->redraw_panels();
		} else if (line.rfind("ERROR:", 0) == 0) {
			self->error_ = line.substr(6);
		}
	}
	return 0;
}

int Recorder::handle_tick(void *data) {
	auto *self = static_cast<Recorder *>(data);
	if (!self->active()) return 0;
	self->server_->redraw_panels();   // the time on the pill
	wl_event_source_timer_update(self->tick_, 1000);
	return 0;
}

void Recorder::finished(bool ok) {
	wl_event_source_timer_update(tick_, 0);
	const double length = elapsed();
	started_ = 0;
	server_->redraw_panels();
	Notifications *n = server_->notifications.get();
	if (!ok || file_.empty()) {
		if (n && !(stop_requested_ && file_.empty() && error_.empty())) {
			n->post("Couldn't record the screen",
				error_.find("wf-recorder") != std::string::npos
					? "Recording needs wf-recorder: sudo dnf install wf-recorder"
					: error_.empty() ? "The recorder stopped unexpectedly." : "No video encoder worked: " + error_,
				"dialog-error-symbolic");
		}
		return;
	}
	if (!n) return;
	const std::string file = file_;
	const std::string dir = file.substr(0, file.rfind('/'));
	const std::string shown = file.substr(file.rfind('/') + 1);
	const long s = static_cast<long>(length);
	char dur[32];
	std::snprintf(dur, sizeof dur, "%ld:%02ld", s / 60, s % 60);
	// A frame from it for the notification, if ffmpeg is there.
	const char *runtime = std::getenv("XDG_RUNTIME_DIR");
	const std::string thumb = std::string(runtime ? runtime : "/tmp") + "/dusk/screencast-" +
		std::to_string(static_cast<long>(std::time(nullptr))) + ".png";
	server_->run_capture("mkdir -p " + shell_quote(thumb.substr(0, thumb.rfind('/'))) +
		"; command -v ffmpeg >/dev/null && ffmpeg -v error -y -ss 0.5 -i " + shell_quote(file) +
		" -frames:v 1 -vf scale=640:-2 " + shell_quote(thumb) + " </dev/null >/dev/null 2>&1; true",
		[this, file, dir, shown, thumb, dur = std::string(dur)](const std::string &) {
			Notifications *n = server_->notifications.get();
			if (!n) return;
			const bool have_thumb = access(thumb.c_str(), R_OK) == 0;
			n->post("Screen recording saved", shown + "\n" + dur,
				have_thumb ? thumb : std::string("video-x-generic"), 1, 0,
				{{"default", "Play"}, {"folder", "Show in folder"}},
				[this, file, dir](const std::string &key) {
					server_->spawn("xdg-open " + shell_quote(key == "folder" ? dir : file));
				});
		});
}

} // namespace dusk
