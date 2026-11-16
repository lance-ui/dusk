#include "ui.hpp"

#include <algorithm>
#include <cmath>

namespace dusk {

bool Fades::enabled = true;

UiLayer::UiLayer(wlr_scene_tree *parent, void *tag_data) {
	tree = wlr_scene_tree_create(parent);
	tree->node.data = tag_data;
	float none[4] = {0, 0, 0, 0};
	shadow_ = wlr_scene_shadow_create(tree, 1, 1, 0, 1.0f, none);
	wlr_scene_node_set_enabled(&shadow_->node, false);
	blur_ = wlr_scene_blur_create(tree, 1, 1);
	wlr_scene_node_set_enabled(&blur_->node, false);
	buffer = wlr_scene_buffer_create(tree, nullptr);
}

UiLayer::~UiLayer() {
	// Highlights are owned by the same object and go first.
	if (shadow_apart_) {
		wlr_scene_node_destroy(&shadow_->node);
	}
	wlr_scene_node_destroy(&tree->node);
}

void UiLayer::put_shadow_in(wlr_scene_tree *parent) {
	wlr_scene_node_reparent(&shadow_->node, parent);
	shadow_apart_ = true;
	show(x_, y_, alpha_, scale_, ox_, oy_);
}

void UiLayer::present(Canvas &canvas, wlr_box card, double radius) {
	w_ = canvas.width();
	h_ = canvas.height();
	card_ = card;
	radius_ = radius;
	canvas.present(buffer);
	show(x_, y_, alpha_, scale_, ox_, oy_);
}

void UiLayer::show(int x, int y, double alpha, double scale, double ox, double oy) {
	x_ = x;
	y_ = y;
	alpha_ = std::clamp(alpha, 0.0, 1.0);
	scale_ = std::max(0.05, scale);
	ox_ = ox;
	oy_ = oy;
	wlr_scene_node_set_position(&tree->node, x, y);

	// The picture, zoomed around (ox, oy).
	const double s = scale_;
	const double bx = ox - ox * s, by = oy - oy * s;
	wlr_scene_buffer_set_dest_size(buffer, std::max(1, static_cast<int>(std::lround(w_ * s))),
		std::max(1, static_cast<int>(std::lround(h_ * s))));
	wlr_scene_node_set_position(&buffer->node, static_cast<int>(std::lround(bx)),
		static_cast<int>(std::lround(by)));
	wlr_scene_buffer_set_opacity(buffer, static_cast<float>(alpha_));
	for (Highlight *h : highlights_) {
		h->refresh();
	}

	// The blur under the card: what's behind shows through, frosted.
	const double blur = blur_amount_ >= 0 ? blur_amount_ : theme::ui_blur;
	if (blur > 0.001 && card_.width > 0 && alpha_ > 0.001) {
		const auto &m = blur_margin_;
		const bool past = m.l || m.t || m.r || m.b;
		const int bw = std::max(1, static_cast<int>(std::lround(card_.width * s))) + m.l + m.r;
		const int bh = std::max(1, static_cast<int>(std::lround(card_.height * s))) + m.t + m.b;
		wlr_scene_blur_set_size(blur_, bw, bh);
		wlr_scene_blur_set_corner_radius(blur_, past ? 0 : static_cast<int>(std::lround(radius_ * s)));
		wlr_scene_blur_set_strength(blur_, static_cast<float>(blur));
		wlr_scene_blur_set_alpha(blur_, static_cast<float>(alpha_));
		wlr_scene_node_set_position(&blur_->node, static_cast<int>(std::lround(bx + card_.x * s)) - m.l,
			static_cast<int>(std::lround(by + card_.y * s)) - m.t);
		wlr_scene_node_set_enabled(&blur_->node, true);
	} else {
		wlr_scene_node_set_enabled(&blur_->node, false);
	}

	// The shadow: wide and soft, or short and offset like a drawing's.
	const int kind = theme::ui_shadow;
	if (kind == 0 || card_.width <= 0 || alpha_ <= 0.001 || body_ <= 0.02) {
		wlr_scene_node_set_enabled(&shadow_->node, false);
		return;
	}
	// (Soft: 0.35.4 made it closer and fainter, a hint of lift, not a halo.)
	const double sigma = kind == 2 ? 3.5 : 11.0;
	const double off_x = kind == 2 ? 3 : 0;
	const double off_y = kind == 2 ? 4 : 3;
	const int cw = static_cast<int>(std::lround(card_.width * s));
	const int ch = static_cast<int>(std::lround(card_.height * s));
	const int cx = static_cast<int>(std::lround(bx + card_.x * s));
	const int cy = static_cast<int>(std::lround(by + card_.y * s));
	const int pad = static_cast<int>(std::ceil(sigma * 2));
	Color c = theme::ui_shadow_color;
	c.a *= alpha_ * alpha_;   // shadows fade a little faster than what casts them
	c.a *= std::min(1.0, body_ * 1.4);   // a see-through card casts less of one
	float color[4];
	to_float(c, color);
	const int r = static_cast<int>(std::lround(radius_ * s));
	wlr_scene_shadow_set_size(shadow_, cw + 2 * pad, ch + 2 * pad);
	wlr_scene_shadow_set_blur_sigma(shadow_, static_cast<float>(sigma));
	wlr_scene_shadow_set_corner_radius(shadow_, r);
	wlr_scene_shadow_set_color(shadow_, color);
	const int sx = cx - pad + static_cast<int>(off_x), sy = cy - pad + static_cast<int>(off_y);
	// In its own layer it's placed in layout coordinates.
	wlr_scene_node_set_position(&shadow_->node, shadow_apart_ ? x + sx : sx, shadow_apart_ ? y + sy : sy);
	// Nothing under the card itself (it may be see-through).
	wlr_scene_shadow_set_clipped_region(shadow_, clipped_region{
		wlr_box{cx - sx, cy - sy, cw, ch}, corner_radii_all(r)});
	wlr_scene_node_set_enabled(&shadow_->node, !shadow_apart_ || enabled_);
}

void UiLayer::set_enabled(bool on) {
	enabled_ = on;
	wlr_scene_node_set_enabled(&tree->node, on);
	if (shadow_apart_ && !on) {
		wlr_scene_node_set_enabled(&shadow_->node, false);
	} else if (shadow_apart_) {
		show(x_, y_, alpha_, scale_, ox_, oy_);
	}
}

void UiLayer::raise() {
	wlr_scene_node_raise_to_top(&tree->node);
}

// ---- Highlight ----------------------------------------------------------------------

Highlight::Highlight(UiLayer &layer, Animator *anim) : layer_(layer), anim_(anim) {
	float none[4] = {0, 0, 0, 0};
	rect_ = wlr_scene_rect_create(layer.tree, 1, 1, none);
	rect_->accepts_input = false;
	wlr_scene_node_set_enabled(&rect_->node, false);
	layer_.highlights_.push_back(this);
}

Highlight::~Highlight() {
	if (ticker_) {
		anim_->remove_ticker(ticker_);
	}
	auto &list = layer_.highlights_;
	list.erase(std::remove(list.begin(), list.end(), this), list.end());
	wlr_scene_node_destroy(&rect_->node);
}

void Highlight::show(const wlr_box &box, double radius, const Color &color) {
	const bool was_hidden = a_ < 0.03;
	tx_ = box.x;
	ty_ = box.y;
	tw_ = box.width;
	th_ = box.height;
	ta_ = 1.0;
	radius_ = radius;
	color_ = color;
	if (was_hidden || !Fades::enabled) {
		// Appears in place rather than sliding in from far away.
		x_ = tx_;
		y_ = ty_;
		w_ = tw_;
		h_ = th_;
	}
	if (!Fades::enabled) {
		a_ = ta_;
	}
	refresh();
	if (Fades::enabled && !ticker_) {
		ticker_ = anim_->add_ticker([this](double dt) { return step(dt); });
	}
}

void Highlight::hide() {
	ta_ = 0;
	if (!Fades::enabled) {
		a_ = 0;
		refresh();
		return;
	}
	if (!ticker_ && a_ > 0) {
		ticker_ = anim_->add_ticker([this](double dt) { return step(dt); });
	}
}

void Highlight::flash() {
	flash_ = 1.0;
	if (Fades::enabled && !ticker_) {
		ticker_ = anim_->add_ticker([this](double dt) { return step(dt); });
	}
}

bool Highlight::step(double dt) {
	const double kp = 1.0 - std::exp(-dt / 45.0);   // position: quick
	const double ka = 1.0 - std::exp(-dt / 55.0);   // fade
	auto go = [](double &v, double t, double k, double eps) {
		if (std::fabs(t - v) < eps) {
			v = t;
			return false;
		}
		v += (t - v) * k;
		return true;
	};
	bool moving = false;
	moving |= go(x_, tx_, kp, 0.3);
	moving |= go(y_, ty_, kp, 0.3);
	moving |= go(w_, tw_, kp, 0.3);
	moving |= go(h_, th_, kp, 0.3);
	moving |= go(a_, ta_, ka, 0.01);
	moving |= go(flash_, 0.0, 1.0 - std::exp(-dt / 90.0), 0.01);
	refresh();
	if (!moving) {
		ticker_ = 0;
	}
	return moving;
}

void Highlight::refresh() {
	const double a = std::clamp(a_, 0.0, 1.0) * layer_.alpha();
	if (a < 0.005 || w_ < 1 || h_ < 1) {
		wlr_scene_node_set_enabled(&rect_->node, false);
		return;
	}
	const double s = layer_.scale();
	const int px = static_cast<int>(std::lround(layer_.map_x(x_)));
	const int py = static_cast<int>(std::lround(layer_.map_y(y_)));
	const int pw = std::max(1, static_cast<int>(std::lround(w_ * s)));
	const int ph = std::max(1, static_cast<int>(std::lround(h_ * s)));
	Color c = color_;
	c.a = std::min(1.0, c.a * (1.0 + 1.2 * flash_)) * a;
	float f[4];
	to_float(c, f);
	wlr_scene_rect_set_size(rect_, pw, ph);
	wlr_scene_rect_set_color(rect_, f);
	wlr_scene_rect_set_corner_radius(rect_, static_cast<int>(std::lround(
		std::min(radius_ * s, std::min(pw, ph) / 2.0))));
	wlr_scene_node_set_position(&rect_->node, px, py);
	wlr_scene_node_set_enabled(&rect_->node, true);
}

// ---- Fades ------------------------------------------------------------------------

Fades::Fades(Animator *anim, std::function<void()> redraw)
		: anim_(anim), redraw_(std::move(redraw)) {}

Fades::~Fades() {
	if (ticker_) {
		anim_->remove_ticker(ticker_);
	}
}

double Fades::get(const std::string &key, double initial) {
	auto it = values_.find(key);
	if (it == values_.end()) {
		values_[key] = Value{initial, initial};
		return initial;
	}
	return it->second.now;
}

void Fades::set(const std::string &key, double target, double initial) {
	auto it = values_.find(key);
	if (it == values_.end()) {
		it = values_.emplace(key, Value{initial, initial}).first;
	}
	if (it->second.target == target) {
		return;
	}
	it->second.target = target;
	if (!enabled) {
		it->second.now = target;
		return;
	}
	if (!ticker_) {
		ticker_ = anim_->add_ticker([this](double dt) { return step(dt); });
	}
}

void Fades::snap(const std::string &key, double value) {
	values_[key] = Value{value, value};
}

void Fades::forget(const std::string &key) {
	values_.erase(key);
}

bool Fades::step(double dt) {
	// Each value covers the same share of the remaining distance per
	// millisecond: quick at first, then settling softly.
	const double k = 1.0 - std::exp(-dt / tau_);
	bool moving = false;
	for (auto &[key, v] : values_) {
		if (v.now == v.target) {
			continue;
		}
		const double gap = v.target - v.now;
		// Close enough: 0.4 px for positions, 1% for 0..1 amounts.
		const double eps = std::max(std::fabs(v.target), std::fabs(v.now)) > 2 ? 0.4 : 0.01;
		if (std::fabs(gap) < eps) {
			v.now = v.target;
		} else {
			v.now += gap * k;
			moving = true;
		}
	}
	if (redraw_) {
		redraw_();   // may set new targets
	}
	moving = false;
	for (auto &[key, v] : values_) {
		if (v.now != v.target) {
			moving = true;
		}
	}
	if (!moving) {
		ticker_ = 0;
	}
	return moving;
}

} // namespace dusk
