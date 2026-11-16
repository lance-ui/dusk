// The app status area ("tray"): apps that keep running in the background
// (Steam, Discord, chat and sync apps...) show an icon here instead of a
// window. Dusk is the StatusNotifierWatcher and host on the session D-Bus
// (the KDE/AppIndicator protocol), so closing such an app's window leaves it
// running here; click its icon to bring it back, right-click for its menu
// (read over com.canonical.dbusmenu and shown as Dusk's own menu).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <cairo.h>

#include "server.hpp"

struct sd_bus;
struct sd_bus_message;
struct sd_bus_slot;
struct sd_bus_error;

namespace dusk {

struct MenuItem;

struct TrayItem {
	std::string key;            // service + path: how it's known
	std::string service, path;
	std::string id, title, status = "Active", icon_name, attention_icon, theme_path, menu;
	bool item_is_menu = false;
	std::shared_ptr<cairo_surface_t> pixmap, attention_pixmap;   // sent as pixels
	bool loaded = false;        // properties read at least once
	std::string app_icon;       // the colour icon of the app that owns it (found from its process)
};

class Tray {
public:
	explicit Tray(Server *server);
	~Tray();
	Tray(const Tray &) = delete;
	Tray &operator=(const Tray &) = delete;

	bool running() const { return bus_ != nullptr; }
	// Shown items, in the order they came (passive ones are left out).
	std::vector<const TrayItem *> items() const;
	// Draws an item's icon into the square (x, y, size).
	void draw_icon(cairo_t *cr, const TrayItem &item, double x, double y, double size, float scale);
	void draw_icon_as_is(cairo_t *cr, const TrayItem &item, double x, double y, double size, float scale);

	// Clicks on an item (x, y: where, for the app's own menus).
	void activate(const std::string &key, int x, int y);
	void secondary(const std::string &key, int x, int y);
	void context_menu(const std::string &key, Output *output, wlr_box anchor, const std::string &edge);
	void scroll(const std::string &key, int delta);

	std::function<void()> changed;   // items came, went or changed their icon

private:
	static int handle_bus_fd(int fd, uint32_t mask, void *data);
	static int handle_bus_timeout(void *data);
	static int handle_message(sd_bus_message *m, void *data, sd_bus_error *error);
	static int handle_name_owner(sd_bus_message *m, void *data, sd_bus_error *error);
	static int handle_item_signal(sd_bus_message *m, void *data, sd_bus_error *error);
	static int handle_watcher_signal(sd_bus_message *m, void *data, sd_bus_error *error);
	void process_bus();
	void update_bus_events();

	int on_watcher_call(sd_bus_message *m);
	void add_item(const std::string &service, const std::string &path);
	void remove_item(const std::string &key);
	void refresh_item(const std::string &key);
	void read_properties(TrayItem &item, sd_bus_message *m);
	void emit_watcher(const char *member, const std::string &arg);
	void use_other_watcher();   // another program is the watcher: be a host only
	void notify();

	Server *server_;
	sd_bus *bus_ = nullptr;
	sd_bus_slot *filter_slot_ = nullptr;
	wl_event_source *fd_source_ = nullptr;
	wl_event_source *timeout_source_ = nullptr;
	wl_event_source *again_ = nullptr;   // says the tray is here again, shortly after login
	int again_count_ = 0;
	static int handle_again(void *data);
	void find_existing_items();
	void find_owner_app(const std::string &key);   // which installed app it belongs to
	bool watcher_ = false;      // we own org.kde.StatusNotifierWatcher
	std::vector<std::unique_ptr<TrayItem>> items_;
	std::map<std::string, sd_bus_slot *> item_slots_;   // their signal matches
};

} // namespace dusk
