// The trash (~/.local/share/Trash): how many things are in it, opening it in
// your file manager, emptying it. Used by the trash on a panel and on the
// desktop.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "server.hpp"

namespace dusk {

struct MenuItem;

class Trash {
public:
	explicit Trash(Server *server);
	~Trash();
	Trash(const Trash &) = delete;
	Trash &operator=(const Trash &) = delete;

	int count() const { return count_; }
	bool is_empty() const { return count_ == 0; }
	void open();                                      // in whatever file manager is installed
	void put(const std::vector<std::string> &paths);  // move files into it
	void empty_now();
	// Open, Empty the trash… (asks first), for a right-click menu shown at
	// (output, anchor, edge).
	void add_menu_items(std::vector<MenuItem> &items, Output *output, wlr_box anchor, const std::string &edge);
	// Called whenever the count changes (panels, the desktop).
	std::vector<std::function<void()>> changed;

private:
	void watch();
	void recount();
	static int handle_inotify(int fd, uint32_t mask, void *data);
	static int handle_timer(void *data);

	Server *server_;
	std::string dir_;   // .../Trash/files
	int count_ = 0;
	int inotify_fd_ = -1;
	wl_event_source *source_ = nullptr;
	wl_event_source *timer_ = nullptr;
};

} // namespace dusk
