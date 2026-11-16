#include "trash.hpp"

#include <dirent.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "popover.hpp"

namespace dusk {

namespace {

std::string trash_files_dir() {
	const char *data = std::getenv("XDG_DATA_HOME");
	const char *home = std::getenv("HOME");
	const std::string base = data && *data ? std::string(data) : std::string(home ? home : "/tmp") + "/.local/share";
	return base + "/Trash/files";
}

void mkdirs(const std::string &path) {
	for (size_t pos = path.find('/', 1);; pos = path.find('/', pos + 1)) {
		mkdir(path.substr(0, pos).c_str(), 0700);
		if (pos == std::string::npos) break;
	}
}

} // namespace

Trash::Trash(Server *server) : server_(server), dir_(trash_files_dir()) {
	mkdirs(dir_);
	timer_ = wl_event_loop_add_timer(server->loop, &Trash::handle_timer, this);
	watch();
	recount();
}

Trash::~Trash() {
	if (source_) wl_event_source_remove(source_);
	if (inotify_fd_ >= 0) close(inotify_fd_);
	if (timer_) wl_event_source_remove(timer_);
}

void Trash::watch() {
	inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd_ < 0) return;
	if (inotify_add_watch(inotify_fd_, dir_.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO) < 0) {
		close(inotify_fd_);
		inotify_fd_ = -1;
		return;
	}
	source_ = wl_event_loop_add_fd(server_->loop, inotify_fd_, WL_EVENT_READABLE, &Trash::handle_inotify, this);
}

int Trash::handle_inotify(int fd, uint32_t, void *data) {
	auto *self = static_cast<Trash *>(data);
	alignas(inotify_event) char buffer[4096];
	while (read(fd, buffer, sizeof buffer) > 0) {
	}
	wl_event_source_timer_update(self->timer_, 200);   // a batch of files: count once
	return 0;
}

int Trash::handle_timer(void *data) {
	static_cast<Trash *>(data)->recount();
	return 0;
}

void Trash::recount() {
	int n = 0;
	if (DIR *d = opendir(dir_.c_str())) {
		while (dirent *e = readdir(d)) {
			const std::string name = e->d_name;
			if (name != "." && name != "..") n++;
		}
		closedir(d);
	}
	if (n == count_) return;
	count_ = n;
	for (auto &fn : changed) {
		if (fn) fn();
	}
}

void Trash::open() {
	// trash:/// is understood by GNOME Files, Nemo, Thunar and Dolphin (through
	// gio/xdg-open); failing that, the folder itself.
	server_->spawn("gio open trash:/// 2>/dev/null || xdg-open trash:/// 2>/dev/null || "
		"{ command -v dolphin >/dev/null && dolphin trash:/; } || xdg-open " + shell_quote(dir_));
}

void Trash::put(const std::vector<std::string> &paths) {
	if (paths.empty()) return;
	std::string cmd = "gio trash --";
	for (const std::string &p : paths) cmd += " " + shell_quote(p);
	server_->spawn(cmd);
}

void Trash::empty_now() {
	server_->spawn("gio trash --empty 2>/dev/null || { rm -rf " + shell_quote(dir_) + "/* " +
		shell_quote(dir_) + "/.[!.]* " + shell_quote(dir_ + "/../info") + "/*; }");
}

void Trash::add_menu_items(std::vector<MenuItem> &items, Output *output, wlr_box anchor, const std::string &edge) {
	Server *srv = server_;
	items.push_back(MenuItem::header(count_ == 0 ? "The trash is empty"
		: count_ == 1 ? "1 item in the trash" : std::to_string(count_) + " items in the trash"));
	items.push_back(MenuItem::action("Open the trash", [this] { open(); }, "trash"));
	MenuItem empty = MenuItem::action("Empty the trash…", [this, srv, output, anchor, edge] {
		std::vector<MenuItem> ask;
		ask.push_back(MenuItem::header("Delete everything in the trash for good?"));
		MenuItem yes = MenuItem::action("Empty the trash", [this] { empty_now(); });
		yes.danger = true;
		ask.push_back(yes);
		ask.push_back(MenuItem::action("Cancel", {}));
		srv->menu->show(output, anchor, edge, std::move(ask));
	});
	empty.enabled = count_ > 0;
	empty.danger = true;
	items.push_back(empty);
}

} // namespace dusk
