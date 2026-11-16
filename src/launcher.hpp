// The app menu: lists installed apps (from .desktop files), type to search.
#pragma once

#include <string>
#include <vector>

#include "server.hpp"
#include "ui.hpp"

namespace dusk {

class Launcher {
public:
	explicit Launcher(Server *server);
	~Launcher();
	Launcher(const Launcher &) = delete;
	Launcher &operator=(const Launcher &) = delete;

	bool is_open() const { return open_; }
	bool is_open_on(const Output *output) const { return open_ && output_ == output; }
	// Opens next to `anchor` (the menu button) on a panel at `edge`.
	void open(Output *output, wlr_box anchor, const std::string &edge);
	void close();
	void toggle(Output *output, wlr_box anchor, const std::string &edge);
	// From the keyboard: next to the first menu button on that screen.
	void toggle(Output *output);
	void output_gone(Output *output);

	bool contains(double lx, double ly) const;
	void motion(double lx, double ly);
	void leave();
	void click(double lx, double ly, uint32_t button);
	void scroll(double delta, bool wheel = true);
	// The scroll bar: press on it and drag (or click the track to jump there).
	bool dragging_bar() const { return bar_drag_; }
	void drag_bar(double lx, double ly);
	void end_drag() { bar_drag_ = false; }
	// Returns true if the key was used by the menu.
	bool handle_key(xkb_keysym_t sym, const std::string &text);

	NodeTag tag{NodeTag::Kind::Launcher, this};

private:
	struct FileHit {
		std::string path, name;
		bool dir = false;
	};
	// The look (Settings › Panels › App menu): a list next to the button
	// (Dusk), Windows 11's Start, or a full-screen grid (GNOME, macOS).
	bool grid() const { return look_ != "dusk"; }
	bool full() const { return look_ == "gnome" || look_ == "macos"; }
	void measure();               // sizes and places of everything, for the look
	int item_at(double px, double py) const;   // grid looks: which app or file
	// What's listed: the best few apps, then (when searching) files and
	// folders, then the rest of the apps (so a file isn't buried under a
	// long list of loosely matching apps).
	// The rows: the best few apps, the files, the settings pages, then the
	// rest of the apps (all apps when there are no files or settings).
	int count() const { return static_cast<int>(shown_.size() + files_.size() + settings_hits_.size()); }
	int extras() const { return static_cast<int>(files_.size() + settings_hits_.size()); }
	int apps_head() const {
		return extras() == 0 ? static_cast<int>(shown_.size()) : std::min(5, static_cast<int>(shown_.size()));
	}
	bool is_file(int i) const { return i >= apps_head() && i < apps_head() + static_cast<int>(files_.size()); }
	bool is_setting(int i) const {
		return i >= apps_head() + static_cast<int>(files_.size()) && i < apps_head() + extras();
	}
	const FileHit &file_of(int i) const { return files_[static_cast<size_t>(i - apps_head())]; }
	int setting_of(int i) const {
		return settings_hits_[static_cast<size_t>(i - apps_head() - static_cast<int>(files_.size()))];
	}
	int app_of(int i) const {
		return shown_[static_cast<size_t>(i < apps_head() ? i : i - extras())];
	}

	// Dusk's menu, while nothing is typed: pinned apps, places, recent files
	// (or every app, A to Z, after "All apps").
	bool home() const { return look_ == "dusk" && query_.empty() && !all_apps_; }
	bool all_apps_ = false;
	struct Hot {
		std::string id;
		wlr_box box;
		double r;
	};
	std::vector<Hot> hots_;     // the home's chips, recent files, links and footer buttons
	int hover_hot_ = -1;
	int hot_at(double px, double py) const;
	void hot_action(const std::string &id);
	struct Recent {
		std::string path, name, where, when;
	};
	std::vector<Recent> recent_;
	void load_recent();
	std::vector<int> settings_hits_;   // Settings pages that match (indexes into the table)
	std::string calc_;                 // the answer, when what's typed is a sum
	wlr_box calc_box_{};
	void draw_home(cairo_t *cr, float scale);
	int places_height() const;
	int pinned_ = 0;
	void draw_footer(cairo_t *cr, float scale);
	std::string item_name(int i) const;
	std::string item_icon(int i) const;
	// Favourites (pinned in the app menu): a row above the list.
	int fav_at(double px, double py) const;
	wlr_box fav_box(int k) const;
	void draw_favs(cairo_t *cr, float scale, const Color &fg);
	void item_menu(int item, int fav, double lx, double ly);
	void toggle_favourite(const std::string &id);
	// Files: searched for in the background as you type.
	void search_files();
	static int handle_search_timer(void *data);
	wlr_box item_box(int index, double scroll) const;
	int max_first() const;
	wlr_box power_box() const;
	wlr_box settings_box() const;
	void filter();
	void redraw();            // soon (once, however often it's asked for before then)
	void redraw_now();
	void redraw_grid();
	static void handle_redraw_idle(void *data);
	wl_event_source *redraw_idle_ = nullptr;
	// Full screen (GNOME, macOS): the windows step aside while it's open, as
	// in the overview, so the blur shows the wallpaper (blurred windows made
	// dark smudges round everything).
	void hide_windows(bool hide);
	bool windows_hidden_ = false;
	// Full screen: built like the overview's backdrop: its own blur over the
	// whole screen (a little past the edges) and a dark tint in front, under
	// the panels; the drawing (and highlights) go on top, without a blur or
	// shadow of their own.
	wlr_scene_tree *full_tree_ = nullptr;
	wlr_scene_buffer *full_bg_ = nullptr;   // the softened wallpaper, tinted
	uint64_t full_bg_key_ = 0;              // which wallpaper / size / look it shows
	void place_backdrop();
	void place();
	void launch(int row);
	void keep_selection_visible();
	int visible_rows() const;
	int row_at(double px, double py) const;
	bool on_power(double px, double py) const;
	bool on_settings(double px, double py) const;

	Server *server_;
	UiLayer layer_;
	Fades fx_;
	Highlight sel_;    // the selected row (keyboard)
	Highlight hl_;     // under the pointer
	void update_highlights();
	bool drawing_ = false;
	Output *output_ = nullptr;
	bool open_ = false;
	wlr_box box_{};
	wlr_box anchor_{};
	std::string edge_ = "bottom";
	uint64_t anim_ = 0;
	double alpha_ = 0.0;

	std::string look_ = "dusk";
	int w_ = 420, h_ = 540;
	// Grid looks: the search field, the grid and its cells.
	wlr_box search_{};
	wlr_box grid_{};
	int dots_y_ = 0;           // GNOME / macOS: the page dots
	int cols_ = 1, cell_w_ = 100, cell_h_ = 100, icon_ = 48, grid_rows_ = 1;
	std::string user_name_;
	bool keyed_ = false;          // moved with the arrow keys: show which app is picked

	std::vector<int> shown_;   // indexes into server_->apps.apps()
	std::vector<FileHit> files_;
	std::vector<int> favs_;    // the favourite apps (indexes into apps), shown while not searching
	wlr_box favs_area_{};      // where they're drawn (header and row)
	int list_top_ = 0;         // Dusk's list: where the rows start (under the favourites)
	int hover_fav_ = -1;
	wl_event_source *search_timer_ = nullptr;
	unsigned search_gen_ = 0;
	std::string query_;
	int selected_ = 0;
	int first_row_ = 0;
	int hover_row_ = -1;
	bool hover_power_ = false;
	bool hover_settings_ = false;
	double scroll_accum_ = 0;
	bool bar_drag_ = false;
	bool on_bar(double px, double py) const;
};

} // namespace dusk
