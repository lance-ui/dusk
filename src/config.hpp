// User settings, read from ~/.config/dusk/dusk.conf.
//
// The file is plain `key = value` lines (written by Dusk Settings, but
// easy to edit by hand). Dusk re-reads it whenever it changes.
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "draw.hpp"

namespace dusk {

// One panel. There can be any number, on any screen edge.
struct PanelConfig {
	std::string id = "main";
	std::string edge = "bottom";          // bottom, top, left, right
	std::string style = "full";           // full, floating, dock (only as long as its contents)
	int size = 44;                        // thickness, px
	std::string output = "all";           // all, primary, or a connector name like eDP-1
	bool autohide = false;                // slide away until the pointer touches the edge
	bool labels = false;                  // window titles next to task icons (horizontal only)
	double opacity = 0.75;                // background opacity (0 = only the blur)
	// Left to right (or top to bottom): menu, tasks, pins, spacer, separator,
	// clock, status, media (now-playing chip), clipboard, tray (apps' status icons), showdesktop,
	// overview (workspaces button: opens the overview), trash
	// Open apps at the start, the workspaces in the middle, the rest at the end.
	std::vector<std::string> widgets = {"menu", "tasks", "spacer", "overview", "spacer", "media", "clipboard", "tray", "status", "notifications", "clock"};
	std::vector<std::string> pins;        // desktop file ids for the "pins" widget

	bool vertical() const { return edge == "left" || edge == "right"; }
};

// One screen's settings ("output.<name>.<key>" in the file; the name is
// the connector, e.g. eDP-1 or HDMI-A-1).
struct OutputSettings {
	bool enabled = true;
	int width = 0, height = 0;   // 0 = the screen's preferred mode
	double refresh = 0;          // Hz; 0 = highest for that size
	bool has_position = false;   // false = placed automatically
	int x = 0, y = 0;            // logical pixels
	int rotation = 0;            // 0, 90, 180, 270
	bool vrr = false;            // variable refresh rate (adaptive sync)
};

// A keyboard shortcut of your own: keys -> a command.
struct Shortcut {
	uint32_t mods = 0;      // WLR_MODIFIER_* bits
	uint32_t sym = 0;       // xkb keysym, lower case
	std::string keys;       // as written, e.g. "Super+E"
	std::string command;
};

// A control centre tile's place on its 6-column grid (Arqen-style):
// column, row, and how many cells wide and tall.
struct CcTile {
	std::string id;
	int c = 0, r = 0, w = 1, h = 1;
};

// One control centre tile.
struct TileConfig {
	std::string kind;   // wifi, bluetooth, dark, nightlight, dnd, airplane, caffeine, power, mic
	bool wide = false;  // two columns wide, with a subtitle
};

struct Config {
	// ---- Appearance ----
	bool dark = true;                     // light or dark
	bool oled = false;                    // dark_style = gnome (GNOME's dark greys) | oled (pure black)
	bool high_contrast = false;           // stronger outlines and text (an accessibility setting)
	bool own_icons = true;                // Dusk's own drawn icons (ui_icons = dusk | theme)
	Color accent = rgb(0x4d8df6);
	bool accent_auto = false;             // follow the wallpaper (picked again when it changes)
	std::string accent_for;               // the wallpaper Settings already picked `accent` from
	std::string wallpaper_type = "dusk";  // dusk, halftone, solid, image
	std::string wallpaper;                // image path (wallpaper_type = image)
	std::string wallpaper_mode = "fill";  // fill, fit, center, stretch
	Color wallpaper_color = rgb(0x2b2d3a); // wallpaper_type = solid
	std::string wallpaper_transition = "random"; // random, none, fade, slide, push, zoom, wipe, grow
	int wallpaper_transition_ms = 900;
	// The look of Dusk's own panels, menus and cards (presets in Settings:
	// Ink = line art, Soft, Flat).
	std::string ui_outline = "thin";       // ink, thin, none
	std::string ui_shadow = "soft";       // ink (short, drawn), soft, none
	std::string ui_texture = "none";      // none, halftone (screentone dots)
	int ui_radius = 22;                   // corner radius of cards and popups
	double ui_opacity = 0.92;             // menus, control centre, notifications, OSD (0..1)
	double ui_blur = 1.0;                 // blur behind them (0 = off .. 1 = full)
	double panel_blur = 1.0;              // blur behind the panels (their opacity is each panel's own)
	double settings_opacity = 1.0;        // Dusk's Settings window (1 = solid)
	double settings_blur = 1.0;           // blur behind it when it's see-through
	std::string font = "Google Sans"; // the interface's font (Dusk, and apps)
	std::string mono_font = "Google Sans Code"; // terminals and code
	std::string osk_layout = "normal";    // on-screen keyboard: normal (compact), split, wide
	bool allow_tearing = true;            // full-screen games may skip vsync when they ask (less lag)
	int font_size = 11;                   // apps' text size (pt)
	int mono_font_size = 11;
	// Empty = same as GNOME/GTK uses. The defaults are used when installed
	// (./build.sh installs them); otherwise GNOME's are.
	std::string cursor_theme = "Bibata-Modern-Classic";
	std::string icon_theme = "Papirus";   // Papirus-Dark in dark mode
	int cursor_size = 0;                  // 0 = same as GNOME/GTK uses

	// ---- Windows ----
	std::string border = "thin";          // none, thin, outline
	std::string shadow = "soft";          // none, soft, ink, hard
	bool accent_border = false;           // outline the active window in the accent colour
	int window_radius = 12;               // rounded window corners, px
	int screen_corners = 14;              // rounded screen corners, px (0 = off)
	int titlebar_height = 36;             // compact (GNOME's header bars are 46-47 px)
	bool show_titles = false;             // the window's name in Dusk's title bars
	std::string title_align = "center";   // center, left
	std::string buttons_side = "right";   // right, left
	std::string button_style = "gtk";     // gtk (GNOME: round grey buttons), sketch (corners, red X), plain
	bool show_minimize = true;
	bool show_maximize = false;
	bool apps_follow_buttons = true;      // GNOME/GTK and KDE apps get the same buttons and side
	std::string double_click = "maximize";// maximize, minimize, none
	std::string middle_click = "minimize";// minimize, none
	std::string placement = "center";     // center, cascade, cursor
	bool edge_snap = true;
	int tile_gap = 0;                     // px between tiled windows (and the screen edge)
	// Snap layouts offered at the top of the screen while dragging and on
	// the maximise button: column widths (%) / rows in each column.
	std::vector<std::vector<std::array<double, 4>>> snap_layouts;
	std::string snap_layouts_text = "50-50/1-1;66-34/1-1;33-34-33/1-1-1;50-50/1-2;50-50/2-2;25-50-25/1-1-1";
	bool snap_bar = true;                 // the layouts bar when dragging to the top
	bool focus_follows_mouse = false;

	// ---- Workspaces and the overview (GNOME-like) ----
	bool workspaces_dynamic = false;      // GNOME's way: as many as you use, plus an empty one
	int workspace_count = 1;              // when not dynamic (New workspace adds one)
	bool workspace_wrap = false;          // fixed number: past the last comes the first
	bool tasks_all_workspaces = true;     // "Open apps": windows on every workspace (clicking one goes there)
	std::string super_key = "menu";       // tapping Super opens: menu (the app menu) | overview
	std::string hot_corner = "top-left";  // none, top-left, top-right, bottom-left, bottom-right
	bool dock_magnify = false;            // macOS-look dock: icons grow under the pointer
	bool overview_minimized = true;
	std::string overview_strip = "top";   // the workspace pictures in the overview: top, bottom, left, right, off       // minimised windows in the overview too (faded)
	bool overview_apps = false;           // searching in the overview finds apps too (else only open windows)
	// The overview button on a panel: dots (GNOME: one per workspace), grid,
	// squares, circle, text ("Activities"), distro, custom (a picture or icon name).
	std::string overview_icon = "count";
	std::string overview_icon_custom;

	// ---- Panels ----
	std::vector<PanelConfig> panels = {PanelConfig{}};
	bool clock_24h = true;
	bool show_date = true;
	std::string battery_style = "percent"; // icon (a battery), percent (the icon and a colour-coded %), ring
	std::string clock_click = "calendar"; // calendar, controlcentre
	std::string menu_icon = "dusk";       // the app menu button: dots, squares, dusk, search, distro, custom
	std::string menu_icon_custom;         // menu_icon = custom: an image file or an icon name
	// The app menu's power button and what its menu offers.
	bool menu_power = true;
	std::vector<std::string> power_actions = {"lock", "sleep", "logout", "restart", "shutdown"};
	bool power_confirm = true;            // ask before log out, restart and shut down
	std::string lock_command;             // empty = Dusk's own lock screen
	// The lock screen (and the login screen, which looks the same).
	std::string lock_background = "blur"; // wallpaper, blur, picture, picture_blur, color
	std::string lock_picture;             // lock_background = picture
	Color lock_color = rgb(0x1c1b29);     // lock_background = color
	int lock_blur = 60;                   // 0..100
	int lock_dim = 25;                    // % darker
	std::string lock_clock = "center";    // center, left, corner (bottom left), hidden
	std::string lock_clock_style = "digital"; // digital, stacked (hours over minutes), outline, analog
	std::string lock_clock_font;          // empty = the interface font
	bool lock_media = true;               // what's playing
	bool lock_status = true;              // Wi-Fi, sound and battery, top right
	bool lock_notifications = true;       // which apps have notifications waiting (not what they say)
	bool lock_card = false;               // the password form on a frosted card
	std::string lock_clock_weight = "light"; // light, bold
	bool lock_date = true;
	bool lock_avatar = true;              // your picture
	std::string lock_message;             // a line under the clock
	std::string lock_clock_size = "medium"; // small, medium, large
	bool lock_clock_accent = false;       // the clock in the accent colour (else white)
	std::string lock_form = "middle";     // where the password goes: middle, lower, right
	std::string lock_avatar_shape = "circle"; // circle, rounded
	bool lock_name = true;                // your name under your picture
	bool lock_curtain = false;            // just the clock until a key is pressed
	bool lock_screen_off = true;          // lock when the screen turns off by itself
	bool lock_sleep = true;               // lock before the computer sleeps
	bool week_monday = false;             // calendar weeks start on Monday (default: Sunday)

	// ---- Control centre ----
	std::vector<TileConfig> tiles = {
		{"wifi", true}, {"bluetooth", true}, {"dark", false}, {"nightlight", false},
		{"dnd", false}, {"airplane", false}, {"caffeine", false}, {"power", false},
	};
	std::vector<CcTile> cc_layout;        // empty = the default layout
	std::string cc_side = "auto";         // auto (follow the button), left, right
	bool cc_sliders = true;
	bool cc_media = true;
	bool cc_notifications = true;
	std::string cc_style = "dusk";
	// Notifications: in the control centre ("together", Dusk's way), in a
	// centre of their own from the panel's bell ("separate", as Windows,
	// GNOME and macOS do), or "auto": separate for those looks.
	std::string notif_centre = "auto";
	bool separate_notes() const {
		return notif_centre == "separate" || (notif_centre == "auto" && cc_style != "dusk");
	}
	std::string panel_look = "dusk";      // how panels are drawn: dusk, windows, gnome, macos
	std::vector<std::string> launcher_favorites;   // app ids pinned to the top of the app menu
	std::string launcher_look = "dusk";   // the app menu: dusk (a list), windows (Start), gnome (app grid), macos (Launchpad)
	// The other looks' own arrangements (same format as cc_layout; empty = their default).
	std::vector<CcTile> cc_layout_windows, cc_layout_gnome, cc_layout_macos;        // dusk (Samsung-like tiles you arrange), windows, gnome, macos

	// ---- Sound ----
	bool audio_keep_awake = true;         // don't let the sound card sleep when quiet (stops clicks in headphones)

	// ---- Media and lyrics ----
	bool media_wave_beat = true;          // the visualizer listens to the music (off: a made-up beat)
	std::string media_viz = "bars";       // behind the media card: bars, mirror, dots, wave, hills, off (like Arqen's)
	std::string media_viz_color = "art";  // art (from the cover), accent, text, rainbow
	bool media_marquee = true;            // long titles scroll sideways
	bool lyrics = true;                   // look up synced lyrics for what's playing
	bool lyrics_show = false;             // show them on the screen while it plays
	std::string lyrics_style = "comic";   // classic, karaoke, subtitle, glow, scroll, bounce, typewriter, comic
	std::string lyrics_position = "bottom"; // one of 9: top-left ... bottom-right
	std::string lyrics_color = "accent";  // accent, art (the album cover's colour), white
	bool lyrics_on_top = false;           // over the windows (off: on the desktop, under them)
	int lyrics_delay = 0;                 // ms: lyrics too early? raise it
	int lyrics_speed = 100;               // percent: lyrics drift ahead/behind (sped-up versions)
	int lyrics_size = 100;                // percent
	bool lyrics_recognize = true;         // listen to find songs with odd names (needs SongRec)
	bool lyrics_translate = true;         // translate them (Google Translate's free web service)
	std::string lyrics_translate_to;      // language code, e.g. "fr", "zh-CN" (empty: your system's language)
	std::string lyrics_translate_show = "under";   // under (the line, then the translation) or instead
	bool night_light = false;
	int night_light_temp = 4300;          // kelvin
	bool dnd = false;                     // do not disturb

	// ---- Notifications ----
	bool notifications = true;            // Dusk shows app notifications itself
	std::string notification_position = "bottom-right"; // one of 9: top-left ... bottom-right
	int notification_timeout = 5000;      // ms
	bool toast_over_fullscreen = false;   // pop-ups while a game or video is full screen
	bool notif_group = false;             // the control centre stacks notifications by app (off: one card each)
	int notification_history_size = 100;  // notifications kept in the history (0 = none)
	bool notification_history_keep = true; // ... also after a restart (~/.local/state/dusk)

	// ---- Animations ----
	bool animations = true;               // master switch
	bool reduce_motion = false;           // accessibility: everything appears at once
	double anim_speed = 1.0;              // 0.5 (slow) .. 2 (fast)
	bool anim_windows = true;             // open and close
	bool anim_minimize = true;
	bool anim_menu = true;                // app menu and control centre
	bool anim_osd = true;                 // on-screen display and notifications
	bool anim_panel = true;               // auto-hiding panels
	bool anim_maximize = true;            // windows glide to their new size
	bool anim_hover = true;               // buttons fade and glide
	bool anim_workspaces = true;          // windows slide when switching workspaces
	bool anim_overview = true;            // windows glide into the overview
	std::string anim_curve = "smooth";    // smooth, snappy, bouncy
	std::string anim_open_style = "zoom"; // zoom, slide, fade
	std::string anim_minimize_style = "panel"; // panel (into its button), fade

	// ---- On-screen display ----
	bool osd = true;
	std::string osd_position = "bottom";  // one of 9: top-left ... bottom-right
	std::string osd_orientation = "horizontal"; // horizontal, vertical (an upright pill)
	std::string osd_style = "segments";   // segments, bar
	int osd_timeout = 1500;               // ms
	int volume_step = 5;                  // percent (fixed)
	int brightness_step = 5;              // percent (fixed)
	bool osd_lock_keys = true;            // Caps Lock / Num Lock
	bool osd_layout = true;               // keyboard layout switches

	// ---- Keyboard ----
	std::string keyboard_layout;          // e.g. "us" or "us,ru"; empty = system default
	std::string keyboard_variant;
	std::string keyboard_options;         // e.g. "grp:alt_shift_toggle"
	std::string numlock_on_start = "keep";  // on, off, keep (as the keyboard starts)
	std::string capslock_on_start = "off";  // on, off, keep
	int repeat_rate = 25;                 // keys per second
	int repeat_delay = 600;               // ms

	// ---- Mouse & touchpad ----
	bool tap_to_click = true;
	bool natural_scroll_touchpad = true;
	bool natural_scroll_mouse = false;
	double pointer_speed = 0.0;           // -1 (slow) .. 1 (fast)
	bool left_handed = false;
	std::string accel_profile = "adaptive";   // adaptive | flat
	bool disable_while_typing = true;
	std::string click_method = "default";    // default | areas | fingers
	bool middle_emulation = false;
	// Touchpad swipes: "<fingers>_<up|down|left|right>" -> an action (see
	// gestures.cpp). Two fingers always scroll.
	std::map<std::string, std::string> gestures = {
		{"3_up", "overview"}, {"3_down", "overview-close"},
		{"3_left", "workspace-next"}, {"3_right", "workspace-prev"},
	};
	int csd_radius = 12;   // corner radius of apps that draw their own title bar (GTK 4: 12)

	// ---- Desktop and clipboard ----
	bool desktop_icons = true;            // files and folders in ~/Desktop on the wallpaper
	bool desktop_trash = true;            // a Trash icon on the desktop
	bool desktop_home = false;            // a Home folder icon on the desktop
	bool clipboard_history = true;        // remember what was copied (panel clipboard, Super+V)
	int clipboard_size = 25;              // entries kept
	bool middle_click_paste = true;       // select text, middle-click to paste it
	bool clipboard_keep = true;           // keep the clipboard history after a restart (text and files)

	// ---- Phone & tablet ----
	bool phone_screen = false;            // a virtual second screen shown over VNC (wayvnc)
	std::string phone_size = "2400x1080"; // its size in pixels (the phone's screen, landscape)
	std::string phone_link = "wifi";      // wifi (needs phone_password) or usb (adb reverse)
	std::string phone_password;           // for VNC over Wi-Fi
	int phone_port = 5900;

	// ---- Apps ----
	std::string terminal;                 // empty = pick automatically
	bool autostart = true;
	bool style_apps = true;

	// ---- Power (minutes; 0 = never) ----
	int screen_off_battery = 5;
	int screen_off_ac = 10;
	int sleep_battery = 15;
	int sleep_ac = 0;
	int lock_after_battery = 0;           // lock after this long idle (0 = only with screen off / sleep)
	int lock_after_ac = 0;
	bool battery_warnings = true;
	bool idle_fullscreen = true;          // no screen off or sleep while a window is full screen

	// ---- Screenshots and shortcuts ----
	std::string screenshot_print = "tool";// Print: tool (pick area/window/screen) | screen (at once)
	std::string screenshot_mode = "area"; // what the tool starts with: area, window, screen
	bool screenshot_pointer = false;      // the pointer in the picture
	int screenshot_delay = 0;             // seconds (the tool)
	bool screenshot_save = true;          // save a file (off = clipboard only)
	std::string screenshot_dir;           // empty = Pictures/Screenshots
	std::string screenshot_name;          // strftime pattern; empty = "Screenshot from %Y-%m-%d %H-%M-%S"
	bool screenshot_copy = true;          // also copy to the clipboard
	bool screenshot_hd = true;            // enlarge small ones to 1920 px (sharp after WhatsApp etc.)
	bool screenshot_notify = true;        // a notification with Open / Show in folder
	bool screenshot_sound = false;
	std::string screenshot_editor = "none"; // none, satty, swappy: opens it to draw on first
	// Screen recordings (the screenshot tool's video side, Shift+Ctrl+Alt+R).
	std::string record_audio = "none";    // none, system (what you hear), mic
	int record_fps = 60;                  // 30 or 60
	std::string record_quality = "high";  // standard, high
	std::string record_dir;               // empty = Videos/Screencasts
	std::vector<Shortcut> shortcuts;      // "shortcut.Super+E = nautilus"

	// ---- Displays: output name (e.g. "eDP-1") -> scale ----
	std::map<std::string, float> output_scale;
	std::map<std::string, OutputSettings> outputs;
	std::string primary_output;           // empty = the built-in screen
	std::string edge_resistance = "light"; // off, light, strong: the pointer pauses between screens

	// Settings Dusk itself should write back after loading (one-time updates).
	std::vector<std::pair<std::string, std::string>> pending_writes;

	static std::string directory();
	static std::string file_path();
	// Copies ~/.config/floe/floe.conf over the first time Dusk starts.
	static void migrate_from_floe();
	// Changes one key in the settings file (used by the control centre).
	static void write_key(const std::string &key, const std::string &value);

	// Reads the file over the defaults. Missing file = defaults.
	void load();

	// Is this kind of animation on (master switch and Reduce motion included)?
	bool animate(bool which) const { return animations && !reduce_motion && which; }
	// Duration of an animation, adjusted for the speed setting.
	double ms(double base) const { return base / (anim_speed > 0.1 ? anim_speed : 1.0); }
};

bool parse_color(const std::string &text, Color &out);
std::vector<std::string> split_list(const std::string &text);

} // namespace dusk
