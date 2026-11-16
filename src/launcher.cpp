#include "launcher.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <unistd.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "desktop.hpp"
#include "draw.hpp"
#include "glyphs.hpp"
#include "overview.hpp"
#include "panel.hpp"
#include "popover.hpp"
#include "system.hpp"

namespace dusk {

namespace {

constexpr int kWidth = 440;
constexpr int kHeight = 580;
constexpr int kPad = 16;
constexpr int kSearchHeight = 42;
constexpr int kListTop = kPad + kSearchHeight + 14;
constexpr int kRowHeight = 44;
constexpr int kFooterHeight = 62;
constexpr int kFavH = 84;           // (the other looks' favourites row)
constexpr int kPinH = 78;           // Dusk's pinned apps: one row of icons with names
constexpr int kMaxFiles = 12;

// Settings pages the search finds, by the words you'd use for them.
struct SettingsPage {
	const char *page, *title, *words;
};
const SettingsPage kSettingsPages[] = {
	{"appearance", "Appearance", "theme dark light accent colour color wallpaper background font look style corners"},
	{"panels", "Panels & dock", "panel taskbar dock bar top bottom widgets clock tray"},
	{"controlcentre", "Control centre", "control centre center quick settings tiles"},
	{"desktop", "Desktop & clipboard", "desktop icons clipboard history"},
	{"lockscreen", "Lock screen", "lock screen password idle"},
	{"animations", "Animations", "animations motion effects speed"},
	{"displays", "Displays", "display monitor screen resolution scale refresh rate hdmi tv night light"},
	{"power", "Power & battery", "power battery sleep suspend hibernate performance saver lid"},
	{"notifications", "Notifications", "notifications do not disturb dnd alerts"},
	{"osd", "On-screen display", "osd volume popup on-screen"},
	{"datetime", "Date & time", "date time clock timezone calendar 24-hour"},
	{"language", "Language & region", "language region locale input format"},
	{"windows", "Windows", "windows title bar snapping tiling focus"},
	{"workspaces", "Workspaces", "workspaces virtual desktops overview"},
	{"keyboard", "Keyboard", "keyboard shortcuts layout keys typing repeat"},
	{"pointer", "Mouse & touchpad", "mouse touchpad pointer cursor scrolling tap speed"},
	{"media", "Media & lyrics", "media music lyrics player sound output speakers"},
	{"screenshots", "Screenshots & recording", "screenshot screen recording capture"},
	{"apps", "Default apps", "default apps browser terminal file manager open with"},
	{"startup", "Startup apps", "startup autostart login apps"},
	{"about", "About & updates", "about update version system computer"},
};

// A sum typed into the search ("12*8", "(3+4)/2", "2^10", "15% of 80"
// isn't one): its answer, or nothing.
struct Sum {
	const std::string &t;
	size_t i = 0;
	bool bad = false;
	void ws() { while (i < t.size() && t[i] == ' ') i++; }
	bool eat(const char *op) {
		ws();
		const size_t n = std::strlen(op);
		if (t.compare(i, n, op) == 0) { i += n; return true; }
		return false;
	}
	double atom() {
		ws();
		if (eat("(")) {
			const double v = expr();
			if (!eat(")")) bad = true;
			return v;
		}
		if (eat("-")) return -atom();
		const size_t start = i;
		while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.' || t[i] == ',')) i++;
		if (i == start) { bad = true; return 0; }
		std::string num = t.substr(start, i - start);
		num.erase(std::remove(num.begin(), num.end(), ','), num.end());
		double v = std::atof(num.c_str());
		if (eat("%")) v /= 100;
		return v;
	}
	double power() {
		const double b = atom();
		if (eat("^") || eat("**")) return std::pow(b, power());
		return b;
	}
	double term() {
		double v = power();
		for (;;) {
			if (eat("*") || eat("x") || eat("\xc3\x97")) v *= power();
			else if (eat("/") || eat("\xc3\xb7")) v /= power();
			else return v;
		}
	}
	double expr() {
		double v = term();
		for (;;) {
			if (eat("+")) v += term();
			else if (eat("-")) v -= term();
			else return v;
		}
	}
};

std::string sum_answer(const std::string &q) {
	bool digit = false, op = false;
	for (size_t k = 0; k < q.size(); k++) {
		const char c = q[k];
		if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
		else if (std::strchr("+-*/^x%()., ", c) || static_cast<unsigned char>(c) >= 0x80) op = op || std::strchr("+-*/^x", c) || static_cast<unsigned char>(c) >= 0x80;
		else return "";
	}
	if (!digit || !op) return "";
	Sum p{q};
	const double v = p.expr();
	p.ws();
	if (p.bad || p.i != q.size() || !std::isfinite(v)) return "";
	char buf[64];
	if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", v);
	else std::snprintf(buf, sizeof buf, "%.10g", v);
	return buf;
}

// "%2F" and friends back to characters (file:// addresses).
std::string url_decode(const std::string &u) {
	std::string out;
	for (size_t k = 0; k < u.size(); k++) {
		if (u[k] == '%' && k + 2 < u.size() && std::isxdigit(static_cast<unsigned char>(u[k + 1])) &&
				std::isxdigit(static_cast<unsigned char>(u[k + 2]))) {
			out += static_cast<char>(std::stoi(u.substr(k + 1, 2), nullptr, 16));
			k += 2;
		} else {
			out += u[k];
		}
	}
	return out;
}

// "2026-11-07T21:04:05.123Z" as "4 hours ago".
std::string ago(const std::string &iso) {
	tm t{};
	if (!strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &t)) return "";
	const long d = static_cast<long>(time(nullptr) - timegm(&t));
	if (d < 90) return "just now";
	if (d < 3600) return std::to_string(d / 60) + " min ago";
	if (d < 86400) return std::to_string(d / 3600) + (d / 3600 == 1 ? " hour ago" : " hours ago");
	if (d < 2 * 86400) return "yesterday";
	return std::to_string(d / 86400) + " days ago";
}

std::string up_for() {
	std::ifstream f("/proc/uptime");
	double sec = 0;
	f >> sec;
	const long m = static_cast<long>(sec) / 60, h = m / 60, d = h / 24;
	if (d > 0) return std::to_string(d) + "d " + std::to_string(h % 24) + "h";
	if (h > 0) return std::to_string(h) + " h " + std::to_string(m % 60) + " min";
	return std::to_string(m) + " min";
}

// An icon for a file, by what it is.
std::string file_icon(const std::string &name, bool dir) {
	if (dir) return "folder";
	std::string ext = name.substr(name.find_last_of('.') == std::string::npos ? name.size() : name.find_last_of('.') + 1);
	for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	static const std::pair<const char *, const char *> kKinds[] = {
		{"png", "image-x-generic"}, {"jpg", "image-x-generic"}, {"jpeg", "image-x-generic"}, {"webp", "image-x-generic"},
		{"gif", "image-x-generic"}, {"svg", "image-x-generic"}, {"mp4", "video-x-generic"}, {"mkv", "video-x-generic"},
		{"webm", "video-x-generic"}, {"mov", "video-x-generic"}, {"mp3", "audio-x-generic"}, {"flac", "audio-x-generic"},
		{"ogg", "audio-x-generic"}, {"wav", "audio-x-generic"}, {"m4a", "audio-x-generic"}, {"pdf", "application-pdf"},
		{"zip", "package-x-generic"}, {"gz", "package-x-generic"}, {"xz", "package-x-generic"}, {"7z", "package-x-generic"},
		{"rar", "package-x-generic"}, {"doc", "x-office-document"}, {"docx", "x-office-document"},
		{"odt", "x-office-document"}, {"xls", "x-office-spreadsheet"}, {"xlsx", "x-office-spreadsheet"},
		{"ods", "x-office-spreadsheet"}, {"ppt", "x-office-presentation"}, {"pptx", "x-office-presentation"},
		{"odp", "x-office-presentation"}, {"sh", "text-x-script"}, {"py", "text-x-script"},
	};
	for (const auto &[e, icon] : kKinds) {
		if (ext == e) return icon;
	}
	return "text-x-generic";
}

std::string join(const std::vector<std::string> &v) {
	std::string out;
	for (const std::string &x : v) out += (out.empty() ? "" : ",") + x;
	return out;
}

std::string lower(std::string s) {
	for (char &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

} // namespace

Launcher::Launcher(Server *server)
		: server_(server), layer_(server->layer_menu, &tag),
		fx_(server->anim.get(), [this] { if (open_ || alpha_ > 0) redraw(); }),
		sel_(layer_, server->anim.get()), hl_(layer_, server->anim.get()) {
	layer_.set_enabled(false);
	fx_.set_speed(55);
	search_timer_ = wl_event_loop_add_timer(server->loop, &Launcher::handle_search_timer, this);
	if (const passwd *pw = getpwuid(getuid())) {
		std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
		gecos = gecos.substr(0, gecos.find(','));
		user_name_ = gecos.empty() ? pw->pw_name : gecos;
		if (!user_name_.empty()) user_name_[0] = static_cast<char>(std::toupper(user_name_[0]));
	}
}

// ---- The looks' measurements ----------------------------------------------------------------

void Launcher::measure() {
	look_ = server_->config.launcher_look;
	if (!output_) return;
	const wlr_box u = output_->usable();
	const bool favs = !favs_.empty();
	(void)favs;
	if (look_ == "dusk") {
		w_ = kWidth;
		// As tall as the home's contents (no empty half), the same while you
		// search, so the menu doesn't jump as you type.
		if (query_.empty()) pinned_ = static_cast<int>(favs_.size());   // (kept while searching)
		const int rows = std::min(2, (pinned_ + 5) / 6);
		const wlr_box pins{kPad, kListTop, w_ - 2 * kPad, 28 + (pinned_ > 0 ? rows * kPinH : 40)};
		const int content = pins.y + pins.height + 10 + places_height() + 26 +
			(recent_.empty() ? 30 : static_cast<int>(recent_.size()) * 52) + 12 + kFooterHeight;
		h_ = std::clamp(content, 420, std::min(kHeight, u.height - 16));
		if (home()) {
			favs_area_ = pins;
			list_top_ = h_;
		} else {
			favs_area_ = wlr_box{};
			list_top_ = kListTop + (query_.empty() ? 36 : 0) + (calc_.empty() ? 0 : 70);
		}
		return;
	}
	if (look_ == "windows") {
		// Start: search on top, the apps six to a row, you and the power button along the bottom.
		w_ = std::min(640, u.width - 16);
		h_ = std::min(720, u.height - 16);
		const int pad = 28;
		search_ = wlr_box{pad, 24, w_ - 2 * pad, 38};
		cols_ = std::max(3, std::min(6, (w_ - 2 * pad + 8) / 96));
		cell_h_ = 88;
		icon_ = 34;
		// "Pinned" (the favourites) over "All apps", like Windows 11's Start.
		const int top = 24 + 38 + 16;
		favs_area_ = favs ? wlr_box{pad - 4, top, w_ - 2 * pad + 8, 30 + cell_h_} : wlr_box{};
		grid_ = wlr_box{pad - 4, top + (favs ? favs_area_.height + 8 : 0) + 36, w_ - 2 * pad + 8, 0};
		grid_.height = h_ - 64 - 12 - grid_.y;
		cell_w_ = grid_.width / cols_;
	} else {
		// GNOME's app grid / macOS's Launchpad: the whole screen (above and
		// below the panels), a search field at the top, big icons.
		// It covers the whole screen, under the panels (they stay sharp on
		// top), so its blur has no edge anywhere you can see: a blur ending
		// at the panel pulled the panel's colour in, a dark band (a halo).
		// Its contents keep to the space between the panels.
		const bool mac = look_ == "macos";
		const wlr_box o = output_->box();
		w_ = o.width;
		h_ = o.height;
		const int ux = u.x - o.x, uy = u.y - o.y;
		const int sw = mac ? 260 : 340, sh = mac ? 30 : 40;
		search_ = wlr_box{ux + (u.width - sw) / 2, uy + (mac ? 36 : 28), sw, sh};
		const int side = std::max(40, static_cast<int>(u.width * (mac ? 0.13 : 0.08)));
		cell_w_ = mac ? 150 : 140;
		cell_h_ = mac ? 140 : 128;
		icon_ = mac ? 76 : 64;
		cols_ = std::clamp((u.width - 2 * side) / cell_w_, 3, mac ? 7 : 8);
		const int gw = cols_ * cell_w_;
		const int gx = ux + (u.width - gw) / 2;
		// The favourites: a row of their own above the rest (like a dash), a line under it.
		const int top = search_.y + sh + (mac ? 40 : 32);
		favs_area_ = favs ? wlr_box{gx, top, gw, cell_h_ + 16} : wlr_box{};
		grid_ = wlr_box{gx, top + (favs ? favs_area_.height + 16 : 12), gw, 0};
		grid_.height = uy + u.height - grid_.y - 40;
		dots_y_ = uy + u.height - 22;
	}
	grid_rows_ = std::max(1, grid_.height / cell_h_);
}

int Launcher::max_first() const {
	if (!grid()) return std::max(0, count() - visible_rows());
	const int rows = (count() + cols_ - 1) / cols_;
	return std::max(0, rows - grid_rows_);
}

wlr_box Launcher::item_box(int index, double scroll) const {
	const int row = index / cols_, col = index % cols_;
	return wlr_box{grid_.x + col * cell_w_, static_cast<int>(std::lround(grid_.y + row * cell_h_ - scroll)), cell_w_,
		cell_h_};
}

int Launcher::item_at(double px, double py) const {
	if (px < grid_.x || px >= grid_.x + cols_ * cell_w_ || py < grid_.y || py >= grid_.y + grid_rows_ * cell_h_) {
		return -1;
	}
	const double scroll = const_cast<Fades &>(fx_).get("scroll", first_row_ * cell_h_);
	const int row = static_cast<int>(std::floor((py - grid_.y + scroll) / cell_h_));
	const int col = static_cast<int>((px - grid_.x) / cell_w_);
	const int i = row * cols_ + col;
	return i >= 0 && i < count() ? i : -1;
}

// ---- What's listed ---------------------------------------------------------------------

std::string Launcher::item_name(int i) const {
	if (is_file(i)) return file_of(i).name;
	if (is_setting(i)) return kSettingsPages[setting_of(i)].title;
	return server_->apps.apps()[static_cast<size_t>(app_of(i))].name;
}

std::string Launcher::item_icon(int i) const {
	if (is_file(i)) {
		const FileHit &f = file_of(i);
		return file_icon(f.name, f.dir);
	}
	if (is_setting(i)) return "preferences-system";
	return server_->apps.apps()[static_cast<size_t>(app_of(i))].icon;
}

// ---- Favourites ---------------------------------------------------------------------

wlr_box Launcher::fav_box(int k) const {
	const int n = static_cast<int>(favs_.size());
	if (look_ == "dusk") {
		const int cw = favs_area_.width / 6;
		return wlr_box{favs_area_.x + (k % 6) * cw, favs_area_.y + 28 + (k / 6) * kPinH, cw, kPinH - 4};
	}
	const int shown = std::min(n, cols_);
	// GNOME / macOS: centred like a dock; Windows: from the left like its Pinned row.
	const int x0 = look_ == "windows" ? favs_area_.x : favs_area_.x + (favs_area_.width - shown * cell_w_) / 2;
	const int y0 = look_ == "windows" ? favs_area_.y + 30 : favs_area_.y + 4;
	return wlr_box{x0 + k * cell_w_, y0, cell_w_, cell_h_};
}

int Launcher::fav_at(double px, double py) const {
	if (favs_.empty() || !query_.empty() || (look_ == "dusk" && !home())) return -1;
	const int shown = std::min(static_cast<int>(favs_.size()), look_ == "dusk" ? 12 : cols_);
	for (int k = 0; k < shown; k++) {
		const wlr_box b = fav_box(k);
		if (px >= b.x && px < b.x + b.width && py >= b.y && py < b.y + b.height) return k;
	}
	return -1;
}

void Launcher::draw_favs(cairo_t *cr, float scale, const Color &fg) {
	if (look_ == "dusk" || favs_.empty() || favs_area_.width <= 0) return;   // (Dusk's: draw_home)
	const auto &apps = server_->apps.apps();
	const bool dusk = false, win = look_ == "windows";
	if (win) {
		draw_text(cr, "Pinned", favs_area_.x + 8, favs_area_.y, 200, 24, theme::font_bold, theme::text);
	}
	const int shown = std::min(static_cast<int>(favs_.size()), dusk ? 6 : cols_);
	for (int k = 0; k < shown; k++) {
		const AppInfo &app = apps[static_cast<size_t>(favs_[static_cast<size_t>(k)])];
		const wlr_box b = fav_box(k);
		const double is = dusk ? 32 : icon_;
		const double iy = b.y + (dusk ? 4 : win ? 12 : 10);
		draw_icon(cr, server_->icons, app.icon, app.name, b.x + (b.width - is) / 2.0, iy, is, scale);
		draw_text(cr, app.name, b.x + 4, iy + is + (dusk ? 4 : 6), b.width - 8, 18,
			dusk || win ? theme::font_small : theme::font, fg, Align::Center);
	}
	if (!dusk && !win) {
		// GNOME / macOS: a faint line between the favourites and the rest.
		cairo_rectangle(cr, favs_area_.x + 40, favs_area_.y + favs_area_.height + 7, favs_area_.width - 80, 1);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
		cairo_fill(cr);
	}
}

void Launcher::toggle_favourite(const std::string &id) {
	std::vector<std::string> favs = server_->config.launcher_favorites;
	if (favs.empty()) {
		// (The everyday ones shown until now become yours to change.)
		for (int i : favs_) favs.push_back(server_->apps.apps()[static_cast<size_t>(i)].id);
	}
	auto it = std::find(favs.begin(), favs.end(), id);
	if (it != favs.end()) favs.erase(it);
	else favs.push_back(id);
	server_->write_setting("launcher_favorites", join(favs));
	filter();
	redraw();
}

// An app's .desktop file (to put a copy of it on the desktop). Ids with a
// "-" may live in a subfolder ("kde-org.foo.desktop" = kde/org.foo.desktop).
static std::string desktop_file_of(const std::string &id) {
	namespace fs = std::filesystem;
	const char *home = std::getenv("HOME");
	std::vector<std::string> dirs;
	if (const char *d = std::getenv("XDG_DATA_HOME"); d && *d) dirs.push_back(std::string(d) + "/applications");
	if (home) {
		dirs.push_back(std::string(home) + "/.local/share/applications");
		dirs.push_back(std::string(home) + "/.local/share/flatpak/exports/share/applications");
	}
	std::string sys = std::getenv("XDG_DATA_DIRS") ? std::getenv("XDG_DATA_DIRS") : "/usr/local/share:/usr/share";
	std::stringstream ss(sys);
	for (std::string d; std::getline(ss, d, ':');) if (!d.empty()) dirs.push_back(d + "/applications");
	dirs.push_back("/var/lib/flatpak/exports/share/applications");
	for (const std::string &d : dirs) {
		std::error_code ec;
		if (fs::exists(d + "/" + id, ec)) return d + "/" + id;
		std::string sub = id;
		for (size_t i = 0; i < sub.size(); i++) {
			if (sub[i] != '-') continue;
			sub[i] = '/';
			if (fs::exists(d + "/" + sub, ec)) return d + "/" + sub;
		}
	}
	return {};
}

// Right-click on an app (or a favourite, or a file): its menu.
void Launcher::item_menu(int item, int fav, double lx, double ly) {
	std::vector<MenuItem> items;
	Server *srv = server_;
	const wlr_box at{static_cast<int>(lx), static_cast<int>(ly), 1, 1};
	if (item >= 0 && is_setting(item)) return;
	if (item >= 0 && is_file(item)) {
		const FileHit f = file_of(item);
		items.push_back(MenuItem::header(f.name));
		items.push_back(MenuItem::action("Open", [this, f] {
			server_->spawn("xdg-open " + shell_quote(f.path));
			close();
		}, f.dir ? "folder" : "file"));
		items.push_back(MenuItem::action("Show in its folder", [this, f] {
			server_->spawn("xdg-open " + shell_quote(std::filesystem::path(f.path).parent_path().string()));
			close();
		}, "folder"));
		items.push_back(MenuItem::action("Copy where it is", [srv, f] {
			srv->spawn("printf %s " + shell_quote(f.path) + " | wl-copy");
		}, "copy"));
		if (srv->desktop && f.path.rfind(srv->desktop->folder() + "/", 0) != 0) {
			// A link to it on the desktop (the file stays where it is).
			items.push_back(MenuItem::action("Add to desktop", [srv, f] {
				const std::string dir = srv->desktop->folder();
				srv->spawn("mkdir -p " + shell_quote(dir) + " && ln -s " + shell_quote(f.path) + " " +
					shell_quote(dir + "/" + f.name) + " 2>/dev/null");
			}, "monitor"));
		}
		srv->menu->show(output_, at, "point", std::move(items));
		return;
	}
	const int app_index = fav >= 0 ? favs_[static_cast<size_t>(fav)]
		: item >= 0 ? app_of(item) : -1;
	if (app_index < 0) return;
	const AppInfo app = server_->apps.apps()[static_cast<size_t>(app_index)];
	const auto &favs = server_->config.launcher_favorites;
	const bool is_fav = std::find(favs.begin(), favs.end(), app.id) != favs.end();
	items.push_back(MenuItem::header(app.name));
	items.push_back(MenuItem::action("Open", [this, app] {
		if (app.id.rfind("org.dusk.Settings", 0) == 0) server_->open_settings("");
		else server_->spawn(app.exec);
		close();
	}, "play"));
	items.push_back(MenuItem::action(is_fav ? "Remove from favourites" : "Add to favourites",
		[this, id = app.id] { toggle_favourite(id); }, is_fav ? "star-off" : "star"));
	// On the desktop: a copy of its launcher in the Desktop folder (as GNOME does).
	if (srv->desktop) {
		const std::string on_desktop = srv->desktop->folder() + "/" + app.id;
		std::error_code ec;
		if (std::filesystem::exists(on_desktop, ec)) {
			items.push_back(MenuItem::action("Remove from desktop", [srv, on_desktop] {
				srv->spawn("rm -f " + shell_quote(on_desktop));
			}, "minus"));
		} else if (const std::string src = desktop_file_of(app.id); !src.empty()) {
			items.push_back(MenuItem::action("Add to desktop", [srv, src, on_desktop] {
				const std::string dir = on_desktop.substr(0, on_desktop.rfind('/'));
				srv->spawn("mkdir -p " + shell_quote(dir) + " && cp " + shell_quote(src) + " " + shell_quote(on_desktop) +
					" && chmod +x " + shell_quote(on_desktop) +
					" && gio set " + shell_quote(on_desktop) + " metadata::trusted true 2>/dev/null");
			}, "monitor"));
		}
	}
	// The panel with app buttons: pin it there too.
	for (const PanelConfig &pc : server_->config.panels) {
		const auto &w = pc.widgets;
		if (std::find(w.begin(), w.end(), "tasks") == w.end() && std::find(w.begin(), w.end(), "pins") == w.end()) continue;
		std::vector<std::string> pins = pc.pins;
		const auto it = std::find(pins.begin(), pins.end(), app.id);
		const bool pinned = it != pins.end();
		if (pinned) pins.erase(it);
		else pins.push_back(app.id);
		items.push_back(MenuItem::action(pinned ? "Unpin from panel" : "Pin to panel",
			[srv, key = "panel." + pc.id + ".pins", list = join(pins)] { srv->write_setting(key, list); }, "pin"));
		break;
	}
	srv->menu->show(output_, at, "point", std::move(items));
}

// ---- Files ----------------------------------------------------------------------------

int Launcher::handle_search_timer(void *data) {
	static_cast<Launcher *>(data)->search_files();
	return 0;
}

// Files and folders in your home whose name has what you typed: from the
// file index (plocate / locate: everything, fast, as of its last update)
// and a quick look through your folders (new files), together.
void Launcher::search_files() {
	if (!open_ || query_.size() < 2) return;
	const unsigned gen = ++search_gen_;
	const std::string q = query_;
	std::string glob;
	for (char c : q) {
		if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\') glob += '\\';
		glob += c;
	}
	const std::string cmd =
		"{ if command -v plocate >/dev/null; then timeout 2 plocate -i -l 400 -- " + shell_quote(q) + "; "
		"elif command -v locate >/dev/null; then timeout 2 locate -i -l 400 -- " + shell_quote(q) + "; fi; "
		"timeout 1.5 find \"$HOME\" -maxdepth 5 -name '.*' -prune -o -iname " + shell_quote("*" + glob + "*") +
		" -print 2>/dev/null | head -n 200; } 2>/dev/null";
	server_->run_capture(cmd, [this, gen, q](const std::string &out) {
		if (gen != search_gen_ || q != query_ || !open_) return;
		const char *h = std::getenv("HOME");
		const std::string home = h ? h : "";
		const std::string lq = lower(q);
		struct Cand {
			std::string path, name;
			int rank;
		};
		std::vector<Cand> cands;
		std::istringstream in(out);
		std::string line;
		std::vector<std::string> seen;
		while (std::getline(in, line)) {
			if (line.empty() || line == home) continue;
			// Your own files only, not hidden ones (dot folders: settings, caches).
			if (!home.empty() && line.rfind(home + "/", 0) != 0) continue;
			if (line.find("/.", home.size()) != std::string::npos) continue;
			const std::string name = line.substr(line.find_last_of('/') + 1);
			const std::string ln = lower(name);
			const size_t at = ln.find(lq);
			if (at == std::string::npos) continue;   // the name itself, not just a folder on the way
			if (std::find(seen.begin(), seen.end(), line) != seen.end()) continue;
			seen.push_back(line);
			// Best first: names that start with it, then shallow ones.
			const int depth = static_cast<int>(std::count(line.begin(), line.end(), '/'));
			cands.push_back(Cand{line, name, (at == 0 ? 0 : 100) + depth});
		}
		std::stable_sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.rank < b.rank; });
		files_.clear();
		std::error_code ec;
		for (const Cand &c : cands) {
			if (static_cast<int>(files_.size()) >= kMaxFiles) break;
			if (!std::filesystem::exists(c.path, ec)) continue;   // (the index can be out of date)
			files_.push_back(FileHit{c.path, c.name, std::filesystem::is_directory(c.path, ec)});
		}
		keep_selection_visible();
		redraw();
	});
}

wlr_box Launcher::power_box() const {
	if (!server_->config.menu_power || full() || look_ == "dusk") return wlr_box{};   // (Dusk's: hots_)
	if (look_ == "windows") return wlr_box{w_ - 28 - 40, h_ - 64 + 12, 40, 40};
	return wlr_box{w_ - kPad - 36, h_ - kFooterHeight + 8, 36, 36};
}

wlr_box Launcher::settings_box() const {
	if (full() || look_ == "dusk") return wlr_box{};
	if (look_ == "windows") return wlr_box{w_ - 28 - 40 - 8 - 40, h_ - 64 + 12, 40, 40};
	return wlr_box{kPad, h_ - kFooterHeight + 8, 130, 36};
}

Launcher::~Launcher() {
	if (redraw_idle_) wl_event_source_remove(redraw_idle_);
	if (full_tree_) {
		if (layer_.tree->node.parent == full_tree_) wlr_scene_node_reparent(&layer_.tree->node, server_->layer_menu);
		wlr_scene_node_destroy(&full_tree_->node);
	}
	if (search_timer_) wl_event_source_remove(search_timer_);
	if (anim_) {
		server_->anim->cancel(anim_);
	}
}

void Launcher::filter() {
	const auto &apps = server_->apps.apps();
	shown_.clear();
	const std::string q = lower(query_);
	std::vector<int> starts, contains;
	for (int i = 0; i < static_cast<int>(apps.size()); i++) {
		const AppInfo &app = apps[static_cast<size_t>(i)];
		if (!app.visible) {
			continue;
		}
		const std::string name = lower(app.name);
		if (q.empty() || name.rfind(q, 0) == 0) {
			starts.push_back(i);
		} else if (name.find(q) != std::string::npos ||
				lower(app.comment).find(q) != std::string::npos ||
				lower(app.exec).find(q) != std::string::npos) {
			contains.push_back(i);
		}
	}
	if (q.empty() && server_->config.launcher_look == "dusk") {
		// All apps: A to Z.
		std::stable_sort(starts.begin(), starts.end(), [&apps](int a, int b) {
			return lower(apps[static_cast<size_t>(a)].name) < lower(apps[static_cast<size_t>(b)].name);
		});
	}
	shown_ = starts;
	shown_.insert(shown_.end(), contains.begin(), contains.end());
	// Favourites, in their order (while not searching; gone when there are none).
	favs_.clear();
	if (q.empty()) {
		std::vector<std::string> pins = server_->config.launcher_favorites;
		if (pins.empty()) {
			// Nothing pinned yet: the everyday few (files, browser, terminal,
			// software, text editor, Settings), whichever are installed.
			static const char *kEveryday[] = {"org.gnome.nautilus", "firefox", "org.gnome.ptyxis", "org.gnome.console",
				"org.gnome.terminal", "org.gnome.software", "org.gnome.texteditor", "org.dusk.settings",
				"org.gnome.calculator"};
			for (const char *want : kEveryday) {
				for (const AppInfo &a : apps) {
					if (pins.size() < 6 && a.visible && lower(a.id).rfind(want, 0) == 0 &&
							std::find(pins.begin(), pins.end(), a.id) == pins.end()) {
						pins.push_back(a.id);
						break;
					}
				}
			}
		}
		for (const std::string &id : pins) {
			for (int i = 0; i < static_cast<int>(apps.size()); i++) {
				if (apps[static_cast<size_t>(i)].id == id) {
					favs_.push_back(i);
					break;
				}
			}
		}
	}
	// Settings pages, and the answer when it's a sum (Dusk's menu).
	settings_hits_.clear();
	calc_.clear();
	if (look_ == "dusk" && q.size() >= 2) {
		for (int k = 0; k < static_cast<int>(std::size(kSettingsPages)); k++) {
			const std::string words = lower(std::string(kSettingsPages[k].title) + " " + kSettingsPages[k].words);
			// Each word typed starts a word of the page's.
			bool all = true;
			std::istringstream in(q);
			std::string w;
			while (in >> w) {
				const size_t at = words.find(w);
				if (at == std::string::npos || (at > 0 && words[at - 1] != ' ')) { all = false; break; }
			}
			if (all && settings_hits_.size() < 3) settings_hits_.push_back(k);
		}
	}
	if (look_ == "dusk") calc_ = sum_answer(query_);
	// Files: looked for a moment after you stop typing.
	files_.clear();
	search_gen_++;
	if (search_timer_) wl_event_source_timer_update(search_timer_, query_.size() >= 2 ? 220 : 0);
	hover_fav_ = -1;
	measure();
	selected_ = 0;
	first_row_ = 0;
	// A new list: no gliding from where the old one was.
	fx_.snap("scroll", 0);
}

int Launcher::visible_rows() const {
	return std::max(0, (h_ - list_top_ - kFooterHeight - 4) / kRowHeight);
}

int Launcher::hot_at(double px, double py) const {
	for (int k = static_cast<int>(hots_.size()) - 1; k >= 0; k--) {
		const wlr_box &b = hots_[static_cast<size_t>(k)].box;
		if (px >= b.x && px < b.x + b.width && py >= b.y && py < b.y + b.height) return k;
	}
	return -1;
}

void Launcher::hot_action(const std::string &id) {
	const char *h = std::getenv("HOME");
	const std::string home = h ? h : "";
	if (id == "allapps" || id == "back") {
		all_apps_ = id == "allapps";
		hover_hot_ = -1;
		first_row_ = 0;
		selected_ = 0;
		fx_.snap("scroll", 0);
		measure();
		redraw();
	} else if (id == "settings") {
		close();
		server_->open_settings();
	} else if (id == "lock") {
		close();
		server_->power_action("lock");
	} else if (id == "power") {
		for (const Hot &t : hots_) {
			if (t.id != "power") continue;
			server_->show_power_menu(output_, wlr_box{box_.x + t.box.x, box_.y + t.box.y, t.box.width, t.box.height},
				"below");
		}
	} else if (id == "calc") {
		server_->spawn("printf %s " + shell_quote(calc_) + " | wl-copy");
		close();
	} else if (id == "me") {
		close();
		server_->open_settings("about");
	} else if (id.rfind("place:", 0) == 0) {
		const std::string where = id.substr(6);
		server_->spawn(where == "trash" ? std::string("gio open trash:///")
			: "xdg-open " + shell_quote(where.empty() ? home : home + "/" + where));
		close();
	} else if (id.rfind("recent:", 0) == 0) {
		const size_t k = std::stoul(id.substr(7));
		if (k < recent_.size()) server_->spawn("xdg-open " + shell_quote(recent_[k].path));
		close();
	} else if (id.rfind("folder:", 0) == 0) {
		const size_t k = std::stoul(id.substr(7));
		if (k < recent_.size()) {
			server_->spawn("xdg-open " + shell_quote(std::filesystem::path(recent_[k].path).parent_path().string()));
		}
		close();
	}
}

// The files you opened lately (GTK's list, which most apps add to).
void Launcher::load_recent() {
	recent_.clear();
	const char *h = std::getenv("HOME");
	if (!h) return;
	const std::string home = h;
	std::ifstream f(home + "/.local/share/recently-used.xbel");
	if (!f) return;
	struct Item {
		std::string path, modified;
	};
	std::vector<Item> items;
	std::string line;
	auto attr = [](const std::string &l, const std::string &name) {
		const size_t a = l.find(" " + name + "=\"");
		if (a == std::string::npos) return std::string();
		const size_t b = a + name.size() + 3, e = l.find('"', b);
		return e == std::string::npos ? std::string() : l.substr(b, e - b);
	};
	while (std::getline(f, line)) {
		if (line.find("<bookmark ") == std::string::npos) continue;
		const std::string href = attr(line, "href");
		if (href.rfind("file://", 0) != 0) continue;
		std::string m = attr(line, "visited");
		const std::string mod = attr(line, "modified");
		if (mod > m) m = mod;
		items.push_back(Item{url_decode(href.substr(7)), m});
	}
	std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.modified > b.modified; });
	std::error_code ec;
	for (const Item &it : items) {
		if (recent_.size() >= 3) break;
		if (it.path.find("/.") != std::string::npos) continue;   // (hidden files)
		if (!std::filesystem::is_regular_file(it.path, ec)) continue;
		std::string where = std::filesystem::path(it.path).parent_path().string();
		if (where.rfind(home, 0) == 0) where = "~" + where.substr(home.size());
		recent_.push_back(Recent{it.path, std::filesystem::path(it.path).filename().string(), where, ago(it.modified)});
	}
}

// Next to the menu button, on the side away from its panel's screen edge
// (or in the middle of the screen when opened from the keyboard with no
// panel). Slides a little while fading in/out.
void Launcher::place() {
	if (!output_) {
		return;
	}
	const wlr_box o = output_->box();
	const wlr_box u = output_->usable();
	const int gap = 8;
	int x, y;
	if (full()) {
		x = o.x;
		y = o.y;
	} else if (anchor_.width <= 0) {
		x = o.x + (o.width - w_) / 2;
		y = o.y + (o.height - h_) / 2;
	} else if (edge_ == "top" || edge_ == "bottom") {
		// Windows 11's Start sits in the middle of the screen, over the taskbar.
		x = look_ == "windows" ? u.x + (u.width - w_) / 2 : anchor_.x;
		y = edge_ == "top" ? anchor_.y + anchor_.height + gap + 6 : anchor_.y - h_ - gap - 6;
	} else {
		y = anchor_.y;
		x = edge_ == "left" ? anchor_.x + anchor_.width + gap + 6 : anchor_.x - w_ - gap - 6;
	}
	if (!full()) {
		x = std::clamp(x, u.x + gap, std::max(u.x + gap, u.x + u.width - w_ - gap));
		y = std::clamp(y, u.y + gap, std::max(u.y + gap, u.y + u.height - h_ - gap));
	}
	box_ = wlr_box{x, y, w_, h_};
	// It grows out of the corner nearest the menu button (full screen: from the middle).
	double ox = w_ / 2.0, oy = h_ / 2.0;
	if (anchor_.width > 0 && !full()) {
		ox = std::clamp<double>(anchor_.x + anchor_.width / 2.0 - x, 0, w_);
		oy = std::clamp<double>(anchor_.y + anchor_.height / 2.0 - y, 0, h_);
		if (edge_ == "bottom") oy = h_;
		else if (edge_ == "top") oy = 0;
		else if (edge_ == "left") ox = 0;
		else ox = w_;
	}
	const double t = std::clamp(alpha_, 0.0, 1.1);
	// Full screen (GNOME, macOS): only a fade, and no shadow round it; the
	// zoom and the shadow made the blur behind it jump about while it
	// opened and closed (the overview's glitch).
	layer_.set_body_opacity(full() ? 0.0 : 1.0);
	layer_.set_blur(full() ? 0.0 : -1.0);   // full screen: the backdrop blurs instead
	layer_.show(x, y, std::min(1.0, t * 1.3), full() ? 1.0 : 0.9 + 0.1 * t, ox, oy);
	place_backdrop();
}

// The backdrop: the wallpaper itself, softened (a small copy stretched back
// up is a smooth blur) and darkened, over the whole screen and under the
// panels. Not a live blur of the screen: that also picked up the panels,
// their shadows and the screen's edges, as dark bands and corners.
void Launcher::place_backdrop() {
	const bool on = full() && output_ && alpha_ > 0.001;
	if (!on) {
		if (full_tree_) wlr_scene_node_set_enabled(&full_tree_->node, false);
		return;
	}
	if (!full_tree_) {
		full_tree_ = wlr_scene_tree_create(server_->layer_overview);
		full_tree_->node.data = &tag;
		full_bg_ = wlr_scene_buffer_create(full_tree_, nullptr);
	}
	const wlr_box o = output_->box();
	const float scale = output_->output->scale;
	const bool mac = look_ == "macos";
	const uint64_t key = output_->wall_soft_gen * 1000003u + static_cast<uint64_t>(o.width) * 7919u +
		static_cast<uint64_t>(o.height) * 31u + static_cast<uint64_t>(scale * 100) + (mac ? 1 : 0) + (theme::dark ? 0 : 2) +
		reinterpret_cast<uintptr_t>(output_);
	if (key != full_bg_key_) {
		full_bg_key_ = key;
		Canvas canvas(o.width, o.height, scale);
		cairo_t *cr = canvas.cr();
		if (cairo_surface_t *soft = output_->wall_soft) {
			// Stretched back up in two steps (once straight up showed its blocks).
			const int sw = cairo_image_surface_get_width(soft), sh = cairo_image_surface_get_height(soft);
			const int mw = std::max(sw, o.width / 6), mh = std::max(sh, o.height / 6);
			cairo_surface_t *mid = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, mw, mh);
			cairo_t *mc = cairo_create(mid);
			cairo_scale(mc, static_cast<double>(mw) / sw, static_cast<double>(mh) / sh);
			cairo_set_source_surface(mc, soft, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(mc), CAIRO_FILTER_BILINEAR);
			cairo_pattern_set_extend(cairo_get_source(mc), CAIRO_EXTEND_PAD);
			cairo_paint(mc);
			cairo_destroy(mc);
			cairo_save(cr);
			cairo_scale(cr, static_cast<double>(o.width) / mw, static_cast<double>(o.height) / mh);
			cairo_set_source_surface(cr, mid, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
			cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);   // no dark edges
			cairo_paint(cr);
			cairo_restore(cr);
			cairo_surface_destroy(mid);
		} else {
			cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
			cairo_paint(cr);
		}
		// Darkened the overview's way (its tint and strength), so it looks the
		// same and the white names read on any wallpaper.
		// (Frosted pale in the light theme, with dark names.)
		if (theme::dark) cairo_set_source_rgba(cr, 0x16 / 255.0, 0x16 / 255.0, 0x19 / 255.0, mac ? 0.55 : 0.72);
		else cairo_set_source_rgba(cr, 0xee / 255.0, 0xee / 255.0, 0xf2 / 255.0, mac ? 0.50 : 0.62);
		cairo_paint(cr);
		canvas.present(full_bg_);
	}
	wlr_scene_buffer_set_dest_size(full_bg_, o.width, o.height);
	wlr_scene_node_set_position(&full_bg_->node, o.x, o.y);
	wlr_scene_buffer_set_opacity(full_bg_, static_cast<float>(std::clamp(alpha_, 0.0, 1.0)));
	wlr_scene_node_set_enabled(&full_tree_->node, true);
	// The drawing goes right over the backdrop (and under the panels).
	if (layer_.tree->node.parent != full_tree_) wlr_scene_node_reparent(&layer_.tree->node, full_tree_);
	wlr_scene_node_raise_to_top(&layer_.tree->node);
}

void Launcher::open(Output *output, wlr_box anchor, const std::string &edge) {
	if (!output) {
		return;
	}
	output_ = output;
	anchor_ = anchor;
	edge_ = edge;
	open_ = true;
	query_.clear();
	all_apps_ = false;
	hover_hot_ = -1;
	if (server_->config.launcher_look == "dusk") load_recent();
	hover_row_ = -1;
	keyed_ = false;
	measure();
	server_->apps.reload_if_changed();   // picks up newly installed apps
	filter();
	hide_windows(full());
	// A card: over everything (full screen goes into its backdrop: place_backdrop).
	if (!full()) wlr_scene_node_reparent(&layer_.tree->node, server_->layer_menu);
	redraw_now();
	place();
	layer_.set_enabled(true);
	layer_.raise();

	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(alpha_, 1.0, c.animate(c.anim_menu) ? c.ms(230) : 0,
		[this] { place(); }, [this] { anim_ = 0; });
	server_->redraw_panels();   // menu button shows as pressed
}

void Launcher::close() {
	if (!open_) {
		return;
	}
	open_ = false;
	hide_windows(false);
	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(alpha_, 0.0, c.animate(c.anim_menu) ? c.ms(160) : 0,
		[this] { place(); },
		[this] {
			anim_ = 0;
			if (!open_) {
				layer_.set_enabled(false);
			}
		});
	server_->redraw_panels();
}

void Launcher::toggle(Output *output, wlr_box anchor, const std::string &edge) {
	if (open_) {
		close();
	} else {
		open(output, anchor, edge);
	}
}

void Launcher::toggle(Output *output) {
	if (open_ || !output) {
		close();
		return;
	}
	for (const auto &panel : output->panels) {
		const auto &w = panel->config().widgets;
		if (std::find(w.begin(), w.end(), "menu") != w.end()) {
			open(output, panel->anchor("menu"), panel->config().edge);
			return;
		}
	}
	open(output, wlr_box{}, "bottom");
}

void Launcher::hide_windows(bool hide) {
	if (hide == windows_hidden_) return;
	// The overview hides (and shows) them itself while it's open.
	const bool overview = server_->overview && server_->overview->is_open();
	windows_hidden_ = hide;
	if (overview) return;
	wlr_scene_node_set_enabled(&server_->layer_views->node, !hide);
	wlr_scene_node_set_enabled(&server_->layer_fullscreen->node, !hide);
	wlr_scene_node_set_enabled(&server_->layer_desktop->node, !hide);
	// The panels' shadows too: they lie under the windows, so the blur took
	// them in as dark bands along the panels (a hidden dock's along the
	// bottom). The panels themselves stay, sharp, on top.
	wlr_scene_node_set_enabled(&server_->layer_panel_shadow->node, !hide);
	if (hide) {
		wlr_seat_pointer_notify_clear_focus(server_->seat);
	} else {
		for (View *v : server_->views) v->sync_enabled();
	}
}

void Launcher::output_gone(Output *output) {
	if (output_ == output) {
		hide_windows(false);
		open_ = false;
		server_->anim->cancel(anim_);
		anim_ = 0;
		alpha_ = 0;
		layer_.set_enabled(false);
		output_ = nullptr;
	}
}

bool Launcher::contains(double lx, double ly) const {
	return open_ && lx >= box_.x && lx < box_.x + box_.width &&
		ly >= box_.y && ly < box_.y + box_.height;
}

int Launcher::row_at(double px, double py) const {
	if (grid()) return item_at(px, py);
	if (home()) return -1;
	if (px < kPad || px >= w_ - kPad || py < list_top_) {
		return -1;
	}
	if (py >= list_top_ + visible_rows() * kRowHeight) {
		return -1;
	}
	// Rows as drawn, while the list may still be gliding.
	const double scroll = const_cast<Fades &>(fx_).get("scroll", first_row_ * kRowHeight);
	const int row = static_cast<int>(std::floor((py - list_top_ + scroll) / kRowHeight));
	return row >= 0 && row < count() ? row : -1;
}

bool Launcher::on_power(double px, double py) const {
	const wlr_box b = power_box();
	return b.width > 0 && px >= b.x && px < b.x + b.width && py >= b.y && py < b.y + b.height;
}

bool Launcher::on_settings(double px, double py) const {
	const wlr_box b = settings_box();
	return b.width > 0 && px >= b.x && px < b.x + b.width && py >= b.y && py < b.y + b.height;
}

void Launcher::motion(double lx, double ly) {
	const double px = lx - box_.x, py = ly - box_.y;
	const int row = row_at(px, py);
	const int fav = fav_at(px, py);
	const bool power = on_power(px, py);
	const bool settings = on_settings(px, py);
	const int hot = fav >= 0 ? -1 : hot_at(px, py);
	if (row != hover_row_ || fav != hover_fav_ || power != hover_power_ || settings != hover_settings_ ||
			hot != hover_hot_) {
		hover_hot_ = hot;
		hover_row_ = row;
		hover_fav_ = fav;
		hover_power_ = power;
		hover_settings_ = settings;
		update_highlights();   // no redraw needed
	}
}

void Launcher::leave() {
	if (open_ && (hover_row_ != -1 || hover_fav_ != -1 || hover_power_ || hover_settings_ || hover_hot_ != -1)) {
		hover_hot_ = -1;
		hover_row_ = -1;
		hover_fav_ = -1;
		hover_power_ = false;
		hover_settings_ = false;
		update_highlights();
	}
}

// The selection and hover highlights, where the rows are drawn right now.
void Launcher::update_highlights() {
	const Color hover = full() ? (theme::dark ? rgb(0xffffff, 0.12) : rgb(0x000000, 0.07)) : with_alpha(theme::text, theme::dark ? 0.08 : 0.06);
	if (grid()) {
		const double scroll = fx_.get("scroll", first_row_ * cell_h_);
		const double ir = look_ == "windows" ? 6 : 18;
		auto cell = [&](int i, wlr_box *out) {
			if (i < 0 || i >= count()) return false;
			const wlr_box b = item_box(i, scroll);
			if (b.y < grid_.y - b.height / 2 || b.y + b.height > grid_.y + grid_rows_ * cell_h_ + b.height / 2) return false;
			*out = wlr_box{b.x + 4, b.y + 2, b.width - 8, b.height - 4};
			return true;
		};
		wlr_box b;
		if ((!query_.empty() || keyed_) && cell(selected_, &b)) {
			sel_.show(b, ir, full() ? (theme::dark ? rgb(0xffffff, 0.2) : rgb(0x000000, 0.10)) : with_alpha(theme::accent, theme::dark ? 0.30 : 0.22));
		} else {
			sel_.hide();
		}
		if (hover_fav_ >= 0) {
			const wlr_box fb = fav_box(hover_fav_);
			hl_.show(wlr_box{fb.x + 4, fb.y + 2, fb.width - 8, fb.height - 4}, ir, hover);
		} else if (hover_row_ >= 0 && (hover_row_ != selected_ || (query_.empty() && !keyed_)) && cell(hover_row_, &b)) {
			hl_.show(b, ir, hover);
		} else if (hover_settings_ || hover_power_) {
			hl_.show(hover_settings_ ? settings_box() : power_box(), 8, hover);
		} else {
			hl_.hide();
		}
		return;
	}
	const double scroll = fx_.get("scroll", first_row_ * kRowHeight);
	const double row_r = inner_radius(theme::ui_radius, kPad);
	const int rh = kRowHeight - 4;
	const double list_bottom = list_top_ + visible_rows() * kRowHeight;
	auto row_box = [&](int row, wlr_box *out) {
		const double ry = list_top_ + row * kRowHeight - scroll;
		if (row < 0 || row >= count() || ry < list_top_ - rh / 2.0 ||
				ry + rh > list_bottom + rh / 2.0) {
			return false;
		}
		*out = wlr_box{kPad, static_cast<int>(std::lround(ry)), w_ - 2 * kPad, rh};
		return true;
	};
	wlr_box b;
	if (!home() && row_box(selected_, &b)) {
		sel_.show(b, row_r, with_alpha(theme::accent, theme::dark ? 0.22 : 0.16));
	} else {
		sel_.hide();
	}
	if (hover_fav_ >= 0) {
		const wlr_box fb = fav_box(hover_fav_);
		hl_.show(wlr_box{fb.x + 4, fb.y, fb.width - 8, fb.height}, std::min(16.0, row_r), hover);
	} else if (hover_hot_ >= 0 && hover_hot_ < static_cast<int>(hots_.size())) {
		const Hot &t = hots_[static_cast<size_t>(hover_hot_)];
		hl_.show(t.box, t.r, hover);
	} else if (hover_row_ >= 0 && hover_row_ != selected_ && row_box(hover_row_, &b)) {
		hl_.show(b, row_r, hover);
	} else if (hover_settings_) {
		hl_.show(settings_box(), 18, hover);
	} else if (hover_power_) {
		hl_.show(power_box(), 18, hover);
	} else {
		hl_.hide();
	}
}

// The scroll bar's track (a bit wider than it's drawn, so it's easy to hit).
bool Launcher::on_bar(double px, double py) const {
	const int rows = visible_rows();
	return !grid() && count() > rows && px >= w_ - 18 && px < w_ &&
		py >= list_top_ && py < list_top_ + rows * kRowHeight;
}

void Launcher::drag_bar(double lx, double ly) {
	(void)lx;
	const int rows = visible_rows();
	const int total = count();
	if (total <= rows) return;
	const double track = rows * kRowHeight;
	const double thumb = std::max(24.0, track * rows / total);
	const double frac = std::clamp((ly - box_.y - list_top_ - thumb / 2) / std::max(1.0, track - thumb), 0.0, 1.0);
	const int first = static_cast<int>(std::lround(frac * (total - rows)));
	if (first != first_row_) {
		first_row_ = first;
		redraw();
	}
}

void Launcher::click(double lx, double ly, uint32_t button) {
	const double px = lx - box_.x, py = ly - box_.y;
	if (button == BTN_RIGHT) {
		const int fav = fav_at(px, py);
		const int row = fav >= 0 ? -1 : row_at(px, py);
		if (fav >= 0 || row >= 0) item_menu(row, fav, lx, ly);
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}
	if (const int fav = fav_at(px, py); fav >= 0) {
		const AppInfo &app = server_->apps.apps()[static_cast<size_t>(favs_[static_cast<size_t>(fav)])];
		if (app.id.rfind("org.dusk.Settings", 0) == 0) server_->open_settings("");
		else server_->spawn(app.exec);
		close();
		return;
	}
	if (const int hot = hot_at(px, py); hot >= 0 && look_ == "dusk") {
		hot_action(hots_[static_cast<size_t>(hot)].id);
		return;
	}
	if (on_bar(px, py)) {
		bar_drag_ = true;
		drag_bar(lx, ly);
		return;
	}
	if (on_settings(px, py)) {
		close();
		server_->open_settings();
		return;
	}
	if (on_power(px, py)) {
		// Right by the button (under it, or over it when there's no room),
		// wherever the app menu is.
		const wlr_box p = power_box();
		server_->show_power_menu(output_, wlr_box{box_.x + p.x, box_.y + p.y, p.width, p.height}, "below");
		return;
	}
	const int row = row_at(px, py);
	if (row >= 0) {
		launch(row);
	} else if (full()) {
		close();   // the empty screen around the apps
	}
}

void Launcher::scroll(double delta, bool wheel) {
	// A mouse wheel moves a row per notch (15 units, or a part of one for
	// high-resolution wheels); a touchpad moves with the fingers.
	const int unit = grid() ? cell_h_ : kRowHeight;
	scroll_accum_ += wheel ? delta * (unit / 15.0) : delta * 1.4;
	int steps = 0;
	while (scroll_accum_ >= unit * 0.75) { scroll_accum_ -= unit; steps++; }
	while (scroll_accum_ <= -unit * 0.75) { scroll_accum_ += unit; steps--; }
	if (steps == 0) {
		return;
	}
	first_row_ = std::clamp(first_row_ + steps, 0, max_first());
	redraw();
}

void Launcher::keep_selection_visible() {
	if (grid()) {
		const int row = selected_ / std::max(1, cols_);
		if (row < first_row_) first_row_ = row;
		else if (row >= first_row_ + grid_rows_) first_row_ = row - grid_rows_ + 1;
		first_row_ = std::clamp(first_row_, 0, max_first());
		return;
	}
	if (selected_ < first_row_) {
		first_row_ = selected_;
	} else if (selected_ >= first_row_ + visible_rows()) {
		first_row_ = selected_ - visible_rows() + 1;
	}
}

bool Launcher::handle_key(xkb_keysym_t sym, const std::string &text) {
	const int count = this->count();
	switch (sym) {
	case XKB_KEY_Escape:
		if (look_ == "dusk" && query_.empty() && all_apps_) {
			hot_action("back");
			return true;
		}
		close();
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (!calc_.empty()) {
			hot_action("calc");   // a sum: Enter copies the answer
			return true;
		}
		if (home()) return true;
		if (count > 0) {
			launch(selected_);
		}
		return true;
	case XKB_KEY_Down:
		if (home()) {
			hot_action("allapps");   // the arrow keys work in the list of all apps
			return true;
		}
		if (count > 0) selected_ = std::min(selected_ + (grid() ? cols_ : 1), count - 1);
		if (grid() && query_.empty()) keyed_ = true;
		break;
	case XKB_KEY_Up:
		selected_ = std::max(selected_ - (grid() ? cols_ : 1), 0);
		if (grid() && query_.empty()) keyed_ = true;
		break;
	case XKB_KEY_Right:
		if (!grid()) return true;
		if (count > 0) selected_ = std::min(selected_ + 1, count - 1);
		keyed_ = true;
		break;
	case XKB_KEY_Left:
		if (!grid()) return true;
		selected_ = std::max(selected_ - 1, 0);
		keyed_ = true;
		break;
	case XKB_KEY_Page_Down:
		if (count > 0) selected_ = std::min(selected_ + (grid() ? cols_ * grid_rows_ : visible_rows()), count - 1);
		break;
	case XKB_KEY_Page_Up:
		selected_ = std::max(selected_ - (grid() ? cols_ * grid_rows_ : visible_rows()), 0);
		break;
	case XKB_KEY_BackSpace:
		if (!query_.empty()) {
			// Remove one whole UTF-8 character.
			size_t cut = query_.size() - 1;
			while (cut > 0 && (static_cast<unsigned char>(query_[cut]) & 0xc0) == 0x80) {
				cut--;
			}
			query_.erase(cut);
			filter();
		} else if (look_ == "dusk" && all_apps_) {
			hot_action("back");
			return true;
		}
		break;
	case XKB_KEY_Tab:
		break;
	default:
		if (!text.empty() && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f) {
			query_ += text;
			filter();
		}
		break;
	}
	keep_selection_visible();
	redraw();
	return true;   // the menu takes all keys while open
}

void Launcher::launch(int row) {
	if (row < 0 || row >= count()) {
		return;
	}
	if (is_file(row)) {
		// A file or folder: opened with its app (a folder in the file manager).
		server_->spawn("xdg-open " + shell_quote(file_of(row).path));
		close();
		return;
	}
	if (is_setting(row)) {
		server_->open_settings(kSettingsPages[setting_of(row)].page);
		close();
		return;
	}
	const AppInfo &app = server_->apps.apps()[static_cast<size_t>(app_of(row))];
	if (app.id.rfind("org.dusk.Settings", 0) == 0) {
		server_->open_settings("");   // the one that's open comes forward
		close();
		return;
	}
	server_->spawn(app.exec);
	close();
}

// Drawn once when the event loop is next free, not every time something
// asks (the hover fades, the scroll, the files arriving, each key): a
// full-screen grid drawn several times a frame flickered, like the overview
// did before the same fix.
void Launcher::redraw() {
	if (!output_ || redraw_idle_) return;
	redraw_idle_ = wl_event_loop_add_idle(server_->loop, &Launcher::handle_redraw_idle, this);
}

void Launcher::handle_redraw_idle(void *data) {
	auto *self = static_cast<Launcher *>(data);
	self->redraw_idle_ = nullptr;
	self->redraw_now();
}

void Launcher::redraw_now() {
	if (!output_ || drawing_) {
		return;
	}
	if (grid()) {
		redraw_grid();
		return;
	}
	drawing_ = true;
	const auto &apps = server_->apps.apps();
	const float scale = output_->output->scale;
	Canvas canvas(w_, h_, scale);
	cairo_t *cr = canvas.cr();
	const double radius = theme::ui_radius;
	ui_card(cr, 0, 0, w_, h_, radius, theme::menu_bg);

	hots_.clear();
	// Search field: a soft pill, its ends following the card's corners.
	const double field_r = std::min<double>(kSearchHeight / 2.0, inner_radius(radius, kPad));
	fill_rounded(cr, kPad, kPad, w_ - 2 * kPad, kSearchHeight, field_r, theme::field_bg);
	if (!query_.empty()) {
		rounded_rect(cr, kPad + 0.5, kPad + 0.5, w_ - 2 * kPad - 1, kSearchHeight - 1, field_r);
		set_color(cr, with_alpha(theme::accent, 0.55));
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	draw_named_icon(cr, server_->icons, "search", kPad + 14, kPad + kSearchHeight / 2.0 - 9, 18, theme::text_dim, scale);
	const int text_x = kPad + 42;
	if (query_.empty()) {
		draw_text(cr, "Search apps, files, settings or do a sum", text_x + 6, kPad, w_ - 2 * kPad - 56,
			kSearchHeight, theme::font, theme::text_dim);
	} else {
		draw_text(cr, query_, text_x, kPad, w_ - 2 * kPad - 56, kSearchHeight, theme::font_large, theme::text);
	}
	const int caret_x = text_x + (query_.empty() ? 0 : text_width(query_, theme::font_large)) + 1;
	if (caret_x < w_ - kPad - 16) {
		cairo_rectangle(cr, caret_x, kPad + 12, 2, kSearchHeight - 24);
		set_color(cr, theme::accent);
		cairo_fill(cr);
	}

	if (home()) {
		draw_home(cr, scale);
		draw_footer(cr, scale);
		layer_.present(canvas, wlr_box{0, 0, w_, h_}, radius);
		drawing_ = false;
		update_highlights();
		return;
	}

	if (query_.empty()) {
		// Every app, A to Z, with the way back.
		const int by = kListTop;
		draw_named_icon(cr, server_->icons, "chevron-left", kPad + 6, by + 7, 16, theme::text, scale);
		draw_text(cr, "All apps", kPad + 26, by, 200, 30, theme::font_title, theme::text);
		hots_.push_back(Hot{"back", wlr_box{kPad, by, text_width("All apps", theme::font_title) + 38, 30}, 15});
		draw_text(cr, std::to_string(shown_.size()) + " apps", w_ - kPad - 160, by, 154, 30, theme::font_small,
			theme::text_dim, Align::Right);
	}
	if (!calc_.empty()) {
		// The answer to a sum, as a card (a click or Enter copies it).
		calc_box_ = wlr_box{kPad, kListTop, w_ - 2 * kPad, 60};
		fill_rounded(cr, calc_box_.x, calc_box_.y, calc_box_.width, calc_box_.height, inner_radius(radius, kPad),
			theme::card_bg);
		const double ix = calc_box_.x + 12, iy = calc_box_.y + 12;
		fill_rounded(cr, ix, iy, 36, 36, 10, with_alpha(theme::accent, 0.16));
		draw_named_icon(cr, server_->icons, "sparkles", ix + 9, iy + 9, 18, theme::accent, scale);
		draw_text(cr, "= " + calc_, ix + 48, calc_box_.y + 9, calc_box_.width - 72, 24, theme::font_heading, theme::text);
		draw_text(cr, "Calculation · Enter copies the answer", ix + 48, calc_box_.y + 33, calc_box_.width - 72, 18,
			theme::font_small, theme::text_dim);
		hots_.push_back(Hot{"calc", calc_box_, inner_radius(radius, kPad)});
	}

	// The list: it scrolls smoothly and the selection glides between rows.
	const int rows = visible_rows();
	if (count() == 0 && calc_.empty()) {
		draw_text(cr, apps.empty() ? "No apps found" : query_.size() >= 2 ? "Looking…" : "No matches", kPad, list_top_,
			w_ - 2 * kPad, kRowHeight * 2, theme::font, theme::text_dim, Align::Center);
	}
	fx_.set("scroll", first_row_ * kRowHeight, 0);
	const double scroll = fx_.get("scroll");
	const double list_h = rows * kRowHeight;
	cairo_save(cr);
	cairo_rectangle(cr, 0, list_top_ - 2, w_, list_h + 2);
	cairo_clip(cr);
	const double rh = kRowHeight - 4;
	const int first = std::max(0, static_cast<int>(scroll / kRowHeight));
	const int head = apps_head(), files_end = head + static_cast<int>(files_.size()), extras_end = head + extras();
	// "Apps", "Files", "Settings": where each part starts (on the right of the row).
	auto part = [&](int row) -> const char * {
		if (extras() == 0) return nullptr;
		if (row == 0 && head > 0) return "Apps";
		if (row == head && !files_.empty()) return "Files";
		if (row == files_end && !settings_hits_.empty()) return "Settings";
		if (row == extras_end && extras_end < count()) return "More apps";
		return nullptr;
	};
	const std::string home_dir = std::getenv("HOME") ? std::getenv("HOME") : "";
	for (int row = first; row < count() && row <= first + rows + 1; row++) {
		const double ry = list_top_ + row * kRowHeight - scroll;
		if (row > 0 && part(row)) {
			cairo_rectangle(cr, kPad + 12, ry - 1, w_ - 2 * kPad - 24, 1);
			set_color(cr, theme::divider);
			cairo_fill(cr);
		}
		if (const char *label = part(row)) {
			draw_text(cr, label, w_ - kPad - 110, ry, 98, rh, theme::font_small, with_alpha(theme::text_dim, 0.8),
				Align::Right);
		}
		const std::string name = item_name(row);
		const double tx = kPad + 50, tw = w_ - 2 * kPad - 60 - (part(row) ? 70 : 0);
		if (is_setting(row)) {
			fill_rounded(cr, kPad + 10, ry + (rh - 30) / 2.0, 30, 30, 9, with_alpha(theme::text, 0.07));
			draw_named_icon(cr, server_->icons, "gear", kPad + 17, ry + (rh - 16) / 2.0, 16, theme::text, scale);
			draw_text(cr, name, tx, ry + 3, tw, 20, theme::font, theme::text);
			draw_text(cr, "Settings", tx, ry + 21, tw, 16, theme::font_small, theme::text_dim);
			continue;
		}
		draw_icon(cr, server_->icons, item_icon(row), name, kPad + 10, ry + (rh - 30) / 2.0, 30, scale);
		if (is_file(row)) {
			// The name, and the folder it's in (from your home).
			const FileHit &f = file_of(row);
			std::string where = f.path.substr(0, f.path.find_last_of('/'));
			if (!home_dir.empty() && where.rfind(home_dir, 0) == 0) where = "~" + where.substr(home_dir.size());
			draw_text(cr, name, tx, ry + 3, tw, 20, theme::font, theme::text);
			draw_text(cr, where, tx, ry + 21, tw, 16, theme::font_small, theme::text_dim);
		} else {
			const AppInfo &app = apps[static_cast<size_t>(app_of(row))];
			if (!app.comment.empty() && !query_.empty()) {
				draw_text(cr, name, tx, ry + 3, tw, 20, theme::font, theme::text);
				draw_text(cr, app.comment, tx, ry + 21, tw, 16, theme::font_small, theme::text_dim);
			} else {
				draw_text(cr, name, tx, ry, tw, rh, theme::font, theme::text);
			}
		}
	}
	cairo_restore(cr);

	// Scroll indicator.
	const int total = count();
	if (total > rows && rows > 0) {
		const double track = rows * kRowHeight;
		const double thumb = std::max(24.0, track * rows / total);
		const double pos = (track - thumb) * (scroll / kRowHeight) / std::max(1, total - rows);
		fill_rounded(cr, w_ - 9, list_top_ + pos, 4, thumb, 2, with_alpha(theme::text, 0.25));
	}

	draw_footer(cr, scale);
	layer_.present(canvas, wlr_box{0, 0, w_, h_}, radius);
	drawing_ = false;
	update_highlights();
}

namespace {
struct Place {
	const char *label, *icon, *where;
};
const Place kPlaces[] = {{"Home", "house", ""}, {"Documents", "file", "Documents"}, {"Downloads", "download", "Downloads"},
	{"Pictures", "image", "Pictures"}, {"Music", "audio-lines", "Music"}, {"Trash", "trash", "trash"}};
constexpr int kChipH = 32;
} // namespace

// The places' chips: their title and rows (they wrap).
int Launcher::places_height() const {
	int x = kPad, rows = 1;
	for (const Place &pl : kPlaces) {
		const int cw = text_width(pl.label, theme::font_small) + 44;
		if (x + cw > w_ - kPad) {
			x = kPad;
			rows++;
		}
		x += cw + 6;
	}
	return 28 + rows * (kChipH + 8) - 8 + 18;
}

// Dusk's menu before you type: pinned apps, your places, recent files.
void Launcher::draw_home(cairo_t *cr, float scale) {
	const auto &apps = server_->apps.apps();
	const double r = inner_radius(theme::ui_radius, kPad);
	int y = favs_area_.y;
	draw_text(cr, "Pinned", kPad + 2, y, 200, 22, theme::font_title, theme::text_dim);
	{
		const std::string all = "All apps";
		const int aw = text_width(all, theme::font_small) + 34;
		const int ax = w_ - kPad - aw;
		fill_rounded(cr, ax, y - 2, aw, 26, 13, with_alpha(theme::text, 0.06));
		draw_text(cr, all, ax + 12, y - 2, aw - 26, 26, theme::font_small, theme::text);
		draw_named_icon(cr, server_->icons, "chevron-right", ax + aw - 22, y + 4, 14, theme::text_dim, scale);
		hots_.push_back(Hot{"allapps", wlr_box{ax, y - 2, aw, 26}, 13});
	}
	if (favs_.empty()) {
		draw_text(cr, "Right-click an app in All apps to pin it here", kPad + 2, y + 30, w_ - 2 * kPad, 30,
			theme::font_small, theme::text_dim);
	}
	const int shown = std::min(static_cast<int>(favs_.size()), 12);
	for (int k = 0; k < shown; k++) {
		const AppInfo &app = apps[static_cast<size_t>(favs_[static_cast<size_t>(k)])];
		const wlr_box b = fav_box(k);
		const double is = 40;
		draw_icon(cr, server_->icons, app.icon, app.name, b.x + (b.width - is) / 2.0, b.y + 6, is, scale);
		draw_text(cr, app.name, b.x + 2, b.y + 6 + is + 5, b.width - 4, 18, theme::font_small, theme::text, Align::Center);
	}
	y = favs_area_.y + favs_area_.height + 10;

	// Places: chips that open your folders.
	draw_text(cr, "Places", kPad + 2, y, 200, 22, theme::font_title, theme::text_dim);
	y += 28;
	int x = kPad;
	const int ch = kChipH;
	for (const auto &pl : kPlaces) {
		const int cw = text_width(pl.label, theme::font_small) + 44;
		if (x + cw > w_ - kPad) {
			// The next row.
			x = kPad;
			y += ch + 8;
		}
		rounded_rect(cr, x + 0.5, y + 0.5, cw - 1, ch - 1, ch / 2.0);
		set_color(cr, theme::divider.a > 0 ? with_alpha(theme::text, 0.12) : theme::divider);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		draw_named_icon(cr, server_->icons, pl.icon, x + 12, y + (ch - 16) / 2.0, 16, theme::text, scale);
		draw_text(cr, pl.label, x + 34, y, cw - 40, ch, theme::font_small, theme::text);
		hots_.push_back(Hot{std::string("place:") + pl.where, wlr_box{x, y, cw, ch}, ch / 2.0});
		x += cw + 6;
	}
	y += ch + 18;
	if (recent_.empty()) {
		draw_text(cr, "Recent", kPad + 2, y, 200, 22, theme::font_title, theme::text_dim);
		draw_text(cr, "Files you open show up here", kPad + 2, y + 26, w_ - 2 * kPad, 22, theme::font_small,
			theme::text_dim);
	}

	// Recent files, newest first.
	if (!recent_.empty()) {
		draw_text(cr, "Recent", kPad + 2, y, 200, 22, theme::font_title, theme::text_dim);
		y += 26;
		const int limit = h_ - kFooterHeight - 6;
		for (size_t k = 0; k < recent_.size(); k++) {
			const Recent &rc = recent_[k];
			const int rh = 50;
			if (y + rh > limit) break;
			fill_rounded(cr, kPad + 8, y + 8, 34, 34, 10, with_alpha(theme::text, 0.07));
			draw_icon(cr, server_->icons, file_icon(rc.name, false), rc.name, kPad + 15, y + 15, 20, scale);
			const double tx = kPad + 54, tw = w_ - 2 * kPad - 64;
			draw_text(cr, rc.name, tx, y + 6, tw, 20, theme::font, theme::text);
			draw_text(cr, rc.where + (rc.when.empty() ? "" : " · " + rc.when), tx, y + 26, tw, 17, theme::font_small,
				theme::text_dim);
			hots_.push_back(Hot{"recent:" + std::to_string(k), wlr_box{kPad, y, w_ - 2 * kPad, rh}, r});
			y += rh + 2;
		}
	}
}

// Along the bottom: you (and how long it's been on), Settings, lock, power.
void Launcher::draw_footer(cairo_t *cr, float scale) {
	const int fy = h_ - kFooterHeight;
	cairo_rectangle(cr, kPad, fy, w_ - 2 * kPad, 1);
	set_color(cr, theme::divider);
	cairo_fill(cr);
	const double cy = fy + kFooterHeight / 2.0 + 1;
	const double as = 36;
	cairo_new_path(cr);
	cairo_arc(cr, kPad + 4 + as / 2, cy, as / 2, 0, 2 * M_PI);
	set_color(cr, theme::accent);
	cairo_fill(cr);
	draw_text(cr, user_name_.substr(0, 1), kPad + 4, cy - as / 2, as, as, theme::font_bold, theme::on_accent, Align::Center);
	const SystemStatus &st = *server_->status;
	std::string sub = "Up " + up_for();
	if (st.has_battery) sub += " · " + std::to_string(st.battery) + "% battery";
	const double tx = kPad + 4 + as + 10, tw = w_ - 2 * kPad - 3 * 44 - as - 20;
	draw_text(cr, user_name_, tx, cy - 19, tw, 20, theme::font_bold, theme::text);
	draw_text(cr, sub, tx, cy + 1, tw, 17, theme::font_small, theme::text_dim);
	hots_.push_back(Hot{"me", wlr_box{kPad, static_cast<int>(cy - 22), static_cast<int>(tw + as + 14), 44}, 22});
	std::vector<std::pair<const char *, const char *>> buttons = {{"settings", "gear"}, {"lock", "lock"}};
	if (server_->config.menu_power) buttons.push_back({"power", "power"});
	const int bs = 36;
	int bx = w_ - kPad - static_cast<int>(buttons.size()) * (bs + 8) + 8;
	for (const auto &[id, icon] : buttons) {
		const int by = static_cast<int>(cy - bs / 2.0);
		fill_rounded(cr, bx, by, bs, bs, bs / 2.0, with_alpha(theme::text, 0.06));
		draw_named_icon(cr, server_->icons, icon, bx + (bs - 18) / 2.0, by + (bs - 18) / 2.0, 18, theme::text, scale);
		hots_.push_back(Hot{id, wlr_box{bx, by, bs, bs}, bs / 2.0});
		bx += bs + 8;
	}
}

// Windows 11's Start, GNOME's app grid and macOS's Launchpad: the apps as
// big icons with their names under them.
void Launcher::redraw_grid() {
	drawing_ = true;
	const auto &apps = server_->apps.apps();
	const float scale = output_->output->scale;
	Canvas canvas(w_, h_, scale);
	cairo_t *cr = canvas.cr();
	const bool win = look_ == "windows", mac = look_ == "macos";
	double radius = 0;
	Color fg = theme::text, dim = theme::text_dim;
	if (win) {
		radius = std::min<double>(theme::ui_radius, 10.0);
		ui_card(cr, 0, 0, w_, h_, radius, theme::menu_bg);
	} else {
		// Over everything (blurred behind), darkened so white names read on any wallpaper.
		// (The backdrop behind it is blurred and tinted: see place_backdrop.)
		fg = theme::dark ? rgb(0xffffff) : rgb(0x1c1c21);
		dim = with_alpha(fg, 0.65);
	}

	// The search field.
	const wlr_box &sb = search_;
	const double sr = win ? sb.height / 2.0 : mac ? 8 : sb.height / 2.0;
	fill_rounded(cr, sb.x, sb.y, sb.width, sb.height, sr,
		win ? theme::field_bg : theme::dark ? rgb(0xffffff, mac ? 0.16 : 0.12) : rgb(0x000000, mac ? 0.08 : 0.06));
	const bool centred = !win && query_.empty();
	const char *font = mac ? theme::font : theme::font_large;
	const std::string hint = win ? "Search for apps, files and folders" : mac ? "Search" : "Type to search apps and files";
	const int hint_w = text_width(query_.empty() ? hint : query_, font);
	const double gx = centred ? sb.x + (sb.width - hint_w - 24) / 2.0 : sb.x + 12;
	draw_named_icon(cr, server_->icons, "search", gx, sb.y + sb.height / 2.0 - 8, 16, dim, scale);
	const double tx = gx + 24;
	draw_text(cr, query_.empty() ? hint : query_, tx, sb.y, sb.x + sb.width - tx - 10, sb.height, font,
		query_.empty() ? dim : fg);
	if (!query_.empty()) {
		const double cx = tx + (query_.empty() ? 0 : text_width(query_, font)) + 1;
		if (cx < sb.x + sb.width - 12) {
			cairo_rectangle(cr, cx, sb.y + 9, 2, sb.height - 18);
			set_color(cr, win ? theme::accent : fg);
			cairo_fill(cr);
		}
	}
	draw_favs(cr, scale, fg);
	if (win) {
		draw_text(cr, query_.empty() ? "All apps" : "Best matches", grid_.x + 8, grid_.y - 34, 200, 24,
			theme::font_bold, theme::text);
	}

	// The apps (and, when searching, files and folders).
	if (count() == 0) {
		draw_text(cr, apps.empty() ? "No apps found" : query_.size() >= 2 ? "Looking…" : "No matches", grid_.x,
			grid_.y + 20, grid_.width, 40, theme::font, dim, Align::Center);
	}
	fx_.set("scroll", first_row_ * cell_h_, 0);
	const double scroll = fx_.get("scroll");
	const int area_h = grid_rows_ * cell_h_;
	cairo_save(cr);
	cairo_rectangle(cr, 0, grid_.y - 2, w_, area_h + 4);
	cairo_clip(cr);
	const int first = std::max(0, static_cast<int>(scroll / cell_h_)) * cols_;
	const int last = std::min(count(), first + (grid_rows_ + 2) * cols_);
	const char *name_font = win ? theme::font_small : theme::font;
	for (int i = first; i < last; i++) {
		const std::string name = item_name(i);
		const wlr_box b = item_box(i, scroll);
		// Files a little smaller than apps.
		const double is = is_file(i) ? icon_ * 0.8 : icon_;
		const double iy = b.y + (win ? 12 : 10) + (icon_ - is) / 2;
		draw_icon(cr, server_->icons, item_icon(i), name, b.x + (b.width - is) / 2.0, iy, is, scale);
		draw_text(cr, name, b.x + 6, b.y + (win ? 12 : 10) + icon_ + (win ? 6 : 8), b.width - 12, 20, name_font,
			is_file(i) ? dim : fg, Align::Center);
	}
	cairo_restore(cr);

	// More below (or above): a soft hint at the edge, and page dots for GNOME / macOS.
	const int pages = (max_first() + grid_rows_) / std::max(1, grid_rows_);
	if (!win && pages > 1) {
		const int page = std::min(pages - 1, (first_row_ + grid_rows_ / 2) / std::max(1, grid_rows_));
		const double dy = dots_y_, step = 16;
		const double x0 = w_ / 2.0 - (pages - 1) * step / 2;
		for (int k = 0; k < pages; k++) {
			cairo_new_path(cr);
			cairo_arc(cr, x0 + k * step, dy, k == page ? 4 : 3, 0, 2 * M_PI);
			cairo_set_source_rgba(cr, 1, 1, 1, k == page ? 0.95 : 0.4);
			cairo_fill(cr);
		}
	} else if (win && max_first() > 0) {
		const double track = area_h;
		const int total_rows = max_first() + grid_rows_;
		const double thumb = std::max(24.0, track * grid_rows_ / total_rows);
		const double pos = (track - thumb) * (scroll / cell_h_) / std::max(1, max_first());
		fill_rounded(cr, w_ - 10, grid_.y + pos, 4, thumb, 2, with_alpha(theme::text, 0.3));
	}

	// Windows 11: you, Settings and the power button along the bottom.
	if (win) {
		const int fy = h_ - 64;
		cairo_save(cr);
		rounded_rect(cr, 0, 0, w_, h_, radius);
		cairo_clip(cr);
		cairo_rectangle(cr, 0, fy, w_, 64);
		set_color(cr, with_alpha(theme::text, theme::dark ? 0.05 : 0.04));
		cairo_fill(cr);
		cairo_rectangle(cr, 0, fy, w_, 1);
		set_color(cr, theme::divider);
		cairo_fill(cr);
		cairo_restore(cr);
		const double ax = 28, ay = fy + 16, as = 32;
		cairo_new_path(cr);
		cairo_arc(cr, ax + as / 2, ay + as / 2, as / 2, 0, 2 * M_PI);
		set_color(cr, theme::accent);
		cairo_fill(cr);
		const std::string initial = user_name_.empty() ? "?" : user_name_.substr(0, 1);
		draw_text(cr, initial, ax, ay, as, as, theme::font_bold, theme::on_accent, Align::Center);
		draw_text(cr, user_name_, ax + as + 12, fy, 220, 64, theme::font, theme::text);
		const wlr_box st = settings_box();
		draw_named_icon(cr, server_->icons, "gear", st.x + 10, st.y + 10, 20, theme::text, scale);
		if (server_->config.menu_power) {
			const wlr_box p = power_box();
			draw_named_icon(cr, server_->icons, "power", p.x + 10, p.y + 10, 20, theme::text, scale);
		}
	}
	layer_.present(canvas, wlr_box{0, 0, w_, h_}, radius);
	drawing_ = false;
	update_highlights();
}

} // namespace dusk
