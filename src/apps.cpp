#include "apps.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "draw.hpp"

namespace dusk {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
	for (char &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

std::vector<std::string> split(const std::string &s, char sep) {
	std::vector<std::string> parts;
	std::string part;
	std::istringstream in(s);
	while (std::getline(in, part, sep)) {
		if (!part.empty()) {
			parts.push_back(part);
		}
	}
	return parts;
}

// XDG data directories (highest priority first), incl. Flatpak's.
std::vector<std::string> data_dirs() {
	std::vector<std::string> dirs;
	const char *home = getenv("HOME");
	const char *data_home = getenv("XDG_DATA_HOME");
	if (data_home && *data_home) {
		dirs.push_back(data_home);
	} else if (home) {
		dirs.push_back(std::string(home) + "/.local/share");
	}
	if (home) {
		dirs.push_back(std::string(home) + "/.local/share/flatpak/exports/share");
	}
	dirs.push_back("/var/lib/flatpak/exports/share");
	const char *env = getenv("XDG_DATA_DIRS");
	for (const std::string &d : split(env && *env ? env : "/usr/local/share:/usr/share", ':')) {
		dirs.push_back(d);
	}
	std::vector<std::string> unique;
	for (const std::string &d : dirs) {
		if (std::find(unique.begin(), unique.end(), d) == unique.end()) {
			unique.push_back(d);
		}
	}
	return unique;
}

// Turns an Exec= line into a shell command: drops %f/%U-style field codes
// and flatpak's @@ markers.
std::string clean_exec(const std::string &exec) {
	std::string out;
	std::istringstream in(exec);
	std::string token;
	while (in >> token) {
		if (token == "@@" || token == "@@u" || token == "@@f") {
			continue;
		}
		if (token.size() == 2 && token[0] == '%') {
			if (token[1] == '%') {
				token = "%";
			} else {
				continue;
			}
		}
		if (!out.empty()) {
			out += ' ';
		}
		out += token;
	}
	return out;
}

std::string basename_of(const std::string &command) {
	std::string first = command.substr(0, command.find(' '));
	const auto slash = first.rfind('/');
	return slash == std::string::npos ? first : first.substr(slash + 1);
}

} // namespace

// ---- .desktop files -------------------------------------------------------

std::string AppDatabase::folders_now() const {
	std::string sig;
	for (const std::string &dir : data_dirs()) {
		const fs::path apps_dir = fs::path(dir) / "applications";
		std::error_code ec;
		if (!fs::is_directory(apps_dir, ec)) continue;
		auto stamp = [&sig](const fs::path &p) {
			std::error_code e;
			const auto t = fs::last_write_time(p, e);
			sig += p.string() + "@" + std::to_string(e ? 0 : t.time_since_epoch().count()) + ";";
		};
		stamp(apps_dir);
		for (auto it = fs::recursive_directory_iterator(apps_dir, fs::directory_options::skip_permission_denied, ec);
				it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) break;
			if (it->is_directory(ec)) stamp(it->path());
		}
	}
	return sig;
}

void AppDatabase::reload_if_changed() {
	const std::string now = folders_now();
	if (now == folders_seen_ && !apps_.empty()) return;
	reload();
}

void AppDatabase::reload() {
	folders_seen_ = folders_now();
	apps_.clear();
	icon_cache_.clear();
	title_cache_.clear();
	id_found_.clear();
	std::set<std::string> seen;

	// Language for Name[xx] keys, e.g. "en_US.UTF-8" -> "en_US", "en".
	std::string lang_full, lang_short;
	if (const char *lang = getenv("LANG")) {
		lang_full = lang;
		lang_full = lang_full.substr(0, lang_full.find('.'));
		lang_short = lang_full.substr(0, lang_full.find('_'));
	}

	for (const std::string &dir : data_dirs()) {
		const fs::path apps_dir = fs::path(dir) / "applications";
		std::error_code ec;
		if (!fs::is_directory(apps_dir, ec)) {
			continue;
		}
		for (auto it = fs::recursive_directory_iterator(apps_dir,
				fs::directory_options::skip_permission_denied, ec);
				it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) {
				break;
			}
			const fs::path &path = it->path();
			if (path.extension() != ".desktop") {
				continue;
			}
			std::string id = fs::relative(path, apps_dir, ec).string();
			std::replace(id.begin(), id.end(), '/', '-');
			if (!seen.insert(id).second) {
				continue;   // a higher-priority directory already had it
			}

			std::ifstream file(path);
			std::string line, name, name_full, name_short, exec, comment, type, icon, wm_class;
			std::string only_show_in;
			bool in_entry = false, no_display = false, hidden = false, terminal = false;
			while (std::getline(file, line)) {
				if (!line.empty() && line[0] == '[') {
					in_entry = line == "[Desktop Entry]";
					continue;
				}
				if (!in_entry || line.empty() || line[0] == '#') {
					continue;
				}
				const size_t eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				std::string key = line.substr(0, eq);
				std::string value = line.substr(eq + 1);
				while (!key.empty() && key.back() == ' ') key.pop_back();
				while (!value.empty() && value.front() == ' ') value.erase(0, 1);

				if (key == "Type") type = value;
				else if (key == "Name") name = value;
				else if (!lang_full.empty() && key == "Name[" + lang_full + "]") name_full = value;
				else if (!lang_short.empty() && key == "Name[" + lang_short + "]") name_short = value;
				else if (key == "Exec") exec = value;
				else if (key == "Comment") comment = value;
				else if (key == "Icon") icon = value;
				else if (key == "StartupWMClass") wm_class = value;
				else if (key == "NoDisplay") no_display = value == "true";
				else if (key == "Hidden") hidden = value == "true";
				else if (key == "Terminal") terminal = value == "true";
				else if (key == "OnlyShowIn") only_show_in = value;
			}
			if (type != "Application" || hidden) {
				continue;
			}
			bool visible = !no_display && !terminal && !exec.empty();
			if (!only_show_in.empty()) {
				auto desktops = split(only_show_in, ';');
				if (std::find(desktops.begin(), desktops.end(), "Dusk") == desktops.end()) {
					visible = false;
				}
			}
			if (!name_full.empty()) name = name_full;
			else if (!name_short.empty()) name = name_short;
			if (name.empty()) {
				continue;
			}
			apps_.push_back(AppInfo{id, name, clean_exec(exec), icon, wm_class, comment, visible});
		}
	}
	std::sort(apps_.begin(), apps_.end(), [](const AppInfo &a, const AppInfo &b) {
		return lower(a.name) < lower(b.name);
	});
}

std::string AppDatabase::icon_for(const std::string &app_id) {
	if (app_id.empty()) {
		return "";
	}
	auto cached = icon_cache_.find(app_id);
	if (cached != icon_cache_.end()) {
		return cached->second;
	}
	const std::string want = lower(app_id);
	const AppInfo *match = nullptr;
	// Best: desktop file named after the app id; then WM class; then a
	// desktop file whose name ends with it (e.g. "org.mozilla.firefox" for
	// "firefox"); then the program name.
	for (const AppInfo &a : apps_) {
		if (lower(a.id) == want + ".desktop") { match = &a; break; }
	}
	if (!match) {
		for (const AppInfo &a : apps_) {
			if (!a.wm_class.empty() && lower(a.wm_class) == want) { match = &a; break; }
		}
	}
	if (!match) {
		for (const AppInfo &a : apps_) {
			const std::string id = lower(a.id);
			if (id.size() > want.size() + 8 &&
					id.compare(id.size() - want.size() - 8, want.size() + 8, "." + want + ".desktop") == 0) {
				match = &a;
				break;
			}
		}
	}
	if (!match) {
		for (const AppInfo &a : apps_) {
			if (lower(basename_of(a.exec)) == want) { match = &a; break; }
		}
	}
	std::string icon = match && !match->icon.empty() ? match->icon : app_id;
	icon_cache_[app_id] = icon;
	id_found_[app_id] = match != nullptr;
	return icon;
}

namespace {
// Letters and digits only, lower case: "Rocket League®" and
// "RocketLeague.exe" both become "rocketleague".
std::string squash(const std::string &s) {
	std::string out;
	for (char c : s) {
		if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}
} // namespace

std::string AppDatabase::icon_for(const std::string &app_id, const std::string &title) {
	const std::string by_id = icon_for(app_id);
	if (id_found_[app_id] || (app_id.empty() && title.empty())) return by_id;
	const std::string key = app_id + '\x1f' + title;
	if (auto it = title_cache_.find(key); it != title_cache_.end()) return it->second;
	if (title_cache_.size() > 400) title_cache_.clear();
	std::string icon = by_id;
	std::string id = lower(app_id);
	// Steam games: steam_app_<number>; Steam puts "steam_icon_<number>" in
	// the icon theme, and its shortcuts run steam://rungameid/<number>.
	if (id.rfind("steam_app_", 0) == 0) {
		const std::string num = id.substr(10);
		icon = "steam_icon_" + num;
		for (const AppInfo &a : apps_) {
			if (a.exec.find("rungameid/" + num) != std::string::npos && !a.icon.empty()) {
				icon = a.icon;
				break;
			}
		}
		title_cache_[key] = icon;
		return icon;
	}
	if (id.size() > 4 && id.compare(id.size() - 4, 4, ".exe") == 0) id.resize(id.size() - 4);
	const std::string sid = squash(id), stitle = squash(title);
	const AppInfo *best = nullptr;
	for (const AppInfo &a : apps_) {
		const std::string sname = squash(a.name);
		if (sname.size() < 3 || a.icon.empty()) continue;
		if (sname == stitle || sname == sid) {
			best = &a;
			break;
		}
		// "Rocket League" in "Rocket League (64-bit, DX11)"
		if (!best && sname.size() >= 5 && stitle.rfind(sname, 0) == 0) best = &a;
	}
	if (best) icon = best->icon;
	title_cache_[key] = icon;
	return icon;
}

// ---- icon theme -----------------------------------------------------------

IconLoader::~IconLoader() {
	clear();
}

void IconLoader::clear() {
	for (auto &[key, surface] : cache_) {
		if (surface) {
			cairo_surface_destroy(surface);
		}
	}
	cache_.clear();
	dirs_.clear();   // icons installed meanwhile are found too
}

void IconLoader::set_theme(const std::string &theme) {
	if (!theme.empty() && theme != theme_) {
		theme_ = theme;
		clear();
	}
}

namespace {

// "48x48" -> 48, "48x48@2" -> 96, "32" -> 32, "scalable"/"symbolic" -> 0, else -1.
int dir_size(const std::string &d) {
	if (d == "scalable" || d == "symbolic") return 0;
	if (d.empty() || !std::isdigit(static_cast<unsigned char>(d[0]))) return -1;
	int n = 0, scale = 1;
	size_t at = 0;
	while (at < d.size() && std::isdigit(static_cast<unsigned char>(d[at]))) n = n * 10 + (d[at++] - '0');
	if (at < d.size() && d[at] == 'x') {
		at++;
		while (at < d.size() && std::isdigit(static_cast<unsigned char>(d[at]))) at++;
	}
	if (at < d.size() && d[at] == '@') scale = std::max(1, std::atoi(d.c_str() + at + 1));
	return n * scale;
}

} // namespace

// Every icon folder of a theme with the size of its icons (0 = scalable).
// Both layouts: size/context (hicolor, Adwaita) and context/size (Papirus, Breeze).
const std::vector<std::pair<std::string, int>> &IconLoader::theme_dirs(const std::string &root) const {
	auto it = dirs_.find(root);
	if (it != dirs_.end()) return it->second;
	std::vector<std::pair<std::string, int>> out;
	std::error_code ec;
	for (const auto &d1 : fs::directory_iterator(root, ec)) {
		if (!d1.is_directory(ec)) continue;
		const std::string n1 = d1.path().filename().string();
		const int s1 = dir_size(n1);
		for (const auto &d2 : fs::directory_iterator(d1.path(), ec)) {
			if (!d2.is_directory(ec)) continue;
			const int s2 = s1 >= 0 ? s1 : dir_size(d2.path().filename().string());
			if (s2 >= 0) out.emplace_back(d2.path().string(), s2);
		}
	}
	return dirs_[root] = std::move(out);
}

// The sharpest file for an icon drawn `px` pixels wide: a scalable one if
// the theme has it, else the smallest picture at least that big (else the
// biggest there is). Picking the first size found used to blow small
// pictures up (a blurry distribution logo in the menu button).
std::string IconLoader::find_file(const std::string &name, bool symbolic, int px) const {
	if (name.empty()) {
		return "";
	}
	std::error_code ec;
	if (name[0] == '/') {
		return fs::exists(name, ec) ? name : "";
	}

	std::vector<std::string> bases;
	if (const char *home = getenv("HOME")) {
		bases.push_back(std::string(home) + "/.local/share/icons");
		bases.push_back(std::string(home) + "/.icons");
	}
	for (const std::string &d : data_dirs()) {
		bases.push_back(d + "/icons");
	}
	for (const std::string &theme : {theme_, std::string("Adwaita"), std::string("hicolor")}) {
		std::string best_png;
		int best_size = -1;
		for (const std::string &base : bases) {
			const std::string root = base + "/" + theme;
			if (!fs::is_directory(root, ec)) {
				continue;
			}
			for (const auto &[dir, size] : theme_dirs(root)) {
				// Symbolic icons: the small, single-colour ones.
				if (symbolic && size > 64) continue;
				if (fs::exists(dir + "/" + name + ".svg", ec)) {
					return dir + "/" + name + ".svg";
				}
				const std::string png = dir + "/" + name + ".png";
				if (!fs::exists(png, ec)) continue;
				const bool better = best_size < 0 || (size >= px && (best_size < px || size < best_size)) ||
					(size < px && best_size < px && size > best_size);
				if (better) {
					best_png = png;
					best_size = size;
				}
			}
		}
		if (!best_png.empty()) {
			return best_png;
		}
	}
	if (!symbolic) {
		for (const char *ext : {".svg", ".png"}) {
			const std::string p = std::string("/usr/share/pixmaps/") + name + ext;
			if (fs::exists(p, ec)) {
				return p;
			}
		}
	}
	return "";
}

cairo_surface_t *IconLoader::get_symbolic(const std::string &name, int px) {
	const std::string key = "sym:" + name + "@" + std::to_string(px);
	auto it = cache_.find(key);
	if (it != cache_.end()) {
		return it->second;
	}
	cairo_surface_t *surface = nullptr;
	const std::string file = find_file(name, true, px);
	if (!file.empty()) {
		surface = load_image_sized(file, px, px);
	}
	cache_[key] = surface;
	return surface;
}

cairo_surface_t *IconLoader::get(const std::string &name, int px) {
	const std::string key = name + "@" + std::to_string(px);
	auto it = cache_.find(key);
	if (it != cache_.end()) {
		return it->second;
	}
	cairo_surface_t *surface = nullptr;
	const std::string file = find_file(name, false, px);
	if (!file.empty()) {
		surface = load_image_sized(file, px, px);
	}
	cache_[key] = surface;   // remember misses too
	return surface;
}

void draw_icon(cairo_t *cr, IconLoader &icons, const std::string &icon_name,
		const std::string &label, double x, double y, double size, float scale) {
	const int px = static_cast<int>(size * (scale > 0 ? scale : 1.0f) + 0.5);
	if (cairo_surface_t *icon = icons.get(icon_name, px)) {
		const double iw = cairo_image_surface_get_width(icon);
		const double ih = cairo_image_surface_get_height(icon);
		const double s = size / std::max(iw, ih);
		cairo_save(cr);
		cairo_translate(cr, x + (size - iw * s) / 2, y + (size - ih * s) / 2);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, icon, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		return;
	}
	draw_app_badge(cr, x + size / 2, y + size / 2, size / 2 - 1,
		icon_name.empty() ? label : icon_name, label);
}

bool draw_symbolic(cairo_t *cr, IconLoader &icons, std::initializer_list<const char *> names,
		double x, double y, double size, const Color &color, float scale) {
	const int px = static_cast<int>(size * (scale > 0 ? scale : 1.0f) + 0.5);
	for (const char *name : names) {
		cairo_surface_t *icon = icons.get_symbolic(name, px);
		if (!icon) {
			continue;
		}
		const double iw = cairo_image_surface_get_width(icon);
		const double ih = cairo_image_surface_get_height(icon);
		const double s = size / std::max(iw, ih);
		cairo_save(cr);
		set_color(cr, color);
		cairo_translate(cr, x + (size - iw * s) / 2, y + (size - ih * s) / 2);
		cairo_scale(cr, s, s);
		// The icon's shape, filled with our colour (symbolic icons are meant
		// to be recoloured like this).
		cairo_mask_surface(cr, icon, 0, 0);
		cairo_restore(cr);
		return true;
	}
	return false;
}

} // namespace dusk
