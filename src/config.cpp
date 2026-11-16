#include "config.hpp"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "wlr.hpp"

namespace dusk {

namespace {

std::string trim(const std::string &s) {
	const auto first = s.find_first_not_of(" \t\r");
	if (first == std::string::npos) {
		return "";
	}
	const auto last = s.find_last_not_of(" \t\r");
	return s.substr(first, last - first + 1);
}

bool parse_bool(const std::string &v, bool fallback) {
	if (v == "true" || v == "yes" || v == "1" || v == "on") return true;
	if (v == "false" || v == "no" || v == "0" || v == "off") return false;
	return fallback;
}

int parse_int(const std::string &v, int fallback, int lo, int hi) {
	char *end = nullptr;
	const long n = std::strtol(v.c_str(), &end, 10);
	if (end == v.c_str()) {
		return fallback;
	}
	return static_cast<int>(std::clamp<long>(n, lo, hi));
}

std::string expand_home(const std::string &v) {
	const char *home = std::getenv("HOME");
	if (home && (v == "~" || v.rfind("~/", 0) == 0)) return home + v.substr(1);
	return v;
}

// "Super+Shift+E" -> modifiers and a key. False if the key isn't known.
bool parse_shortcut(const std::string &keys, Shortcut *out) {
	std::vector<std::string> parts;
	std::stringstream ss(keys);
	std::string p;
	while (std::getline(ss, p, '+')) {
		p = trim(p);
		if (!p.empty()) parts.push_back(p);
	}
	if (parts.empty()) return false;
	uint32_t mods = 0;
	for (size_t i = 0; i + 1 < parts.size(); i++) {
		std::string m = parts[i];
		std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::tolower(c); });
		if (m == "super" || m == "logo" || m == "meta" || m == "win") mods |= WLR_MODIFIER_LOGO;
		else if (m == "ctrl" || m == "control") mods |= WLR_MODIFIER_CTRL;
		else if (m == "alt") mods |= WLR_MODIFIER_ALT;
		else if (m == "shift") mods |= WLR_MODIFIER_SHIFT;
		else return false;
	}
	const xkb_keysym_t sym = xkb_keysym_from_name(parts.back().c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol) return false;
	out->mods = mods;
	out->sym = xkb_keysym_to_lower(sym);
	out->keys = keys;
	return true;
}

// "50-50/1-2;66-34/1-1" -> layouts, each a list of zones (x, y, w, h in
// fractions): the column widths in %, then how many rows each column has.
std::vector<std::vector<std::array<double, 4>>> parse_layouts(const std::string &text) {
	std::vector<std::vector<std::array<double, 4>>> out;
	std::stringstream all(text);
	std::string one;
	while (std::getline(all, one, ';')) {
		one = trim(one);
		const size_t slash = one.find('/');
		std::vector<double> cols;
		std::vector<int> rows;
		std::stringstream cs(one.substr(0, slash));
		std::string part;
		while (std::getline(cs, part, '-')) {
			const double v = std::atof(part.c_str());
			if (v > 0) cols.push_back(v);
		}
		if (slash != std::string::npos) {
			std::stringstream rs(one.substr(slash + 1));
			while (std::getline(rs, part, '-')) rows.push_back(std::clamp(std::atoi(part.c_str()), 1, 4));
		}
		if (cols.empty() || cols.size() > 6) continue;
		double sum = 0;
		for (double c : cols) sum += c;
		std::vector<std::array<double, 4>> zones;
		double x = 0;
		for (size_t i = 0; i < cols.size(); i++) {
			const double w = cols[i] / sum;
			const int n = i < rows.size() ? rows[i] : 1;
			for (int r = 0; r < n; r++) {
				zones.push_back({x, static_cast<double>(r) / n, w, 1.0 / n});
			}
			x += w;
		}
		out.push_back(zones);
	}
	return out;
}

double parse_double(const std::string &v, double fallback, double lo, double hi) {
	char *end = nullptr;
	const double n = std::strtod(v.c_str(), &end);
	if (end == v.c_str()) {
		return fallback;
	}
	return std::clamp(n, lo, hi);
}

// Accepts only one of `allowed`, otherwise keeps the default.
void parse_choice(const std::string &v, std::string &out,
		std::initializer_list<const char *> allowed) {
	for (const char *a : allowed) {
		if (v == a) {
			out = v;
			return;
		}
	}
}

void mkdir_p(const std::string &dir) {
	for (size_t pos = dir.find('/', 1);; pos = dir.find('/', pos + 1)) {
		mkdir(dir.substr(0, pos).c_str(), 0755);
		if (pos == std::string::npos) {
			break;
		}
	}
}

std::string config_home() {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) {
		return xdg;
	}
	const char *home = getenv("HOME");
	return std::string(home ? home : "/tmp") + "/.config";
}

const char *kWidgets[] = {"menu", "tasks", "pins", "spacer", "separator", "clock", "status",
	"media", "clipboard", "tray", "showdesktop", "overview", "trash", "keyboard", "notifications"};
const char *kTiles[] = {"wifi", "bluetooth", "dark", "nightlight", "dnd", "airplane",
	"caffeine", "power", "mic"};

} // namespace

std::vector<std::string> split_list(const std::string &text) {
	std::vector<std::string> out;
	std::stringstream ss(text);
	std::string item;
	while (std::getline(ss, item, ',')) {
		item = trim(item);
		if (!item.empty()) {
			out.push_back(item);
		}
	}
	return out;
}

bool parse_color(const std::string &text, Color &out) {
	std::string t = text;
	if (!t.empty() && t[0] == '#') {
		t.erase(0, 1);
	}
	if (t.size() != 6) {
		return false;
	}
	char *end = nullptr;
	const unsigned long hex = std::strtoul(t.c_str(), &end, 16);
	if (*end != '\0') {
		return false;
	}
	out = rgb(static_cast<unsigned>(hex));
	return true;
}

std::string Config::directory() {
	return config_home() + "/dusk";
}

std::string Config::file_path() {
	return directory() + "/dusk.conf";
}

void Config::migrate_from_floe() {
	const std::string now = file_path();
	const std::string old = config_home() + "/floe/floe.conf";
	if (access(now.c_str(), F_OK) == 0 || access(old.c_str(), R_OK) != 0) {
		return;
	}
	mkdir_p(directory());
	std::ifstream in(old);
	std::ofstream out(now);
	out << in.rdbuf();
	// An old autostart script comes along too.
	const std::string old_auto = config_home() + "/floe/autostart";
	const std::string new_auto = directory() + "/autostart";
	if (access(old_auto.c_str(), X_OK) == 0 && access(new_auto.c_str(), F_OK) != 0) {
		std::ifstream ai(old_auto);
		std::ofstream ao(new_auto);
		ao << ai.rdbuf();
		ao.close();
		chmod(new_auto.c_str(), 0755);
	}
}

void Config::write_key(const std::string &key, const std::string &value) {
	std::vector<std::string> lines;
	{
		std::ifstream in(file_path());
		std::string line;
		while (std::getline(in, line)) {
			lines.push_back(line);
		}
	}
	bool found = false;
	for (std::string &line : lines) {
		const std::string t = trim(line);
		const auto eq = t.find('=');
		if (!t.empty() && t[0] != '#' && eq != std::string::npos && trim(t.substr(0, eq)) == key) {
			line = key + " = " + value;
			found = true;
		}
	}
	if (!found) {
		lines.push_back(key + " = " + value);
	}
	mkdir_p(directory());
	const std::string tmp = file_path() + ".tmp";
	{
		std::ofstream out(tmp);
		for (const std::string &line : lines) {
			out << line << '\n';
		}
	}
	// Settings can hold private data (shortcut names, recent documents):
	// never wider than the owner, whatever the umask is.
	::chmod(tmp.c_str(), 0600);
	if (std::rename(tmp.c_str(), file_path().c_str()) != 0) {
		std::remove(tmp.c_str());
	}
}

void Config::load() {
	*this = Config{};   // start from defaults
	std::map<std::string, std::string> raw;
	std::vector<std::string> order;
	{
		std::ifstream file(file_path());
		std::string line;
		while (std::getline(file, line)) {
			line = trim(line);
			if (line.empty() || line[0] == '#' || line[0] == ';') {
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const std::string k = trim(line.substr(0, eq));
			if (!raw.count(k)) {
				order.push_back(k);
			}
			raw[k] = trim(line.substr(eq + 1));
		}
	}

	for (const std::string &k : order) {
		const std::string &v = raw[k];
		// Appearance
		if (k == "theme") dark = v != "light";
		else if (k == "dark_style") oled = false;   // (pure black is gone: always GNOME's dark)
		else if (k == "high_contrast") high_contrast = parse_bool(v, high_contrast);
		else if (k == "ui_icons") own_icons = v != "theme";
		else if (k == "accent") parse_color(v, accent);
		else if (k == "accent_auto") accent_auto = parse_bool(v, false);
		else if (k == "accent_for") accent_for = v;
		else if (k == "wallpaper_type") parse_choice(v, wallpaper_type, {"dusk", "halftone", "solid", "image"});
		else if (k == "wallpaper") wallpaper = v;
		else if (k == "wallpaper_transition") parse_choice(v, wallpaper_transition,
			{"random", "fade", "slide", "push", "zoom", "wipe", "grow", "circle", "pointer", "iris", "none"});
		else if (k == "wallpaper_transition_ms") wallpaper_transition_ms = parse_int(v, 900, 150, 4000);
		else if (k == "wallpaper_mode") parse_choice(v, wallpaper_mode, {"fill", "fit", "center", "stretch"});
		else if (k == "wallpaper_color") parse_color(v, wallpaper_color);
		else if (k == "ui_outline") parse_choice(v, ui_outline, {"ink", "thin", "none"});
		else if (k == "ui_shadow") parse_choice(v, ui_shadow, {"soft", "ink", "none"});
		else if (k == "ui_texture") parse_choice(v, ui_texture, {"none", "halftone"});
		else if (k == "ui_radius") ui_radius = parse_int(v, 22, 6, 36);
		else if (k == "ui_opacity") ui_opacity = parse_int(v, 92, 0, 100) / 100.0;
		else if (k == "ui_blur") ui_blur = parse_int(v, 100, 0, 100) / 100.0;
		else if (k == "panel_blur") panel_blur = parse_int(v, 100, 0, 100) / 100.0;
		else if (k == "settings_opacity") settings_opacity = parse_int(v, 100, 20, 100) / 100.0;
		else if (k == "settings_blur") settings_blur = parse_int(v, 100, 0, 100) / 100.0;
		else if (k == "font") font = v.empty() ? "Google Sans" : v;
		else if (k == "mono_font") mono_font = v.empty() ? "Google Sans Code" : v;
		else if (k == "osk_layout") parse_choice(v, osk_layout, {"normal", "split", "wide"});
		else if (k == "allow_tearing") allow_tearing = parse_bool(v, true);
		else if (k == "font_size") font_size = parse_int(v, 11, 7, 20);
		else if (k == "mono_font_size") mono_font_size = parse_int(v, 11, 7, 24);
		else if (k == "cursor_theme") cursor_theme = v;
		else if (k == "icon_theme") icon_theme = v;
		else if (k == "cursor_size") cursor_size = parse_int(v, 0, 0, 256);
		// Windows
		else if (k == "border") parse_choice(v, border, {"none", "thin", "outline"});
		else if (k == "shadow") parse_choice(v, shadow, {"none", "soft", "ink", "hard"});
		else if (k == "accent_border") accent_border = parse_bool(v, accent_border);
		else if (k == "window_radius") window_radius = parse_int(v, 12, 0, 40);
		else if (k == "screen_corners") screen_corners = parse_int(v, 14, 0, 60);
		else if (k == "titlebar_height") titlebar_height = parse_int(v, 36, 24, 60);
		else if (k == "show_titles") show_titles = parse_bool(v, false);
		else if (k == "title_align") parse_choice(v, title_align, {"center", "left"});
		else if (k == "buttons_side") parse_choice(v, buttons_side, {"right", "left"});
		else if (k == "button_style") parse_choice(v == "gnome" ? "gtk" : v, button_style, {"gtk", "sketch", "plain"});
		else if (k == "show_minimize") show_minimize = parse_bool(v, show_minimize);
		else if (k == "show_maximize") show_maximize = parse_bool(v, show_maximize);
		else if (k == "apps_follow_buttons") apps_follow_buttons = parse_bool(v, true);
		else if (k == "desktop_icons") desktop_icons = parse_bool(v, true);
		else if (k == "desktop_trash") desktop_trash = parse_bool(v, true);
		else if (k == "desktop_home") desktop_home = parse_bool(v, false);
		else if (k == "workspaces_dynamic") workspaces_dynamic = parse_bool(v, false);
		else if (k == "workspace_count") workspace_count = parse_int(v, 1, 1, 12);
		else if (k == "workspace_wrap") workspace_wrap = parse_bool(v, false);
		else if (k == "tasks_all_workspaces") tasks_all_workspaces = parse_bool(v, true);
		else if (k == "super_key") parse_choice(v, super_key, {"menu", "overview"});
		else if (k == "hot_corner") parse_choice(v, hot_corner, {"none", "top-left", "top-right", "bottom-left", "bottom-right"});
		else if (k == "overview_apps") overview_apps = parse_bool(v, false);
		else if (k == "overview_minimized") overview_minimized = parse_bool(v, true);
		else if (k == "overview_strip") overview_strip = v;
		else if (k == "dock_magnify") dock_magnify = parse_bool(v, false);
		else if (k == "overview_icon") parse_choice(v == "grid" || v == "squares" || v == "circle" || v == "text" ? "dots" : v,   // retired looks
			overview_icon, {"dots", "led", "numbers", "roman", "letters", "lines", "rings", "boxes", "count", "distro", "custom"});
		else if (k == "overview_icon_custom") overview_icon_custom = v;
		else if (k == "anim_workspaces") anim_workspaces = parse_bool(v, true);
		else if (k == "anim_overview") anim_overview = parse_bool(v, true);
		else if (k == "clipboard_history") clipboard_history = parse_bool(v, true);
		else if (k == "clipboard_size") clipboard_size = parse_int(v, 25, 5, 200);
		else if (k == "middle_click_paste") middle_click_paste = parse_bool(v, true);
		else if (k == "clipboard_keep") clipboard_keep = parse_bool(v, true);
		else if (k == "notification_history_size") notification_history_size = parse_int(v, 100, 0, 500);
		else if (k == "notification_history_keep") notification_history_keep = parse_bool(v, true);
		else if (k == "double_click") parse_choice(v, double_click, {"maximize", "minimize", "none"});
		else if (k == "middle_click") parse_choice(v, middle_click, {"minimize", "none"});
		else if (k == "placement") parse_choice(v, placement, {"center", "cascade", "cursor"});
		else if (k == "edge_snap") edge_snap = parse_bool(v, edge_snap);
		else if (k == "focus_follows_mouse") focus_follows_mouse = parse_bool(v, focus_follows_mouse);
		// Panels (the per-panel keys are read below)
		else if (k == "clock_24h") clock_24h = parse_bool(v, clock_24h);
		else if (k == "show_date") show_date = parse_bool(v, show_date);
		else if (k == "battery_style") parse_choice(v, battery_style, {"icon", "percent", "ring"});
		else if (k == "clock_click") parse_choice(v, clock_click, {"calendar", "notifications", "controlcentre"});
		else if (k == "menu_icon") parse_choice(v, menu_icon, {"dots", "squares", "dusk", "search", "distro", "custom"});
		else if (k == "menu_icon_custom") menu_icon_custom = v;
		else if (k == "menu_power") menu_power = parse_bool(v, true);
		else if (k == "power_confirm") power_confirm = parse_bool(v, true);
		else if (k == "lock_command") lock_command = v;
		else if (k == "lock_background") parse_choice(v, lock_background, {"wallpaper", "blur", "picture", "picture_blur", "color"});
		else if (k == "lock_picture") lock_picture = v;
		else if (k == "lock_color") parse_color(v, lock_color);
		else if (k == "lock_blur") lock_blur = parse_int(v, 60, 0, 100);
		else if (k == "lock_dim") lock_dim = parse_int(v, 25, 0, 85);
		else if (k == "lock_clock") parse_choice(v, lock_clock, {"center", "left", "corner", "hidden"});
		else if (k == "lock_clock_style") parse_choice(v, lock_clock_style, {"digital", "stacked", "outline", "analog"});
		else if (k == "lock_clock_font") lock_clock_font = v;
		else if (k == "lock_media") lock_media = parse_bool(v, true);
		else if (k == "lock_notifications") lock_notifications = parse_bool(v, true);
		else if (k == "lock_status") lock_status = parse_bool(v, true);
		else if (k == "lock_card") lock_card = parse_bool(v, false);
		else if (k == "lock_clock_weight") parse_choice(v, lock_clock_weight, {"light", "bold"});
		else if (k == "lock_date") lock_date = parse_bool(v, true);
		else if (k == "lock_avatar") lock_avatar = parse_bool(v, true);
		else if (k == "lock_message") lock_message = v;
		else if (k == "lock_screen_off") lock_screen_off = parse_bool(v, true);
		else if (k == "lock_clock_size") parse_choice(v, lock_clock_size, {"small", "medium", "large"});
		else if (k == "lock_clock_accent") lock_clock_accent = parse_bool(v, false);
		else if (k == "lock_form") parse_choice(v, lock_form, {"middle", "lower", "right"});
		else if (k == "lock_avatar_shape") parse_choice(v, lock_avatar_shape, {"circle", "rounded"});
		else if (k == "lock_name") lock_name = parse_bool(v, true);
		else if (k == "lock_curtain") lock_curtain = parse_bool(v, false);
		else if (k == "lock_sleep") lock_sleep = parse_bool(v, true);
		else if (k == "power_actions") {
			power_actions.clear();
			for (const std::string &a : split_list(v)) {
				static const char *kKnown[] = {"lock", "sleep", "hibernate", "logout", "restart",
					"shutdown", "firmware"};
				if (std::find(std::begin(kKnown), std::end(kKnown), a) != std::end(kKnown) &&
						std::find(power_actions.begin(), power_actions.end(), a) == power_actions.end()) {
					power_actions.push_back(a);
				}
			}
		}
		else if (k == "week_start") week_monday = v == "monday";
		// Control centre
		else if (k == "cc_tiles") {
			tiles.clear();
			for (const std::string &item : split_list(v)) {
				TileConfig t;
				const auto colon = item.find(':');
				t.kind = item.substr(0, colon);
				t.wide = colon != std::string::npos && item.substr(colon + 1) == "wide";
				if (std::find_if(std::begin(kTiles), std::end(kTiles),
						[&](const char *n) { return t.kind == n; }) != std::end(kTiles)) {
					tiles.push_back(t);
				}
			}
		}
		else if (k == "cc_side") parse_choice(v, cc_side, {"auto", "left", "right"});
		else if (k == "cc_sliders") cc_sliders = parse_bool(v, cc_sliders);
		else if (k == "cc_media") cc_media = parse_bool(v, cc_media);
		else if (k == "cc_notifications") cc_notifications = parse_bool(v, cc_notifications);
		else if (k == "cc_style") parse_choice(v, cc_style, {"dusk", "windows", "gnome", "macos"});
		else if (k == "notif_centre") parse_choice(v, notif_centre, {"auto", "together", "separate"});
		else if (k == "panel_look") parse_choice(v, panel_look, {"dusk", "windows", "gnome", "macos"});
		else if (k == "launcher_favorites") launcher_favorites = split_list(v);
		else if (k == "launcher_look") parse_choice(v, launcher_look, {"dusk", "windows", "gnome", "macos"});

		else if (k == "audio_keep_awake") audio_keep_awake = parse_bool(v, true);
		else if (k == "media_wave_beat") media_wave_beat = parse_bool(v, true);
		else if (k == "media_viz") parse_choice(v, media_viz, {"bars", "mirror", "dots", "wave", "hills", "off"});
		else if (k == "media_viz_color") parse_choice(v, media_viz_color, {"art", "accent", "text", "rainbow"});
		else if (k == "media_marquee") media_marquee = parse_bool(v, true);
		else if (k == "lyrics") lyrics = parse_bool(v, true);
		else if (k == "lyrics_show") lyrics_show = parse_bool(v, false);
		else if (k == "lyrics_style") parse_choice(v, lyrics_style,
			{"classic", "karaoke", "subtitle", "glow", "scroll", "bounce", "typewriter", "comic"});
		else if (k == "lyrics_position") parse_choice(v, lyrics_position, {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right"});
		else if (k == "lyrics_translate") lyrics_translate = parse_bool(v, true);
		else if (k == "lyrics_translate_to") lyrics_translate_to = v;
		else if (k == "lyrics_translate_show") parse_choice(v, lyrics_translate_show, {"under", "instead"});
		else if (k == "lyrics_color") parse_choice(v, lyrics_color, {"accent", "art", "white"});
		else if (k == "lyrics_on_top") lyrics_on_top = parse_bool(v, false);
		else if (k == "lyrics_delay") lyrics_delay = parse_int(v, 0, -10000, 10000);
		else if (k == "lyrics_speed") lyrics_speed = parse_int(v, 100, 70, 140);
		else if (k == "lyrics_size") lyrics_size = parse_int(v, 100, 60, 200);
		else if (k == "lyrics_recognize") lyrics_recognize = parse_bool(v, true);
		else if (k == "night_light") night_light = parse_bool(v, night_light);
		else if (k == "night_light_temp") night_light_temp = parse_int(v, 4300, 2500, 6500);
		else if (k == "dnd") dnd = parse_bool(v, dnd);
		// Notifications
		else if (k == "notifications") notifications = parse_bool(v, notifications);
		else if (k == "notification_position")
			parse_choice(v == "top-center" ? std::string("top") : v, notification_position,
				{"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom",
					"bottom-right"});
		else if (k == "cc_layout" || k == "cc_layout_win11" || k == "cc_layout_gnome" || k == "cc_layout_macos") {
			std::vector<CcTile> &cc_layout = k == "cc_layout" ? this->cc_layout : k == "cc_layout_win11"
				? cc_layout_windows : k == "cc_layout_gnome" ? cc_layout_gnome : cc_layout_macos;
			// "wifi:0:0:3:1,bluetooth:3:0:3:1,..."
			std::stringstream all(v);
			std::string one;
			while (std::getline(all, one, ',')) {
				CcTile t;
				char id[32] = {};
				if (std::sscanf(trim(one).c_str(), "%31[^:]:%d:%d:%d:%d", id, &t.c, &t.r, &t.w, &t.h) == 5) {
					t.id = id;
					t.c = std::clamp(t.c, 0, 5);
					t.r = std::clamp(t.r, 0, 20);
					t.w = std::clamp(t.w, 1, 6 - t.c);
					t.h = std::clamp(t.h, 1, 6);
					cc_layout.push_back(t);
				}
			}
		}
		else if (k == "toast_over_fullscreen") toast_over_fullscreen = parse_bool(v, false);
		else if (k == "notif_group") notif_group = parse_bool(v, false);
		else if (k == "notification_timeout") notification_timeout = parse_int(v, 5000, 1000, 30000);
		// Animations
		else if (k == "animations") animations = parse_bool(v, animations);
		else if (k == "reduce_motion") reduce_motion = parse_bool(v, reduce_motion);
		else if (k == "anim_speed") anim_speed = parse_double(v, 1.0, 0.25, 3.0);
		else if (k == "anim_windows") anim_windows = parse_bool(v, anim_windows);
		else if (k == "anim_minimize") anim_minimize = parse_bool(v, anim_minimize);
		else if (k == "anim_menu") anim_menu = parse_bool(v, anim_menu);
		else if (k == "anim_osd") anim_osd = parse_bool(v, anim_osd);
		else if (k == "anim_panel") anim_panel = parse_bool(v, anim_panel);
		else if (k == "anim_maximize") anim_maximize = parse_bool(v, anim_maximize);
		else if (k == "anim_hover") anim_hover = parse_bool(v, anim_hover);
		else if (k == "anim_curve") parse_choice(v, anim_curve, {"smooth", "snappy", "bouncy"});
		else if (k == "anim_open_style") parse_choice(v, anim_open_style, {"zoom", "slide", "fade"});
		else if (k == "anim_minimize_style") parse_choice(v, anim_minimize_style, {"panel", "fade"});
		// OSD
		else if (k == "osd") osd = parse_bool(v, osd);
		else if (k == "osd_position") parse_choice(v, osd_position, {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right"});
		else if (k == "osd_orientation") parse_choice(v, osd_orientation, {"horizontal", "vertical"});
		else if (k == "osd_style") parse_choice(v, osd_style, {"segments", "bar"});
		else if (k == "osd_timeout") osd_timeout = parse_int(v, 1500, 500, 10000);
		else if (k == "osd_lock_keys") osd_lock_keys = parse_bool(v, osd_lock_keys);
		else if (k == "osd_layout") osd_layout = parse_bool(v, osd_layout);
		// Keyboard
		else if (k == "keyboard_layout") keyboard_layout = v;
		else if (k == "keyboard_variant") keyboard_variant = v;
		else if (k == "keyboard_options") keyboard_options = v;
		else if (k == "numlock_on_start") parse_choice(v, numlock_on_start, {"on", "off", "keep"});
		else if (k == "capslock_on_start") parse_choice(v, capslock_on_start, {"on", "off", "keep"});
		else if (k == "repeat_rate") repeat_rate = parse_int(v, repeat_rate, 1, 100);
		else if (k == "repeat_delay") repeat_delay = parse_int(v, repeat_delay, 100, 2000);
		// Mouse & touchpad
		else if (k == "tap_to_click") tap_to_click = parse_bool(v, tap_to_click);
		else if (k == "natural_scroll_touchpad")
			natural_scroll_touchpad = parse_bool(v, natural_scroll_touchpad);
		else if (k == "natural_scroll_mouse")
			natural_scroll_mouse = parse_bool(v, natural_scroll_mouse);
		else if (k == "pointer_speed") pointer_speed = parse_double(v, 0.0, -1.0, 1.0);
		else if (k == "left_handed") left_handed = parse_bool(v, false);
		else if (k == "accel_profile") accel_profile = v == "flat" ? "flat" : "adaptive";
		else if (k == "disable_while_typing") disable_while_typing = parse_bool(v, true);
		else if (k == "click_method") click_method = (v == "areas" || v == "fingers") ? v : "default";
		else if (k == "middle_emulation") middle_emulation = parse_bool(v, false);
		else if (k.rfind("gesture_", 0) == 0 && k.size() > 8) gestures[k.substr(8)] = v.empty() ? "none" : v;
		else if (k == "autostart") autostart = parse_bool(v, true);
		else if (k == "style_apps") style_apps = parse_bool(v, true);
		else if (k == "screen_off_battery") screen_off_battery = parse_int(v, 5, 0, 240);
		else if (k == "screen_off_ac") screen_off_ac = parse_int(v, 10, 0, 240);
		else if (k == "sleep_battery") sleep_battery = parse_int(v, 15, 0, 480);
		else if (k == "sleep_ac") sleep_ac = parse_int(v, 0, 0, 480);
		else if (k == "lock_after_battery") lock_after_battery = parse_int(v, 0, 0, 480);
		else if (k == "lock_after_ac") lock_after_ac = parse_int(v, 0, 0, 480);
		else if (k == "phone_screen") phone_screen = parse_bool(v, false);
		else if (k == "phone_size") phone_size = v.empty() ? "2400x1080" : v;
		else if (k == "phone_link") parse_choice(v, phone_link, {"wifi", "usb"});
		else if (k == "phone_password") phone_password = v;
		else if (k == "phone_port") phone_port = parse_int(v, 5900, 1024, 65535);
		else if (k == "idle_fullscreen") idle_fullscreen = parse_bool(v, true);
		else if (k == "battery_warnings") battery_warnings = parse_bool(v, true);
		else if (k == "screenshot_dir") screenshot_dir = expand_home(v);
		else if (k == "screenshot_copy") screenshot_copy = parse_bool(v, true);
		else if (k == "screenshot_hd") screenshot_hd = parse_bool(v, true);
		else if (k == "screenshot_sound") screenshot_sound = parse_bool(v, false);
		else if (k == "screenshot_print") parse_choice(v, screenshot_print, {"tool", "screen"});
		else if (k == "screenshot_mode") parse_choice(v, screenshot_mode, {"area", "window", "screen"});
		else if (k == "screenshot_pointer") screenshot_pointer = parse_bool(v, false);
		else if (k == "screenshot_delay") screenshot_delay = parse_int(v, 0, 0, 60);
		else if (k == "screenshot_save") screenshot_save = parse_bool(v, true);
		else if (k == "screenshot_name") screenshot_name = v;
		else if (k == "screenshot_notify") screenshot_notify = parse_bool(v, true);
		else if (k == "screenshot_editor") parse_choice(v, screenshot_editor, {"none", "satty", "swappy"});
		else if (k == "record_audio") parse_choice(v, record_audio, {"none", "system", "mic"});
		else if (k == "record_fps") record_fps = parse_int(v, 60, 30, 60) >= 45 ? 60 : 30;
		else if (k == "record_quality") parse_choice(v, record_quality, {"standard", "high"});
		else if (k == "record_dir") record_dir = expand_home(v);
		else if (k.rfind("shortcut.", 0) == 0 && k.size() > 9 && !v.empty()) {
			Shortcut sc;
			if (parse_shortcut(k.substr(9), &sc)) {
				sc.command = v;
				shortcuts.push_back(sc);
			}
		}
		else if (k == "csd_radius") csd_radius = parse_int(v, 12, 0, 40);
		// Apps
		else if (k == "terminal") terminal = v;
		// Displays
		else if (k == "primary_output") primary_output = v;
		else if (k == "tile_gap") tile_gap = parse_int(v, 0, 0, 40);
		else if (k == "snap_bar") snap_bar = parse_bool(v, true);
		else if (k == "snap_layouts") snap_layouts_text = v;
		else if (k == "edge_resistance") parse_choice(v, edge_resistance, {"off", "light", "strong"});
		else if (k.rfind("output.", 0) == 0 && k.rfind('.') > 7) {
			const size_t dot = k.rfind('.');
			OutputSettings &o = outputs[k.substr(7, dot - 7)];
			const std::string field = k.substr(dot + 1);
			if (field == "enabled") o.enabled = parse_bool(v, true);
			else if (field == "vrr") o.vrr = parse_bool(v, false);
			else if (field == "rotation") o.rotation = (parse_int(v, 0, 0, 270) / 90) * 90;
			else if (field == "mode") {
				// "1920x1080@60.000" or "1920x1080" or "auto"
				int w = 0, h = 0;
				double r = 0;
				if (std::sscanf(v.c_str(), "%dx%d@%lf", &w, &h, &r) >= 2 && w > 0 && h > 0) {
					o.width = w;
					o.height = h;
					o.refresh = r;
				}
			} else if (field == "position") {
				int x = 0, y = 0;
				if (std::sscanf(v.c_str(), "%d,%d", &x, &y) == 2) {
					o.has_position = true;
					o.x = x;
					o.y = y;
				}
			}
		}
		else if (k.rfind("scale.", 0) == 0 && k.size() > 6) {
			const double s = parse_double(v, 0.0, 0.5, 4.0) > 2.0 ? 2.0 : parse_double(v, 0.0, 0.5, 4.0);   // 200% at most
			if (s > 0) {
				output_scale[k.substr(6)] = static_cast<float>(s);
			}
		}
	}

	snap_layouts = parse_layouts(snap_layouts_text);
	if (snap_layouts.empty()) {
		snap_layouts = parse_layouts("50-50/1-1");
	}

	// The 0.6 "Comic" preset had screentone and hard shadows; 0.7's is subtler.
	if (raw.count("ui_style") && raw["ui_style"] == "comic" && raw.count("ui_texture") &&
			raw["ui_texture"] == "halftone" && shadow == "hard") {
		shadow = "ink";
	}

	// ---- Panels: "panels = main, top" plus "panel.<id>.<key>" lines ----
	auto get = [&](const std::string &key) -> const std::string * {
		auto it = raw.find(key);
		return it == raw.end() ? nullptr : &it->second;
	};
	std::vector<std::string> ids = {"main"};
	if (const std::string *list = get("panels")) {
		ids = split_list(*list);
	}
	panels.clear();
	for (const std::string &id : ids) {
		PanelConfig p;
		p.id = id;
		if (id == "main" && !get("panels")) {
			// Settings from Floe 0.4, which had a single panel.
			if (const std::string *v = get("panel_position")) parse_choice(*v, p.edge, {"bottom", "top"});
			if (const std::string *v = get("panel_style")) parse_choice(*v, p.style, {"floating", "full"});
			if (const std::string *v = get("panel_labels")) p.labels = parse_bool(*v, p.labels);
			if (const std::string *v = get("panel_align"); v && *v == "center") {
				p.widgets = {"menu", "spacer", "tasks", "spacer", "status", "notifications", "clock"};
			}
		}
		const std::string pre = "panel." + id + ".";
		if (const std::string *v = get(pre + "edge")) parse_choice(*v, p.edge, {"bottom", "top", "left", "right"});
		if (const std::string *v = get(pre + "style")) parse_choice(*v, p.style, {"floating", "full", "dock"});
		if (const std::string *v = get(pre + "size")) p.size = parse_int(*v, p.size, 28, 96);
		if (const std::string *v = get(pre + "output")) p.output = v->empty() ? "all" : *v;
		if (const std::string *v = get(pre + "autohide")) p.autohide = parse_bool(*v, p.autohide);
		if (const std::string *v = get(pre + "labels")) p.labels = parse_bool(*v, p.labels);
		if (const std::string *v = get(pre + "opacity")) p.opacity = parse_double(*v, p.opacity, 0.0, 1.0);
		if (const std::string *v = get(pre + "widgets")) {
			p.widgets.clear();
			for (const std::string &w : split_list(*v)) {
				if (std::find_if(std::begin(kWidgets), std::end(kWidgets),
						[&](const char *n) { return w == n; }) != std::end(kWidgets)) {
					p.widgets.push_back(w);
				}
			}
			// Panels set up before the tray existed get it once, before the
			// status icons (written back by Dusk, so Settings shows it too).
			bool grew = false;
			if (!get("tray_added") && std::find(p.widgets.begin(), p.widgets.end(), "tray") == p.widgets.end()) {
				auto at = std::find(p.widgets.begin(), p.widgets.end(), "status");
				if (at != p.widgets.end()) {
					p.widgets.insert(at, "tray");
					grew = true;
				}
			}
			// 0.14: the overview button next to the app menu, and the trash
			// before the status icons (both can be removed again).
			if (!get("overview_added") && std::find(p.widgets.begin(), p.widgets.end(), "overview") == p.widgets.end()) {
				auto at = std::find(p.widgets.begin(), p.widgets.end(), "menu");
				if (at != p.widgets.end()) {
					p.widgets.insert(at + 1, "overview");
					grew = true;
				}
			}
			if (!get("trash_added") && std::find(p.widgets.begin(), p.widgets.end(), "trash") == p.widgets.end()) {
				auto at = std::find(p.widgets.begin(), p.widgets.end(), "status");
				if (at != p.widgets.end()) {
					p.widgets.insert(at, "trash");
					grew = true;
				}
			}
			// 0.35.3: the notifications bell (shown only when notifications
			// have their own centre), after the status icons.
			if (!get("bell_added") && std::find(p.widgets.begin(), p.widgets.end(), "notifications") == p.widgets.end()) {
				auto at = std::find(p.widgets.begin(), p.widgets.end(), "status");
				if (at != p.widgets.end()) {
					p.widgets.insert(at + 1, "notifications");
					grew = true;
				}
			}
			if (grew) {
				std::string list;
				for (const std::string &w : p.widgets) list += (list.empty() ? "" : ",") + w;
				pending_writes.push_back({pre + "widgets", list});
			}
		}
		if (const std::string *v = get(pre + "pins")) p.pins = split_list(*v);
		// Pinned apps show with the open ones now (one "apps" widget, like a
		// dock): a separate "pins" widget is folded into "tasks".
		if (std::find(p.widgets.begin(), p.widgets.end(), "pins") != p.widgets.end()) {
			if (std::find(p.widgets.begin(), p.widgets.end(), "tasks") != p.widgets.end()) {
				p.widgets.erase(std::remove(p.widgets.begin(), p.widgets.end(), "pins"), p.widgets.end());
			} else {
				std::replace(p.widgets.begin(), p.widgets.end(), std::string("pins"), std::string("tasks"));
			}
		}
		panels.push_back(std::move(p));
	}
	if (!get("tray_added")) {
		pending_writes.push_back({"tray_added", "true"});
	}
	if (!get("overview_added")) pending_writes.push_back({"overview_added", "true"});
	if (!get("trash_added")) pending_writes.push_back({"trash_added", "true"});
	if (!get("bell_added")) pending_writes.push_back({"bell_added", "true"});
	// 0.20.4: notifications one card each from now on (once; turn grouping
	// back on and it stays on).
	if (!get("notif_ungrouped")) {
		notif_group = false;
		pending_writes.push_back({"notif_group", "false"});
		pending_writes.push_back({"notif_ungrouped", "true"});
	}
	// 0.21: the plain "Sans" default became Google Sans (once: picking
	// Sans again afterwards sticks).
	if (!get("fonts_google")) {
		if (font == "Sans" || font == "Noto Sans") {
			font = "Google Sans";
			pending_writes.push_back({"font", font});
		}
		pending_writes.push_back({"fonts_google", "true"});
	}
	// 0.21.2: no more pure black; the Comic look is the default (once).
	if (get("dark_style") && *get("dark_style") != "gnome") pending_writes.push_back({"dark_style", "gnome"});
	if (!get("look_comic")) {
		ui_outline = "ink";
		ui_shadow = "ink";
		border = "outline";
		shadow = "ink";
		pending_writes.push_back({"ui_outline", "ink"});
		pending_writes.push_back({"ui_shadow", "ink"});
		pending_writes.push_back({"border", "outline"});
		pending_writes.push_back({"shadow", "ink"});
		pending_writes.push_back({"ui_style", "comic"});
		pending_writes.push_back({"look_comic", "true"});
	}
	// 0.35: the calm look (thin hairlines, soft faint shadows) replaces the
	// drawn ink lines, once; the others stay one click away in Appearance.
	if (!get("look_calm")) {
		ui_outline = "thin";
		ui_shadow = "soft";
		border = "thin";
		shadow = "soft";
		pending_writes.push_back({"ui_outline", "thin"});
		pending_writes.push_back({"ui_shadow", "soft"});
		pending_writes.push_back({"border", "thin"});
		pending_writes.push_back({"shadow", "soft"});
		pending_writes.push_back({"ui_style", "calm"});
		// The control centre's new arrangement (Edit still changes it).
		cc_layout.clear();
		pending_writes.push_back({"cc_layout", ""});
		pending_writes.push_back({"look_calm", "true"});
	}
	// 0.35.1: Google Sans was soft at 100% (it's drawn for print and big
	// screens); Adwaita Sans (GNOME's, from Inter) or Inter is made for
	// small screen text. Once, and only from the old default.
	if (!get("font_screen") && font == "Google Sans") {
		std::string families;
		if (FILE *p = popen("fc-list : family 2>/dev/null", "r")) {
			char buf[512];
			while (fgets(buf, sizeof buf, p)) families += buf;
			pclose(p);
		}
		for (const char *f : {"Adwaita Sans", "Inter", "Noto Sans"}) {
			if (families.find(std::string(f) + "\n") != std::string::npos ||
					families.find(std::string(f) + ",") != std::string::npos) {
				font = f;
				pending_writes.push_back({"font", font});
				break;
			}
		}
		pending_writes.push_back({"font_screen", "true"});
	}
	// 0.35.1: panels, menus and the control centre nearly solid: text never
	// sits on a busy, tinted wallpaper (75% let an orange wallpaper turn
	// them brown and grey ones muddy). Only if you hadn't set it.
	if (!get("ui_solid")) {
		if (!get("ui_opacity")) {
			ui_opacity = 0.92;
			pending_writes.push_back({"ui_opacity", "92"});
		}
		// The title bar's buttons: round and grey (GNOME's), not corner
		// brackets with a red ✕ (part of the old drawn look).
		if (!get("button_style")) {
			button_style = "gtk";
			pending_writes.push_back({"button_style", "gtk"});
		}
		pending_writes.push_back({"ui_solid", "true"});
	}
	// 0.21.2: Google Sans Flex (0.21.0-0.21.1's) became Google Sans.
	if (font == "Google Sans Flex") {
		font = "Google Sans";
		pending_writes.push_back({"font", font});
	}
}

} // namespace dusk
