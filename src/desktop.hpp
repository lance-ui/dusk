// Files and folders on the desktop: what's in your Desktop folder shows as
// icons on the main screen's wallpaper (under the windows).
//
//   - Double-click opens (folders in your file manager, apps from their
//     .desktop file). Drag icons to arrange them; drop one on a folder to
//     move it in. Positions are remembered.
//   - Drag files from an app (e.g. Files) onto the desktop to put them there.
//   - Click to select (Ctrl+click for several). With the desktop clicked,
//     Delete moves to the trash, F2 renames, Enter opens, Ctrl+C / Ctrl+X /
//     Ctrl+V copy, cut and paste, Ctrl+A selects everything.
//   - Right-click an icon for its menu; right-click the wallpaper for "New
//     folder", "Paste" and the rest.
#pragma once

#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "server.hpp"

namespace dusk {

struct MenuItem;

class Desktop {
public:
	explicit Desktop(Server *server);
	~Desktop();
	Desktop(const Desktop &) = delete;
	Desktop &operator=(const Desktop &) = delete;

	bool enabled() const { return server_->config.desktop_icons; }
	std::string folder() const { return dir_; }
	void relayout();                       // screens or panels changed
	void settings_changed(bool look);      // icons on/off, light/dark...
	void output_gone(Output *output);

	// Pointer, in layout coordinates. `hit` is true when the pointer is on an icon.
	void motion(double lx, double ly, bool hit);
	void press(double lx, double ly, uint32_t button, uint32_t time_msec, bool ctrl);
	void release(double lx, double ly);
	void click_empty();                    // a click on the bare wallpaper
	// Keys while the desktop has been clicked (no window active), or while renaming.
	bool wants_keyboard() const;
	bool handle_key(xkb_keysym_t sym, uint32_t modifiers, const std::string &text);
	void focus_lost();                     // a window was activated

	// The wallpaper's right-click menu: New folder, Paste...
	void add_menu_items(std::vector<MenuItem> &items, double lx, double ly);
	// Files dragged here from an app. `copy` keeps the originals.
	void drop_files(const std::vector<std::string> &paths, bool copy, double lx, double ly);
	// Is (lx, ly) somewhere files can be dropped (the desktop, or a folder on it)?
	bool takes_drop(double lx, double ly) const;

	NodeTag tag{NodeTag::Kind::Desktop, this};

private:
	struct Item {
		std::string name;          // file name
		std::string path;
		std::string label;         // what's shown (a .desktop file's Name)
		std::string icon;          // icon theme name (or path)
		bool dir = false;
		bool app = false;          // a .desktop launcher
		bool image = false;
		std::string special;       // "trash" or "home": not a file in the Desktop folder
		std::time_t mtime = 0;
		int col = -1, row = -1;
		wlr_scene_buffer *node = nullptr;
		cairo_surface_t *thumb = nullptr;
	};

	void scan();
	void place();
	void draw_item(Item &item);
	void draw_all();
	void clear_items();
	int item_at(double lx, double ly) const;
	wlr_box cell_box(int col, int row) const;     // layout coords
	bool cell_from_point(double lx, double ly, int *col, int *row) const;
	bool cell_free(int col, int row, const Item *except) const;
	void nearest_free(int *col, int *row, const Item *except) const;
	void load_positions();
	void save_positions();
	void open_item(const Item &item);
	void show_item_menu(int index, double lx, double ly);
	std::vector<std::string> selected_paths() const;
	void trash(const std::vector<std::string> &paths);
	void paste();
	void new_folder();
	void start_rename(const std::string &name);
	void finish_rename(bool keep, bool can_stay);
	void move_into(const std::vector<std::string> &paths, const std::string &target_dir, bool copy);
	std::string unique_name(const std::string &dir, const std::string &name) const;
	Output *screen() const;   // the main screen
	void watch();
	static int handle_inotify(int fd, uint32_t mask, void *data);
	static int handle_rescan(void *data);

	Server *server_;
	std::string dir_;
	wlr_scene_tree *tree_ = nullptr;
	Output *output_ = nullptr;
	std::vector<Item> items_;
	std::map<std::string, std::pair<int, int>> saved_;   // name -> col, row
	std::set<std::string> selected_;
	std::string hover_;
	int cols_ = 1, rows_ = 1;
	wlr_box area_{};

	// Clicks and drags.
	std::string pressed_;
	double press_x_ = 0, press_y_ = 0;
	bool dragging_ = false;
	std::string last_click_;
	uint32_t last_click_time_ = 0;
	bool keys_ = false;               // the desktop was clicked: it gets keys

	// Renaming.
	std::string renaming_;            // the item's name
	std::string rename_text_;
	size_t rename_cursor_ = 0;        // byte offset in rename_text_
	bool rename_fresh_ = true;        // the first key replaces the name (not its extension)
	std::string rename_error_;

	// Where newly arriving files go (drops, paste, new folder).
	std::map<std::string, std::pair<int, int>> pending_;
	std::string rename_when_seen_;    // a new folder: rename it once it's there

	int inotify_fd_ = -1;
	wl_event_source *inotify_source_ = nullptr;
	wl_event_source *rescan_timer_ = nullptr;
	std::string applied_;             // what the icons were last drawn for
};

} // namespace dusk
