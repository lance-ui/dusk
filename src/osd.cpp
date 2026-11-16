#include "osd.hpp"

#include <algorithm>
#include <cmath>

#include "draw.hpp"
#include "glyphs.hpp"
#include "system.hpp"

namespace dusk {

Osd::Osd(Server *server) : server_(server) {
	layer_ = std::make_unique<UiLayer>(server->layer_overlay, &tag_);
	layer_->buffer->point_accepts_input = &Osd::accepts_input;
	layer_->set_enabled(false);
	timer_ = wl_event_loop_add_timer(server->loop, &Osd::handle_timeout, this);
}

Osd::~Osd() {
	if (anim_) {
		server_->anim->cancel(anim_);
	}
	wl_event_source_remove(timer_);
	layer_.reset();
}

int Osd::handle_timeout(void *data) {
	static_cast<Osd *>(data)->hide();
	return 0;
}

void Osd::show(Kind kind, int level, bool off, const std::string &text) {
	const Config &c = server_->config;
	if (!c.osd) {
		return;
	}
	kind_ = kind;
	level_ = std::clamp(level, -1, 100);
	off_ = off;
	text_ = text;
	redraw();
	place();
	layer_->set_enabled(true);

	if (!visible_) {
		visible_ = true;
		place();
		server_->anim->cancel(anim_);
		// Fades in from wherever it is, also when it was just fading out.
		anim_ = server_->anim->tween(alpha_, 1.0, c.animate(c.anim_osd) ? c.ms(160) : 0,
			[this] { place(); }, [this] { anim_ = 0; });
	}
	wl_event_source_timer_update(timer_, c.osd_timeout);
}

void Osd::hide() {
	if (!visible_) {
		return;
	}
	visible_ = false;
	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(alpha_, 0.0, c.animate(c.anim_osd) ? c.ms(220) : 0,
		[this] { place(); },
		[this] {
			anim_ = 0;
			if (!visible_) {
				layer_->set_enabled(false);
			}
		});
}

void Osd::place() {
	Output *out = server_->output_at(server_->cursor->x, server_->cursor->y);
	if (!out) {
		return;
	}
	const wlr_box u = out->usable();
	const std::string &where = server_->config.osd_position;
	const int margin = 24;
	// One of nine places (Settings: click where on the screen).
	const bool top = where.rfind("top", 0) == 0, bottom = where.rfind("bottom", 0) == 0;
	const bool left = where.find("left") != std::string::npos, right = where.find("right") != std::string::npos;
	int x = left ? u.x + margin : right ? u.x + u.width - width_ - margin : u.x + (u.width - width_) / 2;
	int y = top ? u.y + margin : bottom ? u.y + u.height - height_ - margin - 12 : u.y + (u.height - height_) / 2;
	// A short slide in from the nearest edge while fading in/out.
	const int slide = static_cast<int>(std::lround((1.0 - alpha_) * 10));
	if (top) y -= slide;
	else if (bottom) y += slide;
	else if (left) x -= slide;
	else if (right) x += slide;
	else y += slide;
	layer_->show(x, y, alpha_);
	pos_x_ = x;
	pos_y_ = y;
}

namespace {

// The level, along a bar from (x, y) `length` long and `thick` across;
// `vertical` fills from the bottom up.
void draw_level(cairo_t *cr, bool segments, bool vertical, double x, double y, double length,
		double thick, int level, bool off) {
	const Color lit = off ? theme::text_dim : theme::accent;
	const Color unlit = with_alpha(theme::text, theme::dark ? 0.12 : 0.10);
	const double w = vertical ? thick : length, h = vertical ? length : thick;
	if (!segments) {
		fill_rounded(cr, x, y, w, h, thick / 2, unlit);
		if (level > 0) {
			const double f = std::max(thick, length * level / 100.0);
			if (vertical) {
				fill_rounded(cr, x, y + length - f, thick, f, thick / 2, lit);
			} else {
				fill_rounded(cr, x, y, f, thick, thick / 2, lit);
			}
		}
		return;
	}
	// Slanted LED-style segments, shared with the panel's workspace pager
	// (draw.hpp): the leading one glows brighter.
	const int n = vertical ? 18 : 24;
	led_segments(cr, x, y, length, thick, vertical, n,
		static_cast<int>(std::lround(level / 100.0 * n)), lit, off);
}

} // namespace

void Osd::redraw() {
	const bool has_bar = level_ >= 0;
	// Upright (Settings > On-screen display > Orientation): the level ones
	// are a tall pill with the bar; the on/off ones (microphone, Caps Lock,
	// layout) a short one with the icon and its state.
	const bool vertical = server_->config.osd_orientation == "vertical";
	width_ = vertical ? 76 : has_bar ? 340 : 280;
	height_ = vertical ? (has_bar ? 300 : 150) : has_bar ? 92 : 64;
	Output *out = server_->output_at(server_->cursor->x, server_->cursor->y);
	Canvas canvas(width_, height_, out ? out->output->scale : 1.0f);
	cairo_t *cr = canvas.cr();

	const double r = vertical ? width_ / 2.0 : has_bar ? std::min<double>(theme::ui_radius, height_ / 2.0) : height_ / 2.0;
	ui_card(cr, 0, 0, width_, height_, r, theme::menu_bg);

	// Icon in a circle: accent when on, plain when muted/off. Upright:
	// at the bottom, under the bar.
	const double icx = vertical ? width_ / 2.0 : 18 + 20;
	const double icy = vertical ? height_ - 18 - 20 : has_bar ? 32 : height_ / 2.0;
	cairo_new_path(cr);
	cairo_arc(cr, icx, icy, 20, 0, 2 * M_PI);
	set_color(cr, off_ ? theme::field_bg : theme::accent);
	cairo_fill(cr);
	set_color(cr, off_ ? theme::text_dim : theme::on_accent);

	std::string title;
	switch (kind_) {
	case Kind::Volume:
		draw_glyph(cr, speaker_icon(level_, off_), icx, icy, 22, off_ ? theme::text_dim : theme::on_accent);
		title = off_ ? "Muted" : "Volume";
		break;
	case Kind::Mic:
		draw_glyph(cr, off_ ? "mic-off" : "mic", icx, icy, 22, off_ ? theme::text_dim : theme::on_accent);
		title = off_ ? "Microphone off" : "Microphone on";
		break;
	case Kind::Brightness:
		draw_glyph(cr, level_ < 34 ? "sun-dim" : "sun", icx, icy, 22, off_ ? theme::text_dim : theme::on_accent);
		title = "Brightness";
		break;
	case Kind::CapsLock:
		draw_glyph(cr, "lock", icx, icy, 20, off_ ? theme::text_dim : theme::on_accent);
		title = off_ ? "Caps Lock off" : "Caps Lock on";
		break;
	case Kind::NumLock:
		draw_glyph(cr, "lock", icx, icy, 20, off_ ? theme::text_dim : theme::on_accent);
		title = off_ ? "Num Lock off" : "Num Lock on";
		break;
	case Kind::Layout:
		draw_glyph(cr, "keyboard", icx, icy, 22, off_ ? theme::text_dim : theme::on_accent);
		title = text_.empty() ? "Keyboard layout" : text_;
		break;
	}

	if (vertical && !has_bar) {
		// The state above the icon: "On" / "Off", or the layout's short name.
		std::string state = off_ ? "Off" : "On";
		if (kind_ == Kind::Layout) {
			state = text_.empty() ? "Layout" : text_.substr(0, std::min<size_t>(text_.size(), 3));
		}
		draw_text(cr, state, 4, 20, width_ - 8, 30, theme::font_bold,
			off_ ? theme::text_dim : theme::text, Align::Center);
		const char *what = kind_ == Kind::Mic ? "Mic" : kind_ == Kind::CapsLock ? "Caps"
			: kind_ == Kind::NumLock ? "Num" : "Keys";
		draw_text(cr, what, 4, 46, width_ - 8, 22, theme::font, theme::text_dim, Align::Center);
		layer_->present(canvas, wlr_box{0, 0, width_, height_}, r);
		return;
	}
	if (vertical) {
		draw_text(cr, off_ ? "—" : std::to_string(level_), 0, 14, width_, 28, theme::font_large,
			off_ ? theme::text_dim : theme::text, Align::Center);
		const double top = 52, bottom = icy - 20 - 14;
		draw_level(cr, server_->config.osd_style != "bar", true, (width_ - 12) / 2.0, top,
			bottom - top, 12, level_, off_);
		layer_->present(canvas, wlr_box{0, 0, width_, height_}, r);
		return;
	}

	const double tx = icx + 32;
	if (!has_bar) {
		draw_text(cr, title, tx, 0, width_ - tx - 18, height_, theme::font_large, theme::text);
		layer_->present(canvas, wlr_box{0, 0, width_, height_}, r);
		return;
	}

	draw_text(cr, title, tx, icy - 14, 180, 28, theme::font_large, theme::text);
	draw_text(cr, off_ ? "—" : std::to_string(level_) + "%", width_ - 110, icy - 16, 92, 32,
		theme::font_huge, off_ ? theme::text_dim : theme::text, Align::Right);
	draw_level(cr, server_->config.osd_style != "bar", false, 18, 64, width_ - 36, 10, level_, off_);
	layer_->present(canvas, wlr_box{0, 0, width_, height_}, r);
}

// ---- pointer interaction --------------------------------------------------------

// True while what is on the screen is a level bar that can be set (volume,
// brightness): the on/off states and the keyboard layout are not.
bool Osd::interactive() const {
	return level_ >= 0 && (kind_ == Kind::Volume || kind_ == Kind::Brightness);
}

bool Osd::accepts_input(wlr_scene_buffer *buffer, double *sx, double *sy) {
	wlr_scene_tree *tree = buffer->node.parent;
	auto *tag = tree ? static_cast<NodeTag *>(tree->node.data) : nullptr;
	if (!tag || tag->kind != NodeTag::Kind::Osd) {
		return false;
	}
	const Osd *o = static_cast<const Osd *>(tag->object);
	return o->visible_ && o->interactive() && *sx >= 0 && *sy >= 0 &&
		*sx < o->width_ && *sy < o->height_;
}

bool Osd::grabs_input(double lx, double ly) const {
	return visible_ && interactive() && lx >= pos_x_ && ly >= pos_y_ &&
		lx < pos_x_ + width_ && ly < pos_y_ + height_;
}

void Osd::pointer_set(double lx, double ly) {
	if (!interactive()) {
		return;
	}
	// Where the pointer sits along the bar, with the geometry redraw()
	// draws (across: 18 in from each side; upright: 52 down to 72 above the
	// bottom of the card).
	double frac;
	if (server_->config.osd_orientation == "vertical") {
		frac = ((height_ - 72.0) - (ly - pos_y_)) / std::max(1.0, height_ - 124.0);
	} else {
		frac = ((lx - pos_x_) - 18.0) / std::max(1.0, width_ - 36.0);
	}
	const int level = static_cast<int>(std::lround(std::clamp(frac, 0.0, 1.0) * 100));
	if (kind_ == Kind::Volume) {
		if (server_->status) {
			server_->status->set_volume(level);   // raising it also unmutes
		}
		show(Kind::Volume, level, false);
	} else if (kind_ == Kind::Brightness) {
		if (server_->status) {
			server_->status->set_brightness(level);
		}
		show(Kind::Brightness, level, false);
	}
}

void Osd::pointer_scroll(double delta) {
	if (!interactive()) {
		return;
	}
	// A notch of the wheel is one step, like the keyboard's: scrolling
	// towards you lowers, as everywhere else in Dusk. Both helpers show the
	// OSD again with the new value themselves.
	if (kind_ == Kind::Volume) {
		server_->volume_key(delta > 0 ? "down" : "up", false);
	} else if (kind_ == Kind::Brightness) {
		server_->brightness_key(delta <= 0);
	}
}

} // namespace dusk
