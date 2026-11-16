// The overview, like GNOME's Activities: tap Super (or push the pointer into
// the hot corner, or click the overview button on a panel) and every window
// of the workspace glides out into a tidy grid on a card of the wallpaper,
// with the workspaces in a row above it and a search field on top.
//
//   - Click a window to go to it; middle-click or its ✕ closes it.
//   - Type to search: windows (from every workspace) by title and app, and
//     apps to open. Enter picks the first; arrows move between them.
//   - The workspaces before and after peek in at the sides: click one to go
//     there, or drag a window onto it to move it. After the last one is a
//     "New workspace" card. Scrolling switches workspaces. Escape goes back.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ghost.hpp"
#include "server.hpp"

namespace dusk {

class Overview {
public:
	explicit Overview(Server *server);
	~Overview();
	Overview(const Overview &) = delete;
	Overview &operator=(const Overview &) = delete;

	bool is_open() const { return open_; }
	bool shown() const { return !screens_.empty(); }   // open, or still closing
	void open(Output *from = nullptr);
	// Closes; with `pick`, that window is focused once it's back in place.
	void close(View *pick = nullptr);
	void toggle(Output *from = nullptr);
	void workspaces_changed();          // windows or workspaces changed: lay out again
	void view_gone(View *view);
	void output_gone(Output *output);

	void motion(double lx, double ly);
	void press(double lx, double ly, uint32_t button);
	void release(double lx, double ly, uint32_t button);
	void scroll(double delta, bool horizontal);
	bool handle_key(xkb_keysym_t sym, uint32_t modifiers, const std::string &text);

	NodeTag tag{NodeTag::Kind::Overview, this};

private:
	struct Screen;
	struct Window {
		View *view = nullptr;
		Screen *screen = nullptr;
		std::unique_ptr<Ghost> ghost;
		wlr_scene_rect *glow = nullptr;      // hover / keyboard selection, behind it
		wlr_scene_buffer *badge = nullptr;   // its app's icon
		wlr_scene_buffer *label = nullptr;   // its title (hover / selection)
		wlr_scene_buffer *close = nullptr;   // ✕ (hover)
		GhostRect real;                      // where it is on the screen
		GhostRect place;                     // where it goes in the overview
		GhostRect from;                      // where the current animation started
		double away = 0;                     // leaving with a workspace switch: how far it slides
		bool in_layout = false;
		bool was_in_layout = false;
		std::string label_drawn;             // what the label buffer shows (redrawn only when it changes)
		int label_w = 0;
		int close_drawn = -1;                // 0 grey, 1 red
		Window() = default;
		Window(const Window &) = delete;
		Window &operator=(const Window &) = delete;
		~Window() {
			for (wlr_scene_node *n : {glow ? &glow->node : nullptr, badge ? &badge->node : nullptr,
					label ? &label->node : nullptr, close ? &close->node : nullptr}) {
				if (n) wlr_scene_node_destroy(n);
			}
		}
	};
	struct WsThumb {
		int index = 0;
		wlr_box box{};
		wlr_scene_rect *ring = nullptr;      // accent ring round the one shown
		wlr_scene_buffer *wall = nullptr;
		wlr_scene_rect *plus = nullptr;      // empty workspace: a faint fill
		std::vector<std::unique_ptr<Ghost>> minis;
	};
	struct AppHit {
		int app = -1;                        // index into server->apps.apps()
		wlr_box box{};
	};
	struct Screen {
		Output *output = nullptr;
		bool primary = false;
		wlr_scene_tree *tree = nullptr;
		wlr_scene_blur *blur = nullptr;
		wlr_scene_rect *dim = nullptr;
		wlr_scene_buffer *card = nullptr;    // the workspace: its wallpaper
		wlr_scene_buffer *peek[2] = {};      // the workspaces before and after, at the sides
		wlr_scene_tree *strip = nullptr;     // workspace thumbnails
		wlr_scene_buffer *search = nullptr;
		wlr_scene_buffer *apps = nullptr;    // app results (search)
		wlr_scene_buffer *add = nullptr;     // "New workspace", after the last one
		wlr_scene_buffer *label = nullptr;   // "Workspace 2 of 3"
		wlr_box add_box{};
		wlr_box remove_box{};                // the ✕ after "Workspace 2 of 3" (or on the shown one's picture)
		wlr_box strip_box{};                 // all the workspace pictures (empty: none shown)
		wlr_box strip_add{};                 // the "+" at the end of them
		std::vector<wlr_scene_buffer *> strip_bits;   // the numbers, the ✕ and the +: fade with the rest
		wlr_scene_tree *windows = nullptr;   // the window copies, on top
		wlr_box card_box{};
		wlr_box peek_box[2]{};
		wlr_box search_box{};
		wlr_box apps_box{};
		std::vector<std::unique_ptr<WsThumb>> thumbs;
		std::vector<AppHit> app_hits;
	};

	void build_screen(Output *output, bool primary);
	void destroy_all();
	void layout();                        // everything's place, from the state now
	void layout_screen(Screen &s);
	void place_windows(Screen &s, std::vector<Window *> &wins, const wlr_box &area, double max_scale);
	void build_strip(Screen &s);
	void draw_search(Screen &s);
	void draw_apps(Screen &s);
	void draw_window_extras(Window &w);
	void apply(double t);                 // the open/close animation at t (0 = real places, 1 = overview)
	void glide();                         // after a layout change: from where they are to the new places
	void set_alpha(double a);
	bool matches(const View *view) const;
	std::vector<int> app_results() const;
	std::vector<Window *> selectable();   // in keyboard order
	void select(int index);
	void activate_selection();
	Window *window_at(double lx, double ly, bool *on_close = nullptr);
	WsThumb *thumb_at(double lx, double ly, Screen **screen = nullptr);
	int peek_at(double lx, double ly) const;   // -1 / +1 for the side workspaces, 0 none
	int app_at(double lx, double ly) const;
	void set_hover(Window *w);
	Screen *screen_for(Output *output);
	void finish_close();

	Server *server_;
	bool open_ = false;
	std::vector<std::unique_ptr<Screen>> screens_;
	std::vector<std::unique_ptr<Window>> windows_;
	double t_ = 0;                        // 0 closed .. 1 open
	uint64_t anim_ = 0, glide_anim_ = 0;
	double alpha_ = 0;
	View *pick_ = nullptr;                // focus this one when closed
	std::string query_;
	int selected_ = -1;                   // in selectable() (+ apps after the windows)
	Window *hover_ = nullptr;
	bool hover_close_ = false;
	// Dragging a window onto a workspace.
	Window *pressed_ = nullptr;
	double press_x_ = 0, press_y_ = 0;
	bool dragging_ = false;
	double scroll_accum_ = 0;
	// Workspace changes come in bursts (switching, then tidying up empty
	// ones): rebuilt once, a moment later, not three times in a row.
	wl_event_source *rebuild_ = nullptr;
	int shown_ws_ = 0;     // the workspace the card showed last
	int slide_dir_ = 0;    // which way the windows go when it changes
	static void handle_rebuild(void *data);
	void rebuild();
	uint32_t last_scroll_ms_ = 0;
	bool layers_hidden_ = false;
};

} // namespace dusk
