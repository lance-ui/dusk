// Core types of the Dusk compositor.
#pragma once

#include <sys/types.h>

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "anim.hpp"
#include "audiolevel.hpp"
#include "lyrics.hpp"
#include "recorder.hpp"
#include "apps.hpp"
#include "config.hpp"
#include "ghost.hpp"
#include "imageload.hpp"
#include "listener.hpp"
#include "wlr.hpp"

namespace dusk {

// Sizes in logical pixels. (The title bar height is a setting:
// theme::title_height.)
constexpr int kResizeMargin = 6;    // invisible grab area outside the frame
constexpr int kSnapThreshold = 2;   // px from a screen edge that triggers snapping

class Server;
class View;

// A line drawn on a screenshot before it's taken (layout coordinates).
struct InkStroke {
	std::vector<std::pair<double, double>> points;
	Color color;
	double width = 4;
};

class Panel;
class Launcher;
class Osd;
class ControlCentre;
class Notifications;
class SystemStatus;
class Popover;
class ContextMenu;
class SnapLayouts;
class LockScreen;
class Calendar;
class MediaCard;
class Clipboard;
class ClipboardCard;
class Osk;
class Desktop;
class ScreenshotTool;
class WallpaperWorker;
class Tray;
class Overview;
class Trash;
class AudioLevel;
class Lyrics;
class Recorder;
class Canvas;
struct Toast;

// Stored in wlr_scene_node::data on our top-level scene trees, so a hit
// test can tell which object a scene node belongs to.
struct NodeTag {
	enum class Kind { View, Panel, Launcher, Unmanaged, ControlCentre, Toast, Popover, Desktop, Overview, Osd } kind;
	void *object;
};

// What is under the pointer.
enum class Region { None, Content, Titlebar, Close, Maximize, Minimize, Edge };

struct Output {
	Server *server;
	wlr_output *output;
	wlr_scene_buffer *wallpaper = nullptr;
	wlr_scene_buffer *corners[4] = {};   // rounded screen corner masks
	// A game or video full screen here: Dusk gets out of the way (no
	// corners drawn over it, so the GPU can show it directly).
	bool game = false;
	bool tearing = false;   // the game asked to skip vsync, and it's on
	int corner_radius = -1;
	std::vector<std::unique_ptr<Panel>> panels;
	int wallpaper_w = 0, wallpaper_h = 0;
	float wallpaper_scale = 0;
	// wallpaper.cpp: what's shown (to tell a new picture from a resize), the
	// picture asked of the worker, and a transition under way.
	std::string wall_shown, wall_wanted;
	uint64_t wall_generation = 0;
	wlr_scene_buffer *wall_next = nullptr;   // the new one, coming in over the old
	std::vector<wlr_scene_buffer *> wall_strips;   // the diagonal wipe: the new one in thin strips
	uint64_t wall_anim = 0;
	void show_wallpaper(Canvas &canvas, const std::string &content);
	// A small copy of the wallpaper (1/16 its size): stretched back up it's a
	// smooth blur, for full-screen backdrops (the app grid) that mustn't
	// pick up anything else on the screen. `wall_soft_gen` counts new ones.
	cairo_surface_t *wall_soft = nullptr;
	uint64_t wall_soft_gen = 0;
	void finish_wallpaper_transition();
	// Space kept free by panels at each edge: top, right, bottom, left.
	int reserved[4] = {0, 0, 0, 0};
	int gamma_temp = -1;   // night light currently applied (0 = off)
	std::string applied;   // the settings last applied (to skip needless mode sets)

	Listener on_frame, on_request_state, on_destroy;

	Output(Server *server, wlr_output *output);
	~Output();

	std::string name() const { return output->name ? output->name : ""; }
	wlr_box box() const;      // whole output, layout coordinates
	wlr_box usable() const;   // minus the panels
	bool is_primary() const;
	void update_wallpaper();
	void update_corners();
	void update_panels();     // create/remove panels to match the settings
	// Applies the settings file: on/off, resolution and refresh rate,
	// rotation, scale, variable refresh and position.
	void configure();
	bool enabled() const { return scene_output != nullptr; }
	wlr_scene_output *scene_output = nullptr;
	void apply_night_light();

	void handle_frame(void *data);
	void handle_request_state(void *data);
	void handle_destroy(void *data);
};

struct Keyboard {
	Server *server;
	wlr_keyboard *keyboard;
	std::vector<uint32_t> swallowed;   // keys whose press we kept from apps
	bool caps = false, num = false;    // lock key state, for the OSD
	xkb_layout_index_t layout = 0;
	Listener on_modifiers, on_key, on_destroy;

	bool is_virtual = false;   // from a program (remote desktop, on-screen keyboard)
	Keyboard(Server *server, wlr_keyboard *keyboard, bool is_virtual = false);
	void apply_settings();
	void set_locks(int num_lock, int caps_lock);   // 1 on, 0 off, -1 leave it
	void handle_modifiers(void *data);
	void handle_key(void *data);
	void handle_destroy(void *data);
};

// A mouse or touchpad, kept so settings can be re-applied when they change.
struct Pointer {
	Server *server;
	wlr_input_device *device;
	Listener on_destroy;

	Pointer(Server *server, wlr_input_device *device);
	void apply_settings();
	void handle_destroy(void *data);
};

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

// What all windows have in common: position, the frame we draw, and the
// window states (maximised, snapped, minimised, fullscreen). XdgView
// (Wayland apps) and XwaylandView (X11 apps) fill in the app-specific parts.
class View {
public:
	Server *server;
	uint64_t id;                          // creation order, for the taskbar

	wlr_scene_tree *tree = nullptr;       // positioned at the frame's top-left
	wlr_scene_tree *content = nullptr;    // the app's surfaces
	wlr_scene_tree *frame = nullptr;      // our decorations
	wlr_scene_rect *backdrop = nullptr;   // black behind fullscreen windows
	wlr_scene_shadow *shadow = nullptr;
	wlr_scene_blur *frost = nullptr;      // blur behind see-through windows (Dusk's Settings)
	wlr_scene_rect *border = nullptr;     // the outline (a rounded ring)
	wlr_scene_rect *csd_outline = nullptr; // the same outline around apps that draw their own title bar
	wlr_scene_buffer *titlebar = nullptr;
	NodeTag tag{NodeTag::Kind::View, this};

	int x = 0, y = 0;               // frame position (layout coords)
	std::string home_output;        // the screen it was on before that was unplugged
	int home_x = 0, home_y = 0;     // its position there (relative to that screen)
	bool mapped = false;
	bool minimized = false;
	bool maximized = false;
	bool fullscreen = false;
	// Tiled: Left/Right halves (Super+arrows, screen edges) or any zone of
	// a snap layout. `zone` is in fractions of the screen's free area.
	enum class Snap { None, Left, Right, Zone } snap = Snap::None;
	struct Zone {
		double x = 0, y = 0, w = 1, h = 1;
	};
	Zone zone;
	std::string tile_output;        // the screen it's tiled or maximised on
	wlr_box restore{};              // frame pos + content size before max/snap/fullscreen
	bool fullscreen_was_maximized = false;
	Snap fullscreen_was_snap = Snap::None;
	bool server_side = false;       // do we draw the title bar?
	bool placed = false;
	// Workspaces (GNOME-like): the one it's on, or on all of them.
	int workspace = 0;
	bool sticky = false;
	bool urgent = false;            // asked for attention (shown on its panel button)
	// Moved for a workspace switch (the slide), on top of x/y.
	int slide_x = 0;

	// Titlebar button under the pointer / being pressed.
	Region hover = Region::None;
	Region pressed = Region::None;

	explicit View(Server *server);
	virtual ~View();
	View(const View &) = delete;
	View &operator=(const View &) = delete;

	// ---- implemented by XdgView / XwaylandView ----
	virtual wlr_surface *surface() const = 0;
	virtual std::string title() const = 0;
	virtual std::string app_id() const = 0;
	virtual int content_width() const = 0;     // the app's own size
	virtual int content_height() const = 0;
	virtual int geometry_x() const { return 0; }  // offset of the visible
	virtual int geometry_y() const { return 0; }  // part in the surface
	virtual int min_width() const { return 0; }   // the smallest the app can be (0 = any)
	virtual int min_height() const { return 0; }
	virtual bool ready() const = 0;            // may we send it states yet?
	// Asks the app for a new size. Returns a serial the app acknowledges
	// when it has drawn that size (0 if the app can't tell us).
	virtual uint32_t request_content_size(int w, int h) = 0;
	virtual uint32_t acked_serial() const { return 0; }
	virtual bool acks_sizes() const { return false; }
	virtual void send_activated(bool on) = 0;
	virtual void send_maximized(bool on) = 0;
	virtual void send_tiled(bool) {}
	virtual void send_fullscreen(bool on) = 0;
	virtual void send_resizing(bool) {}
	virtual void send_position() {}           // X11 apps must be told where they are
	virtual void close() = 0;

	// ---- shared ----
	bool active() const;
	bool visible() const;          // mapped, not minimised, on the workspace shown
	bool on_shown_workspace() const;
	void sync_enabled();           // shows/hides the window for its workspace and state
	void set_slide(int dx, double alpha);   // a workspace switch moves and fades it
	bool decorated() const { return server_side && !fullscreen; }
	int corner_radius() const;   // 0 when maximised, snapped or fullscreen
	// The window's outline: none while it's maximised or full screen (edge to
	// edge, nothing to outline).
	int frame_line() const { return maximized || fullscreen ? 0 : theme::border_width; }
	int deco_left() const { return decorated() ? frame_line() : 0; }
	int deco_top() const { return decorated() ? frame_line() + theme::title_height : 0; }
	int deco_width() const { return decorated() ? 2 * frame_line() : 0; }
	int deco_height() const {
		return decorated() ? 2 * frame_line() + theme::title_height : 0;
	}
	wlr_box frame_box() const;
	Output *output() const;

	void move_to(int nx, int ny);
	// Moves and resizes. The move waits until the app has drawn the new
	// size, so the window doesn't jump to the new place at the old size.
	void configure_frame(int nx, int ny, int content_w, int content_h);
	void set_activated(bool on);
	void set_server_side(bool on);
	void draw_csd_outline();
	void redraw_frame();
	void update_effects();       // rounded corners of the app's surface
	void invalidate_frame() { last_frame_w_ = -1; }
	void update_positions();
	void update_layer();   // fullscreen windows go above the panel while focused

	void toggle_maximize();
	void set_maximized(bool on, Output *on_output = nullptr);
	void set_fullscreen(bool on);
	void refit();
	void snap_to(Snap where, Output *on = nullptr);
	void tile_to(const Zone &zone, Output *on);
	Output *tile_target() const;
	void sync_tiled();
	bool sent_tiled_ = false;   // tile_output if it's on, else where the window is
	wlr_box zone_frame(const Zone &zone, Output *on) const;   // where a zone puts the frame
	wlr_box button_box(Region which) const;   // a title bar button, layout coords
	void unsnap_for_drag(double cursor_x, double cursor_y);
	void minimize();
	void restore_from_minimize();
	void ensure_restore_box();

	Region region_at(double lx, double ly, uint32_t *edges) const;

	// Animations. While one runs, the real window is hidden (`anim_alpha`
	// 0) and a stretched/faded copy of it (a "ghost") is shown instead.
	double anim_alpha = 1.0;
	void animate_open();
	void snapshot_and_fade_out();   // close animation: the ghost fades away
	void cancel_animation();        // jump to the end (e.g. a drag starts)
	bool task_rect(GhostRect *out) const;   // its panel button, if shown

protected:
	// Called by the subclasses when the app maps/unmaps/commits/goes away.
	void did_map();
	void did_unmap();
	void did_commit();
	void did_destroy();   // detaches from the server; caller then deletes

private:
	struct Button {
		Region kind;
		int x;   // titlebar-local
	};
	std::vector<Button> buttons(int titlebar_width) const;

	int last_frame_w_ = -1;
	int seen_cw_ = -1, seen_ch_ = -1, seen_sw_ = -1, seen_sh_ = -1, seen_gx_ = 0, seen_gy_ = 0;
	bool last_active_ = false;
	std::string last_title_;
	Region last_hover_ = Region::None;
	Region last_pressed_ = Region::None;

	void hide_real(bool hide);
	void take_ghost();
	void drop_ghost();
	void run_ghost(const GhostRect &from, const GhostRect &to, double ms,
		std::function<void()> done);
	void start_open_ghost();
	void try_finish_morph();
	static int handle_morph_timer(void *data);
	GhostRect frame_rect(double alpha) const;

	std::shared_ptr<Ghost> ghost_;
	std::shared_ptr<Ghost> fading_;   // the copy fading out after a resize
	uint64_t fade_anim_ = 0;
	void drop_fading();
	uint64_t ghost_anim_ = 0;
	bool hidden_ = false;
	bool morphing_ = false, morph_ready_ = false;
	wl_event_source *morph_timer_ = nullptr;
	wl_event_source *open_idle_ = nullptr;

	// A move waiting for the app to resize (see configure_frame).
	bool move_pending_ = false;
	int pending_x_ = 0, pending_y_ = 0;
	uint32_t pending_serial_ = 0;
	double pending_since_ = 0;
};

// A Wayland app window (xdg-shell toplevel).
class XdgView : public View {
public:
	wlr_xdg_toplevel *toplevel;
	wlr_xdg_toplevel_decoration_v1 *decoration = nullptr;
	wlr_xdg_toplevel_decoration_v1_mode decoration_mode =
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;

	XdgView(Server *server, wlr_xdg_toplevel *toplevel);

	wlr_surface *surface() const override { return toplevel->base->surface; }
	std::string title() const override;
	std::string app_id() const override;
	int content_width() const override;
	int content_height() const override;
	int geometry_x() const override { return toplevel->base->geometry.x; }
	int geometry_y() const override { return toplevel->base->geometry.y; }
	int min_width() const override { return toplevel->current.min_width; }
	int min_height() const override { return toplevel->current.min_height; }
	bool ready() const override { return toplevel->base->initialized; }
	uint32_t request_content_size(int w, int h) override;
	uint32_t acked_serial() const override { return toplevel->base->current.configure_serial; }
	bool acks_sizes() const override { return true; }
	void send_activated(bool on) override;
	void send_maximized(bool on) override;
	void send_tiled(bool on) override;
	void send_fullscreen(bool on) override;
	void send_resizing(bool on) override;
	void close() override;

	void attach_decoration(wlr_xdg_toplevel_decoration_v1 *deco);

private:
	void handle_map(void *data);
	void handle_unmap(void *data);
	void handle_commit(void *data);
	void handle_destroy(void *data);
	void handle_request_move(void *data);
	void handle_request_resize(void *data);
	void handle_request_maximize(void *data);
	void handle_request_minimize(void *data);
	void handle_request_fullscreen(void *data);
	void handle_set_title(void *data);
	void handle_new_popup(void *data);
	void handle_decoration_request_mode(void *data);
	void handle_decoration_destroy(void *data);
	void apply_decoration_mode();

	Listener on_map_, on_unmap_, on_commit_, on_destroy_;
	Listener on_request_move_, on_request_resize_, on_request_maximize_;
	Listener on_request_minimize_, on_request_fullscreen_, on_set_title_;
	Listener on_new_popup_;
	Listener on_deco_request_mode_, on_deco_destroy_;
};

// Menus and dropdowns of a Wayland app.
struct Popup {
	View *view;
	wlr_xdg_popup *popup;
	Listener on_commit, on_new_popup, on_reposition, on_destroy;

	Popup(View *view, wlr_xdg_popup *popup);
	void unconstrain();
	void handle_commit(void *data);
	void handle_new_popup(void *data);
	void handle_reposition(void *data);
	void handle_destroy(void *data);
};

#ifdef DUSK_XWAYLAND
// An X11 app window, running through XWayland.
class XwaylandView : public View {
public:
	wlr_xwayland_surface *xsurface;

	XwaylandView(Server *server, wlr_xwayland_surface *xsurface);

	wlr_surface *surface() const override { return xsurface->surface; }
	std::string title() const override;
	std::string app_id() const override;
	int content_width() const override { return xsurface->width; }
	int content_height() const override { return xsurface->height; }
	bool ready() const override { return xsurface->surface != nullptr; }
	uint32_t request_content_size(int w, int h) override;
	void send_activated(bool on) override;
	void send_maximized(bool on) override;
	void send_fullscreen(bool on) override;
	void send_position() override;
	void close() override;

private:
	void configure(int cx, int cy, int w, int h);

	void handle_associate(void *data);
	void handle_dissociate(void *data);
	void handle_map(void *data);
	void handle_unmap(void *data);
	void handle_commit(void *data);
	void handle_destroy(void *data);
	void handle_request_configure(void *data);
	// Games (Wine/Proton's "borderless" mode, older X11 games) often just
	// make a frameless window exactly the screen's size instead of asking
	// for fullscreen: that's taken as fullscreen, so the panels go under.
	Output *fills_an_output(int x, int y, int w, int h) const;
	bool auto_fullscreen_ = false;
	void handle_request_move(void *data);
	void handle_request_resize(void *data);
	void handle_request_minimize(void *data);
	void handle_request_maximize(void *data);
	void handle_request_fullscreen(void *data);
	void handle_request_activate(void *data);
	void handle_set_title(void *data);
	void handle_set_decorations(void *data);

	Listener on_associate_, on_dissociate_, on_destroy_;
	Listener on_map_, on_unmap_, on_commit_;
	Listener on_request_configure_, on_request_move_, on_request_resize_;
	Listener on_request_minimize_, on_request_maximize_, on_request_fullscreen_;
	Listener on_request_activate_, on_set_title_, on_set_class_, on_set_decorations_;
};

// X11 menus, tooltips and dropdowns ("override-redirect" windows): drawn
// where the app says, with no frame and no taskbar entry.
class Unmanaged {
public:
	Server *server;
	wlr_xwayland_surface *xsurface;
	wlr_scene_tree *tree = nullptr;
	NodeTag tag{NodeTag::Kind::Unmanaged, this};

	Unmanaged(Server *server, wlr_xwayland_surface *xsurface);

private:
	void handle_associate(void *data);
	void handle_dissociate(void *data);
	void handle_map(void *data);
	void handle_unmap(void *data);
	void handle_set_geometry(void *data);
	void handle_request_configure(void *data);
	void handle_destroy(void *data);

	Listener on_associate_, on_dissociate_, on_destroy_;
	Listener on_map_, on_unmap_, on_set_geometry_, on_request_configure_;
};
#endif

// ---------------------------------------------------------------------------
// The compositor
// ---------------------------------------------------------------------------

enum class CursorMode { Passthrough, Move, Resize };

struct Hit {
	Region region = Region::None;
	View *view = nullptr;
	Panel *panel = nullptr;
	bool launcher = false;
	bool control_centre = false;
	Toast *toast = nullptr;
	Popover *popover = nullptr;
	bool desktop = false;   // an icon on the desktop
	bool overview = false;  // the overview (Super)
	uint32_t edges = 0;
	wlr_surface *surface = nullptr;
	double sx = 0, sy = 0;
};

class Server {
public:
	wl_display *display = nullptr;
	wl_event_loop *loop = nullptr;
	wlr_backend *backend = nullptr;
	wlr_session *session = nullptr;
	wlr_renderer *renderer = nullptr;
	wlr_allocator *allocator = nullptr;
	wlr_compositor *compositor = nullptr;

	wlr_scene *scene = nullptr;
	wlr_output_layout *layout = nullptr;
	wlr_backend *headless = nullptr;   // for virtual screens (phone.cpp)
	wlr_virtual_keyboard_manager_v1 *virtual_keyboards = nullptr;
	wlr_tearing_control_manager_v1 *tearing_control = nullptr;
	bool game_mode = false;   // something is full screen and focused (background work pauses)
	wlr_virtual_pointer_manager_v1 *virtual_pointers = nullptr;
	Listener on_virtual_keyboard, on_virtual_pointer;
	// ---- phone.cpp: a phone or tablet as a second screen (VNC) ----
	wlr_output *phone_output = nullptr;
	std::string phone_applied;         // the settings it was made with
	pid_t phone_vnc = -1;              // wayvnc
	pid_t phone_adb = -1;              // adb reverse (USB)
	void sync_phone_screen();          // after the settings changed
	void stop_phone_screen();
	wlr_scene_output_layout *scene_layout = nullptr;
	// Stacking layers, bottom to top.
	wlr_scene_tree *layer_background = nullptr;
	wlr_scene_tree *layer_panel_shadow = nullptr;   // panels' shadows: under the windows
	wlr_scene_tree *layer_views = nullptr;
	wlr_scene_tree *layer_overview = nullptr;  // the overview: over windows, under the panels
	wlr_scene_tree *layer_panel = nullptr;
	wlr_scene_tree *layer_fullscreen = nullptr;
	wlr_scene_tree *layer_unmanaged = nullptr;
	wlr_scene_tree *layer_menu = nullptr;      // app menu, control centre
	wlr_scene_tree *layer_notify = nullptr;    // notification popups
	wlr_scene_tree *layer_overlay = nullptr;   // OSD, rounded screen corners
	wlr_scene_tree *layer_lock = nullptr;      // the lock screen, over everything

	wlr_xdg_shell *xdg_shell = nullptr;
	wlr_xdg_decoration_manager_v1 *decoration_manager = nullptr;
#ifdef DUSK_XWAYLAND
	wlr_xwayland *xwayland = nullptr;
#endif

	wlr_cursor *cursor = nullptr;
	wlr_xcursor_manager *cursor_manager = nullptr;
	std::string cursor_theme;       // what is loaded now
	int cursor_size = 0;
	wlr_seat *seat = nullptr;

	std::vector<Output *> outputs;
	std::list<View *> views;        // front = top of the stack
	std::vector<Keyboard *> keyboards;
	std::vector<Pointer *> pointers;
	std::unique_ptr<Launcher> launcher;
	std::unique_ptr<Osd> osd;
	std::unique_ptr<ControlCentre> control_centre;
	std::unique_ptr<Notifications> notifications;
	std::unique_ptr<SystemStatus> status;
	std::unique_ptr<ImageLoader> images;   // pictures read in the background (album art)
	std::unique_ptr<Calendar> calendar;
	std::unique_ptr<MediaCard> media_card;
	std::unique_ptr<ContextMenu> menu;     // right-click menus
	std::unique_ptr<SnapLayouts> snap_layouts;
	std::unique_ptr<LockScreen> lock;
	bool locked() const;
	std::unique_ptr<Clipboard> clipboard;            // what was copied (history)
	std::unique_ptr<ClipboardCard> clipboard_card;   // its list (panel, Super+V)
	std::unique_ptr<Osk> osk;                         // the on-screen keyboard (panel, Super+K)
	std::unique_ptr<ScreenshotTool> shot_tool;       // Print: area, window or screen
	std::unique_ptr<Tray> tray;                      // apps' status icons (StatusNotifierItem)
	size_t tray_count = 0;
	std::unique_ptr<Desktop> desktop;                // files and folders on the wallpaper
	std::unique_ptr<Overview> overview;              // Super: windows, workspaces and search
	std::unique_ptr<Trash> trash;                    // the trash (count, open, empty)
	std::unique_ptr<AudioLevel> audio_level;         // the music's loudness and beat (the media wave)
	std::unique_ptr<Lyrics> lyrics;                  // synced lyrics for what's playing
	std::unique_ptr<Recorder> recorder;              // screen recording
	wlr_scene_tree *layer_desktop = nullptr;         // desktop icons: above the wallpaper
	bool desktop_grab = false;                       // dragging a desktop icon
	// Every popover, for the pointer and the keyboard.
	std::vector<Popover *> popovers() const;

	// ---- workspaces (workspaces.cpp) ----
	int workspace = 0;               // the one shown
	int workspace_count = 2;         // dynamic: the used ones and an empty one at the end
	uint64_t workspace_anim = 0;
	std::vector<View *> workspace_leaving;   // sliding out during a switch
	void switch_workspace(int index, bool animate = true);
	void move_to_workspace(View *view, int index, bool follow);
	void add_workspace();            // one more at the end, and go there
	void remove_workspace(int index);   // its windows go to the one before
	void normalize_workspaces();     // dynamic: drop empty ones, keep one spare at the end
	bool workspace_used(int index) const;
	void finish_workspace_switch();
	// Hot corner (Settings > Workspaces): pushing the pointer into it opens the overview.
	bool hot_corner_armed = true;
	void check_hot_corner(double x, double y);

	// ---- protocols for games, browsers and portals ----
	wlr_xdg_activation_v1 *activation = nullptr;
	wlr_pointer_constraints_v1 *pointer_constraints = nullptr;
	wlr_relative_pointer_manager_v1 *relative_pointer = nullptr;
	wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit = nullptr;
	wlr_pointer_constraint_v1 *active_constraint = nullptr;
	Listener on_request_activate, on_new_constraint, on_constraint_destroy, on_new_inhibitor;
	void handle_request_activate(void *data);
	void handle_new_constraint(void *data);
	void handle_constraint_destroy(void *data);
	void handle_new_inhibitor(void *data);
	void update_constraint();        // the keyboard went to another surface
	bool shortcuts_inhibited() const;
	void inhibitor_release(struct ShortcutInhibitor *i);   // the keys come back to Dusk
	void unstick();   // Super+Escape   // the focused app wants all keys (games, VMs, remote desktops)
	std::vector<struct ShortcutInhibitor *> inhibitors;
	View *view_for_surface(wlr_surface *surface) const;

	// ---- touchpad gestures (gestures.cpp) ----
	wlr_pointer_gestures_v1 *pointer_gestures = nullptr;
	Listener on_swipe_begin, on_swipe_update, on_swipe_end, on_pinch_begin, on_pinch_update, on_pinch_end;
	Listener on_hold_begin, on_hold_end;
	uint32_t swipe_fingers = 0;
	double swipe_dx = 0, swipe_dy = 0;
	bool swipe_ours = false, swipe_fired = false;
	void setup_gestures();
	void handle_swipe_begin(void *data);
	void handle_swipe_update(void *data);
	void handle_swipe_end(void *data);
	void handle_pinch_begin(void *data);
	void handle_pinch_update(void *data);
	void handle_pinch_end(void *data);
	void handle_hold_begin(void *data);
	void handle_hold_end(void *data);
	void run_action(const std::string &action);   // a gesture's (or a shortcut's) action

	// ---- tiling while dragging (where the window lands if let go) ----
	enum class Drop { None, Maximize, Left, Right, Zone };
	Drop drop_kind = Drop::None;
	Output *drop_output = nullptr;
	View::Zone drop_zone;
	wlr_scene_rect *snap_preview = nullptr;
	void update_drop_target(View *view, double cx, double cy);
	void apply_drop(View *view);
	void show_snap_preview(const wlr_box &box, View *below);
	void hide_snap_preview();
	// Hovering the maximise button opens the snap layouts after a moment.
	wl_event_source *max_hover_timer = nullptr;
	View *max_hover_view = nullptr;
	static int handle_max_hover(void *data);
	void open_snap_menu(View *view);
	std::unique_ptr<Animator> anim;
	AppDatabase apps;
	IconLoader icons;
	std::string color_scheme;   // what we last told GTK apps
	std::string gnome_accent_name;
	// What the settings file said last time, and which groups of settings
	// were applied (so changing one setting doesn't redo everything).
	std::string last_config_text;
	std::string applied_look, applied_wallpaper, applied_keyboard;
	// accent_auto: the accent picked from this wallpaper (path|mtime).
	void resolve_accent();
	void write_pending_settings();   // one-time updates the settings file asked for
	std::string auto_accent_for, written_auto_accent;
	wl_event_source *accent_timer = nullptr;   // applies an accent picked from a new wallpaper
	wl_event_source *theme_timer = nullptr;    // restyles other apps after a wallpaper transition
	std::string pending_theme_cmd;
	Color auto_accent;
	std::string applied_buttons;   // the title bar button layout last given to GNOME/KDE apps
	std::string applied_paste;     // middle-click paste, as last told to GTK
	void sync_app_buttons();

	Config config;
	std::unique_ptr<WallpaperWorker> wall_worker;   // decodes and scales pictures off-thread

	View *focused = nullptr;
	View *hover_view = nullptr;     // titlebar button under the pointer
	View *pressed_view = nullptr;   // titlebar button being clicked

	// Interactive move/resize state.
	CursorMode cursor_mode = CursorMode::Passthrough;
	std::function<Panel *()> panel_drag;   // a pinned app pressed on a panel (maybe being dragged)
	View *grab_view = nullptr;
	double grab_x = 0, grab_y = 0;
	double press_x = 0, press_y = 0;
	wlr_box grab_box{};
	uint32_t resize_edges = 0;
	bool cc_grab = false;           // dragging a control centre slider
	bool osd_grab = false;          // setting the level on the OSD's bar
	Popover *popover_grab = nullptr; // dragging inside a popover (media position)

	// While a button is held on a window, keep sending it pointer events
	// even if the pointer leaves it (drag-selecting text etc).
	int buttons_down = 0;
	wlr_surface *implicit_grab_surface = nullptr;
	double implicit_origin_x = 0, implicit_origin_y = 0;

	// Last titlebar click, for double-click to maximise.
	View *last_title_click_view = nullptr;
	uint32_t last_title_click_time = 0;

	// Alt+Tab
	std::vector<View *> switcher;
	size_t switcher_index = 0;

	// Super key alone opens the app menu.
	bool super_tapped = false;

	// "Show desktop": the windows it minimised, to bring back.
	std::vector<View *> shown_desktop;

	std::string cursor_image;
	wl_event_source *clock_timer = nullptr;

	// ---- idle (idle.cpp) ----
	wlr_idle_notifier_v1 *idle_notifier = nullptr;
	wlr_idle_inhibit_manager_v1 *idle_inhibit = nullptr;
	wl_event_source *idle_timer = nullptr;
	double last_activity = 0;      // now_ms() of the last input
	bool screens_off = false;
	bool slept = false;            // suspended since the last input
	bool idle_locked = false;      // locked by the idle timer since the last input
	int battery_warned = 101;      // lowest level already warned about
	uint32_t battery_note = 0;
	// Apps asking to stay awake over D-Bus (org.freedesktop.ScreenSaver,
	// ...PowerManagement.Inhibit: SDL games, Steam, browsers, video players),
	// logind "idle" inhibitors, and gamepads in use (idle.cpp).
	std::unique_ptr<struct AwakeKeepers, void (*)(struct AwakeKeepers *)> awake{nullptr, nullptr};
	bool logind_idle_blocked = false;
	double logind_checked = 0;
	void start_idle();
	void stop_idle();
	void note_activity();          // any key, click or pointer move
	void idle_tick();
	void screens_off_now();
	void wake_screens();
	static int handle_idle_timer(void *data);

	// ---- screenshot.cpp ----
	// Runs a shell command; `done` is called when it has finished.
	void run_then(const std::string &command, std::function<void(bool ok)> done);
	void screenshot(const std::string &what);   // "tool", "screen", "all" or "window"
	// Captures `box` (layout coordinates; width 0 = every screen) after
	// `delay` seconds, then saves/copies it as Settings > Screenshots say.
	void capture(wlr_box box, bool pointer, int delay, std::vector<InkStroke> ink = {});
	wl_event_source *config_watch = nullptr;
	int inotify_fd = -1;
	bool shutting_down = false;
	std::string socket;

	Listener on_new_output, on_new_input, on_layout_change;
	Listener on_new_toplevel, on_new_decoration;
	Listener on_cursor_motion, on_cursor_motion_absolute, on_cursor_button;
	Listener on_cursor_axis, on_cursor_frame;
	Listener on_request_cursor, on_request_selection, on_request_primary_selection;
	// Drag and drop between apps (files, text, pictures).
	Listener on_request_start_drag, on_start_drag, on_drag_icon_destroy;
	wlr_scene_tree *drag_icon = nullptr;
	// Desktop icons dragged into an app: our own drag (source, picture).
	bool start_file_drag(const std::vector<std::string> &paths, wlr_buffer *picture, int grab_x,
		int grab_y, wlr_surface *over);
	wlr_data_source *own_drag_source = nullptr;
	wlr_scene_buffer *own_drag_picture = nullptr;
	int own_drag_dx = 0, own_drag_dy = 0;
	Listener on_own_drag_destroy, on_own_drag_origin_gone;
	void handle_own_drag_destroy(void *data);
	void handle_own_drag_origin_gone(void *data);
	// Files dropped onto the desktop: the drag ends when they've been read.
	bool drop_on_desktop(uint32_t time, uint32_t button);
	bool drop_waiting = false;
	std::function<void()> drop_finish;
	wl_event_source *drop_timer = nullptr;
	static int handle_drop_timer(void *data);
	Listener on_request_cursor_shape;
#ifdef DUSK_XWAYLAND
	Listener on_xwayland_ready, on_xwayland_new_surface;
#endif

	bool start();
	void run();
	void shutdown();

	// Settings
	void watch_config();
	void reload_config();
	void apply_config();
	void apply_theme();
	// Changes one setting from inside Dusk (control centre toggles). The
	// file watcher then applies it like any other change.
	void write_setting(const std::string &key, const std::string &value);

	// Windows
	void focus(View *view);
	void focus_surface(wlr_surface *surface);   // keyboard only, no window change
	void focus_next_after(View *gone);
	void raise(View *view);
	void views_changed();   // taskbar needs a redraw
	void redraw_panels();
	wl_event_source *panels_idle_ = nullptr;
	void begin_move(View *view);
	void begin_resize(View *view, uint32_t edges);
	void place_new_view(View *view);
	void toggle_show_desktop();
	void close_popups();    // app menu and control centre

	// Outputs
	Output *output_at(double lx, double ly);
	Output *output_for_view(const View *view);
	Output *focused_output();
	float max_scale() const;
	void arrange();
	void show_desktop_menu(double x, double y);
	// The power menu (power_actions in the settings), next to `anchor`.
	void show_power_menu(Output *output, wlr_box anchor, const std::string &edge);
	void power_action(const std::string &action);
	std::string lock_command() const;
	// Writes what every screen can do and how it's set up, for Settings
	// ($XDG_RUNTIME_DIR/dusk/outputs).
	void write_outputs_info();
	// Brings windows left on a screen that was turned off or unplugged
	// back onto one that is on.
	void rescue_views();
	// A screen is going away: remember where its windows were, so
	// bring_back() can put them back when it returns.
	void evacuate(Output *output);
	void move_to_next_screen(View *view, int direction);
	void move_to_screen(View *view, Output *to, bool focus_it = true);
	void bring_back(Output *output);
	// After a screen went away: grabs end, windows and the pointer come
	// back onto a screen that's still there.
	void screen_gone(bool pointer_was_there);
	// Holds the pointer at an edge shared with another screen until it's
	// pushed a bit further (so windows can be dropped there to snap).
	void edge_resist(double &dx, double &dy);
	double push_x = 0, push_y = 0, push_at = 0;
	double push_since = 0;   // when the pointer started pressing on the edge
	void schedule_frames();

	// Pointer
	Hit hit_test(double lx, double ly);
	void process_motion(uint32_t time);
	void set_cursor(const char *name);
	void load_cursor_theme();
	std::string effective_icon_theme() const;

	// Keyboard
	bool handle_keybinding(xkb_keysym_t sym, uint32_t modifiers);
	void end_switcher();

	void spawn(const std::string &command);
	// Runs a shell command without blocking and hands its output to `done`.
	void run_capture(const std::string &command, std::function<void(std::string)> done);
	void sync_audio_sleep();
	void volume_key(const char *action, bool mic);
	void brightness_key(bool up);
	void spawn_terminal();
	void open_settings(const std::string &page = "");
	std::string helper_path(const std::string &name) const;

	void handle_new_output(void *data);
	void handle_new_input(void *data);
	void handle_virtual_keyboard(void *data);
	void handle_virtual_pointer(void *data);
	void handle_layout_change(void *data);
	void handle_new_toplevel(void *data);
	void handle_new_decoration(void *data);
	void handle_cursor_motion(void *data);
	void handle_cursor_motion_absolute(void *data);
	void handle_cursor_button(void *data);
	void handle_cursor_axis(void *data);
	void handle_cursor_frame(void *data);
	void handle_request_cursor(void *data);
	void handle_request_cursor_shape(void *data);
	void handle_request_selection(void *data);
	void handle_request_primary_selection(void *data);
	void handle_request_start_drag(void *data);
	void handle_start_drag(void *data);
	void handle_drag_icon_destroy(void *data);
#ifdef DUSK_XWAYLAND
	void start_xwayland();
	void update_xwayland_cursor();
	void handle_xwayland_ready(void *data);
	void handle_xwayland_new_surface(void *data);
#endif
};

// Quotes text for /bin/sh.
std::string shell_quote(const std::string &text);


// How far a full-screen blur may reach past each edge of `o`: off the edges
// with no other screen there (so the blur doesn't darken them), not onto a
// neighbouring screen.
inline void blur_margins(const std::vector<Output *> &outputs, const Output *self, int m, int *l, int *t, int *r, int *b) {
	const wlr_box o = self->box();
	*l = *t = *r = *b = m;
	for (const Output *x : outputs) {
		if (x == self || !x->enabled()) continue;
		const wlr_box n = x->box();
		const bool rows = n.y < o.y + o.height && o.y < n.y + n.height;
		const bool cols = n.x < o.x + o.width && o.x < n.x + n.width;
		if (rows && n.x + n.width <= o.x && n.x + n.width >= o.x - m) *l = 0;
		if (rows && n.x >= o.x + o.width && n.x <= o.x + o.width + m) *r = 0;
		if (cols && n.y + n.height <= o.y && n.y + n.height >= o.y - m) *t = 0;
		if (cols && n.y >= o.y + o.height && n.y <= o.y + o.height + m) *b = 0;
	}
}

} // namespace dusk
