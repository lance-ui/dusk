// Tiling: where a dragged window lands (screen edges, corners for a
// quarter, the snap layouts bar at the top centre), the preview of that spot, and the snap layouts menu on the
// maximise button.
#include "server.hpp"

#include <algorithm>

#include "popover.hpp"

namespace dusk {

namespace {
constexpr int kEdge = 8;   // px from a screen edge that counts as "at the edge"
}

void Server::show_snap_preview(const wlr_box &box, View *below) {
	float color[4];
	to_float(with_alpha(theme::accent, 0.28), color);
	if (!snap_preview) {
		snap_preview = wlr_scene_rect_create(layer_views, box.width, box.height, color);
		snap_preview->accepts_input = false;
	}
	wlr_scene_rect_set_size(snap_preview, box.width, box.height);
	wlr_scene_rect_set_color(snap_preview, color);
	wlr_scene_rect_set_corner_radius(snap_preview, std::max(8, config.window_radius));
	wlr_scene_node_set_position(&snap_preview->node, box.x, box.y);
	// Under the window being dragged, over everything else.
	if (below && below->tree->node.parent == layer_views) {
		wlr_scene_node_place_below(&snap_preview->node, &below->tree->node);
	} else {
		wlr_scene_node_raise_to_top(&snap_preview->node);
	}
	wlr_scene_node_set_enabled(&snap_preview->node, true);
}

void Server::hide_snap_preview() {
	if (snap_preview) {
		wlr_scene_node_set_enabled(&snap_preview->node, false);
	}
}

void Server::update_drop_target(View *view, double cx, double cy) {
	drop_kind = Drop::None;
	drop_output = output_at(cx, cy);
	if (!drop_output) {
		hide_snap_preview();
		return;
	}
	const wlr_box b = drop_output->box();
	View::Zone zone;
	if (snap_layouts->drag_motion(view, drop_output, cx, cy, &zone)) {
		drop_kind = Drop::Zone;
		drop_zone = zone;
	} else if (config.edge_snap) {
		// Corners: a quarter of the screen. Near enough counts: along an edge,
		// this close to the corner.
		const double cw = std::max(60.0, b.width * 0.08), ch = std::max(60.0, b.height * 0.12);
		const bool at_left = cx <= b.x + kEdge, at_right = cx >= b.x + b.width - 1 - kEdge;
		const bool at_top = cy <= b.y + kEdge, at_bottom = cy >= b.y + b.height - 1 - kEdge;
		const bool near_left = cx <= b.x + cw, near_right = cx >= b.x + b.width - cw;
		const bool near_top = cy <= b.y + ch, near_bottom = cy >= b.y + b.height - ch;
		const bool left_corner = (at_left && (near_top || near_bottom)) || ((at_top || at_bottom) && near_left);
		const bool right_corner = (at_right && (near_top || near_bottom)) || ((at_top || at_bottom) && near_right);
		if (left_corner || right_corner) {
			drop_kind = Drop::Zone;
			drop_zone = View::Zone{left_corner ? 0.0 : 0.5, (at_top || near_top) && !at_bottom ? 0.0 : 0.5, 0.5, 0.5};
		} else if (at_top) {
			drop_kind = Drop::Maximize;
		} else if (at_left) {
			drop_kind = Drop::Left;
		} else if (at_right) {
			drop_kind = Drop::Right;
		}
	}
	switch (drop_kind) {
	case Drop::None:
		hide_snap_preview();
		break;
	case Drop::Maximize:
		show_snap_preview(drop_output->usable(), view);
		break;
	case Drop::Left:
		show_snap_preview(view->zone_frame(View::Zone{0, 0, 0.5, 1}, drop_output), view);
		break;
	case Drop::Right:
		show_snap_preview(view->zone_frame(View::Zone{0.5, 0, 0.5, 1}, drop_output), view);
		break;
	case Drop::Zone:
		show_snap_preview(view->zone_frame(drop_zone, drop_output), view);
		break;
	}
}

void Server::apply_drop(View *view) {
	Output *out = drop_output;
	if (!out || !out->enabled()) {
		return;
	}
	switch (drop_kind) {
	case Drop::None:
		break;
	case Drop::Maximize:
		view->set_maximized(true, out);
		break;
	case Drop::Left:
		view->snap_to(View::Snap::Left, out);
		break;
	case Drop::Right:
		view->snap_to(View::Snap::Right, out);
		break;
	case Drop::Zone:
		view->tile_to(drop_zone, out);
		break;
	}
}

int Server::handle_max_hover(void *data) {
	auto *s = static_cast<Server *>(data);
	View *v = s->max_hover_view;
	if (v && std::find(s->views.begin(), s->views.end(), v) != s->views.end() &&
			s->cursor_mode == CursorMode::Passthrough) {
		s->open_snap_menu(v);
	}
	return 0;
}

void Server::open_snap_menu(View *view) {
	if (!view || view->fullscreen) {
		return;
	}
	Output *out = view->tile_target();
	if (!out) {
		return;
	}
	wlr_box anchor = view->button_box(Region::Maximize);
	if (anchor.width <= 0) {
		// No maximise button (hidden, or the app draws its own title bar):
		// under the middle of the window's top edge.
		const wlr_box f = view->frame_box();
		anchor = wlr_box{f.x + f.width / 2 - 1, f.y + 4, 2, std::max(2, view->deco_top() - 8)};
	}
	close_popups();
	snap_layouts->open_for(view, out, anchor);
}

} // namespace dusk
