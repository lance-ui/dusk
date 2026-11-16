// The control centre (like Arqen shell's): a sidebar with you at the top
// (picture, name, how long the computer has been on) and buttons to edit
// the tiles, open Settings and power off; a grid of tiles you arrange
// yourself (toggles, upright sliders, battery, what's playing); and the
// notifications underneath, stacked by app. It opens from a panel's status
// icons on the side of the screen that button is on.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "server.hpp"
#include "mediaview.hpp"
#include "ui.hpp"

namespace dusk {

class ControlCentre {
public:
	explicit ControlCentre(Server *server);
	~ControlCentre();
	ControlCentre(const ControlCentre &) = delete;
	ControlCentre &operator=(const ControlCentre &) = delete;

	bool is_open() const { return open_; }
	bool is_open_on(const Output *output) const { return open_ && output_ == output; }
	void open(Output *output, wlr_box anchor, const std::string &edge);
	// The notification centre: the same card showing only the date and the
	// notifications (when they're kept apart from the controls).
	void open_notes(Output *output, wlr_box anchor, const std::string &edge);
	void toggle_notes(Output *output, wlr_box anchor, const std::string &edge);
	bool notes_open_on(const Output *output) const { return open_ && notes_only_ && output_ == output; }
	bool notes_open() const { return open_ && notes_only_; }
	bool tiles_open_on(const Output *output) const { return open_ && !notes_only_ && output_ == output; }
	void close();
	void toggle(Output *output, wlr_box anchor, const std::string &edge);
	void toggle(Output *output);   // from the keyboard
	void output_gone(Output *output);
	void refresh();                // something it shows changed

	bool contains(double lx, double ly) const;
	void motion(double lx, double ly);
	void leave();
	void press(double lx, double ly, uint32_t button);   // returns grab via server->cc_grab
	void release();
	void scroll(double lx, double ly, double delta);
	bool handle_key(xkb_keysym_t sym, const std::string &text);
	bool wants_keyboard() const { return open_ && typing_; }

	NodeTag tag{NodeTag::Kind::ControlCentre, this};

private:
	struct Hot {
		std::string id;
		wlr_box box;      // local; what reacts to the pointer
		wlr_box vis;      // what lights up (often the same)
		double radius;    // of the highlight; < 0 = none
	};
	enum class Page { Tiles, Wifi, Bluetooth };
	bool notes_only_ = false;
	int notes_total_ = 0;   // the notifications' height, all of them
	wlr_box notes_anchor_{};
	std::string notes_edge_;
	void redraw_notes();

	void place();
	void redraw();
	void draw_header(cairo_t *cr, int &y, float scale);
	void draw_grid(cairo_t *cr, int &y, float scale);
	void draw_tile(cairo_t *cr, const CcTile &t, const wlr_box &b, float scale, bool floating);
	void draw_edit_tray(cairo_t *cr, int &y, float scale);
	void draw_detail(cairo_t *cr, int &y, int bottom, float scale);
	void draw_notifications(cairo_t *cr, int y, int bottom, float scale);
	// The other looks (Settings > Control centre > Look): Windows 11, GNOME, macOS.
	void draw_windows(cairo_t *cr, int &y, float scale);
	void draw_gnome(cairo_t *cr, int &y, float scale);
	void draw_macos(cairo_t *cr, int &y, float scale);
	bool compact() const;          // one of those: a card sized to fit, not a sidebar
	void draw_media(cairo_t *cr, const wlr_box &b, double radius, float scale, MediaView::Look look,
		bool hot);
	void draw_hslider(cairo_t *cr, const std::string &tile, double x, double y, double w, double h, float scale,
		int look, bool hot = true);
	void draw_pill_toggle(cairo_t *cr, const std::string &tile, double x, double y, double w, double h,
		float scale, int look, bool hot = true);
	// The media card is drawn on its own small layer over the rest, so the
	// wave and a scrolling title move without redrawing everything.
	void redraw_live();
	void place_live();
	void run_media();
	void media_menu(double lx, double ly);
	void output_menu(double lx, double ly);
	void add_hot(const std::string &id, double x, double y, double w, double h,
		double radius = -1, wlr_box vis = {});
	void update_highlight();
	std::string hot_at(double px, double py) const;
	void activate(const std::string &id);
	void open_page(Page page);
	void tile_action(const std::string &tile);   // the tile's main click
	void tile_more(const std::string &tile, double lx, double ly);   // its list or options
	void note_menu(uint32_t id, double lx, double ly);   // a notification's options (clicked)
	void show_tile_menu(const std::string &tile, double lx, double ly);
	void show_menu(double lx, double ly);   // right-click anywhere else
	void show_power_menu(double lx, double ly);
	void drag_slider(double lx, double ly);
	void drag_edit(double px, double py);
	void end_edit_drag();
	void save_layout();
	std::string layout_key() const;   // each look keeps its own arrangement
	int look_index() const;           // 0 Dusk, 1 Windows, 2 GNOME, 3 macOS
	wlr_box tile_box(const CcTile &t) const;   // local
	static int handle_timer(void *data);
	void queue_redraw();               // at most once a frame
	static int handle_redraw_timer(void *data);
	wl_event_source *redraw_timer_ = nullptr;
	bool redraw_queued_ = false;

	Server *server_;
	UiLayer layer_;
	Fades fx_;
	Highlight hl_;
	bool drawing_ = false;
	double hv(const std::string &id);    // eased hover amount
	int card_radius() const;
	wl_event_source *timer_;
	Output *output_ = nullptr;
	bool open_ = false;
	bool left_side_ = false;
	wlr_box box_{};
	double alpha_ = 0;
	uint64_t anim_ = 0;

	std::vector<Hot> hots_;
	std::string hover_;
	Page page_ = Page::Tiles;
	std::string dragging_;       // slider being dragged ("sl:<tile>")
	wlr_box drag_track_{};       // its track, local
	bool drag_vertical_ = true;
	int notes_scroll_ = 0, notes_max_scroll_ = 0;
	wlr_box notes_area_{};
	int list_scroll_ = 0, list_max_scroll_ = 0;
	wlr_box list_area_{};

	// The tile grid.
	std::vector<CcTile> layout_;   // what's shown, in grid order
	int grid_top_ = 0;             // local y of the grid's first row
	double cell_ = 50;             // a cell's width
	double row_h_ = 50;
	int drag_value_ = -1;          // the slider's value while dragged (changes only)            // and height (square, but shorter rows for the Windows look)
	bool editing_ = false;
	std::string edit_tile_;        // being moved or resized
	bool edit_resize_ = false;
	double grab_dx_ = 0, grab_dy_ = 0;   // pointer within the tile when it was picked up
	double edit_px_ = 0, edit_py_ = 0;   // the pointer now (local)
	CcTile edit_target_;           // where it would land
	bool edit_target_ok_ = false;
	std::set<std::string> open_groups_;   // notification stacks that are opened up

	// Wi-Fi password entry.
	bool typing_ = false;
	std::string password_;
	std::string password_for_;   // SSID
	std::string wifi_message_;
	bool connecting_ = false;

	// You: picture and name.
	cairo_surface_t *avatar_ = nullptr;
	bool avatar_tried_ = false;
	std::string user_name_;

	// What's playing.
	std::unique_ptr<MediaView> media_;
	wlr_scene_buffer *live_ = nullptr;   // the media card, redrawn on its own
	wlr_box media_box_{};                // where it is (local); width 0 = not shown
	MediaView::Look media_look_ = MediaView::Look::Samsung;
	double media_radius_ = 0;
	bool media_live_ = false;
	uint64_t media_ticker_ = 0;
	double live_wait_ = 0;
	int width_ = 392;
	std::string last_style_ = "dusk";
	bool bottom_ = false;                // compact looks: opens from a panel at the bottom
};

} // namespace dusk
