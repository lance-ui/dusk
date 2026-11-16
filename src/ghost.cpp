#include "ghost.hpp"

#include <algorithm>
#include <cmath>

namespace dusk {

GhostRect lerp(const GhostRect &a, const GhostRect &b, double t) {
	return GhostRect{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t,
		a.h + (b.h - a.h) * t, a.alpha + (b.alpha - a.alpha) * t};
}

GhostRect zoomed(const GhostRect &r, double scale, double alpha) {
	const double w = r.w * scale, h = r.h * scale;
	return GhostRect{r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h, alpha};
}

namespace {

bool no_input(wlr_scene_buffer *, double *, double *) {
	return false;
}

int round_int(double v) {
	return static_cast<int>(std::lround(v));
}

} // namespace

Ghost::Ghost(wlr_scene_tree *parent, wlr_scene_node *source, int frame_x, int frame_y,
		int frame_w, int frame_h)
		: origin_x_(frame_x), origin_y_(frame_y), ref_w_(std::max(1, frame_w)),
		ref_h_(std::max(1, frame_h)) {
	tree_ = wlr_scene_tree_create(parent);
	copy(source, true);
	current = GhostRect{static_cast<double>(frame_x), static_cast<double>(frame_y), ref_w_, ref_h_, 1};
	apply(current);
}

Ghost::~Ghost() {
	wlr_scene_node_destroy(&tree_->node);
}

void Ghost::place_above(wlr_scene_node *sibling) {
	if (sibling->parent == tree_->node.parent) {
		wlr_scene_node_place_above(&tree_->node, sibling);
	}
}

void Ghost::raise() {
	wlr_scene_node_raise_to_top(&tree_->node);
}

void Ghost::copy(wlr_scene_node *node, bool root) {
	if (!node->enabled && !root) {
		return;
	}
	int lx = 0, ly = 0;
	wlr_scene_node_coords(node, &lx, &ly);
	const double rx = lx - origin_x_, ry = ly - origin_y_;
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		auto *tree = reinterpret_cast<wlr_scene_tree *>(node);
		wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			copy(child, false);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		wlr_scene_buffer *src = wlr_scene_buffer_from_node(node);
		if (!src->buffer) {
			break;
		}
		wlr_scene_buffer *c = wlr_scene_buffer_create(tree_, src->buffer);
		const wlr_fbox &sb = src->src_box;
		if (sb.x >= 0 && sb.y >= 0 && sb.width >= 0 && sb.height >= 0) {
			wlr_scene_buffer_set_source_box(c, &sb);
		}
		wlr_scene_buffer_set_transform(c, src->transform);
		wlr_scene_buffer_set_corner_radii(c, src->corners);
		c->point_accepts_input = no_input;
		const double w = src->dst_width > 0 ? src->dst_width : src->buffer->width;
		const double h = src->dst_height > 0 ? src->dst_height : src->buffer->height;
		parts_.push_back(Part{&c->node, 0, rx, ry, w, h, Color{}, clipped_region{}});
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		wlr_scene_rect *src = reinterpret_cast<wlr_scene_rect *>(node);
		wlr_scene_rect *c = wlr_scene_rect_create(tree_, src->width, src->height, src->color);
		wlr_scene_rect_set_corner_radii(c, src->corners);
		c->accepts_input = false;
		const float *f = src->color;
		parts_.push_back(Part{&c->node, 1, rx, ry, static_cast<double>(src->width),
			static_cast<double>(src->height), Color{f[0], f[1], f[2], f[3]}, src->clipped_region});
		break;
	}
	case WLR_SCENE_NODE_SHADOW: {
		wlr_scene_shadow *src = reinterpret_cast<wlr_scene_shadow *>(node);
		wlr_scene_shadow *c = wlr_scene_shadow_create(tree_, src->width, src->height,
			src->corner_radius, src->blur_sigma, src->color);
		const float *f = src->color;
		parts_.push_back(Part{&c->node, 2, rx, ry, static_cast<double>(src->width),
			static_cast<double>(src->height), Color{f[0], f[1], f[2], f[3]}, src->clipped_region, src->blur_sigma});
		break;
	}
	default:
		break;
	}
}

// Stretches every part from the frame's original size to `r`.
void Ghost::apply(const GhostRect &r) {
	current = r;
	double sx = r.w / ref_w_, sy = r.h / ref_h_;
	double ox = 0, oy = 0;
	if (uniform) {
		const double k = std::min(sx, sy);
		ox = (r.w - ref_w_ * k) / 2;
		oy = (r.h - ref_h_ * k) / 2;
		sx = sy = k;
	}
	const double a = std::clamp(r.alpha, 0.0, 1.0);
	// Fully faded: off altogether (nothing left drawn, and nothing to draw).
	wlr_scene_node_set_enabled(&tree_->node, a > 0.002);
	for (Part &p : parts_) {
		const int px = round_int(r.x + ox + p.x * sx), py = round_int(r.y + oy + p.y * sy);
		const int pw = std::max(1, round_int(p.w * sx)), ph = std::max(1, round_int(p.h * sy));
		wlr_scene_node_set_position(p.node, px, py);
		if (p.kind == 0) {
			auto *b = wlr_scene_buffer_from_node(p.node);
			wlr_scene_buffer_set_dest_size(b, pw, ph);
			wlr_scene_buffer_set_opacity(b, static_cast<float>(a));
			continue;
		}
		// Rect and shadow colours are premultiplied: scale them all.
		float f[4] = {static_cast<float>(p.color.r * a), static_cast<float>(p.color.g * a),
			static_cast<float>(p.color.b * a), static_cast<float>(p.color.a * a)};
		clipped_region clip = p.clip;
		clip.area.x = round_int(clip.area.x * sx);
		clip.area.y = round_int(clip.area.y * sy);
		clip.area.width = round_int(clip.area.width * sx);
		clip.area.height = round_int(clip.area.height * sy);
		if (p.kind == 1) {
			auto *rect = reinterpret_cast<wlr_scene_rect *>(p.node);
			wlr_scene_rect_set_size(rect, pw, ph);
			wlr_scene_rect_set_color(rect, f);
			wlr_scene_rect_set_clipped_region(rect, clip);
		} else {
			auto *sh = reinterpret_cast<wlr_scene_shadow *>(p.node);
			wlr_scene_shadow_set_size(sh, pw, ph);
			// The shadow's softness shrinks with the copy (a full-size blur
			// round a small copy was a dark smudge).
			wlr_scene_shadow_set_blur_sigma(sh, static_cast<float>(std::max(1.0, p.sigma * std::min(sx, sy))));
			wlr_scene_shadow_set_color(sh, f);
			wlr_scene_shadow_set_clipped_region(sh, clip);
		}
	}
}

} // namespace dusk
