// A frozen copy of a window's picture (its surfaces, frame and shadow) that
// can be moved, stretched and faded on its own. Window animations use it:
// opening and closing, minimising into the panel, and gliding to a new
// size while the app redraws underneath.
#pragma once

#include <vector>

#include "draw.hpp"
#include "wlr.hpp"

namespace dusk {

// Where the window's frame is drawn, and how opaque.
struct GhostRect {
	double x = 0, y = 0, w = 1, h = 1, alpha = 1;
};

GhostRect lerp(const GhostRect &a, const GhostRect &b, double t);
// The rect shrunk/grown by `scale` around its centre.
GhostRect zoomed(const GhostRect &r, double scale, double alpha);

class Ghost {
public:
	// Copies everything visible under `source`. Positions are taken
	// relative to the frame at (frame_x, frame_y), size frame_w x frame_h.
	Ghost(wlr_scene_tree *parent, wlr_scene_node *source, int frame_x, int frame_y,
		int frame_w, int frame_h);
	~Ghost();
	Ghost(const Ghost &) = delete;
	Ghost &operator=(const Ghost &) = delete;

	void apply(const GhostRect &r);
	void place_above(wlr_scene_node *sibling);
	void raise();

	GhostRect current;   // as last applied
	// Keep the picture's proportions (scale evenly, centred in the rect)
	// instead of stretching it: for windows changing shape.
	bool uniform = false;

private:
	struct Part {
		wlr_scene_node *node;
		int kind;            // 0 buffer, 1 rect, 2 shadow
		double x, y, w, h;   // relative to the frame, at full size
		Color color;         // rects and shadows
		clipped_region clip; // relative to the part
		double sigma = 0;    // shadows: their softness at full size
	};
	void copy(wlr_scene_node *node, bool root);

	wlr_scene_tree *tree_;
	std::vector<Part> parts_;
	int origin_x_, origin_y_;
	double ref_w_, ref_h_;
};

} // namespace dusk
