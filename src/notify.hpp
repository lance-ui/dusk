// Desktop notifications: Dusk is the notification server apps talk to
// (org.freedesktop.Notifications on the session D-Bus). New notifications
// pop up in a corner; the control centre keeps them until you clear them
// (also across restarts).
#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "server.hpp"
#include "ui.hpp"

struct sd_bus;
struct sd_bus_message;
struct sd_bus_slot;
struct sd_bus_error;

namespace dusk {

struct Notification {
	uint32_t id = 0;
	uint64_t hid = 0;       // its place in the history (kept across restarts)
	std::string app, icon, summary, body, desktop_entry, image;
	std::string app_icon;   // the app's own icon (from its .desktop file), for the header
	std::shared_ptr<cairo_surface_t> picture;   // sent as pixels (image-data)
	std::vector<std::pair<std::string, std::string>> actions;   // key, label
	int urgency = 1;        // 0 low, 1 normal, 2 critical
	int timeout = -1;       // ms; -1 = default, 0 = stays
	bool transient = false; // not kept in the list
	std::time_t time = 0;
};

// One popup.
struct Toast {
	Notifications *owner;
	uint32_t id;
	std::unique_ptr<UiLayer> layer;
	NodeTag tag{NodeTag::Kind::Toast, this};
	void show() {   // position, fade and a slight zoom as it appears
		layer->show(x, static_cast<int>(std::lround(y)), alpha, 0.94 + 0.06 * std::min(1.0, alpha),
			width / 2.0, 0);
		update_timer_bar();
	}
	// The little line at the bottom right that shows how long it stays.
	void update_timer_bar();
	wlr_scene_rect *timer_bar = nullptr;   // drawn by the GPU, so it moves without redraws
	double deadline = 0;      // now_ms() when it goes away (0: it stays)
	double total = 0;         // its whole time on screen, ms
	double paused_left = -1;  // while pointed at: the time that was left
	wl_event_source *timer = nullptr;
	int width = 0, height = 0;
	double alpha = 0;       // fade in/out
	double y = 0;           // animated position (layout coords)
	double target_y = 0;
	int x = 0;
	uint64_t fade_anim = 0, move_anim = 0;
	double move_t = 1.0;
	double move_from = 0;
	bool closing = false;
	bool placed = false;    // has a position yet
	int hover = -1;         // -1 none, 0 card, 1 close button, 2+ action buttons
	std::vector<wlr_box> action_boxes;   // toast-local
	wlr_box close_box{};
};

class Notifications {
public:
	explicit Notifications(Server *server);
	~Notifications();
	Notifications(const Notifications &) = delete;
	Notifications &operator=(const Notifications &) = delete;

	bool running() const { return bus_ != nullptr; }
	const std::deque<Notification> &list() const { return list_; }
	int unseen = 0;   // arrived since the control centre was last opened (the panel shows a bell)
	std::function<void()> history_changed;   // a new one came in (the bell)

	// From the control centre.
	void dismiss(uint32_t id);
	void clear_all();
	void activate(uint32_t id);   // the "default" action, if any
	const Notification *get(uint32_t id) const { return find(id); }
	void run_action(uint32_t id, const std::string &key) { invoke(id, key); }

	// A notification from Dusk itself (battery, screenshots). `on_action`
	// gets the key of the button clicked ("default" = the card itself).
	uint32_t post(const std::string &summary, const std::string &body, const std::string &icon,
		int urgency = 1, uint32_t replaces = 0,
		std::vector<std::pair<std::string, std::string>> actions = {},
		std::function<void(const std::string &)> on_action = {});

	// Popup input.
	void toast_motion(Toast *toast, double lx, double ly);
	void toast_leave();
	void toast_click(Toast *toast, double lx, double ly, uint32_t button);

	void relayout();              // after screen or panel changes
	std::function<void()> changed;   // the list changed (control centre redraws)

private:
	static int handle_bus_fd(int fd, uint32_t mask, void *data);
	static int handle_bus_timeout(void *data);
	static int handle_message(sd_bus_message *m, void *data, sd_bus_error *error);
	static int handle_toast_timeout(void *data);
	void process_bus();
	void update_bus_events();

	int on_notify(sd_bus_message *m);
	void show_toast(const Notification &n);
	void draw_toast(Toast *toast);
	void close_toast(Toast *toast);
	void destroy_toast(Toast *toast);
	Toast *find_toast(uint32_t id);
	const Notification *find(uint32_t id) const;
	void emit_closed(uint32_t id, uint32_t reason);
	void emit_action(uint32_t id, const std::string &key);
	void invoke(uint32_t id, const std::string &key);
	void arm_timer(Toast *toast);
	void pause_timer(Toast *toast);
	void resume_timer(Toast *toast);
	void run_timer_bars();   // moves the countdown lines every frame while any is running
	uint64_t ticker_ = 0;

	Server *server_;
	sd_bus *bus_ = nullptr;
	sd_bus_slot *filter_slot_ = nullptr;
	wl_event_source *fd_source_ = nullptr;
	wl_event_source *timeout_source_ = nullptr;
	uint32_t next_id_ = 1;
	std::map<uint32_t, std::function<void(const std::string &)>> own_actions_;
	std::deque<Notification> list_;
	uint64_t next_hid_ = 1;
	std::vector<std::pair<std::string, uint32_t>> seen_;   // (app, id) already counted for the bell
	void remember(const Notification &n);
	void list_changed();   // saved, and the control centre told
	void load_list();
	void save_list();
	std::vector<std::unique_ptr<Toast>> toasts_;
	std::map<std::string, cairo_surface_t *> previews_;   // big pictures for toasts
	cairo_surface_t *preview_for(const Notification &n, float scale);
	Toast *hovered_ = nullptr;
};

// The picture for a notification (its own picture, icon or the app's icon)
// into the square (x, y, size), with the app's small icon as the fallback.
void draw_notification_icon(cairo_t *cr, IconLoader &icons, const Notification &n, double x, double y,
	double size, float scale);
// The app's icon name for a notification (from desktop-entry or its name).
std::string app_icon_for(const std::vector<AppInfo> &apps, const std::string &desktop_entry,
	const std::string &app_name);

} // namespace dusk
