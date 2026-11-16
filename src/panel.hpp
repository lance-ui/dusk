// A panel: a bar on one screen edge holding widgets (app menu, window
// buttons, pinned apps, clock, status icons...). There can be several,
// on any edge, each set up in Settings.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "server.hpp"
#include "ui.hpp"

namespace dusk {

struct MenuItem;

class Panel {
public:
	Panel(Server *server, Output *output, const PanelConfig &config);
	~Panel();
	Panel(const Panel &) = delete;
	Panel &operator=(const Panel &) = delete;

	const PanelConfig &config() const { return config_; }
	void set_config(const PanelConfig &config);
	Output *output() const { return output_; }

	int thickness() const;   // the strip along the edge, including margins
	int reserved() const;    // space kept free for windows (0 when auto-hiding)
	// Positions the panel `offset` px in from its screen edge (panels on
	// the same edge stack), between `from` and `to` along the edge.
	void place(int offset, int from, int to);
	void redraw();

	bool contains(double lx, double ly) const;
	void motion(double lx, double ly);
	void leave();
	void click(double lx, double ly, uint32_t button);
	// Pinned apps are dragged into a new order: a press on one waits to see
	// if it's a click (on release) or a drag (it moves along as you go).
	bool pin_press(double lx, double ly);
	void pin_drag(double lx, double ly);
	void pin_release(double lx, double ly);
	// A function that finds this panel again later (it may have been rebuilt).
	std::function<Panel *()> finder() const;
	// The wheel over the panel: true if a widget used it (the overview
	// button switches workspaces).
	bool scroll(double lx, double ly, double delta);
	// Every pointer move on this panel's screen, for auto-hide.
	void pointer_moved(double lx, double ly);
	// Where the control centre / app menu should appear from (layout coords).
	wlr_box anchor(const std::string &widget) const;
	wlr_box bar_box() const;   // the visible bar, layout coords
	// Where a window's button is (layout coords), for minimise animations.
	bool task_box(const View *view, wlr_box *out) const;

	NodeTag tag{NodeTag::Kind::Panel, this};

	// Only the bar itself takes clicks, not the see-through margin around it.
	static bool accepts_input(wlr_scene_buffer *buffer, double *sx, double *sy);

private:
	struct Slot {
		std::string kind;        // the widget
		wlr_box box{};           // panel-local
		View *view = nullptr;    // tasks: the window
		int pin = -1;            // pins: index into config_.pins
		std::string tray;        // tray: the item's key
		int widget = -1;         // index into config_.widgets
		bool icon_only = false;  // tasks: no room for a title
		std::string key;         // stable name for its hover/position animations
	};

	void layout();
	// Right-click: a widget's own menu, or (on empty bar, a flexible space
	// or a separator) the panel's.
	void show_menu(const Slot *slot, double lx, double ly);
	void show_panel_menu(const Slot *slot, double lx, double ly);
	void add_widget_items(const Slot &slot, std::vector<MenuItem> &items);
	void draw_slot(cairo_t *cr, const Slot &slot, double hover, float scale);
	void draw_battery_ring(cairo_t *cr, double x, double y, double w, double h);
	std::string media_label() const;
	int slot_at(double px, double py) const;
	void show(bool on);
	static int handle_hide_timer(void *data);
	bool something_open() const;   // menu or control centre opened from here
	const AppInfo *pin_app(int index) const;
	View *pin_window(int index) const;
	std::vector<std::string> status_icons() const;

	Server *server_;
	Output *output_;
	PanelConfig config_;
	int drag_pin_ = -1;                   // the pinned app pressed (its index now)
	double drag_x_ = 0, drag_y_ = 0;      // where it was pressed
	bool pin_dragging_ = false;
	std::vector<std::string> pins_before_;
	UiLayer layer_;
	Fades fx_;
	Highlight hl_;     // hover, drawn by the GPU
	double slot_radius(const Slot &slot) const;
	double corner(const wlr_box &b) const;   // a button's corners, for the look
	void update_highlight();
	bool drawing_ = false;
	wlr_box strip_{};     // layout coords, when fully shown
	wlr_box bar_{};       // panel-local
	std::vector<Slot> slots_;
	int hover_ = -1;

	// The macOS dock's magnification (Settings › Panels › Magnify).
	bool magnifies() const;
	double magnify_at(const wlr_box &b) const;   // 0..1 for an icon at b
	double mag_ = 0;              // how far it's grown (eases in and out)
	double mag_x_ = 0, mag_y_ = 0;   // the pointer, panel-local
	uint64_t mag_anim_ = 0;
	wl_event_source *mag_idle_ = nullptr;
	void queue_magnify();

	// Auto-hide
	double shown_ = 1.0;
	uint64_t anim_ = 0;
	bool want_shown_ = true;
	wl_event_source *hide_timer_ = nullptr;
	bool hide_armed_ = false;
	double scroll_accum_ = 0;
	void draw_overview_button(cairo_t *cr, const wlr_box &b, double hover, float scale);
};

} // namespace dusk
