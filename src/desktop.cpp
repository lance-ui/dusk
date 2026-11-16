#include "desktop.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <linux/input-event-codes.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clipboard.hpp"
#include "launcher.hpp"
#include "notify.hpp"
#include "draw.hpp"
#include "glyphs.hpp"
#include "popover.hpp"
#include "trash.hpp"

namespace dusk {

namespace {

constexpr int kCellW = 96;
constexpr int kCellH = 106;
constexpr int kIcon = 52;
constexpr int kMargin = 14;
constexpr off_t kThumbMax = 25 << 20;   // pictures bigger than this get a plain icon
constexpr int kMaxThumbs = 60;

std::string home_dir() {
	const char *home = std::getenv("HOME");
	return home ? home : "/tmp";
}

// The Desktop folder (localised name from user-dirs.dirs, like Files uses).
std::string desktop_dir() {
	const std::string home = home_dir();
	std::string dir = home + "/Desktop";
	const char *xdg = std::getenv("XDG_CONFIG_HOME");
	const std::string file = (xdg && *xdg ? std::string(xdg) : home + "/.config") + "/user-dirs.dirs";
	std::ifstream f(file);
	std::string line;
	while (std::getline(f, line)) {
		if (line.rfind("XDG_DESKTOP_DIR=\"", 0) != 0) continue;
		std::string v = line.substr(17);
		v = v.substr(0, v.find('"'));
		if (v.rfind("$HOME", 0) == 0) v = home + v.substr(5);
		// Some set-ups point the desktop at the home folder itself; showing
		// all of that on the wallpaper would be a mess.
		if (!v.empty() && v != home && v != home + "/") dir = v;
	}
	while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
	return dir;
}

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

std::string extension(const std::string &name) {
	const size_t dot = name.rfind('.');
	return dot == std::string::npos || dot == 0 ? "" : lower(name.substr(dot + 1));
}

// Icon theme names for a file, by its extension (the icon theme draws them).
std::vector<std::string> file_icons(const std::string &name, bool *image) {
	const std::string ext = extension(name);
	auto in = [&ext](std::initializer_list<const char *> list) {
		for (const char *e : list) if (ext == e) return true;
		return false;
	};
	*image = false;
	if (in({"png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "tif", "tiff", "avif", "jxl", "heic", "ico"})) {
		*image = true;
		return {"image-x-generic"};
	}
	if (in({"mp4", "mkv", "webm", "mov", "avi", "m4v", "wmv", "flv", "mpg", "mpeg"})) return {"video-x-generic"};
	if (in({"mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", "wma"})) return {"audio-x-generic"};
	if (ext == "pdf") return {"application-pdf", "x-office-document"};
	if (in({"zip", "tar", "gz", "tgz", "xz", "bz2", "7z", "rar", "zst", "lz", "lzma"}))
		return {"package-x-generic", "application-x-archive"};
	if (in({"odt", "doc", "docx", "rtf", "pages"})) return {"x-office-document"};
	if (in({"ods", "xls", "xlsx", "csv", "numbers"})) return {"x-office-spreadsheet"};
	if (in({"odp", "ppt", "pptx", "key"})) return {"x-office-presentation"};
	if (in({"sh", "py", "js", "ts", "c", "cpp", "h", "hpp", "rs", "go", "java", "rb", "pl", "lua"}))
		return {"text-x-script", "text-x-generic"};
	if (in({"html", "htm"})) return {"text-html", "text-x-generic"};
	if (in({"iso", "img"})) return {"media-optical", "application-x-cd-image"};
	if (in({"appimage", "run", "bin", "exe"})) return {"application-x-executable"};
	if (in({"rpm", "deb", "flatpakref"})) return {"package-x-generic"};
	if (in({"ttf", "otf", "woff", "woff2"})) return {"font-x-generic"};
	return {"text-x-generic"};
}

// Name and icon from a .desktop launcher.
void read_launcher(const std::string &path, std::string *name, std::string *icon) {
	std::ifstream f(path);
	std::string line;
	bool entry = false;
	while (std::getline(f, line)) {
		if (!line.empty() && line[0] == '[') {
			entry = line == "[Desktop Entry]";
			continue;
		}
		if (!entry) continue;
		if (line.rfind("Name=", 0) == 0 && name->empty()) *name = line.substr(5);
		else if (line.rfind("Icon=", 0) == 0 && icon->empty()) *icon = line.substr(5);
	}
}

// The file's name, centred and wrapped onto `lines` lines, in white with a
// dark edge so it reads on any wallpaper.
void label_text(cairo_t *cr, const std::string &text, double x, double y, double w, int lines,
		const Color &color, bool halo) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(theme::font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text.c_str(), -1);
	pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_height(layout, -lines);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	if (halo) {
		cairo_save(cr);
		cairo_move_to(cr, x, y + 0.5);
		pango_cairo_layout_path(cr, layout);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
		cairo_set_line_width(cr, 3.2);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke(cr);
		cairo_restore(cr);
	}
	set_color(cr, color);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

[[maybe_unused]] std::string utf8_pop(std::string s) {
	if (s.empty()) return s;
	size_t cut = s.size() - 1;
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xc0) == 0x80) cut--;
	return s.substr(0, cut);
}

bool exists(const std::string &path) {
	struct stat st{};
	return lstat(path.c_str(), &st) == 0;
}

std::string positions_file() {
	return Config::directory() + "/desktop-icons";
}

} // namespace

Desktop::Desktop(Server *server) : server_(server), dir_(desktop_dir()) {
	tree_ = wlr_scene_tree_create(server->layer_desktop);
	tree_->node.data = &tag;
	// Make sure the folder is there (so dropping onto the desktop works).
	for (size_t pos = dir_.find('/', 1);; pos = dir_.find('/', pos + 1)) {
		mkdir(dir_.substr(0, pos).c_str(), 0755);
		if (pos == std::string::npos) break;
	}
	rescan_timer_ = wl_event_loop_add_timer(server->loop, &Desktop::handle_rescan, this);
	// The Trash icon shows whether there's something in it.
	if (server->trash) {
		server->trash->changed.push_back([this] {
			for (Item &it : items_) {
				if (it.special == "trash") draw_item(it);
			}
		});
	}
	load_positions();
	watch();
	scan();
}

Desktop::~Desktop() {
	clear_items();
	if (inotify_source_) wl_event_source_remove(inotify_source_);
	if (inotify_fd_ >= 0) close(inotify_fd_);
	if (rescan_timer_) wl_event_source_remove(rescan_timer_);
	wlr_scene_node_destroy(&tree_->node);
}

void Desktop::watch() {
	inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd_ < 0) return;
	if (inotify_add_watch(inotify_fd_, dir_.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
			IN_CLOSE_WRITE | IN_ATTRIB) < 0) {
		close(inotify_fd_);
		inotify_fd_ = -1;
		return;
	}
	inotify_source_ = wl_event_loop_add_fd(server_->loop, inotify_fd_, WL_EVENT_READABLE,
		&Desktop::handle_inotify, this);
}

int Desktop::handle_inotify(int fd, uint32_t, void *data) {
	auto *self = static_cast<Desktop *>(data);
	alignas(inotify_event) char buffer[4096];
	while (read(fd, buffer, sizeof buffer) > 0) {
	}
	// Several changes usually come together (a copy, a download): look once.
	wl_event_source_timer_update(self->rescan_timer_, 150);
	return 0;
}

int Desktop::handle_rescan(void *data) {
	static_cast<Desktop *>(data)->scan();
	return 0;
}

Output *Desktop::screen() const {
	for (Output *o : server_->outputs) {
		if (o->enabled() && o->is_primary()) return o;
	}
	for (Output *o : server_->outputs) {
		if (o->enabled()) return o;
	}
	return nullptr;
}

void Desktop::clear_items() {
	for (Item &it : items_) {
		if (it.node) wlr_scene_node_destroy(&it.node->node);
		if (it.thumb) cairo_surface_destroy(it.thumb);
	}
	items_.clear();
}

void Desktop::output_gone(Output *output) {
	if (output_ == output) {
		output_ = nullptr;
		wlr_scene_node_set_enabled(&tree_->node, false);
	}
}

// ---- the folder -----------------------------------------------------------------

void Desktop::scan() {
	std::vector<Item> next;
	if (DIR *d = opendir(dir_.c_str())) {
		while (dirent *e = readdir(d)) {
			const std::string name = e->d_name;
			if (name.empty() || name[0] == '.') continue;   // hidden
			Item it;
			it.name = name;
			it.path = dir_ + "/" + name;
			struct stat st{};
			if (stat(it.path.c_str(), &st) != 0 && lstat(it.path.c_str(), &st) != 0) continue;
			it.dir = S_ISDIR(st.st_mode);
			it.mtime = st.st_mtime;
			it.label = name;
			if (it.dir) {
				it.icon = "folder";
			} else if (extension(name) == "desktop") {
				it.app = true;
				std::string label, icon;
				read_launcher(it.path, &label, &icon);
				if (!label.empty()) it.label = label;
				it.icon = icon.empty() ? "application-x-executable" : icon;
			} else {
				bool image = false;
				it.icon = file_icons(name, &image).front();
				it.image = image && st.st_size <= kThumbMax;
			}
			next.push_back(std::move(it));
		}
		closedir(d);
	}
	// Home and Trash, like GNOME's desktop icons (Settings > Desktop).
	if (server_->config.desktop_home) {
		Item it;
		it.name = ":home";
		it.special = "home";
		it.path = home_dir();
		it.label = "Home";
		it.icon = "user-home";
		it.dir = true;
		next.push_back(std::move(it));
	}
	if (server_->config.desktop_trash) {
		Item it;
		it.name = ":trash";
		it.special = "trash";
		it.label = "Trash";
		it.icon = "user-trash";
		next.push_back(std::move(it));
	}
	// Keep what hasn't changed (its picture and thumbnail).
	for (Item &n : next) {
		for (Item &old : items_) {
			if (old.name == n.name && old.node) {
				n.node = old.node;
				old.node = nullptr;
				if (old.mtime == n.mtime) {
					n.thumb = old.thumb;
					old.thumb = nullptr;
				}
				break;
			}
		}
	}
	clear_items();   // what's left over is gone
	items_ = std::move(next);
	applied_.clear();   // names, icons or contents may have changed: draw them all again
	// Home and Trash first, then folders, then by name.
	std::sort(items_.begin(), items_.end(), [](const Item &a, const Item &b) {
		if (a.special.empty() != b.special.empty()) return !a.special.empty();
		if (a.special != b.special) return a.special == "home";
		if (a.dir != b.dir) return a.dir;
		return lower(a.label) < lower(b.label);
	});
	for (auto it = selected_.begin(); it != selected_.end();) {
		const std::string n = *it;
		it = std::any_of(items_.begin(), items_.end(), [&n](const Item &x) { return x.name == n; })
			? std::next(it) : selected_.erase(it);
	}
	if (!renaming_.empty() && std::none_of(items_.begin(), items_.end(),
			[this](const Item &x) { return x.name == renaming_; })) {
		renaming_.clear();
	}
	relayout();
	if (!rename_when_seen_.empty()) {
		const std::string name = rename_when_seen_;
		if (std::any_of(items_.begin(), items_.end(), [&name](const Item &x) { return x.name == name; })) {
			rename_when_seen_.clear();
			selected_ = {name};
			start_rename(name);
		}
	}
}

// ---- the grid -------------------------------------------------------------------

void Desktop::relayout() {
	output_ = enabled() ? screen() : nullptr;
	wlr_scene_node_set_enabled(&tree_->node, output_ != nullptr);
	if (!output_) {
		return;
	}
	const wlr_box u = output_->usable();
	area_ = wlr_box{u.x + kMargin, u.y + kMargin, std::max(kCellW, u.width - 2 * kMargin),
		std::max(kCellH, u.height - 2 * kMargin)};
	cols_ = std::max(1, area_.width / kCellW);
	rows_ = std::max(1, area_.height / kCellH);
	place();
	// Redraw when the screen's scale or the look changed.
	const std::string key = std::to_string(output_->output->scale) + theme::font + std::to_string(theme::dark) +
		std::to_string(theme::accent.r) + std::to_string(theme::accent.g) + std::to_string(theme::accent.b);
	const bool all = key != applied_;
	applied_ = key;
	for (Item &it : items_) {
		if (all || !it.node) draw_item(it);
		const wlr_box b = cell_box(it.col, it.row);
		wlr_scene_node_set_position(&it.node->node, b.x, b.y);
	}
}

wlr_box Desktop::cell_box(int col, int row) const {
	return wlr_box{area_.x + col * kCellW, area_.y + row * kCellH, kCellW, kCellH};
}

bool Desktop::cell_from_point(double lx, double ly, int *col, int *row) const {
	if (!output_) return false;
	*col = std::clamp(static_cast<int>(std::floor((lx - area_.x) / kCellW)), 0, cols_ - 1);
	*row = std::clamp(static_cast<int>(std::floor((ly - area_.y) / kCellH)), 0, rows_ - 1);
	return true;
}

bool Desktop::cell_free(int col, int row, const Item *except) const {
	for (const Item &it : items_) {
		if (&it != except && it.col == col && it.row == row) return false;
	}
	return true;
}

// The free cell nearest to (col, row).
void Desktop::nearest_free(int *col, int *row, const Item *except) const {
	int best_c = *col, best_r = *row;
	double best = 1e9;
	for (int c = 0; c < cols_; c++) {
		for (int r = 0; r < rows_; r++) {
			if (!cell_free(c, r, except)) continue;
			const double d = std::hypot(c - *col, (r - *row) * 1.1);
			if (d < best) {
				best = d;
				best_c = c;
				best_r = r;
			}
		}
	}
	*col = best_c;
	*row = best_r;
}

// Saved places first, then new arrivals near where they were dropped, then
// the rest down the columns from the top left (like other desktops).
void Desktop::place() {
	for (Item &it : items_) {
		it.col = it.row = -1;
	}
	bool saved_any = false;
	for (Item &it : items_) {
		auto s = saved_.find(it.name);
		if (s != saved_.end() && s->second.first < cols_ && s->second.second < rows_ &&
				cell_free(s->second.first, s->second.second, &it)) {
			it.col = s->second.first;
			it.row = s->second.second;
		}
	}
	for (Item &it : items_) {
		auto p = pending_.find(it.name);
		if (it.col >= 0 || p == pending_.end()) continue;
		int c = std::clamp(p->second.first, 0, cols_ - 1), r = std::clamp(p->second.second, 0, rows_ - 1);
		nearest_free(&c, &r, &it);
		it.col = c;
		it.row = r;
		saved_[it.name] = {c, r};
		pending_.erase(p);
		saved_any = true;
	}
	int next = 0;
	for (Item &it : items_) {
		if (it.col >= 0) continue;
		while (next < cols_ * rows_ && !cell_free(next / rows_, next % rows_, &it)) next++;
		if (next < cols_ * rows_) {
			it.col = next / rows_;
			it.row = next % rows_;
		} else {
			it.col = cols_ - 1;   // full: they pile up in the last cell
			it.row = rows_ - 1;
		}
	}
	if (saved_any) save_positions();
}

void Desktop::load_positions() {
	std::ifstream f(positions_file());
	std::string line;
	while (std::getline(f, line)) {
		int c = 0, r = 0, used = 0;
		if (std::sscanf(line.c_str(), "%d %d %n", &c, &r, &used) >= 2 && used > 0 &&
				static_cast<size_t>(used) < line.size() && c >= 0 && r >= 0) {
			saved_[line.substr(static_cast<size_t>(used))] = {c, r};
		}
	}
}

void Desktop::save_positions() {
	const std::string path = positions_file();
	const std::string tmp = path + ".tmp";
	{
		std::ofstream f(tmp);
		f << "# Where icons are on the desktop: column row name (written by Dusk)\n";
		for (const auto &[name, cell] : saved_) {
			// Only files that are still there.
			if (std::any_of(items_.begin(), items_.end(), [&name = name](const Item &x) { return x.name == name; })) {
				f << cell.first << ' ' << cell.second << ' ' << name << '\n';
			}
		}
	}
	std::rename(tmp.c_str(), path.c_str());
}

// ---- drawing ---------------------------------------------------------------------

void Desktop::draw_item(Item &it) {
	if (!output_) return;
	if (!it.node) {
		it.node = wlr_scene_buffer_create(tree_, nullptr);
	}
	const float scale = output_->output->scale;
	Canvas canvas(kCellW, kCellH, scale);
	cairo_t *cr = canvas.cr();
	const bool selected = selected_.count(it.name) > 0;
	const bool hover = hover_ == it.name;
	const bool renaming = renaming_ == it.name;

	if (selected) {
		fill_rounded(cr, 3, 2, kCellW - 6, kCellH - 4, 12, with_alpha(theme::accent, 0.34));
		rounded_rect(cr, 3.5, 2.5, kCellW - 7, kCellH - 5, 12);
		set_color(cr, with_alpha(theme::accent, 0.75));
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else if (hover) {
		fill_rounded(cr, 3, 2, kCellW - 6, kCellH - 4, 12, rgb(0xffffff, 0.14));
	}

	// The icon: a picture's own thumbnail, else the icon theme's.
	const double ix = (kCellW - kIcon) / 2.0, iy = 8;
	const int px = static_cast<int>(std::lround(kIcon * scale));
	if (it.image && !it.thumb) {
		int thumbs = 0;
		for (const Item &x : items_) thumbs += x.thumb != nullptr;
		if (thumbs < kMaxThumbs) it.thumb = load_image_sized(it.path, px, px);
		if (!it.thumb) it.image = false;
	}
	if (it.thumb) {
		const double tw = cairo_image_surface_get_width(it.thumb), th = cairo_image_surface_get_height(it.thumb);
		const double s = std::min(kIcon / tw, kIcon / th);
		const double w = tw * s, h = th * s;
		const double x = ix + (kIcon - w) / 2, y = iy + (kIcon - h) / 2;
		cairo_save(cr);
		rounded_rect(cr, x, y, w, h, 5);
		cairo_clip_preserve(cr);
		cairo_translate(cr, x, y);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, it.thumb, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 5);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else {
		bool image = false;
		std::vector<std::string> names = {it.icon};
		if (it.special == "trash") {
			names = server_->trash && !server_->trash->is_empty()
				? std::vector<std::string>{"user-trash-full", "user-trash"} : std::vector<std::string>{"user-trash"};
		}
		if (!it.dir && !it.app) {
			for (const std::string &n : file_icons(it.name, &image)) names.push_back(n);
		}
		names.push_back(it.dir ? "folder" : "text-x-generic");
		bool drawn = false;
		for (const std::string &n : names) {
			if (cairo_surface_t *icon = server_->icons.get(n, px)) {
				const double iw = cairo_image_surface_get_width(icon), ih = cairo_image_surface_get_height(icon);
				const double s = kIcon / std::max(iw, ih);
				cairo_save(cr);
				cairo_translate(cr, ix + (kIcon - iw * s) / 2, iy + (kIcon - ih * s) / 2);
				cairo_scale(cr, s, s);
				cairo_set_source_surface(cr, icon, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
				cairo_paint(cr);
				cairo_restore(cr);
				drawn = true;
				break;
			}
		}
		if (!drawn) {
			draw_glyph(cr, it.special == "trash" ? "trash" : it.special == "home" ? "house" : it.dir ? "folder" : "file",
				kCellW / 2.0, iy + kIcon / 2.0, kIcon * 0.8, rgb(0xffffff, 0.92));
		}
	}

	// The name, or the box you type a new one in.
	const double ty = iy + kIcon + 6;
	if (renaming) {
		const double fx = 2, fw = kCellW - 4, fh = 24;
		fill_rounded(cr, fx, ty - 2, fw, fh, 7, rgb(0xffffff, 0.96));
		rounded_rect(cr, fx + 0.5, ty - 1.5, fw - 1, fh - 1, 7);
		set_color(cr, rename_error_.empty() ? theme::accent : theme::danger);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		// The text, its end shown if it's long; the part a key would replace
		// is highlighted until you type.
		std::string shown = rename_text_;
		size_t cut_total = 0;   // bytes dropped from the front (long names show their end)
		const int room = static_cast<int>(fw - 14);
		while (shown.size() > 1 && text_width(shown, theme::font_small) > room && cut_total < rename_cursor_) {
			size_t cut = 1;
			while (cut < shown.size() && (static_cast<unsigned char>(shown[cut]) & 0xc0) == 0x80) cut++;
			shown.erase(0, cut);
			cut_total += cut;
		}
		const int tw = std::min(room, text_width(shown, theme::font_small));
		const double tx = fx + (fw - tw) / 2.0;
		if (rename_fresh_ && !shown.empty()) {
			std::string base = shown;
			const size_t dot = base.rfind('.');
			if (!it.dir && dot != std::string::npos && dot > 0) base = base.substr(0, dot);
			fill_rounded(cr, tx - 1, ty + 2, std::min(tw, text_width(base, theme::font_small)) + 2, fh - 8, 3,
				with_alpha(theme::accent, 0.35));
		}
		draw_text(cr, shown, tx, ty - 2, tw + 2, fh, theme::font_small, rgb(0x1c1c21));
		if (!rename_fresh_) {
			const size_t at = rename_cursor_ >= cut_total ? rename_cursor_ - cut_total : 0;
			const int cx = std::min(tw, text_width(shown.substr(0, std::min(at, shown.size())), theme::font_small));
			cairo_rectangle(cr, tx + cx, ty + 2, 1.5, fh - 8);
			set_color(cr, theme::accent);
			cairo_fill(cr);
		}
		if (!rename_error_.empty()) {
			label_text(cr, rename_error_, 2, ty + fh + 1, kCellW - 4, 1, rgb(0xffb4b4), true);
		}
	} else {
		label_text(cr, it.label, 5, ty, kCellW - 10, selected ? 3 : 2, rgb(0xffffff), true);
	}
	canvas.present(it.node);
}

void Desktop::draw_all() {
	for (Item &it : items_) draw_item(it);
}

void Desktop::settings_changed(bool look) {
	if (!enabled()) {
		selected_.clear();
		renaming_.clear();
		keys_ = false;
	}
	if (look) applied_.clear();
	scan();   // Home and Trash may have been switched on or off
}

// ---- the pointer -------------------------------------------------------------------

int Desktop::item_at(double lx, double ly) const {
	if (!output_) return -1;
	for (size_t i = 0; i < items_.size(); i++) {
		const wlr_box b = cell_box(items_[i].col, items_[i].row);
		if (lx >= b.x && ly >= b.y && lx < b.x + b.width && ly < b.y + b.height) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void Desktop::motion(double lx, double ly, bool hit) {
	if (!pressed_.empty()) {
		// Dragging icons: they follow the pointer, keeping their places
		// relative to each other.
		if (!dragging_ && std::hypot(lx - press_x_, ly - press_y_) >= 6) {
			dragging_ = true;
			if (!selected_.count(pressed_)) selected_ = {pressed_};
			for (Item &it : items_) {
				if (selected_.count(it.name)) {
					wlr_scene_node_raise_to_top(&it.node->node);
					wlr_scene_buffer_set_opacity(it.node, 0.8f);
				}
			}
		}
		if (dragging_) {
			// Over an app's window: it becomes a real drag the app can take
			// (a file manager, a mail, an editor...).
			const Hit hit = server_->hit_test(lx, ly);
			if (hit.surface && hit.view && !selected_paths().empty()) {
				wlr_buffer *picture = nullptr;
				int gx = 0, gy = 0;
				for (const Item &it : items_) {
					if (it.name == pressed_ && it.node) {
						picture = it.node->buffer;
						const wlr_box b = cell_box(it.col, it.row);
						gx = static_cast<int>(std::lround(press_x_)) - b.x;
						gy = static_cast<int>(std::lround(press_y_)) - b.y;
					}
				}
				const std::vector<std::string> paths = selected_paths();
				// Not ours any more while the drag starts (it moves the pointer focus).
				server_->desktop_grab = false;
				if (server_->start_file_drag(paths, picture, gx, gy, hit.surface)) {
					pressed_.clear();
					dragging_ = false;
					for (Item &it : items_) {
						if (it.node) wlr_scene_buffer_set_opacity(it.node, 1.0f);
					}
					relayout();   // the icons go back to their places meanwhile
					return;
				}
				server_->desktop_grab = true;
			}
			const int dx = static_cast<int>(std::lround(lx - press_x_)), dy = static_cast<int>(std::lround(ly - press_y_));
			for (Item &it : items_) {
				if (!selected_.count(it.name)) continue;
				const wlr_box b = cell_box(it.col, it.row);
				wlr_scene_node_set_position(&it.node->node, b.x + dx, b.y + dy);
			}
		}
		return;
	}
	const int i = hit ? item_at(lx, ly) : -1;
	const std::string now = i >= 0 ? items_[static_cast<size_t>(i)].name : "";
	if (now == hover_) return;
	const std::string before = hover_;
	hover_ = now;
	for (Item &it : items_) {
		if (it.name == before || it.name == now) draw_item(it);
	}
}

void Desktop::press(double lx, double ly, uint32_t button, uint32_t time_msec, bool ctrl) {
	int i = item_at(lx, ly);
	keys_ = true;
	if (!renaming_.empty() && (i < 0 || items_[static_cast<size_t>(i)].name != renaming_)) {
		finish_rename(true, false);
		i = item_at(lx, ly);   // the folder was read again: the list may be in a new order
	}
	if (i < 0) {
		click_empty();
		return;
	}
	Item &it = items_[static_cast<size_t>(i)];
	const std::set<std::string> before = selected_;
	if (button == BTN_RIGHT) {
		if (!selected_.count(it.name)) selected_ = {it.name};
	} else if (ctrl && button == BTN_LEFT) {
		if (!selected_.erase(it.name)) selected_.insert(it.name);
	} else if (!selected_.count(it.name)) {
		selected_ = {it.name};
	}
	if (selected_ != before) draw_all();

	if (button == BTN_RIGHT) {
		show_item_menu(i, lx, ly);
		return;
	}
	if (button == BTN_MIDDLE) {
		open_item(it);
		return;
	}
	if (button != BTN_LEFT || ctrl) return;
	const bool double_click = last_click_ == it.name && time_msec - last_click_time_ < 400;
	last_click_ = it.name;
	last_click_time_ = time_msec;
	if (double_click) {
		last_click_.clear();
		for (const Item &x : items_) {
			if (selected_.count(x.name)) open_item(x);
		}
		return;
	}
	pressed_ = it.name;
	press_x_ = lx;
	press_y_ = ly;
	dragging_ = false;
	server_->desktop_grab = true;   // the pointer is ours until it's let go
}

void Desktop::release(double lx, double ly) {
	server_->desktop_grab = false;
	const std::string pressed = pressed_;
	pressed_.clear();
	if (!dragging_) {
		return;
	}
	dragging_ = false;
	for (Item &it : items_) {
		if (it.node) wlr_scene_buffer_set_opacity(it.node, 1.0f);
	}
	// Dropped on a folder: they go into it.
	const int target = item_at(lx, ly);
	if (target >= 0) {
		const Item &t = items_[static_cast<size_t>(target)];
		if (t.special == "trash" && !selected_.count(t.name)) {
			if (server_->trash) server_->trash->put(selected_paths());
			relayout();
			return;
		}
		if (t.dir && !selected_.count(t.name)) {
			move_into(selected_paths(), t.path, false);
			relayout();   // (what moved disappears when the folder is read again)
			return;
		}
	}
	// Otherwise they move by as many cells as the pointer moved.
	const Item *lead = nullptr;
	for (const Item &it : items_) {
		if (it.name == pressed) lead = &it;
	}
	int lc = 0, lr = 0;
	if (!lead || !cell_from_point(lx, ly, &lc, &lr)) {
		relayout();
		return;
	}
	const int dc = lc - lead->col, dr = lr - lead->row;
	// Lift them all off the grid first, so they can swap places with each other.
	std::vector<Item *> moving;
	std::map<Item *, std::pair<int, int>> want;
	for (Item &it : items_) {
		if (!selected_.count(it.name)) continue;
		want[&it] = {std::clamp(it.col + dc, 0, cols_ - 1), std::clamp(it.row + dr, 0, rows_ - 1)};
		it.col = it.row = -1;
		moving.push_back(&it);
	}
	for (Item *it : moving) {
		int c = want[it].first, r = want[it].second;
		if (!cell_free(c, r, it)) nearest_free(&c, &r, it);
		it->col = c;
		it->row = r;
		saved_[it->name] = {c, r};
	}
	save_positions();
	relayout();
}

void Desktop::click_empty() {
	keys_ = enabled();
	if (!renaming_.empty()) finish_rename(true, false);
	if (!selected_.empty()) {
		selected_.clear();
		draw_all();
	}
}

// ---- keys ------------------------------------------------------------------------

bool Desktop::wants_keyboard() const {
	return enabled() && output_ && (!renaming_.empty() || (keys_ && server_->focused == nullptr));
}

void Desktop::focus_lost() {
	keys_ = false;
	if (!renaming_.empty()) finish_rename(true, false);
}

bool Desktop::handle_key(xkb_keysym_t sym, uint32_t modifiers, const std::string &text) {
	const bool ctrl = modifiers & WLR_MODIFIER_CTRL;
	if (!renaming_.empty()) {
		auto prev = [this](size_t at) {
			if (at == 0) return at;
			at--;
			while (at > 0 && (static_cast<unsigned char>(rename_text_[at]) & 0xc0) == 0x80) at--;
			return at;
		};
		auto next = [this](size_t at) {
			if (at >= rename_text_.size()) return rename_text_.size();
			at++;
			while (at < rename_text_.size() && (static_cast<unsigned char>(rename_text_[at]) & 0xc0) == 0x80) at++;
			return at;
		};
		// While "fresh", the name before the extension is selected.
		const Item *item = nullptr;
		for (const Item &x : items_) if (x.name == renaming_) item = &x;
		const size_t dot = rename_text_.rfind('.');
		const size_t base_end = item && !item->dir && dot != std::string::npos && dot > 0 ? dot
			: rename_text_.size();
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
			finish_rename(true, true);
			return true;
		} else if (sym == XKB_KEY_Escape) {
			finish_rename(false, false);
			return true;
		} else if (sym == XKB_KEY_BackSpace || sym == XKB_KEY_Delete) {
			if (rename_fresh_) {
				rename_text_.erase(0, base_end);
				rename_cursor_ = 0;
			} else if (sym == XKB_KEY_BackSpace && rename_cursor_ > 0) {
				const size_t from = prev(rename_cursor_);
				rename_text_.erase(from, rename_cursor_ - from);
				rename_cursor_ = from;
			} else if (sym == XKB_KEY_Delete && rename_cursor_ < rename_text_.size()) {
				rename_text_.erase(rename_cursor_, next(rename_cursor_) - rename_cursor_);
			}
			rename_fresh_ = false;
		} else if (sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_Home || sym == XKB_KEY_End) {
			if (rename_fresh_) {
				rename_cursor_ = sym == XKB_KEY_Home ? 0 : sym == XKB_KEY_End ? rename_text_.size() : base_end;
			} else if (sym == XKB_KEY_Left) {
				rename_cursor_ = prev(rename_cursor_);
			} else if (sym == XKB_KEY_Right) {
				rename_cursor_ = next(rename_cursor_);
			} else {
				rename_cursor_ = sym == XKB_KEY_Home ? 0 : rename_text_.size();
			}
			rename_fresh_ = false;
		} else if (!text.empty() && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f && !ctrl) {
			if (text.find('/') != std::string::npos) return true;   // not allowed in names
			if (rename_fresh_) {
				// The new name replaces the old one; the extension stays.
				rename_text_.erase(0, base_end);
				rename_cursor_ = 0;
				rename_fresh_ = false;
			}
			rename_text_.insert(rename_cursor_, text);
			rename_cursor_ += text.size();
		} else {
			return true;   // keys don't reach apps while typing a name
		}
		rename_error_.clear();
		for (Item &it : items_) {
			if (it.name == renaming_) draw_item(it);
		}
		return true;
	}
	const std::vector<std::string> paths = selected_paths();
	switch (sym) {
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		if (!paths.empty()) trash(paths);
		return true;
	case XKB_KEY_F2:
		if (selected_.size() == 1 && selected_.begin()->front() != ':') start_rename(*selected_.begin());
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		for (const Item &x : items_) {
			if (selected_.count(x.name)) open_item(x);
		}
		return true;
	case XKB_KEY_Escape:
		if (!selected_.empty()) {
			selected_.clear();
			draw_all();
		}
		return true;
	default:
		break;
	}
	if (ctrl) {
		const xkb_keysym_t k = xkb_keysym_to_lower(sym);
		if (k == XKB_KEY_a) {
			for (const Item &x : items_) selected_.insert(x.name);
			draw_all();
			return true;
		}
		if ((k == XKB_KEY_c || k == XKB_KEY_x) && !paths.empty() && server_->clipboard) {
			server_->clipboard->copy_files(paths, k == XKB_KEY_x);
			return true;
		}
		if (k == XKB_KEY_v) {
			paste();
			return true;
		}
	}
	return false;
}

// ---- doing things ------------------------------------------------------------------

std::vector<std::string> Desktop::selected_paths() const {
	std::vector<std::string> out;
	for (const Item &it : items_) {
		if (selected_.count(it.name) && it.special.empty()) out.push_back(it.path);
	}
	return out;
}

void Desktop::open_item(const Item &it) {
	if (it.special == "trash") {
		if (server_->trash) server_->trash->open();
		return;
	}
	const std::string q = shell_quote(it.path);
	if (it.app) {
		server_->spawn("gio launch " + q + " 2>/dev/null || xdg-open " + q);
	} else {
		server_->spawn("xdg-open " + q);
	}
}

void Desktop::trash(const std::vector<std::string> &paths) {
	if (server_->trash) server_->trash->put(paths);
}

std::string Desktop::unique_name(const std::string &dir, const std::string &name) const {
	if (!exists(dir + "/" + name)) return name;
	std::string base = name, ext;
	const size_t dot = name.rfind('.');
	struct stat st{};
	const bool is_dir = stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
	if (!is_dir && dot != std::string::npos && dot > 0) {
		base = name.substr(0, dot);
		ext = name.substr(dot);
	}
	for (int n = 2;; n++) {
		const std::string candidate = base + " (" + std::to_string(n) + ")" + ext;
		if (!exists(dir + "/" + candidate)) return candidate;
	}
}

// Moves (or copies) files into a folder; ones already there get " (2)" names.
void Desktop::move_into(const std::vector<std::string> &paths, const std::string &target, bool copy) {
	struct stat target_st{};
	const bool have_target = stat(target.c_str(), &target_st) == 0;
	std::string slow;   // copies, and moves across disks: done by cp/mv in the background
	for (const std::string &src : paths) {
		if (src.empty() || src[0] != '/') continue;   // not a local file
		std::string name = src.substr(src.find_last_of('/') + 1);
		const std::string parent = src.substr(0, src.find_last_of('/'));
		if (!copy && parent == target) continue;          // already there
		if ((target + "/").rfind(src + "/", 0) == 0) continue;   // a folder into itself
		name = unique_name(target, name);
		const std::string dst = target + "/" + name;
		if (target == dir_) {
			if (!pending_.count(name)) pending_[name] = pending_.empty() ? std::pair<int, int>{0, 0}
				: pending_.begin()->second;
		}
		struct stat st{};
		const bool same_disk = have_target && stat(src.c_str(), &st) == 0 && st.st_dev == target_st.st_dev;
		if (!copy && same_disk && std::rename(src.c_str(), dst.c_str()) == 0) {
			continue;
		}
		slow += std::string(slow.empty() ? "" : "; ") + (copy ? "cp -r --reflink=auto -- " : "mv -n -- ") +
			shell_quote(src) + " " + shell_quote(dst);
	}
	if (!slow.empty()) {
		server_->spawn(slow);
	}
}

void Desktop::drop_files(const std::vector<std::string> &paths, bool copy, double lx, double ly) {
	if (!output_) return;
	std::string target = dir_;
	const int i = item_at(lx, ly);
	if (i >= 0 && items_[static_cast<size_t>(i)].special == "trash") {
		if (server_->trash) server_->trash->put(paths);
		return;
	}
	if (i >= 0 && items_[static_cast<size_t>(i)].dir) {
		target = items_[static_cast<size_t>(i)].path;
	} else {
		int c = 0, r = 0;
		cell_from_point(lx, ly, &c, &r);
		for (const std::string &p : paths) {
			const std::string name = unique_name(dir_, p.substr(p.find_last_of('/') + 1));
			pending_[name] = {c, r};
		}
	}
	move_into(paths, target, copy);
	scan();
}

bool Desktop::takes_drop(double lx, double ly) const {
	if (!enabled() || !output_) return false;
	const wlr_box b = output_->box();
	return lx >= b.x && ly >= b.y && lx < b.x + b.width && ly < b.y + b.height;
}

void Desktop::paste() {
	if (!server_->clipboard) return;
	server_->clipboard->get_files([this](std::vector<std::string> paths, bool cut) {
		move_into(paths, dir_, !cut);
		scan();
	});
}

void Desktop::new_folder() {
	const std::string name = unique_name(dir_, "New folder");
	if (mkdir((dir_ + "/" + name).c_str(), 0755) != 0) return;
	if (!pending_.count(name)) pending_[name] = {0, 0};
	rename_when_seen_ = name;
	scan();
}

void Desktop::start_rename(const std::string &name) {
	renaming_ = name;
	rename_text_ = name;
	rename_cursor_ = name.size();
	rename_fresh_ = true;
	rename_error_.clear();
	keys_ = true;
	for (Item &it : items_) {
		if (it.name == name) draw_item(it);
	}
}

// keep: use the typed name. can_stay: a name that's taken leaves the box
// open to fix it (from the keyboard); otherwise it's given up.
void Desktop::finish_rename(bool keep, bool can_stay) {
	if (renaming_.empty()) return;
	const std::string old = renaming_;
	std::string now = rename_text_;
	while (!now.empty() && now.back() == ' ') now.pop_back();
	if (keep && !now.empty() && now != old && now != "." && now != ".." &&
			now.find('/') == std::string::npos) {
		if (exists(dir_ + "/" + now)) {
			if (can_stay) {
				rename_error_ = "Already taken";
				for (Item &it : items_) if (it.name == old) draw_item(it);
				return;
			}
		} else if (std::rename((dir_ + "/" + old).c_str(), (dir_ + "/" + now).c_str()) == 0) {
			if (auto s = saved_.find(old); s != saved_.end()) {
				saved_[now] = s->second;
				saved_.erase(s);
				save_positions();
			} else {
				for (const Item &it : items_) {
					if (it.name == old) pending_[now] = {it.col, it.row};
				}
			}
			if (selected_.erase(old)) selected_.insert(now);
		}
	}
	renaming_.clear();
	rename_error_.clear();
	scan();
}

void Desktop::show_item_menu(int index, double lx, double ly) {
	if (index < 0 || index >= static_cast<int>(items_.size())) return;
	const Item &it = items_[static_cast<size_t>(index)];
	const std::vector<std::string> paths = selected_paths();
	const bool several = paths.size() > 1;
	Server *srv = server_;
	std::vector<MenuItem> items;
	if (!it.special.empty()) {
		const wlr_box at{static_cast<int>(lx), static_cast<int>(ly), 1, 1};
		if (it.special == "trash" && srv->trash) {
			srv->trash->add_menu_items(items, output_, at, "point");
		} else {
			const std::string home = it.path;
			items.push_back(MenuItem::header("Home"));
			items.push_back(MenuItem::action("Open", [srv, home] { srv->spawn("xdg-open " + shell_quote(home)); }, "open"));
		}
		items.push_back(MenuItem::separator());
		const std::string key = it.special == "trash" ? "desktop_trash" : "desktop_home";
		items.push_back(MenuItem::action("Remove from the desktop", [srv, key] { srv->write_setting(key, "false"); }, "minus"));
		srv->close_popups();
		srv->menu->show(output_, at, "point", std::move(items));
		return;
	}
	items.push_back(MenuItem::header(several ? std::to_string(paths.size()) + " items" : it.label));
	items.push_back(MenuItem::action("Open", [this, paths] {
		for (const Item &x : items_) {
			if (std::find(paths.begin(), paths.end(), x.path) != paths.end()) open_item(x);
		}
	}, "open"));
	items.push_back(MenuItem::separator());
	items.push_back(MenuItem::action("Copy", [srv, paths] {
		if (srv->clipboard) srv->clipboard->copy_files(paths, false);
	}, "copy"));
	items.push_back(MenuItem::action("Cut", [srv, paths] {
		if (srv->clipboard) srv->clipboard->copy_files(paths, true);
	}));
	if (!several) {
		const std::string name = it.name;
		items.push_back(MenuItem::action("Rename…", [this, name] { start_rename(name); }, "edit"));
	}
	MenuItem bin = MenuItem::action("Move to the trash", [this, paths] { trash(paths); }, "trash");
	bin.danger = true;
	items.push_back(bin);
	items.push_back(MenuItem::separator());
	const std::string folder = dir_;
	items.push_back(MenuItem::action("Open the Desktop folder",
		[srv, folder] { srv->spawn("xdg-open " + shell_quote(folder)); }, "folder"));
	srv->close_popups();
	srv->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point",
		std::move(items));
}

void Desktop::add_menu_items(std::vector<MenuItem> &items, double lx, double ly) {
	Server *srv = server_;
	const bool on = enabled() && output_;
	if (on) {
		int c = 0, r = 0;
		cell_from_point(lx, ly, &c, &r);
		items.push_back(MenuItem::action("New folder", [this, c, r] {
			const std::string name = unique_name(dir_, "New folder");
			pending_[name] = {c, r};
			new_folder();
		}, "folder-new"));
		MenuItem paste_item = MenuItem::action("Paste", [this, c, r] {
			if (!server_->clipboard) return;
			server_->clipboard->get_files([this, c, r](std::vector<std::string> paths, bool cut) {
				for (const std::string &p : paths) {
					pending_[unique_name(dir_, p.substr(p.find_last_of('/') + 1))] = {c, r};
				}
				move_into(paths, dir_, !cut);
				scan();
			});
		}, "paste");
		paste_item.enabled = srv->clipboard && srv->clipboard->has_files();
		items.push_back(paste_item);
		// Apps: picked in the app menu (right-click one › Add to desktop).
		items.push_back(MenuItem::action("Add an app…", [this, srv] {
			if (srv->launcher && output_) srv->launcher->toggle(output_);
			if (srv->notifications) {
				srv->notifications->post("Add an app to the desktop", "Right-click it in the app menu, then Add to desktop.",
					"dialog-information-symbolic", 0);
			}
		}, "apps"));
		const std::string folder = dir_;
		items.push_back(MenuItem::action("Open the Desktop folder",
			[srv, folder] { srv->spawn("xdg-open " + shell_quote(folder)); }, "folder"));
		items.push_back(MenuItem::action("Arrange by name", [this] {
			saved_.clear();
			pending_.clear();
			save_positions();
			relayout();
		}));
	}
	items.push_back(MenuItem::check("Show desktop icons", enabled(), [srv] {
		srv->write_setting("desktop_icons", srv->config.desktop_icons ? "false" : "true");
	}));
	if (on) {
		items.push_back(MenuItem::check("Trash on the desktop", srv->config.desktop_trash, [srv] {
			srv->write_setting("desktop_trash", srv->config.desktop_trash ? "false" : "true");
		}));
		items.push_back(MenuItem::check("Home folder on the desktop", srv->config.desktop_home, [srv] {
			srv->write_setting("desktop_home", srv->config.desktop_home ? "false" : "true");
		}));
	}
	items.push_back(MenuItem::separator());
}

} // namespace dusk
