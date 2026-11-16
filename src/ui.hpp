// Building blocks shared by Dusk's own interface (panels, menus, the control
// centre, popups): a layer that holds a drawn picture with a GPU shadow under
// it and can fade and zoom, and "Fades" that make hover/press highlights ease
// in and out instead of switching instantly.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "anim.hpp"
#include "draw.hpp"
#include "wlr.hpp"

namespace dusk {

class Highlight;

class UiLayer {
public:
	UiLayer(wlr_scene_tree *parent, void *tag_data);
	~UiLayer();
	UiLayer(const UiLayer &) = delete;
	UiLayer &operator=(const UiLayer &) = delete;

	wlr_scene_tree *tree;
	wlr_scene_buffer *buffer;

	// Hands over a finished drawing. `card` is where the card is inside
	// it (for the shadow); radius is its corner radius.
	void present(Canvas &canvas, wlr_box card, double radius);
	// Puts the top-left of the drawing at (x, y) in layout coordinates,
	// `alpha` opaque, zoomed by `scale` around (ox, oy) (drawing coords).
	void show(int x, int y, double alpha, double scale = 1.0, double ox = 0, double oy = 0);
	void set_enabled(bool on);
	void raise();
	// Draws the shadow in another layer (e.g. under the windows, for
	// panels: then tiled windows cover it instead of it covering them).
	void put_shadow_in(wlr_scene_tree *parent);
	// How solid the card's own background is (a panel at 0% opacity): its
	// shadow fades with it, and is gone at 0.
	void set_body_opacity(double a) { body_ = a; }
	// Its own blur (the panels' setting), instead of the menus' one. -1: theirs.
	void set_blur(double b) { blur_amount_ = b; }
	// The blur reaches this far past the card (a full-screen one: off the
	// screen's edges, where a blur ending at the edge darkened it).
	void set_blur_margin(int left, int top, int right, int bottom) { blur_margin_ = {left, top, right, bottom}; }
	int width() const { return w_; }
	int height() const { return h_; }
	// Where a point of the drawing ends up (tree-local), with the zoom.
	double map_x(double x) const { return ox_ - ox_ * scale_ + x * scale_; }
	double map_y(double y) const { return oy_ - oy_ * scale_ + y * scale_; }
	double scale() const { return scale_; }
	double alpha() const { return alpha_; }

private:
	friend class Highlight;
	std::vector<Highlight *> highlights_;
	wlr_scene_shadow *shadow_;
	double blur_amount_ = -1;
	struct { int l = 0, t = 0, r = 0, b = 0; } blur_margin_;
	wlr_scene_blur *blur_;           // frosts what's behind the card (Settings: blur)
	bool shadow_apart_ = false;   // in another layer (put_shadow_in)
	bool enabled_ = true;
	int w_ = 0, h_ = 0;
	wlr_box card_{};
	double radius_ = 0;
	int x_ = 0, y_ = 0;
	double alpha_ = 1, scale_ = 1, ox_ = 0, oy_ = 0;
	double body_ = 1;
};

// A rounded highlight drawn by the GPU over a UiLayer (hover, selection).
// It slides between items and fades in and out on its own, so moving the
// mouse never has to redraw the layer's picture.
class Highlight {
public:
	Highlight(UiLayer &layer, Animator *anim);
	~Highlight();
	Highlight(const Highlight &) = delete;
	Highlight &operator=(const Highlight &) = delete;

	// Moves to `box` (drawing coordinates) and fades in.
	void show(const wlr_box &box, double radius, const Color &color);
	void hide();
	void flash();    // a brief brighter blink, for clicks
	void refresh();  // the layer moved, zoomed or faded

private:
	bool step(double dt);
	UiLayer &layer_;
	Animator *anim_;
	wlr_scene_rect *rect_;
	uint64_t ticker_ = 0;
	double x_ = 0, y_ = 0, w_ = 0, h_ = 0, a_ = 0;
	double tx_ = 0, ty_ = 0, tw_ = 0, th_ = 0, ta_ = 0;
	double flash_ = 0;
	double radius_ = 0;
	Color color_{};
};

// Values that glide towards a target (0..1 highlights, positions...).
// Call set() whenever the target changes; the owner's redraw runs every
// frame until everything has arrived.
class Fades {
public:
	Fades(Animator *anim, std::function<void()> redraw);
	~Fades();
	Fades(const Fades &) = delete;
	Fades &operator=(const Fades &) = delete;

	// The current value for `key` (starts at `initial` the first time).
	double get(const std::string &key, double initial = 0.0);
	void set(const std::string &key, double target, double initial = 0.0);
	// Jumps there (e.g. when something new appears in place).
	void snap(const std::string &key, double value);
	void forget(const std::string &key);
	// How quickly values follow: time constant in ms (lower = faster).
	void set_speed(double tau_ms) { tau_ = tau_ms; }
	static bool enabled;   // Settings: "buttons fade and glide"

private:
	bool step(double dt);
	struct Value {
		double now, target;
	};
	Animator *anim_;
	std::function<void()> redraw_;
	std::map<std::string, Value> values_;
	uint64_t ticker_ = 0;
	double tau_ = 55;
};

} // namespace dusk
