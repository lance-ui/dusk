#include "osk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "anim.hpp"
#include "clipboard.hpp"
#include "draw.hpp"
#include "glyphs.hpp"

namespace dusk {

namespace {

// The emoji, by category (the first one is the tab's picture).
struct Category {
	const char *name;
	std::vector<const char *> emoji;
};
const std::vector<Category> &categories() {
	static const std::vector<Category> kCats = {
		{"Smileys", {"😀", "😃", "😄", "😁", "😆", "😅", "🤣", "😂", "🙂", "🙃", "😉", "😊", "😇", "🥰", "😍", "🤩",
			"😘", "😗", "😚", "😋", "😛", "😜", "🤪", "😝", "🤗", "🤭", "🤫", "🤔", "🤐", "🤨", "😐", "😑",
			"😶", "😏", "😒", "🙄", "😬", "😌", "😔", "😪", "😴", "😷", "🤒", "🤕", "🤢", "🤮", "🥵", "🥶",
			"🥴", "😵", "🤯", "🤠", "🥳", "😎", "🤓", "🧐", "😕", "😟", "🙁", "😮", "😯", "😲", "😳", "🥺",
			"😦", "😧", "😨", "😰", "😥", "😢", "😭", "😱", "😖", "😣", "😞", "😓", "😩", "😫", "🥱", "😤",
			"😡", "😠", "🤬", "😈", "👿", "💀", "💩", "🤡", "👻", "👽", "🤖", "🎃"}},
		{"People", {"👋", "🤚", "✋", "🖖", "👌", "🤌", "🤏", "✌️", "🤞", "🤟", "🤘", "🤙", "👈", "👉", "👆", "👇",
			"☝️", "👍", "👎", "✊", "👊", "🤛", "🤜", "👏", "🙌", "👐", "🤲", "🤝", "🙏", "💪", "🦾", "👀",
			"🧠", "👶", "🧒", "👦", "👧", "🧑", "👨", "👩", "🧓", "👴", "👵", "🙋", "🤷", "🤦", "🙇", "💃",
			"🕺", "🏃", "🚶", "🧍", "🧑‍💻", "🧑‍🍳", "🧑‍🎓", "🧑‍🎨", "👮", "🥷", "🧙", "🧛", "🧟", "🧞"}},
		{"Hearts", {"❤️", "🧡", "💛", "💚", "💙", "💜", "🖤", "🤍", "🤎", "💔", "❣️", "💕", "💞", "💓", "💗", "💖",
			"💘", "💝", "💟", "♥️", "💋", "💯", "💢", "💥", "💫", "💦", "💨", "💬", "💭", "💤", "✨", "⭐",
			"🌟", "🔥", "⚡", "🌈", "☀️", "🌙", "❄️", "☔", "✅", "❌", "❗", "❓", "➕", "➖", "➡️", "⬅️"}},
		{"Animals", {"🐶", "🐱", "🐭", "🐹", "🐰", "🦊", "🐻", "🐼", "🐨", "🐯", "🦁", "🐮", "🐷", "🐸", "🐵", "🐔",
			"🐧", "🐦", "🐤", "🦆", "🦅", "🦉", "🦇", "🐺", "🐗", "🐴", "🦄", "🐝", "🐛", "🦋", "🐌", "🐞",
			"🐢", "🐍", "🦎", "🐙", "🦑", "🦀", "🐠", "🐟", "🐬", "🐳", "🦈", "🐊", "🐘", "🦒", "🐑", "🐐",
			"🌵", "🌲", "🌳", "🌴", "🌱", "🌿", "🍀", "🍁", "🍂", "🌷", "🌹", "🌻", "🌸", "🌼", "🍄", "🌍"}},
		{"Food", {"🍏", "🍎", "🍐", "🍊", "🍋", "🍌", "🍉", "🍇", "🍓", "🫐", "🍒", "🍑", "🥭", "🍍", "🥥", "🥝",
			"🍅", "🥑", "🥦", "🌽", "🥕", "🥔", "🍞", "🥐", "🧀", "🥚", "🍳", "🥓", "🍗", "🍖", "🌭", "🍔",
			"🍟", "🍕", "🌮", "🌯", "🥗", "🍝", "🍜", "🍲", "🍛", "🍣", "🍱", "🍚", "🍦", "🍩", "🍪", "🎂",
			"🍰", "🧁", "🍫", "🍬", "🍭", "🍯", "☕", "🍵", "🥤", "🧃", "🍺", "🍷", "🥂", "🍾", "🧊", "🍴"}},
		{"Things", {"⚽", "🏀", "🏈", "⚾", "🎾", "🏐", "🎱", "🏓", "🎮", "🕹️", "🎲", "🧩", "🎯", "🎳", "🎸", "🎹",
			"🎧", "🎤", "🎬", "🎨", "📷", "📱", "💻", "🖥️", "⌨️", "🖱️", "💾", "💡", "🔋", "🔌", "📚", "✏️",
			"📝", "📌", "📎", "✂️", "🔒", "🔑", "🔨", "🛠️", "⚙️", "🧲", "💰", "💳", "🎁", "🎈", "🎉", "🎊",
			"🚗", "🚕", "🚌", "🚲", "🛵", "✈️", "🚀", "🛸", "🚢", "🏠", "🏫", "🏥", "⛪", "🗺️", "⏰", "📅"}},
	};
	return kCats;
}

// UTF-8 to code points.
std::vector<uint32_t> codepoints(const std::string &s) {
	std::vector<uint32_t> out;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		uint32_t cp = 0;
		int n = 0;
		if (c < 0x80) { cp = c; n = 1; }
		else if ((c >> 5) == 0x6) { cp = c & 0x1f; n = 2; }
		else if ((c >> 4) == 0xe) { cp = c & 0x0f; n = 3; }
		else { cp = c & 0x07; n = 4; }
		for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
		out.push_back(cp);
		i += n;
	}
	return out;
}

} // namespace

Osk::Osk(Server *server) : Popover(server, 900, 300) {
	xkb_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	repeat_ = wl_event_loop_add_timer(server->loop, &Osk::handle_repeat, this);
}

Osk::~Osk() {
	if (repeat_) wl_event_source_remove(repeat_);
	if (xkb_) xkb_context_unref(xkb_);
}

void Osk::toggle(Output *output) {
	if (is_open()) {
		close();
	} else {
		show(output);
	}
}

void Osk::show(Output *output) {
	if (!output) output = server_->focused_output();
	if (!output) return;
	output_ = output;
	fit();
	const wlr_box u = output->usable();
	open(output, wlr_box{u.x, u.y + u.height - 1, u.width, 1}, "bottom");
}

void Osk::on_open() {
	fit();
}

// As wide as the screen allows: on a wide screen (a laptop or a big tablet
// in landscape) the keys split into two halves at the sides.
// Its size: "normal" (a compact keyboard in the middle, the default),
// "split" (two halves at the sides, for thumbs on a wide screen) or "wide"
// (the whole width). The button on its bar goes round them.
void Osk::fit() {
	if (!output_) return;
	const wlr_box u = output_->usable();
	const std::string &look = server_->config.osk_layout;
	split_ = look == "split" && u.width >= 1000 && u.width > u.height;
	const bool wide = look == "wide" || split_;
	const int w = wide ? u.width - 24 : std::min(u.width - 24, 820);
	const double unit = split_ ? std::min(w * 0.36, 480.0) / 5.5 : std::min((w - 24) / 10.0, 110.0);
	const int row_h = static_cast<int>(std::clamp(unit * 0.70, 36.0, 52.0));
	const int h = 12 + 44 + 5 * (row_h + 6) + 8;
	resize(w, std::min(h, u.height - 20));
	emoji_cols_ = std::clamp(static_cast<int>((w - 24) / 58.0), 6, 16);
	emoji_rows_ = 4;
}

std::vector<std::vector<Osk::Key>> Osk::rows() const {
	std::vector<std::vector<Key>> r;
	auto chars = [&](const std::string &list, int split_after, double indent = 0) {
		std::vector<Key> row;
		const std::vector<uint32_t> cps = codepoints(list);
		int i = 0;
		for (size_t at = 0; at < list.size();) {
			const unsigned char c = static_cast<unsigned char>(list[at]);
			const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : 4;
			std::string ch = list.substr(at, n);
			at += n;
			std::string shown = ch;
			if (mode_ == Mode::Letters && (shift_ || caps_) && ch.size() == 1 && std::isalpha(static_cast<unsigned char>(ch[0]))) {
				shown = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(ch[0]))));
			}
			row.push_back(Key{"c:" + shown, shown, 1, false, i == split_after});
			i++;
		}
		(void)cps;
		(void)indent;
		return row;
	};
	if (mode_ == Mode::Emoji || mode_ == Mode::Clipboard) return r;
	r.push_back(chars("1234567890", 4));
	if (mode_ == Mode::Letters) {
		r.push_back(chars("qwertyuiop", 4));
		r.push_back(chars("asdfghjkl", 4));
		std::vector<Key> r3;
		r3.push_back(Key{"shift", caps_ ? "⇪" : "⇧", 1.5, true});
		for (Key &k : chars("zxcvbnm", 3)) r3.push_back(k);
		r3[4].gap_after = true;   // after "v"
		r3.push_back(Key{"backspace", "⌫", 1.5, true});
		for (size_t i = 0; i < r3.size(); i++) if (i != 4) r3[i].gap_after = false;
		r.push_back(r3);
	} else {
		r.push_back(chars("!@#$%^&*()", 4));
		r.push_back(chars("-_=+/\\:;'\"", 4));
		std::vector<Key> r3 = chars("~`|<>[]{}", 4);
		r3.push_back(Key{"backspace", "⌫", 1.5, true});
		r.push_back(r3);
	}
	std::vector<Key> r4;
	r4.push_back(Key{"mode", mode_ == Mode::Letters ? "!#1" : "abc", 1.5, true});
	r4.push_back(Key{"emoji", "☺", 1, true});
	r4.push_back(Key{"c:,", ",", 1});
	if (split_) {
		r4.push_back(Key{"c: ", "", 2.0, false, true});   // a space bar under each thumb
		r4.push_back(Key{"c: ", "", 2.0});
	} else {
		r4.push_back(Key{"c: ", "", 4.0});
	}
	r4.push_back(Key{"c:.", ".", 1});
	r4.push_back(Key{"enter", "↵", 1.5, true});
	r.push_back(r4);
	return r;
}

void Osk::draw(cairo_t *cr, float scale) {
	const Config &c = server_->config;
	const int pad = 12, bar = 44;
	const Color key_bg = with_alpha(theme::text, theme::dark ? 0.12 : 0.09);
	const Color special_bg = with_alpha(theme::text, theme::dark ? 0.06 : 0.05);
	const std::string family = c.font.empty() ? "Sans" : c.font;

	// The bar along the top: emoji, the clipboard, settings ... and hide.
	{
		const double y = pad / 2.0 + 2;
		struct Tool {
			const char *id, *glyph;
		};
		const Tool tools[] = {{"emoji", ""}, {"clipboard", "clipboard"}, {"layout", "layout-template"},
			{"settings", "settings"}, {"move", "move"}};
		double x = pad;
		for (const Tool &t : tools) {
			const bool on = (std::string(t.id) == "emoji" && mode_ == Mode::Emoji) ||
				(std::string(t.id) == "clipboard" && mode_ == Mode::Clipboard);
			if (on) fill_rounded(cr, x, y, 40, 36, 18, with_alpha(theme::accent, 0.25));
			if (std::string(t.id) == "emoji") {
				draw_text(cr, "☺", x, y, 40, 36, (family + " 15").c_str(), theme::text, Align::Center);
			} else {
				draw_glyph(cr, t.glyph, x + 20, y + 18, 18, theme::text);
			}
			add_hot(std::string("tool:") + t.id, x, y, 40, 36, 18);
			x += 48;
		}
		draw_glyph(cr, "chevron-down", w_ - pad - 20, y + 18, 20, theme::text);
		add_hot("tool:hide", w_ - pad - 40, y, 40, 36, 18);
		// The arrows, beside it (held: they repeat).
		{
			double ax = w_ - pad - 40 - 12 - 4 * 40;
			for (const char *a : {"left", "up", "down", "right"}) {
				const std::string name = a;
				const char *glyph = name == "left" ? "chevron-left" : name == "right" ? "chevron-right"
					: name == "up" ? "chevron-up" : "chevron-down";
				fill_rounded(cr, ax + 2, y + 2, 36, 32, 12, with_alpha(theme::text, theme::dark ? 0.08 : 0.06));
				draw_glyph(cr, glyph, ax + 20, y + 18, 17, theme::text);
				add_hot("arrow:" + name, ax + 2, y + 2, 36, 32, 12);
				ax += 40;
			}
		}
		if (mode_ == Mode::Emoji) {
			// The categories, in the middle of the bar.
			const auto &cats = categories();
			const double tw = 44;
			double cx = (w_ - tw * cats.size()) / 2.0;
			for (size_t i = 0; i < cats.size(); i++) {
				if (static_cast<int>(i) == category_) fill_rounded(cr, cx + 2, y, tw - 4, 36, 14, with_alpha(theme::accent, 0.25));
				draw_text(cr, cats[i].emoji[0], cx, y, tw, 36, "Noto Color Emoji 15", theme::text, Align::Center);
				add_hot("cat:" + std::to_string(i), cx + 2, y, tw - 4, 36, 14);
				cx += tw;
			}
		}
	}

	const double top = pad / 2.0 + bar;
	const double area_h = h_ - top - pad / 2.0;
	const double row_h = area_h / 5.0;

	if (mode_ == Mode::Clipboard) {
		// The clipboard's history, newest first, two to a row: tap one to put
		// it into the app; the wheel scrolls.
		const Clipboard *cb = server_->clipboard.get();
		std::vector<const ClipEntry *> items;
		if (cb) {
			for (const ClipEntry &e : cb->history()) items.push_back(&e);
		}
		const int per_row = w_ > 700 ? 2 : 1, rows_shown = 4;
		const int max_scroll = std::max(0, (static_cast<int>(items.size()) + per_row - 1) / per_row - rows_shown);
		clip_scroll_ = std::min(clip_scroll_, max_scroll);
		const double cw = (w_ - 2.0 * pad) / per_row;
		if (items.empty()) {
			draw_text(cr, "Nothing copied yet", pad, top, w_ - 2 * pad, row_h * 4, theme::font, theme::text_dim, Align::Center);
		}
		for (int i = 0; i < rows_shown * per_row; i++) {
			const size_t at = static_cast<size_t>(clip_scroll_ * per_row + i);
			if (at >= items.size()) break;
			const ClipEntry &e = *items[at];
			const double x = pad + (i % per_row) * cw, y = top + (i / per_row) * row_h;
			fill_rounded(cr, x + 3, y + 3, cw - 6, row_h - 6, 10, key_bg);
			std::string label;
			if (e.kind == ClipEntry::Kind::Text && e.data) {
				label = e.data->substr(0, 160);
				std::replace(label.begin(), label.end(), '\n', ' ');
				std::replace(label.begin(), label.end(), '\t', ' ');
			} else if (e.kind == ClipEntry::Kind::Image) {
				label = "Picture  " + std::to_string(e.image_w) + " × " + std::to_string(e.image_h);
			} else {
				label = e.files.size() == 1 ? e.files[0].substr(e.files[0].rfind('/') + 1)
					: std::to_string(e.files.size()) + " files";
			}
			draw_text(cr, label, x + 16, y + 3, cw - 32, row_h - 6, theme::font, theme::text);
			add_hot("clip:" + std::to_string(e.id), x + 3, y + 3, cw - 6, row_h - 6, 10);
		}
		// The last row: back to the letters, and delete.
		const double y = top + 4 * row_h + 3, kh = row_h - 6;
		const double u = (w_ - 2.0 * pad) / 10;
		fill_rounded(cr, pad + 3, y, 2 * u - 6, kh, 10, special_bg);
		draw_text(cr, "abc", pad, y, 2 * u, kh, (family + " 14").c_str(), theme::text, Align::Center);
		add_hot("mode-letters", pad + 3, y, 2 * u - 6, kh, 10);
		fill_rounded(cr, w_ - pad - 2 * u + 3, y, 2 * u - 6, kh, 10, special_bg);
		draw_text(cr, "⌫", w_ - pad - 2 * u, y, 2 * u, kh, (family + " 14").c_str(), theme::text, Align::Center);
		add_hot("backspace", w_ - pad - 2 * u + 3, y, 2 * u - 6, kh, 10);
		return;
	}

	if (mode_ == Mode::Emoji) {
		const auto &list = categories()[static_cast<size_t>(category_)].emoji;
		const int per_page = emoji_cols_ * emoji_rows_;
		const int pages = std::max(1, (static_cast<int>(list.size()) + per_page - 1) / per_page);
		page_ = std::clamp(page_, 0, pages - 1);
		const double cw = (w_ - 2.0 * pad) / emoji_cols_;
		const double pt = std::clamp(row_h * 0.42, 14.0, 26.0);
		const std::string ef = "Noto Color Emoji " + std::to_string(static_cast<int>(pt));
		for (int i = 0; i < per_page; i++) {
			const size_t at = static_cast<size_t>(page_ * per_page + i);
			if (at >= list.size()) break;
			const double x = pad + (i % emoji_cols_) * cw, y = top + (i / emoji_cols_) * row_h;
			draw_text(cr, list[at], x, y, cw, row_h, ef.c_str(), theme::text, Align::Center);
			add_hot(std::string("e:") + list[at], x + 2, y + 2, cw - 4, row_h - 4, 12);
		}
		// The last row: back to letters, pages, space, delete.
		const double y = top + 4 * row_h + 3, kh = row_h - 6;
		struct Bottom {
			const char *id, *label;
			double units;
		};
		const Bottom keys[] = {{"mode-letters", "abc", 1.5}, {"page-prev", "‹", 1}, {"page-next", "›", 1},
			{"c: ", "", 4.5}, {"backspace", "⌫", 2}};
		double units = 0;
		for (const Bottom &b : keys) units += b.units;
		const double u = (w_ - 2.0 * pad) / units;
		double x = pad;
		for (const Bottom &b : keys) {
			const bool special = std::string(b.id) != "c: ";
			fill_rounded(cr, x + 3, y, b.units * u - 6, kh, 10, special ? special_bg : key_bg);
			draw_text(cr, b.label, x, y, b.units * u, kh, (family + " 14").c_str(), theme::text, Align::Center);
			add_hot(b.id, x + 3, y, b.units * u - 6, kh, 10);
			x += b.units * u;
		}
		(void)scale;
		return;
	}

	const auto r = rows();
	const double font_pt = std::clamp(row_h * 0.30, 11.0, 18.0);
	const std::string kf = family + " " + std::to_string(static_cast<int>(font_pt));
	const std::string kf_small = family + " " + std::to_string(static_cast<int>(font_pt * 0.75));
	for (size_t ri = 0; ri < r.size(); ri++) {
		const auto &row = r[ri];
		const double y = top + ri * row_h + 3, kh = row_h - 6;
		auto draw_key = [&](const Key &k, double x, double w) {
			const bool active = (k.id == "shift" && (shift_ || caps_)) || k.id == "enter";
			Color bg = k.special ? special_bg : key_bg;
			if (active) bg = k.id == "enter" ? theme::accent : with_alpha(theme::accent, 0.35);
			fill_rounded(cr, x + 3, y, w - 6, kh, 10, bg);
			const Color fg = k.id == "enter" ? theme::on_accent : theme::text;
			const bool small = k.label.size() > 1 && k.id.rfind("c:", 0) != 0 && k.label != "⌫" && k.label != "⇧" &&
				k.label != "⇪" && k.label != "↵" && k.label != "☺";
			draw_text(cr, k.label, x + 3, y, w - 6, kh, (small ? kf_small : kf).c_str(), fg, Align::Center);
			add_hot(k.id, x + 3, y, w - 6, kh, 10);
		};
		if (!split_) {
			double units = 0;
			for (const Key &k : row) units += k.units;
			const double u = (w_ - 2.0 * pad) / 10.0;
			double x = pad + ((w_ - 2.0 * pad) - units * u) / 2.0;
			for (const Key &k : row) {
				draw_key(k, x, k.units * u);
				x += k.units * u;
			}
		} else {
			// Two halves at the sides: the keys up to the split on the left,
			// the rest on the right.
			const double half = std::min(w_ * 0.36, 480.0);
			const double u = half / 5.5;
			size_t cut = row.size() / 2;
			for (size_t i = 0; i < row.size(); i++) {
				if (row[i].gap_after) {
					cut = i + 1;
					break;
				}
			}
			double lu = 0, ru = 0;
			for (size_t i = 0; i < row.size(); i++) (i < cut ? lu : ru) += row[i].units;
			double x = pad + (ri == 2 ? u * 0.5 : 0);
			for (size_t i = 0; i < cut; i++) {
				draw_key(row[i], x, row[i].units * u);
				x += row[i].units * u;
			}
			x = w_ - pad - ru * u - (ri == 2 ? u * 0.5 : 0);
			for (size_t i = cut; i < row.size(); i++) {
				draw_key(row[i], x, row[i].units * u);
				x += row[i].units * u;
			}
			(void)lu;
		}
	}
}

bool Osk::draggable(const std::string &id) const {
	// Held: Backspace and the arrows repeat; the handle moves the keyboard.
	return id == "backspace" || id.rfind("arrow:", 0) == 0 || id == "tool:move";
}

void Osk::on_drag(const std::string &id, double px, double py) {
	if (id == "tool:move") {
		if (held_ != id) {
			held_ = id;
			grab_x_ = px;
			grab_y_ = py;
			return;
		}
		// The keyboard follows the handle (and stays on the screen).
		offset_x_ += static_cast<int>(std::lround(px - grab_x_));
		offset_y_ += static_cast<int>(std::lround(py - grab_y_));
		place();
		const wlr_box b = box();
		// (Kept within the screen: the offset is what's left after that.)
		if (output_) {
			const wlr_box u = output_->usable();
			offset_x_ = std::clamp(offset_x_, -(u.width), u.width);
			offset_y_ = std::clamp(offset_y_, -(u.height), 0);
		}
		(void)b;
		return;
	}
	// (Called on every move while it's held: only the first one types.)
	if (held_ == id) return;
	held_ = id;
	press_key(id);
	wl_event_source_timer_update(repeat_, 450);
}

int Osk::handle_repeat(void *data) {
	auto *self = static_cast<Osk *>(data);
	if (self->dragging_ == "backspace" || self->dragging_.rfind("arrow:", 0) == 0) {
		self->press_key(self->dragging_);
		wl_event_source_timer_update(self->repeat_, 60);
	}
	return 0;
}

void Osk::on_scroll(double delta) {
	if (mode_ == Mode::Clipboard) {
		clip_scroll_ = std::max(0, clip_scroll_ + (delta > 0 ? 1 : -1));
		redraw();
		return;
	}
	if (mode_ != Mode::Emoji) return;
	page_ += delta > 0 ? 1 : -1;
	page_ = std::max(0, page_);
	redraw();
}

void Osk::activate(const std::string &id, uint32_t button, double px, double) {
	if (button != BTN_LEFT) return;
	if (px < 0) {
		// Let go of a held key.
		wl_event_source_timer_update(repeat_, 0);
		held_.clear();
		return;
	}
	press_key(id);
}

void Osk::press_key(const std::string &id) {
	if (id.rfind("c:", 0) == 0) {
		type_text(id.substr(2));
		if (shift_ && !caps_) {
			shift_ = false;
			redraw();
		}
		return;
	}
	if (id.rfind("e:", 0) == 0) {
		type_text(id.substr(2));
		return;
	}
	if (id.rfind("clip:", 0) == 0) {
		paste_entry(std::strtoull(id.c_str() + 5, nullptr, 10));
		return;
	}
	if (id == "arrow:left") {
		send_keycode(KEY_LEFT, false);
	} else if (id == "arrow:right") {
		send_keycode(KEY_RIGHT, false);
	} else if (id == "arrow:up") {
		send_keycode(KEY_UP, false);
	} else if (id == "arrow:down") {
		send_keycode(KEY_DOWN, false);
	} else if (id == "backspace") {
		send_keycode(KEY_BACKSPACE, false);
	} else if (id == "enter") {
		send_keycode(KEY_ENTER, false);
	} else if (id == "shift") {
		// Once: the next letter. Twice quickly: Caps Lock. Again: off.
		const double now = now_ms();
		if (caps_) {
			caps_ = shift_ = false;
		} else if (shift_ && now - last_shift_ < 400) {
			caps_ = true;
		} else {
			shift_ = !shift_;
		}
		last_shift_ = now;
		redraw();
	} else if (id == "mode") {
		mode_ = mode_ == Mode::Letters ? Mode::Symbols : Mode::Letters;
		redraw();
	} else if (id == "emoji" || id == "tool:emoji") {
		mode_ = mode_ == Mode::Emoji ? Mode::Letters : Mode::Emoji;
		redraw();
	} else if (id == "mode-letters") {
		mode_ = Mode::Letters;
		redraw();
	} else if (id.rfind("cat:", 0) == 0) {
		category_ = std::atoi(id.c_str() + 4);
		page_ = 0;
		redraw();
	} else if (id == "page-prev") {
		page_ = std::max(0, page_ - 1);
		redraw();
	} else if (id == "page-next") {
		page_++;
		redraw();
	} else if (id == "tool:clipboard") {
		// The clipboard's history, inside the keyboard: tap one to put it in.
		mode_ = mode_ == Mode::Clipboard ? Mode::Letters : Mode::Clipboard;
		clip_scroll_ = 0;
		redraw();
	} else if (id == "tool:layout") {
		// Normal → split (wide screens) → full width → normal.
		const std::string &now = server_->config.osk_layout;
		const std::string next = now == "normal" ? "split" : now == "split" ? "wide" : "normal";
		offset_x_ = offset_y_ = 0;   // back to its place
		server_->write_setting("osk_layout", next);
		fit();
		redraw();
	} else if (id == "tool:settings") {
		server_->open_settings("keyboard");
	} else if (id == "tool:hide") {
		close();
	}
}

// ---- Typing ------------------------------------------------------------------------------

wlr_keyboard *Osk::target() const {
	// A real keyboard's (its keymap is the layout set in Settings).
	for (Keyboard *k : server_->keyboards) {
		if (!k->is_virtual && k->keyboard && k->keyboard->keymap) return k->keyboard;
	}
	for (Keyboard *k : server_->keyboards) {
		if (k->keyboard && k->keyboard->keymap) return k->keyboard;
	}
	return nullptr;
}

bool Osk::send_keycode(uint32_t evdev, bool shift) {
	wlr_keyboard *kb = target();
	if (!kb) return false;
	wlr_seat_set_keyboard(server_->seat, kb);
	wlr_keyboard_modifiers mods{};
	if (shift) {
		const xkb_mod_index_t si = xkb_keymap_mod_get_index(kb->keymap, XKB_MOD_NAME_SHIFT);
		if (si != XKB_MOD_INVALID) mods.depressed = 1u << si;
	}
	mods.group = kb->modifiers.group;
	wlr_seat_keyboard_notify_modifiers(server_->seat, &mods);
	const uint32_t t = static_cast<uint32_t>(now_ms());
	wlr_seat_keyboard_notify_key(server_->seat, t, evdev, WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(server_->seat, t + 1, evdev, WL_KEYBOARD_KEY_STATE_RELEASED);
	wlr_seat_keyboard_notify_modifiers(server_->seat, &kb->modifiers);
	return true;
}

bool Osk::send_combo(uint32_t evdev, bool ctrl, bool shift) {
	wlr_keyboard *kb = target();
	if (!kb) return false;
	wlr_seat_set_keyboard(server_->seat, kb);
	wlr_keyboard_modifiers mods{};
	auto add = [&](const char *name) {
		const xkb_mod_index_t i = xkb_keymap_mod_get_index(kb->keymap, name);
		if (i != XKB_MOD_INVALID) mods.depressed |= 1u << i;
	};
	if (ctrl) add(XKB_MOD_NAME_CTRL);
	if (shift) add(XKB_MOD_NAME_SHIFT);
	mods.group = kb->modifiers.group;
	wlr_seat_keyboard_notify_modifiers(server_->seat, &mods);
	const uint32_t t = static_cast<uint32_t>(now_ms());
	wlr_seat_keyboard_notify_key(server_->seat, t, evdev, WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(server_->seat, t + 1, evdev, WL_KEYBOARD_KEY_STATE_RELEASED);
	wlr_seat_keyboard_notify_modifiers(server_->seat, &kb->modifiers);
	return true;
}

// A clipboard entry into the app: short text is typed; anything else (long
// text, pictures, files) becomes the clipboard and is pasted (Ctrl+V; in a
// terminal Ctrl+Shift+V).
void Osk::paste_entry(uint64_t id) {
	Clipboard *cb = server_->clipboard.get();
	if (!cb) return;
	for (const ClipEntry &e : cb->history()) {
		if (e.id != id) continue;
		if (e.kind == ClipEntry::Kind::Text && e.data && e.data->size() <= 400) {
			const std::string text = *e.data;
			type_string(text);
		} else {
			cb->use(id);
			View *v = server_->focused;
			const std::string app = v ? v->app_id() : "";
			const bool terminal = app.find("term") != std::string::npos || app.find("kitty") != std::string::npos ||
				app.find("alacritty") != std::string::npos || app.find("foot") != std::string::npos ||
				app.find("Ptyxis") != std::string::npos || app.find("ptyxis") != std::string::npos ||
				app.find("Console") != std::string::npos || app.find("wezterm") != std::string::npos;
			send_combo(KEY_V, true, terminal);
		}
		return;
	}
}

// Text of any length: each character through the layout when it has it, the
// rest (emoji...) through a keymap made for them.
void Osk::type_string(const std::string &utf8) {
	const std::vector<uint32_t> cps = codepoints(utf8);
	std::vector<uint32_t> odd;
	auto flush = [&] {
		for (size_t at = 0; at < odd.size(); at += 16) {
			type_codepoints(std::vector<uint32_t>(odd.begin() + at, odd.begin() + std::min(odd.size(), at + 16)));
		}
		odd.clear();
	};
	for (uint32_t cp : cps) {
		if (cp == '\n') {
			flush();
			send_keycode(KEY_ENTER, false);
			continue;
		}
		if (cp == '\t') {
			flush();
			send_keycode(KEY_TAB, false);
			continue;
		}
		std::string one;
		if (cp < 0x80) {
			one = std::string(1, static_cast<char>(cp));
			flush();
			if (!type_text(one)) odd.push_back(cp);
		} else {
			odd.push_back(cp);
		}
	}
	flush();
}

bool Osk::type_text(const std::string &utf8) {
	wlr_keyboard *kb = target();
	if (!kb) return false;
	const std::vector<uint32_t> cps = codepoints(utf8);
	// A single character the layout has (on its first two levels): its key.
	if (cps.size() == 1) {
		const xkb_keysym_t want = xkb_utf32_to_keysym(cps[0]);
		xkb_keymap *km = kb->keymap;
		const xkb_layout_index_t group = kb->modifiers.group;
		for (xkb_keycode_t kc = xkb_keymap_min_keycode(km); kc <= xkb_keymap_max_keycode(km); kc++) {
			for (xkb_level_index_t level = 0; level < 2; level++) {
				const xkb_keysym_t *syms = nullptr;
				const int n = xkb_keymap_key_get_syms_by_level(km, kc, group, level, &syms);
				if (n == 1 && syms[0] == want && kc >= 8) {
					return send_keycode(kc - 8, level == 1);
				}
			}
		}
	}
	return type_codepoints(cps);
}

// Anything else (emoji...): a keymap with just these characters on it, for a
// moment, then the real one back. The app is sent each keymap in turn.
bool Osk::type_codepoints(const std::vector<uint32_t> &cps) {
	wlr_keyboard *kb = target();
	if (!kb || cps.empty() || !xkb_) return false;
	std::string keycodes, symbols;
	const size_t n = std::min<size_t>(cps.size(), 16);
	for (size_t i = 0; i < n; i++) {
		char name[64] = {0};
		xkb_keysym_get_name(xkb_utf32_to_keysym(cps[i]), name, sizeof name);
		keycodes += "<K" + std::to_string(i) + "> = " + std::to_string(9 + i) + "; ";
		symbols += "key <K" + std::to_string(i) + "> { [ " + std::string(name) + " ] }; ";
	}
	const std::string text = "xkb_keymap { xkb_keycodes \"dusk\" { minimum = 8; maximum = 255; " + keycodes +
		"}; xkb_types \"dusk\" { include \"complete\" }; xkb_compatibility \"dusk\" { include \"complete\" }; "
		"xkb_symbols \"dusk\" { " + symbols + "}; };";
	xkb_keymap *temp = xkb_keymap_new_from_string(xkb_, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!temp) return false;
	xkb_keymap *real = xkb_keymap_ref(kb->keymap);
	wlr_seat_set_keyboard(server_->seat, kb);
	wlr_keyboard_set_keymap(kb, temp);
	wlr_keyboard_modifiers none{};
	wlr_seat_keyboard_notify_modifiers(server_->seat, &none);
	uint32_t t = static_cast<uint32_t>(now_ms());
	for (size_t i = 0; i < n; i++) {
		wlr_seat_keyboard_notify_key(server_->seat, t++, static_cast<uint32_t>(1 + i), WL_KEYBOARD_KEY_STATE_PRESSED);
		wlr_seat_keyboard_notify_key(server_->seat, t++, static_cast<uint32_t>(1 + i), WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	wlr_keyboard_set_keymap(kb, real);
	wlr_seat_keyboard_notify_modifiers(server_->seat, &kb->modifiers);
	xkb_keymap_unref(real);
	xkb_keymap_unref(temp);
	return true;
}

} // namespace dusk
