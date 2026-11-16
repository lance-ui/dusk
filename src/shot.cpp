#include "shot.hpp"

#include <tuple>

#include "recorder.hpp"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

#include "apps.hpp"
#include "draw.hpp"
#include "glyphs.hpp"

namespace dusk {

namespace {

constexpr int kBarH = 64;
constexpr int kBtn = 44;
constexpr int kChipH = 38, kChipRow = 50;   // the open windows' row (window mode)

bool no_input(wlr_scene_buffer *, double *, double *) {
	return false;
}

wlr_box normalized(double x1, double y1, double x2, double y2) {
	const int x = static_cast<int>(std::lround(std::min(x1, x2)));
	const int y = static_cast<int>(std::lround(std::min(y1, y2)));
	return wlr_box{x, y, static_cast<int>(std::lround(std::max(x1, x2))) - x,
		static_cast<int>(std::lround(std::max(y1, y2))) - y};
}

bool inside(const wlr_box &b, double x, double y) {
	return x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height;
}

} // namespace

ScreenshotTool::ScreenshotTool(Server *server)
		: server_(server), bar_(server->layer_overlay, nullptr) {
	tree_ = wlr_scene_tree_create(server->layer_overlay);
	const float dim[4] = {0, 0, 0, 0.45f};
	for (auto &r : dim_) {
		r = wlr_scene_rect_create(tree_, 0, 0, dim);
	}
	float accent[4];
	to_float(theme::accent, accent);
	for (auto &r : edge_) {
		r = wlr_scene_rect_create(tree_, 0, 0, accent);
	}
	label_ = wlr_scene_buffer_create(tree_, nullptr);
	label_->point_accepts_input = no_input;
	ink_node_ = wlr_scene_buffer_create(tree_, nullptr);
	ink_node_->point_accepts_input = no_input;
	wlr_scene_node_set_enabled(&tree_->node, false);
	// The bar above the dimming.
	wlr_scene_node_raise_to_top(&bar_.tree->node);
	bar_.set_enabled(false);
}

ScreenshotTool::~ScreenshotTool() {
	wlr_scene_node_destroy(&tree_->node);
}

void ScreenshotTool::open() {
	if (open_) {
		return;
	}
	// Open menus and the control centre stay, so they can be in the picture.
	output_ = server_->output_at(server_->cursor->x, server_->cursor->y);
	if (!output_) {
		return;
	}
	const Config &c = server_->config;
	mode_ = c.screenshot_mode == "window" ? Mode::Window : c.screenshot_mode == "screen" ? Mode::Screen
		: Mode::Area;
	pointer_ = c.screenshot_pointer;
	delay_ = c.screenshot_delay;
	open_ = true;
	selecting_ = moving_ = false;
	hover_.clear();
	px_ = server_->cursor->x;
	py_ = server_->cursor->y;
	// A remembered area only if it's still on a screen.
	if (area_.width > 0 && !server_->output_at(area_.x + area_.width / 2.0, area_.y + area_.height / 2.0)) {
		area_ = wlr_box{};
	}
	float accent[4];
	to_float(video_ ? theme::danger : theme::accent, accent);
	for (auto *r : edge_) wlr_scene_rect_set_color(r, accent);
	wlr_scene_node_raise_to_top(&tree_->node);
	wlr_scene_node_raise_to_top(&bar_.tree->node);
	wlr_scene_node_set_enabled(&tree_->node, true);
	bar_.set_enabled(true);
	collect_chips();
	redraw_bar();
	update_target();
	server_->set_cursor("crosshair");
}

void ScreenshotTool::close() {
	if (!open_) {
		return;
	}
	open_ = false;
	selecting_ = moving_ = false;
	drawing_ = false;
	raised_ = nullptr;
	ink_.clear();
	wlr_scene_node_set_enabled(&ink_node_->node, false);
	wlr_scene_node_set_enabled(&tree_->node, false);
	bar_.set_enabled(false);
	server_->set_cursor("default");
	server_->process_motion(0);
	// Remembered for next time (written once, not on every click).
	const Config &c = server_->config;
	const std::string mode = mode_ == Mode::Window ? "window" : mode_ == Mode::Screen ? "screen" : "area";
	if (mode != c.screenshot_mode) Config::write_key("screenshot_mode", mode);
	if (pointer_ != c.screenshot_pointer) Config::write_key("screenshot_pointer", pointer_ ? "true" : "false");
	if (delay_ != c.screenshot_delay) Config::write_key("screenshot_delay", std::to_string(delay_));
}

void ScreenshotTool::output_gone(Output *output) {
	if (output == output_) {
		close();
	}
}

bool ScreenshotTool::on_bar(double lx, double ly) const {
	return inside(bar_box_, lx, ly);
}

View *ScreenshotTool::chip_view(const std::string &id) const {
	if (id.rfind("win:", 0) != 0) return nullptr;
	const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 4));
	if (i >= chips_.size()) return nullptr;
	View *v = chips_[i];
	// Still there (it may have closed meanwhile)?
	return std::find(server_->views.begin(), server_->views.end(), v) != server_->views.end() && v->mapped ? v : nullptr;
}

std::string ScreenshotTool::hot_at(double lx, double ly) const {
	for (const Hot &h : hots_) {
		if (inside(h.box, lx - bar_box_.x, ly - bar_box_.y)) return h.id;
	}
	return "";
}

void ScreenshotTool::collect_chips() {
	chips_.clear();
	if (mode_ != Mode::Window) return;
	for (View *v : server_->views) {
		if (v->visible()) chips_.push_back(v);
	}
}

void ScreenshotTool::set_video(bool video) {
	video_ = video;
	if (video_) pen_ = false;
	float edge[4];
	to_float(video_ ? theme::danger : theme::accent, edge);
	for (auto *r : edge_) wlr_scene_rect_set_color(r, edge);
	redraw_bar();
	update_target();
}

void ScreenshotTool::set_mode(Mode mode) {
	mode_ = mode;
	collect_chips();
	redraw_bar();
	update_target();
}

// ---- The bar ------------------------------------------------------------------

void ScreenshotTool::redraw_bar() {
	if (!output_) return;
	hots_.clear();
	struct Item {
		std::string id, icon, text;
		bool on;
	};
	// Mode buttons, then options, then Capture and close.
	const std::vector<Item> modes = {
		{"mode:area", "area", "", mode_ == Mode::Area},
		{"mode:window", "window", "", mode_ == Mode::Window},
		{"mode:screen", "display", "", mode_ == Mode::Screen},
	};
	const std::string delay_text = delay_ > 0 ? std::to_string(delay_) + " s" : "No delay";
	const int delay_w = text_width(delay_text, theme::font_small) + 44;
	const int pad = 10, gap = 6;
	const int seg_w = 3 * kBtn + 2 * 4 + 8;
	const int kind_w = 2 * kBtn + 4 + 8;   // photo / video
	// Video has no pen (one sound button instead of the pen, its colour and undo).
	const int tools_w = video_ ? kBtn + gap : 3 * (kBtn + gap);
	const int controls_w = pad + seg_w + 14 + kBtn + gap + delay_w + 14 + tools_w + 8 + kind_w + 14 + 52 + gap + 36 + pad;
	int width = controls_w;
	// Window mode: the open windows on the screens now, front first, as chips
	// in a row under the buttons.
	// Window mode: the open windows (collected when the mode was picked, so
	// the row doesn't shuffle as windows come forward), as chips under the
	// buttons.
	if (mode_ != Mode::Window) chips_.clear();
	chips_.erase(std::remove_if(chips_.begin(), chips_.end(), [this](View *v) {
		return std::find(server_->views.begin(), server_->views.end(), v) == server_->views.end() || !v->mapped;
	}), chips_.end());
	std::vector<int> chip_w;
	int chips_total = 0;
	for (View *v : chips_) {
		const std::string t = v->title().empty() ? v->app_id() : v->title();
		const int w = std::min(220, 12 + 22 + 8 + text_width(t, theme::font_small) + 14);
		chip_w.push_back(w);
		chips_total += w + gap;
	}
	if (!chips_.empty()) {
		const wlr_box u0 = output_->usable();
		width = std::max(width, std::min(chips_total + 2 * pad, u0.width - 80));
	}
	const int height = kBarH + (chips_.empty() ? 0 : kChipRow);
	const float scale = output_->output->scale;
	Canvas canvas(width, height, scale);
	cairo_t *cr = canvas.cr();
	const double radius = std::min<double>(theme::ui_radius, kBarH / 2.0);
	ui_card(cr, 0, 0, width, height, radius, theme::menu_bg);

	auto hover = [this](const std::string &id) { return hover_ == id; };
	const Color hover_c = with_alpha(theme::text, theme::dark ? 0.08 : 0.06);
	const int cy = kBarH / 2;

	// The segmented mode switch (the buttons are centred when the bar is wider
	// for the windows' row).
	int x = pad + (width - controls_w) / 2;
	fill_rounded(cr, x, cy - kBtn / 2 - 4, seg_w, kBtn + 8, (kBtn + 8) / 2.0, theme::field_bg);
	x += 4;
	for (const Item &m : modes) {
		const wlr_box b{x, cy - kBtn / 2, kBtn, kBtn};
		if (m.on) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, theme::accent);
		else if (hover(m.id)) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, hover_c);
		draw_named_icon(cr, server_->icons, m.icon, b.x + 12, b.y + 12, 20,
			m.on ? theme::on_accent : theme::text, scale);
		hots_.push_back(Hot{m.id, b});
		x += kBtn + 4;
	}
	x += 4 + 14;

	// Show the pointer (recordings always have it).
	if (video_) {
		const wlr_box b{x, cy - kBtn / 2, kBtn, kBtn};
		draw_named_icon(cr, server_->icons, "pointer", b.x + 12, b.y + 12, 20, with_alpha(theme::text_dim, 0.35), scale);
		x += kBtn + gap;
	} else {
		const wlr_box b{x, cy - kBtn / 2, kBtn, kBtn};
		if (pointer_) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, with_alpha(theme::accent, 0.25));
		else if (hover("pointer")) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, hover_c);
		draw_named_icon(cr, server_->icons, "pointer", b.x + 12, b.y + 12, 20,
			pointer_ ? theme::accent : theme::text_dim, scale);
		hots_.push_back(Hot{"pointer", b});
		x += kBtn + gap;
	}
	// Delay: click to go round 0, 3, 5, 10 s.
	{
		const wlr_box b{x, cy - 18, delay_w, 36};
		fill_rounded(cr, b.x, b.y, b.width, b.height, 18, hover("delay") ? theme::tile_off : theme::field_bg);
		draw_named_icon(cr, server_->icons, "timer", b.x + 10, b.y + 9, 18,
			delay_ > 0 ? theme::accent : theme::text_dim, scale);
		draw_text(cr, delay_text, b.x + 34, b.y, b.width - 40, b.height, theme::font_small, theme::text);
		hots_.push_back(Hot{"delay", b});
		x += delay_w + 14;
	}
	// Video: the sound (none, what you hear, the microphone).
	if (video_) {
		const std::string &audio = server_->config.record_audio;
		const wlr_box b{x, cy - kBtn / 2, kBtn, kBtn};
		const bool on = audio != "none";
		if (on) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, with_alpha(theme::accent, 0.25));
		else if (hover("audio")) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, hover_c);
		draw_named_icon(cr, server_->icons, audio == "mic" ? "mic" : audio == "system" ? "speaker-on" : "speaker-muted",
			b.x + 12, b.y + 12, 20, on ? theme::accent : theme::text_dim, scale);
		hots_.push_back(Hot{"audio", b});
		x += kBtn + gap + 8;
	}
	// The pen: draw on it first. Then its colour, and undo.
	if (!video_) {
		const wlr_box b{x, cy - kBtn / 2, kBtn, kBtn};
		if (pen_) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, with_alpha(theme::accent, 0.25));
		else if (hover("pen")) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, hover_c);
		draw_named_icon(cr, server_->icons, "edit", b.x + 12, b.y + 12, 20, pen_ ? theme::accent : theme::text_dim, scale);
		hots_.push_back(Hot{"pen", b});
		x += kBtn + gap;
	}
	if (!video_) {
		const wlr_box b{x, cy - kBtn / 2, kBtn, kBtn};
		if (hover("ink")) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, hover_c);
		cairo_new_path(cr);
		cairo_arc(cr, b.x + kBtn / 2.0, cy, 10, 0, 2 * M_PI);
		set_color(cr, ink_color());
		cairo_fill_preserve(cr);
		set_color(cr, with_alpha(theme::text, 0.35));
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		hots_.push_back(Hot{"ink", b});
		x += kBtn + gap;
	}
	if (!video_) {
		const wlr_box b{x, cy - kBtn / 2, kBtn, kBtn};
		if (hover("undo") && !ink_.empty()) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, hover_c);
		draw_named_icon(cr, server_->icons, "refresh", b.x + 12, b.y + 12, 20,
			ink_.empty() ? with_alpha(theme::text_dim, 0.4) : theme::text, scale);
		hots_.push_back(Hot{"undo", b});
		x += kBtn + gap + 8;
	}
	// Photo or video (as in GNOME's tool).
	{
		fill_rounded(cr, x, cy - kBtn / 2 - 4, kind_w, kBtn + 8, (kBtn + 8) / 2.0, theme::field_bg);
		int kx = x + 4;
		for (const auto &[id, icon, on] : std::vector<std::tuple<std::string, std::string, bool>>{
				{"kind:photo", "camera", !video_}, {"kind:video", "video", video_}}) {
			const wlr_box b{kx, cy - kBtn / 2, kBtn, kBtn};
			if (on) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, theme::accent);
			else if (hover(id)) fill_rounded(cr, b.x, b.y, b.width, b.height, kBtn / 2.0, hover_c);
			draw_named_icon(cr, server_->icons, icon, b.x + 12, b.y + 12, 20, on ? theme::on_accent : theme::text, scale);
			hots_.push_back(Hot{id, b});
			kx += kBtn + 4;
		}
		x += kind_w + 14;
	}
	// Capture: a shutter button (a red record button for video).
	{
		const double r = 24, ccx = x + r, ccy = cy;
		const bool ready = target_.width > 0 && target_.height > 0;
		const Color main = video_ ? theme::danger : theme::accent;
		cairo_new_path(cr);
		cairo_arc(cr, ccx, ccy, r, 0, 2 * M_PI);
		set_color(cr, ready ? main : theme::field_bg);
		cairo_fill(cr);
		if (video_) {
			cairo_new_path(cr);
			cairo_arc(cr, ccx, ccy, 9, 0, 2 * M_PI);
			set_color(cr, ready ? rgb(0xffffff) : theme::text_dim);
			cairo_fill(cr);
		}
		cairo_new_path(cr);
		cairo_arc(cr, ccx, ccy, r - 5, 0, 2 * M_PI);
		set_color(cr, ready ? theme::on_accent : theme::text_dim);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
		if (hover("capture") && ready) {
			cairo_arc(cr, ccx, ccy, r - 9, 0, 2 * M_PI);
			set_color(cr, with_alpha(theme::on_accent, 0.35));
			cairo_fill(cr);
		}
		hots_.push_back(Hot{"capture", wlr_box{x, cy - 24, 48, 48}});
		x += 52 + gap;
	}
	// Close.
	{
		const wlr_box b{x, cy - 18, 36, 36};
		if (hover("close")) fill_rounded(cr, b.x, b.y, 36, 36, 18, hover_c);
		draw_named_icon(cr, server_->icons, "close", b.x + 9, b.y + 9, 18, theme::text_dim, scale);
		hots_.push_back(Hot{"close", b});
	}
	// The open windows: icon and name. The ones that don't fit are left out.
	if (!chips_.empty()) {
		int total = 0;
		size_t fit = 0;
		for (; fit < chips_.size(); fit++) {
			if (total + chip_w[fit] > width - 2 * pad) break;
			total += chip_w[fit] + gap;
		}
		chips_.resize(fit);
		int cx = (width - (total - gap)) / 2;
		const int y0 = kBarH - 4 + (kChipRow - kChipH) / 2 - 4;
		for (size_t i = 0; i < chips_.size(); i++) {
			View *v = chips_[i];
			const std::string id = "win:" + std::to_string(i);
			const wlr_box b{cx, y0, chip_w[i], kChipH};
			const bool lit = hover(id) || (hover_.empty() && v == raised_ && target_.width > 0);
			fill_rounded(cr, b.x, b.y, b.width, b.height, kChipH / 2.0,
				lit ? with_alpha(theme::accent, 0.28) : theme::field_bg);
			const std::string t = v->title().empty() ? v->app_id() : v->title();
			draw_icon(cr, server_->icons, server_->apps.icon_for(v->app_id(), v->title()), t, b.x + 10, b.y + (kChipH - 22) / 2.0,
				22, scale);
			draw_text(cr, t, b.x + 10 + 22 + 8, b.y, b.width - 10 - 22 - 8 - 12, b.height, theme::font_small,
				theme::text);
			hots_.push_back(Hot{id, b});
			cx += chip_w[i] + gap;
		}
	}
	bar_.present(canvas, wlr_box{0, 0, width, height}, radius);
	const wlr_box u = output_->usable();
	bar_box_ = wlr_box{u.x + (u.width - width) / 2, u.y + u.height - height - 28, width, height};
	bar_.show(bar_box_.x, bar_box_.y, 1.0);
}

// ---- What gets captured -------------------------------------------------------

void ScreenshotTool::update_target() {
	wlr_box t{};
	if (mode_ == Mode::Area) {
		t = area_;
	} else if (mode_ == Mode::Screen) {
		if (Output *o = server_->output_at(px_, py_)) t = o->box();
	} else if (View *cv = chip_view(hover_)) {
		// Pointing at a window's chip in the bar.
		t = cv->frame_box();
		if (cv != raised_) {
			raised_ = cv;
			server_->raise(cv);
		}
	} else if (on_bar(px_, py_)) {
		// Elsewhere on the bar: keep what was chosen.
		t = target_;
	} else {
		// The window under the pointer (looking through the dimming).
		wlr_scene_node_set_enabled(&tree_->node, false);
		bar_.set_enabled(false);
		const Hit hit = server_->hit_test(px_, py_);
		wlr_scene_node_set_enabled(&tree_->node, true);
		bar_.set_enabled(true);
		if (hit.view) {
			t = hit.view->frame_box();
			// Brought to the front, so nothing covers it in the picture.
			if (hit.view != raised_) {
				raised_ = hit.view;
				server_->raise(hit.view);
			}
		}
	}
	const bool was_ready = target_.width > 0;
	target_ = t;
	if (was_ready != (target_.width > 0)) redraw_bar();
	show_target();
}

void ScreenshotTool::show_target() {
	wlr_box all{};
	wlr_output_layout_get_box(server_->layout, nullptr, &all);
	const wlr_box &t = target_;
	auto put = [](wlr_scene_rect *r, int x, int y, int w, int h) {
		if (w <= 0 || h <= 0) {
			wlr_scene_node_set_enabled(&r->node, false);
			return;
		}
		wlr_scene_node_set_enabled(&r->node, true);
		wlr_scene_node_set_position(&r->node, x, y);
		wlr_scene_rect_set_size(r, w, h);
	};
	if (t.width <= 0 || t.height <= 0) {
		put(dim_[0], all.x, all.y, all.width, all.height);
		for (int i = 1; i < 4; i++) put(dim_[i], 0, 0, 0, 0);
		for (auto *e : edge_) put(e, 0, 0, 0, 0);
		wlr_scene_node_set_enabled(&label_->node, false);
		return;
	}
	const int right = all.x + all.width, bottom = all.y + all.height;
	put(dim_[0], all.x, all.y, all.width, t.y - all.y);                       // above
	put(dim_[1], all.x, t.y + t.height, all.width, bottom - t.y - t.height);  // below
	put(dim_[2], all.x, t.y, t.x - all.x, t.height);                          // left
	put(dim_[3], t.x + t.width, t.y, right - t.x - t.width, t.height);        // right
	const int line = 2;
	put(edge_[0], t.x - line, t.y - line, t.width + 2 * line, line);
	put(edge_[1], t.x - line, t.y + t.height, t.width + 2 * line, line);
	put(edge_[2], t.x - line, t.y, line, t.height);
	put(edge_[3], t.x + t.width, t.y, line, t.height);

	// The size, in a small pill at the top left corner.
	const std::string text = std::to_string(t.width) + " × " + std::to_string(t.height);
	Output *o = server_->output_at(t.x + 1, t.y + 1);
	if (!o) o = output_;
	if (text != label_text_) {
		label_text_ = text;
		const int w = text_width(text, theme::font_small) + 20, h = 26;
		Canvas canvas(w, h, o ? o->output->scale : 1.0f);
		fill_rounded(canvas.cr(), 0, 0, w, h, h / 2.0, with_alpha(rgb(0x000000), 0.65));
		draw_text(canvas.cr(), text, 0, 0, w, h, theme::font_small, rgb(0xffffff), Align::Center);
		canvas.present(label_);
	}
	const int ly = t.y - 34 >= all.y ? t.y - 34 : t.y + 8;
	wlr_scene_node_set_position(&label_->node, t.x + (t.y - 34 >= all.y ? 0 : 8), ly);
	wlr_scene_node_set_enabled(&label_->node, true);
	if (!ink_.empty() && !drawing_) redraw_ink();   // the area moved: the lines inside it
}

// ---- Drawing on it -------------------------------------------------------------------

Color ScreenshotTool::ink_color() const {
	static const Color kInk[] = {rgb(0xe5484d), rgb(0xf5c400), rgb(0x30a46c), rgb(0x3e8ef7), rgb(0xffffff), rgb(0x111111)};
	return kInk[ink_color_ % 6];
}

// The lines, over the whole layout (only what's inside the capture counts).
void ScreenshotTool::redraw_ink() {
	if (ink_.empty()) {
		wlr_scene_node_set_enabled(&ink_node_->node, false);
		return;
	}
	wlr_box all{};
	wlr_output_layout_get_box(server_->layout, nullptr, &all);
	const wlr_box &t = target_;
	if (t.width <= 0 || t.height <= 0) {
		wlr_scene_node_set_enabled(&ink_node_->node, false);
		return;
	}
	Output *o = server_->output_at(t.x + 1, t.y + 1);
	Canvas canvas(t.width, t.height, o ? o->output->scale : 1.0f);
	cairo_t *cr = canvas.cr();
	cairo_translate(cr, -t.x, -t.y);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	for (const InkStroke &s : ink_) {
		if (s.points.empty()) continue;
		set_color(cr, s.color);
		cairo_set_line_width(cr, s.width);
		cairo_move_to(cr, s.points[0].first, s.points[0].second);
		if (s.points.size() == 1) cairo_line_to(cr, s.points[0].first + 0.1, s.points[0].second);
		for (size_t i = 1; i < s.points.size(); i++) cairo_line_to(cr, s.points[i].first, s.points[i].second);
		cairo_stroke(cr);
	}
	canvas.present(ink_node_);
	wlr_scene_node_set_position(&ink_node_->node, t.x, t.y);
	wlr_scene_node_set_enabled(&ink_node_->node, true);
}

// ---- Input -------------------------------------------------------------------------

void ScreenshotTool::motion(double lx, double ly) {
	px_ = lx;
	py_ = ly;
	const std::string h = on_bar(lx, ly) ? hot_at(lx, ly) : "";
	if (h != hover_) {
		hover_ = h;
		redraw_bar();
		if (mode_ == Mode::Window && chip_view(h)) update_target();
	}
	server_->set_cursor(on_bar(lx, ly) ? "default" : moving_ ? "grabbing" : pen_ ? "pencil" : "crosshair");
	if (drawing_ && !ink_.empty()) {
		ink_.back().points.push_back({lx, ly});
		redraw_ink();
		return;
	}
	if (selecting_) {
		area_ = normalized(press_x_, press_y_, lx, ly);
		update_target();
	} else if (moving_) {
		area_ = press_area_;
		area_.x += static_cast<int>(std::lround(lx - press_x_));
		area_.y += static_cast<int>(std::lround(ly - press_y_));
		update_target();
	} else if (mode_ != Mode::Area) {
		update_target();
	}
}

void ScreenshotTool::button(double lx, double ly, uint32_t button, bool pressed) {
	if (button == BTN_RIGHT && pressed) {
		close();
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}
	if (!pressed) {
		if (drawing_) {
			drawing_ = false;
			redraw_bar();   // undo is possible now
			return;
		}
		if (selecting_ && (area_.width < 4 || area_.height < 4)) {
			area_ = wlr_box{};   // a click, not a drag
			update_target();
		}
		selecting_ = moving_ = false;
		return;
	}
	if (on_bar(lx, ly)) {
		const std::string id = hot_at(lx, ly);
		if (id == "mode:area") set_mode(Mode::Area);
		else if (id == "mode:window") set_mode(Mode::Window);
		else if (id == "mode:screen") set_mode(Mode::Screen);
		else if (chip_view(id)) {
			// A window's chip: take that window.
			hover_ = id;
			update_target();
			capture();
		} else if (id == "pointer") {
			pointer_ = !pointer_;
			redraw_bar();
		} else if (id == "kind:photo" || id == "kind:video") {
			set_video(id == "kind:video");
		} else if (id == "audio") {
			const std::string &a = server_->config.record_audio;
			server_->write_setting("record_audio", a == "none" ? "system" : a == "system" ? "mic" : "none");
			redraw_bar();
		} else if (id == "delay") {
			delay_ = delay_ == 0 ? 3 : delay_ == 3 ? 5 : delay_ == 5 ? 10 : 0;
			redraw_bar();
		} else if (id == "pen") {
			pen_ = !pen_;
			redraw_bar();
		} else if (id == "ink") {
			ink_color_ = (ink_color_ + 1) % 6;
			pen_ = true;
			redraw_bar();
		} else if (id == "undo") {
			if (!ink_.empty()) ink_.pop_back();
			redraw_ink();
			redraw_bar();
		} else if (id == "capture") {
			capture();
		} else if (id == "close") {
			close();
		}
		return;
	}
	// With the pen, dragging inside what's to be captured draws on it.
	if (pen_ && !video_ && target_.width > 0 && inside(target_, lx, ly)) {
		InkStroke s;
		s.color = ink_color();
		s.width = 4;
		s.points.push_back({lx, ly});
		ink_.push_back(std::move(s));
		drawing_ = true;
		redraw_ink();
		return;
	}
	if (mode_ == Mode::Area) {
		press_x_ = lx;
		press_y_ = ly;
		if (area_.width > 0 && inside(area_, lx, ly)) {
			moving_ = true;   // drag the area somewhere else
			press_area_ = area_;
		} else {
			selecting_ = true;
			area_ = wlr_box{static_cast<int>(lx), static_cast<int>(ly), 0, 0};
			update_target();
		}
		return;
	}
	// A window or a screen: one click takes it.
	update_target();
	capture();
}

bool ScreenshotTool::key(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Escape:
		close();
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		capture();
		return true;
	case XKB_KEY_1:
	case XKB_KEY_a:
		set_mode(Mode::Area);
		return true;
	case XKB_KEY_2:
	case XKB_KEY_w:
		set_mode(Mode::Window);
		return true;
	case XKB_KEY_3:
	case XKB_KEY_s:
		set_mode(Mode::Screen);
		return true;
	case XKB_KEY_p:
		pointer_ = !pointer_;
		redraw_bar();
		return true;
	case XKB_KEY_c:
		set_video(false);   // as in GNOME: C for a screenshot, V for a video
		return true;
	case XKB_KEY_v:
		set_video(true);
		return true;
	case XKB_KEY_d:
	case XKB_KEY_e:
		pen_ = !pen_;   // draw
		redraw_bar();
		return true;
	case XKB_KEY_z:
	case XKB_KEY_BackSpace:
		if (!ink_.empty()) ink_.pop_back();   // undo a line
		redraw_ink();
		redraw_bar();
		return true;
	case XKB_KEY_Print:
		// Print again: the whole screen under the pointer, at once.
		set_mode(Mode::Screen);
		capture();
		return true;
	default:
		return true;   // nothing else reaches the apps meanwhile
	}
}

void ScreenshotTool::capture() {
	if (target_.width <= 0 || target_.height <= 0) {
		return;
	}
	const wlr_box t = target_;
	const bool with_pointer = pointer_;
	const int delay = delay_;
	if (video_) {
		Output *o = server_->output_at(t.x + t.width / 2.0, t.y + t.height / 2.0);
		ink_.clear();
		close();
		if (server_->recorder && o) server_->recorder->start(t, o, delay);
		return;
	}
	std::vector<InkStroke> ink = std::move(ink_);
	ink_.clear();
	close();
	// Wait for a frame without the tool on it (and the delay) first; the
	// lines drawn go onto the picture afterwards.
	server_->capture(t, with_pointer, delay, std::move(ink));
}

} // namespace dusk
