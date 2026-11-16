// What every window has in common: the title bar we draw, placement and
// window states (maximised, snapped, minimised, fullscreen).
#include <algorithm>
#include <cmath>

#include "draw.hpp"
#include "launcher.hpp"
#include "overview.hpp"
#include "panel.hpp"
#include "server.hpp"
#include "popover.hpp"

namespace dusk {

namespace {

uint64_t next_view_id = 1;

int button_slot() {
	return theme::button_gtk ? 34 : theme::button_sketch ? 30 : 34;
}

// A stroke with a slight bow in it, so lines look drawn by hand.
void hand_line(cairo_t *cr, double x1, double y1, double x2, double y2, double bow) {
	const double mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
	const double dx = x2 - x1, dy = y2 - y1;
	const double len = std::max(0.001, std::hypot(dx, dy));
	cairo_move_to(cr, x1, y1);
	cairo_curve_to(cr, mx - dy / len * bow, my + dx / len * bow, mx - dy / len * bow,
		my + dx / len * bow, x2, y2);
}

// One corner bracket: the corner at (x, y), arms of `arm` length going in
// the directions (hx, 0) and (0, vy).
void bracket(cairo_t *cr, double x, double y, double hx, double vy, double arm) {
	cairo_move_to(cr, x + hx * arm, y);
	cairo_line_to(cr, x, y);
	cairo_line_to(cr, x, y + vy * arm);
}

// The "sketch" buttons: minimise = two corners pointing in, maximise = two
// corners pointing out, restore = pointing in on the other diagonal, and a
// red hand-drawn X for close. Hovered/pressed buttons sit on a dark,
// scribbled blob.
void sketch_button(cairo_t *cr, Region kind, bool maximized, double cx, double cy, bool lit,
		bool down, bool active) {
	if (lit || down) {
		const double r = 9.5, s = 12.5;
		rounded_rect(cr, cx - s, cy - s, 2 * s, 2 * s, r);
		const Color blob = theme::dark ? rgb(down ? 0x1a1a1d : 0x232326)
			: rgb(down ? 0xd9d6de : 0xe6e3ea);
		set_color(cr, blob);
		cairo_fill_preserve(cr);
		cairo_save(cr);
		cairo_clip_preserve(cr);
		// Scribble shading: a few loose diagonal strokes.
		const Color hatch = theme::dark ? rgb(0xffffff, 0.05) : rgb(0x000000, 0.05);
		set_color(cr, hatch);
		cairo_set_line_width(cr, 1.3);
		for (double o = -2 * s; o < 2 * s; o += 4.2) {
			hand_line(cr, cx + o - s, cy + s, cx + o + s, cy - s, 1.2);
		}
		cairo_stroke(cr);
		cairo_restore(cr);
		// An uneven outline, like a pen going round twice.
		set_color(cr, theme::dark ? rgb(0x3a3a3f) : rgb(0xc4c0cb));
		cairo_set_line_width(cr, 1.4);
		cairo_stroke(cr);
		rounded_rect(cr, cx - s + 0.8, cy - s + 0.4, 2 * s - 1.2, 2 * s - 0.6, r + 0.5);
		set_color(cr, theme::dark ? rgb(0x3a3a3f, 0.5) : rgb(0xc4c0cb, 0.5));
		cairo_set_line_width(cr, 0.8);
		cairo_stroke(cr);
	}
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_new_path(cr);
	if (kind == Region::Close) {
		const Color red = rgb(theme::dark ? 0xe0353d : 0xc9202a, active || lit ? 1.0 : 0.65);
		set_color(cr, red);
		cairo_set_line_width(cr, 2.4);
		hand_line(cr, cx - 5, cy - 4.5, cx + 5, cy + 5, -0.6);
		cairo_stroke(cr);
		// The second stroke ends with a little flick, like the drawing.
		hand_line(cr, cx + 5, cy - 5, cx - 5, cy + 4.5, 0.5);
		cairo_stroke(cr);
		cairo_move_to(cr, cx + 2.2, cy + 1.4);
		cairo_curve_to(cr, cx + 4.0, cy + 2.4, cx + 5.8, cy + 2.6, cx + 6.6, cy + 4.0);
		cairo_set_line_width(cr, 1.4);
		cairo_stroke(cr);
		return;
	}
	const Color ink = with_alpha(theme::text, active || lit ? 0.95 : 0.55);
	set_color(cr, ink);
	cairo_set_line_width(cr, 2.0);
	const double arm = 4.0;
	if (kind == Region::Minimize) {
		// Pointing in: top-left and bottom-right.
		bracket(cr, cx - 1.8, cy - 1.8, -1, -1, arm);
		bracket(cr, cx + 1.8, cy + 1.8, 1, 1, arm);
	} else if (maximized) {
		// Restore: pointing in on the other diagonal.
		bracket(cr, cx + 1.8, cy - 1.8, 1, -1, arm);
		bracket(cr, cx - 1.8, cy + 1.8, -1, 1, arm);
	} else {
		// Maximise: pointing out, top-right and bottom-left.
		bracket(cr, cx + 5.5, cy - 5.5, -1, 1, arm);
		bracket(cr, cx - 5.5, cy + 5.5, 1, -1, arm);
	}
	cairo_stroke(cr);
}

} // namespace

// Titlebar buttons from the outer edge in: close, maximise, minimise (the
// last two can be turned off in Settings).
std::vector<View::Button> View::buttons(int titlebar_width) const {
	std::vector<Region> kinds = {Region::Close};
	if (theme::show_maximize) {
		kinds.push_back(Region::Maximize);
	}
	if (theme::show_minimize) {
		kinds.push_back(Region::Minimize);
	}
	std::vector<Button> out;
	const int slot = button_slot();
	for (size_t i = 0; i < kinds.size(); i++) {
		const int n = static_cast<int>(i);
		const int bx = theme::buttons_left ? 4 + n * slot : titlebar_width - (n + 1) * slot - 4;
		out.push_back(Button{kinds[i], bx});
	}
	return out;
}

View::View(Server *server) : server(server), id(next_view_id++) {
	workspace = server->workspace;   // new windows open on the workspace you're on
	tree = wlr_scene_tree_create(server->layer_views);
	tree->node.data = &tag;

	float black[4] = {0, 0, 0, 1};
	backdrop = wlr_scene_rect_create(tree, 1, 1, black);
	wlr_scene_node_set_enabled(&backdrop->node, false);

	float shade[4];
	to_float(theme::shadow, shade);
	shadow = wlr_scene_shadow_create(tree, 1, 1, 0, 1.0f, shade);
	wlr_scene_node_set_enabled(&shadow->node, false);
	frost = wlr_scene_blur_create(tree, 1, 1);
	wlr_scene_node_set_enabled(&frost->node, false);

	frame = wlr_scene_tree_create(tree);
	float color[4];
	to_float(theme::border_inactive, color);
	border = wlr_scene_rect_create(frame, 1, 1, color);
	titlebar = wlr_scene_buffer_create(frame, nullptr);
	// Under the app (it's created after this), so the app's own rounded
	// corners sit on top of the line.
	csd_outline = wlr_scene_rect_create(tree, 1, 1, color);
	wlr_scene_node_set_enabled(&csd_outline->node, false);

	wlr_scene_node_set_enabled(&tree->node, false);
	wlr_scene_node_set_enabled(&frame->node, false);
	morph_timer_ = wl_event_loop_add_timer(server->loop, &View::handle_morph_timer, this);
}

View::~View() {
	server->anim->cancel(ghost_anim_);
	ghost_.reset();
	drop_fading();
	wl_event_source_remove(morph_timer_);
	if (open_idle_) {
		wl_event_source_remove(open_idle_);
	}
	// Destroys our frame and the app's scene tree with it.
	wlr_scene_node_destroy(&tree->node);
}

bool View::on_shown_workspace() const {
	return sticky || workspace == server->workspace ||
		std::find(server->workspace_leaving.begin(), server->workspace_leaving.end(), this) !=
			server->workspace_leaving.end();
}

bool View::visible() const {
	return mapped && !minimized && (sticky || workspace == server->workspace);
}

void View::sync_enabled() {
	wlr_scene_node_set_enabled(&tree->node, mapped && !minimized && on_shown_workspace());
}

bool View::active() const {
	return server->focused == this;
}

wlr_box View::frame_box() const {
	return wlr_box{x, y, content_width() + deco_width(), content_height() + deco_height()};
}

Output *View::output() const {
	if (!placed) {
		return server->output_at(server->cursor->x, server->cursor->y);
	}
	return server->output_for_view(this);
}

void View::move_to(int nx, int ny) {
	move_pending_ = false;
	x = nx;
	y = ny;
	wlr_scene_node_set_position(&tree->node, x, y);
	send_position();
}

void View::configure_frame(int nx, int ny, int content_w, int content_h) {
	content_w = std::max(1, content_w);
	content_h = std::max(1, content_h);

	// Glide to the new size: a stretched copy moves there while the app
	// redraws underneath, then the real window takes over.
	const Config &c = server->config;
	const bool resizes = content_w != content_width() || content_h != content_height();
	// (Not while a button is held: dragging a window out of maximised or
	// into a tile, the copy would be left behind where the window was.)
	const bool morph = mapped && !minimized && resizes && c.animate(c.anim_maximize) &&
		server->cursor_mode == CursorMode::Passthrough && server->buttons_down == 0;
	GhostRect from;
	if (morph) {
		take_ghost();
		ghost_->uniform = true;   // zoom the snapshot, don't squash it
		from = ghost_->current;
		hide_real(true);
	}
	const auto finish = [&] {
		if (!morph) {
			return;
		}
		const GhostRect to{static_cast<double>(nx), static_cast<double>(ny),
			static_cast<double>(content_w + deco_width()), static_cast<double>(content_h + deco_height()), 1};
		morphing_ = true;
		morph_ready_ = false;
		run_ghost(from, to, c.ms(260), [this] {
			morph_ready_ = true;
			try_finish_morph();
		});
		// In case the app never answers.
		wl_event_source_timer_update(morph_timer_, static_cast<int>(c.ms(260)) + 150);
	};

	if (!acks_sizes() || !mapped ||
			(content_w == content_width() && content_h == content_height())) {
		// X11 apps get the position together with the size.
		move_to(nx, ny);
		request_content_size(content_w, content_h);
		finish();
		return;
	}
	const uint32_t serial = request_content_size(content_w, content_h);
	if (serial == 0) {
		move_to(nx, ny);
		finish();
		return;
	}
	move_pending_ = true;
	pending_x_ = nx;
	pending_y_ = ny;
	pending_serial_ = serial;
	pending_since_ = now_ms();
	finish();
}

void View::set_activated(bool on) {
	if (ready()) {
		send_activated(on);
	}
	update_layer();
	redraw_frame();
}

void View::set_server_side(bool on) {
	if (on == server_side) {
		return;
	}
	server_side = on;
	update_positions();
}

// Puts the app's surface inside our frame and redraws the frame.
void View::update_positions() {
	if (content) {
		// wlroots' xdg scene tree already shifts the app's surface by its
		// shadow margin (the xdg geometry), so the tree's origin is the
		// visible window's top-left. Shifting again put GTK windows up and
		// left of where Dusk thought they were.
		wlr_scene_node_set_position(&content->node, deco_left(), deco_top());
	}
	wlr_scene_node_set_enabled(&frame->node, decorated());

	// Fullscreen apps that don't fill the screen get black around them.
	Output *out = fullscreen ? output() : nullptr;
	if (out) {
		const wlr_box b = out->box();
		wlr_scene_rect_set_size(backdrop, b.width, b.height);
		wlr_scene_node_set_position(&backdrop->node, b.x - x, b.y - y);
	}
	wlr_scene_node_set_enabled(&backdrop->node, out != nullptr);
	redraw_frame();
	update_effects();
}

int View::corner_radius() const {
	if (maximized || fullscreen || (snap != Snap::None && server->config.tile_gap == 0)) {
		return 0;   // edge-to-edge windows stay square, like KDE
	}
	return std::max(0, server->config.window_radius);
}

namespace {

// Finds the scene buffer that shows `surface` inside `node`.
wlr_scene_buffer *find_surface_buffer(wlr_scene_node *node, wlr_surface *surface) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
		return ss && ss->surface == surface ? buffer : nullptr;
	}
	if (node->type == WLR_SCENE_NODE_TREE) {
		auto *tree = reinterpret_cast<wlr_scene_tree *>(node);   // node is its first member
		wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			if (wlr_scene_buffer *found = find_surface_buffer(child, surface)) {
				return found;
			}
		}
	}
	return nullptr;
}

} // namespace

// Rounds the corners of the app's own surface so it fits our rounded frame.
void View::update_effects() {
	wlr_surface *s = surface();
	if (!content || !s) {
		return;
	}
	wlr_scene_buffer *buffer = find_surface_buffer(&content->node, s);
	if (!buffer) {
		return;
	}
	const int r = corner_radius();
	if (decorated()) {
		// Our title bar is on top, so only the bottom corners.
		wlr_scene_buffer_set_corner_radii(buffer,
			corner_radii_bottom(std::max(0, r - frame_line())));
	} else if (geometry_x() == 0 && geometry_y() == 0 &&
			content_width() >= s->current.width && content_height() >= s->current.height) {
		// An app drawing its own frame with no shadow margin (e.g. Steam,
		// Discord): round all of it. Apps with their own shadows (GTK) are
		// left alone; they already draw rounded corners.
		wlr_scene_buffer_set_corner_radii(buffer, corner_radii_all(r));
	} else {
		wlr_scene_buffer_set_corner_radii(buffer, corner_radii_none());
	}
}

// A fullscreen window covers the panel, but only while it is focused, so
// Alt+Tab to another window works as expected.
void View::update_layer() {
	// (Also when it's simply the top window on its screen, focused or not: a
	// game whose launcher kept the keyboard, or one that grabbed focus its
	// own X11 way, still covers the panel. Another window raised over it
	// sends it back down.)
	bool top = false;
	if (fullscreen && !active() && mapped) {
		Output *out = output();
		for (View *v : server->views) {
			if (v == this) {
				top = true;
				break;
			}
			if (v->mapped && v->visible() && v->output() == out) break;   // something's above it
		}
	}
	wlr_scene_tree *target = (fullscreen && (active() || top))
		? server->layer_fullscreen : server->layer_views;
	if (tree->node.parent != target) {
		wlr_scene_node_reparent(&tree->node, target);
		if (active()) {
			wlr_scene_node_raise_to_top(&tree->node);
		}
	}
}

void View::redraw_frame() {
	// Apps that draw their own title bar but no shadow (Chrome, Vivaldi,
	// Electron, Steam) get ours too; GTK apps draw their own (in a margin).
	const bool csd_plain = !server_side && !fullscreen && geometry_x() == 0 && geometry_y() == 0;
	const bool show_shadow = (decorated() || csd_plain) && !maximized && snap == Snap::None &&
		theme::shadow_kind != 0 && mapped;
	wlr_scene_node_set_enabled(&shadow->node, show_shadow);
	draw_csd_outline();
	const int b = frame_line();
	const int cw = content_width(), ch = content_height();
	const int w = cw + deco_width(), h = ch + deco_height();
	const int r = corner_radius();
	const int inner = std::max(0, r - b);
	const bool is_active = active();
	const double alpha = std::clamp(anim_alpha, 0.0, 1.0);

	// Shadow: a hard, offset comic-style shadow, or a soft one.
	if (show_shadow) {
		Color sc = theme::shadow;
		if (!is_active) {
			sc.a *= theme::shadow_kind >= 2 ? 0.40 : 0.55;
		}
		sc.a *= alpha;
		float shade[4];
		to_float(sc, shade);
		wlr_scene_shadow_set_color(shadow, shade);
		wlr_scene_shadow_set_corner_radius(shadow, r);
		if (theme::shadow_kind == 2 || theme::shadow_kind == 3) {
			// Hard: a crisp offset copy (comic style). Ink: the same idea but
			// shorter and slightly soft, like a shadow drawn under a figure.
			const bool hard = theme::shadow_kind == 2;
			const int off_x = hard ? 5 : 3, off_y = hard ? 5 : 5;
			const float sigma = hard ? 1.0f : 4.5f;
			const int pad = hard ? 0 : 8;
			wlr_scene_shadow_set_blur_sigma(shadow, sigma);
			wlr_scene_shadow_set_size(shadow, w + 2 * pad, h + 2 * pad);
			wlr_scene_node_set_position(&shadow->node, off_x - pad, off_y - pad);
			wlr_scene_shadow_set_clipped_region(shadow, clipped_region{
				wlr_box{pad - off_x, pad - off_y, w, h}, corner_radii_all(r)});
		} else {
			const int sigma = is_active ? 14 : 9, drop = is_active ? 4 : 2;   // subtle (0.35.4)
			wlr_scene_shadow_set_blur_sigma(shadow, sigma);
			wlr_scene_shadow_set_size(shadow, w + 2 * sigma, h + 2 * sigma);
			wlr_scene_node_set_position(&shadow->node, -sigma, -sigma + drop);
			wlr_scene_shadow_set_clipped_region(shadow, clipped_region{
				wlr_box{sigma, sigma - drop, w, h}, corner_radii_all(r)});
		}
	}
	// Dusk's own Settings, see-through (Appearance › Settings app): frosted
	// behind, like the panels and menus.
	{
		const Config &c = server->config;
		const bool on = mapped && !fullscreen && c.settings_opacity < 0.995 && c.settings_blur > 0.001 &&
			app_id() == "org.dusk.Settings";
		wlr_scene_node_set_enabled(&frost->node, on);
		if (on) {
			wlr_scene_blur_set_size(frost, std::max(1, w), std::max(1, h));
			wlr_scene_blur_set_corner_radius(frost, r);
			wlr_scene_blur_set_strength(frost, static_cast<float>(c.settings_blur));
			wlr_scene_blur_set_alpha(frost, static_cast<float>(alpha));
			wlr_scene_node_set_position(&frost->node, 0, 0);
		}
	}
	if (!decorated()) {
		return;
	}

	// The outline: a rounded rectangle with a rounded hole for the window.
	wlr_scene_node_set_enabled(&border->node, b > 0);
	if (b > 0) {
		Color bc = is_active ? theme::border_active : theme::border_inactive;
		bc.a *= alpha;
		float color[4];
		to_float(bc, color);
		wlr_scene_rect_set_size(border, w, h);
		wlr_scene_rect_set_color(border, color);
		wlr_scene_rect_set_corner_radius(border, r);
		wlr_scene_rect_set_clipped_region(border, clipped_region{
			wlr_box{b, b, w - 2 * b, h - 2 * b}, corner_radii_all(inner)});
	}

	wlr_scene_node_set_position(&titlebar->node, b, b);
	wlr_scene_buffer_set_corner_radii(titlebar, corner_radii_top(inner));

	// Only repaint the titlebar image when something visible changed.
	std::string t = title();
	if (cw == last_frame_w_ && is_active == last_active_ && t == last_title_ &&
			hover == last_hover_ && pressed == last_pressed_) {
		return;
	}
	last_frame_w_ = cw;
	last_active_ = is_active;
	last_title_ = t;
	last_hover_ = hover;
	last_pressed_ = pressed;

	const int tw = std::max(cw, 1);
	const int th = theme::title_height;
	Canvas canvas(tw, th, server->max_scale());
	cairo_t *cr = canvas.cr();

	set_color(cr, is_active ? theme::title_active : theme::title_inactive);
	cairo_paint(cr);

	// A hairline between the title bar and the app, and a light rim along
	// the top edge (the highlight colourists put on top of a shape).
	cairo_rectangle(cr, 0, th - 1, tw, 1);
	set_color(cr, theme::button_gtk ? (theme::dark ? rgb(0x000000, 0.36) : rgb(0x000000, 0.07))
		: theme::divider);
	cairo_fill(cr);
	if (theme::line_width > 0 && !theme::button_gtk) {
		cairo_pattern_t *rim = cairo_pattern_create_linear(0, 0, 0, 3);
		const Color &c = theme::rim;
		cairo_pattern_add_color_stop_rgba(rim, 0, c.r, c.g, c.b, c.a * (is_active ? 1.0 : 0.6));
		cairo_pattern_add_color_stop_rgba(rim, 1, c.r, c.g, c.b, 0);
		cairo_rectangle(cr, 0, 0, tw, 3);
		cairo_set_source(cr, rim);
		cairo_fill(cr);
		cairo_pattern_destroy(rim);
	}

	// App icon and title.
	const std::vector<Button> bs = buttons(tw);
	const int slot = button_slot();
	const int reserve_buttons = static_cast<int>(bs.size()) * slot + 8;
	const Color tcolor = is_active ? theme::title_text : theme::title_text_inactive;
	const double icon = 16;
	if (!theme::show_titles) {
		// No name: just the buttons (Settings > Windows > Title bar).
	} else if (theme::title_left) {
		const double ix = theme::buttons_left ? reserve_buttons + 4 : 10;
		draw_icon(cr, server->icons, server->apps.icon_for(app_id(), title()), t, ix,
			(th - icon) / 2.0, icon, server->max_scale());
		draw_text(cr, t, ix + icon + 8, 0, tw - reserve_buttons - icon - 30, th,
			is_active ? theme::font_title : theme::font, tcolor, Align::Left);
	} else {
		const int reserve = reserve_buttons + 6;
		// GNOME header bars: a bold title, active or not.
		draw_text(cr, t, reserve, 0, tw - 2 * reserve, th,
			theme::button_gtk ? theme::font_bold : is_active ? theme::font_title : theme::font, tcolor,
			Align::Center);
	}

	for (const Button &button : bs) {
		const double cx = button.x + slot / 2.0;
		const double cy = th / 2.0;
		const bool lit = hover == button.kind;
		const bool down = pressed == button.kind;

		if (theme::button_sketch) {
			sketch_button(cr, button.kind, maximized, cx, cy, lit, down, is_active);
			continue;
		}
		if (theme::button_gtk) {
			// Like GTK/libadwaita: a soft round button with a symbolic icon.
			Color bg = theme::button_bg;
			if (down) {
				bg = with_alpha(theme::text, 0.28);
			} else if (lit) {
				bg = with_alpha(theme::text, 0.17);
			}
			if (!is_active && !lit && !down) {
				bg.a *= 0.6;
			}
			// 24 px circles with libadwaita's own glyphs (drawn, so they look
			// the same whatever the icon theme).
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, 12, 0, 2 * M_PI);
			set_color(cr, bg);
			cairo_fill(cr);
		} else if (lit || down) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, 11, 0, 2 * M_PI);
			set_color(cr, button.kind == Region::Close ? theme::close_hover : theme::button_hover);
			cairo_fill(cr);
		}

		// Drawn glyphs (plain style, or when the icon theme has no icon).
		const bool white = !theme::button_gtk && (lit || down) && button.kind == Region::Close;
		set_color(cr, white ? rgb(0xffffff) : tcolor);
		cairo_set_line_width(cr, theme::button_gtk ? 1.6 : 1.5);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_new_path(cr);
		const double g = theme::button_gtk ? 3.5 : 4.0;
		switch (button.kind) {
		case Region::Close:
			cairo_move_to(cr, cx - g, cy - g);
			cairo_line_to(cr, cx + g, cy + g);
			cairo_move_to(cr, cx + g, cy - g);
			cairo_line_to(cr, cx - g, cy + g);
			break;
		case Region::Maximize:
			if (maximized) {
				// "Restore": two overlapping squares.
				rounded_rect(cr, cx - 4.5, cy - 2, 6.5, 6.5, 1.5);
				cairo_move_to(cr, cx - 2, cy - 2);
				cairo_line_to(cr, cx - 2, cy - 4.5);
				cairo_line_to(cr, cx + 4.5, cy - 4.5);
				cairo_line_to(cr, cx + 4.5, cy + 2);
				cairo_line_to(cr, cx + 2, cy + 2);
			} else if (theme::button_gtk) {
				rounded_rect(cr, cx - 4, cy - 4, 8, 8, 1.5);
			} else {
				rounded_rect(cr, cx - 4.5, cy - 4.5, 9, 9, 2);
			}
			break;
		default:
			// GNOME's minimise sits low, like a window going down.
			cairo_move_to(cr, cx - 4, cy + (theme::button_gtk ? 3 : 0.5));
			cairo_line_to(cr, cx + 4, cy + (theme::button_gtk ? 3 : 0.5));
			break;
		}
		cairo_stroke(cr);
	}

	canvas.present(titlebar, true);   // kept: the overview copies it
}

// ---- animations ---------------------------------------------------------------

namespace {

// Applies `alpha` to every picture in the tree (app surfaces, title bar).
void set_tree_opacity(wlr_scene_node *node, float alpha) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(node), alpha);
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		auto *tree = reinterpret_cast<wlr_scene_tree *>(node);
		wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			set_tree_opacity(child, alpha);
		}
	}
}

} // namespace

// ---- animations ---------------------------------------------------------------

GhostRect View::frame_rect(double alpha) const {
	const wlr_box f = frame_box();
	return GhostRect{static_cast<double>(f.x), static_cast<double>(f.y),
		static_cast<double>(f.width), static_cast<double>(f.height), alpha};
}

void View::set_slide(int dx, double alpha) {
	slide_x = dx;
	wlr_scene_node_set_position(&tree->node, x + dx, y);
	if (!hidden_) {
		set_tree_opacity(&tree->node, static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
	}
}

// Hides (or shows again) the real window: its pictures, frame and shadow.
void View::hide_real(bool hide) {
	hidden_ = hide;
	anim_alpha = hide ? 0.0 : 1.0;
	set_tree_opacity(&tree->node, hide ? 0.0f : 1.0f);
	redraw_frame();
}

// Makes the ghost (a copy of the window as it looks now), or keeps the
// one already running so a new animation carries on from where it is.
void View::take_ghost() {
	drop_fading();
	if (ghost_) {
		server->anim->cancel(ghost_anim_);
		ghost_anim_ = 0;
		return;
	}
	// Copy the frame at full strength even if the window is hidden.
	const double saved = anim_alpha;
	anim_alpha = 1.0;
	redraw_frame();
	const wlr_box f = frame_box();
	ghost_ = std::make_shared<Ghost>(tree->node.parent, &tree->node, f.x, f.y, f.width, f.height);
	ghost_->place_above(&tree->node);
	anim_alpha = saved;
	redraw_frame();
	ghost_->current.alpha = hidden_ || minimized ? 0.0 : 1.0;
	ghost_->apply(ghost_->current);
}

void View::drop_ghost() {
	server->anim->cancel(ghost_anim_);
	ghost_anim_ = 0;
	ghost_.reset();
}

void View::run_ghost(const GhostRect &from, const GhostRect &to, double ms,
		std::function<void()> done) {
	std::shared_ptr<Ghost> g = ghost_;
	g->apply(from);
	ghost_anim_ = server->anim->start(ms, [g, from, to](double t) {
		g->apply(lerp(from, to, t));
	}, [this, done] {
		ghost_anim_ = 0;
		if (done) done();
	});
}

void View::cancel_animation() {
	drop_fading();
	if (morphing_) {
		morphing_ = false;
		wl_event_source_timer_update(morph_timer_, 0);
	}
	// A cancelled animation must not leave a pending move behind: the next
	// commit would otherwise jump the window to a stale position.
	move_pending_ = false;
	if (ghost_) {
		drop_ghost();
		if (!minimized) {
			hide_real(false);
		}
	}
}

bool View::task_rect(GhostRect *out) const {
	for (Output *o : server->outputs) {
		for (const auto &panel : o->panels) {
			wlr_box b;
			if (panel->task_box(this, &b)) {
				*out = GhostRect{static_cast<double>(b.x), static_cast<double>(b.y),
					static_cast<double>(b.width), static_cast<double>(b.height), 0};
				return true;
			}
		}
	}
	return false;
}

void View::animate_open() {
	const Config &c = server->config;
	drop_ghost();
	if (!c.animate(c.anim_windows)) {
		hide_real(false);
		return;
	}
	// Hidden until the scene has the app's first picture (after this
	// event), then a copy of it zooms/slides/fades in.
	hide_real(true);
	if (!open_idle_) {
		open_idle_ = wl_event_loop_add_idle(server->loop, [](void *data) {
			static_cast<View *>(data)->start_open_ghost();
		}, this);
	}
}

void View::start_open_ghost() {
	open_idle_ = nullptr;
	if (!mapped || minimized) {
		if (!minimized) hide_real(false);
		return;
	}
	const Config &c = server->config;
	const GhostRect to = frame_rect(1.0);
	GhostRect from;
	if (c.anim_open_style == "slide") {
		from = to;
		from.y += 28;
		from.alpha = 0;
	} else if (c.anim_open_style == "fade") {
		from = frame_rect(0.0);
	} else {
		from = zoomed(to, 0.88, 0.0);
	}
	take_ghost();
	run_ghost(from, to, c.ms(260), [this] {
		hide_real(false);
		drop_ghost();
	});
}

void View::snapshot_and_fade_out() {
	const Config &c = server->config;
	wl_event_source_timer_update(morph_timer_, 0);
	morphing_ = false;
	if (!c.animate(c.anim_windows)) {
		drop_ghost();
		return;
	}
	take_ghost();
	// The ghost now lives on by itself (the window is going away).
	std::shared_ptr<Ghost> g = ghost_;
	ghost_.reset();
	ghost_anim_ = 0;
	const GhostRect from = g->current.alpha > 0 ? g->current : frame_rect(1.0);
	GhostRect to;
	if (c.anim_open_style == "slide") {
		to = from;
		to.y += 22;
		to.alpha = 0;
	} else if (c.anim_open_style == "fade") {
		to = from;
		to.alpha = 0;
	} else {
		to = zoomed(from, 0.9, 0.0);
	}
	server->anim->start(c.ms(210) * std::max(0.3, from.alpha), [g, from, to](double t) {
		g->apply(lerp(from, to, t));
	}, [g] {});
}

int View::handle_morph_timer(void *data) {
	auto *view = static_cast<View *>(data);
	if (view->move_pending_) {
		view->move_to(view->pending_x_, view->pending_y_);   // the app never answered
	}
	view->morph_ready_ = true;
	view->try_finish_morph();
	return 0;
}

// The glide has arrived and the app has drawn its new size: show the
// real window again, the copy fading out over it.
void View::try_finish_morph() {
	if (!morphing_ || !morph_ready_ || move_pending_) {
		return;
	}
	morphing_ = false;
	wl_event_source_timer_update(morph_timer_, 0);
	hide_real(false);
	if (!ghost_) {
		return;
	}
	std::shared_ptr<Ghost> g = ghost_;
	ghost_.reset();
	server->anim->cancel(ghost_anim_);
	ghost_anim_ = 0;
	const GhostRect from = g->current;
	GhostRect to = frame_rect(0.0);
	// A last quick fade of the copy over the real window (kept, so it can be
	// cut short: a copy left fading where the window was looked like a ghost).
	drop_fading();
	fading_ = g;
	fade_anim_ = server->anim->start(server->config.ms(90), [g, from, to](double t) {
		g->apply(lerp(from, to, t));
	}, [this] {
		fade_anim_ = 0;
		fading_.reset();
	});
}

void View::drop_fading() {
	if (fade_anim_) server->anim->cancel(fade_anim_);
	fade_anim_ = 0;
	fading_.reset();
}

// Apps that draw their own title bar (GTK, libadwaita, Electron...) get
// the same outline as our framed windows, just outside their edges.
void View::draw_csd_outline() {
	const int b = std::min(frame_line(), 3);
	const bool show = !server_side && !fullscreen && !maximized && snap == Snap::None && b > 0 &&
		content_width() > 0 && content_height() > 0;
	wlr_scene_node_set_enabled(&csd_outline->node, show);
	if (!show) {
		return;
	}
	const int cw = content_width(), ch = content_height();
	// Apps with a shadow margin round their own corners (GTK 4 and
	// libadwaita use 12 px); the rest we round ourselves (update_effects).
	const bool own_shadow = geometry_x() > 0 || geometry_y() > 0;
	const int r = own_shadow ? server->config.csd_radius : corner_radius();
	Color bc = active() ? theme::border_active : theme::border_inactive;
	bc.a *= std::clamp(anim_alpha, 0.0, 1.0);
	float color[4];
	to_float(bc, color);
	wlr_scene_rect_set_size(csd_outline, cw + 2 * b, ch + 2 * b);
	wlr_scene_node_set_position(&csd_outline->node, -b, -b);
	wlr_scene_rect_set_color(csd_outline, color);
	wlr_scene_rect_set_corner_radius(csd_outline, r > 0 ? r + b : 0);
	wlr_scene_rect_set_clipped_region(csd_outline, clipped_region{
		wlr_box{b, b, cw, ch}, corner_radii_all(r)});
}

Region View::region_at(double lx, double ly, uint32_t *edges) const {
	*edges = 0;
	const wlr_box f = frame_box();
	const double rx = lx - f.x, ry = ly - f.y;
	const bool inside = rx >= 0 && ry >= 0 && rx < f.width && ry < f.height;

	if (!decorated()) {
		return inside ? Region::Content : Region::None;
	}

	const bool resizable = !maximized;
	const int margin = resizable ? kResizeMargin : 0;
	if (rx < -margin || ry < -margin || rx >= f.width + margin || ry >= f.height + margin) {
		return Region::None;
	}

	if (resizable) {
		const int corner = 18;
		uint32_t e = 0;
		if (rx < frame_line()) {
			e |= WLR_EDGE_LEFT;
		} else if (rx >= f.width - frame_line()) {
			e |= WLR_EDGE_RIGHT;
		}
		if (ry < frame_line() + 2) {
			e |= WLR_EDGE_TOP;
		} else if (ry >= f.height - frame_line()) {
			e |= WLR_EDGE_BOTTOM;
		}
		if (e) {
			// Near a corner, resize both ways.
			if (e & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
				if (ry < corner) {
					e |= WLR_EDGE_TOP;
				} else if (ry >= f.height - corner) {
					e |= WLR_EDGE_BOTTOM;
				}
			}
			if (e & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) {
				if (rx < corner) {
					e |= WLR_EDGE_LEFT;
				} else if (rx >= f.width - corner) {
					e |= WLR_EDGE_RIGHT;
				}
			}
			*edges = e;
			return Region::Edge;
		}
	}

	if (!inside) {
		return Region::None;
	}
	if (ry < frame_line() + theme::title_height) {
		const double tx = rx - frame_line();
		const int tw = f.width - 2 * frame_line();
		for (const Button &b : buttons(tw)) {
			if (tx >= b.x && tx < b.x + button_slot()) {
				return b.kind;
			}
		}
		return Region::Titlebar;
	}
	return Region::Content;
}

// ---- window states --------------------------------------------------------

// A window that started maximised or fullscreen has no "normal" size yet;
// give it a reasonable one, centred, for when it is restored.
void View::ensure_restore_box() {
	if (restore.width > 0 && restore.height > 0) {
		return;
	}
	Output *out = output();
	const wlr_box u = out ? out->usable() : wlr_box{0, 0, 1280, 720};
	restore.width = std::max(320, u.width * 3 / 5 - deco_width());
	restore.height = std::max(240, u.height * 2 / 3 - deco_height());
	restore.x = u.x + (u.width - restore.width - deco_width()) / 2;
	restore.y = u.y + (u.height - restore.height - deco_height()) / 2;
}

void View::toggle_maximize() {
	set_maximized(!maximized);
}

void View::set_maximized(bool on, Output *on_output) {
	if (fullscreen) {
		// Remember it for when the window leaves fullscreen.
		fullscreen_was_maximized = on;
		return;
	}
	Output *out = on_output ? on_output : (on ? output() : tile_target());
	if (!out) {
		return;
	}
	if (on) {
		if (!maximized && snap == Snap::None) {
			restore = wlr_box{x, y, content_width(), content_height()};
		}
		maximized = true;
		snap = Snap::None;
		tile_output = out->name();
		const wlr_box u = out->usable();
		configure_frame(u.x, u.y, u.width - deco_width(), u.height - deco_height());
	} else if (maximized) {
		maximized = false;
		tile_output.clear();
		ensure_restore_box();
		configure_frame(restore.x, restore.y, restore.width, restore.height);
	}
	if (ready()) {
		send_maximized(maximized);
	}
	invalidate_frame();   // the restore icon changed
	redraw_frame();
}

void View::set_fullscreen(bool on) {
	if (on == fullscreen) {
		if (ready()) {
			send_fullscreen(on);   // confirm the state the app asked about
		}
		return;
	}
	Output *out = output();
	if (!out) {
		return;
	}
	if (on) {
		if (!maximized && snap == Snap::None) {
			restore = wlr_box{x, y, content_width(), content_height()};
		}
		fullscreen_was_maximized = maximized;
		fullscreen_was_snap = snap;
		maximized = false;
		snap = Snap::None;
		fullscreen = true;
		if (ready()) {
			send_fullscreen(true);
		}
		const wlr_box b = out->box();
		configure_frame(b.x, b.y, b.width, b.height);
	} else {
		fullscreen = false;
		if (ready()) {
			send_fullscreen(false);
		}
		if (fullscreen_was_maximized) {
			maximized = true;
			refit();
		} else if (fullscreen_was_snap != Snap::None) {
			snap = fullscreen_was_snap;
			refit();
		} else {
			ensure_restore_box();
			configure_frame(restore.x, restore.y, restore.width, restore.height);
		}
	}
	invalidate_frame();
	update_layer();
	update_positions();
	server->views_changed();
}

// The screen a tiled or maximised window belongs to: the one it was tiled
// on (not where its middle happens to be), if that screen is still on.
Output *View::tile_target() const {
	if (!tile_output.empty()) {
		for (Output *o : server->outputs) {
			if (o->enabled() && o->name() == tile_output) {
				return o;
			}
		}
	}
	return output();
}

wlr_box View::zone_frame(const Zone &z, Output *on) const {
	const wlr_box u = on->usable();
	const int gap = std::max(0, server->config.tile_gap);
	int x0 = u.x + static_cast<int>(std::lround(z.x * u.width));
	int x1 = u.x + static_cast<int>(std::lround((z.x + z.w) * u.width));
	int y0 = u.y + static_cast<int>(std::lround(z.y * u.height));
	int y1 = u.y + static_cast<int>(std::lround((z.y + z.h) * u.height));
	// Gaps: a full one at the screen's edges, half on each side between windows.
	x0 += z.x <= 0.001 ? gap : gap / 2;
	y0 += z.y <= 0.001 ? gap : gap / 2;
	x1 -= z.x + z.w >= 0.999 ? gap : gap - gap / 2;
	y1 -= z.y + z.h >= 0.999 ? gap : gap - gap / 2;
	return wlr_box{x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
}

// Tells the app whether it's tiled: GTK apps then drop their shadow and
// rounded corners and fill the spot exactly.
void View::sync_tiled() {
	const bool tiled = snap != Snap::None && !maximized && !fullscreen;
	if (tiled != sent_tiled_ && ready()) {
		sent_tiled_ = tiled;
		send_tiled(tiled);
	}
}

// Re-applies the fullscreen/maximised/tiled size, e.g. after the screen changed.
void View::refit() {
	sync_tiled();
	if (!fullscreen && !maximized && snap == Snap::None) {
		// A floating window: kept on its screen, and made smaller if the
		// screen got smaller (a bigger scale leaves less room).
		Output *out = mapped && placed ? output() : nullptr;
		if (!out) return;
		const wlr_box u = out->usable();
		const wlr_box f = frame_box();
		if (u.width <= 0 || u.height <= 0) return;
		const int fw = std::min(f.width, u.width), fh = std::min(f.height, u.height);
		// A window can be wider than the usable area: clamp the bounds first
		// so std::clamp is never handed a lower bound above its upper one.
		const int max_x = std::max(u.x, u.x + u.width - fw);
		const int max_y = std::max(u.y, u.y + u.height - fh);
		const int nx = std::clamp(f.x, u.x, max_x);
		const int ny = std::clamp(f.y, u.y, max_y);
		if (fw != f.width || fh != f.height) {
			configure_frame(nx, ny, std::max(1, fw - deco_width()), std::max(1, fh - deco_height()));
		} else if (f.y < u.y || f.y > u.y + u.height - 40 || f.x + f.width < u.x + 60 || f.x > u.x + u.width - 60) {
			// (Only when it's really out of reach: a window you pushed half
			// off the side on purpose stays where it is.)
			move_to(nx, ny);
		}
		return;
	}
	Output *out = fullscreen ? output() : tile_target();
	if (!out) {
		return;
	}
	if (fullscreen) {
		const wlr_box b = out->box();
		configure_frame(b.x, b.y, b.width, b.height);
		update_positions();
		return;
	}
	if (maximized) {
		const wlr_box u = out->usable();
		configure_frame(u.x, u.y, u.width - deco_width(), u.height - deco_height());
	} else {
		const wlr_box f = zone_frame(zone, out);
		configure_frame(f.x, f.y, std::max(1, f.width - deco_width()), std::max(1, f.height - deco_height()));
	}
}

void View::tile_to(const Zone &z, Output *on) {
	if (fullscreen) {
		return;
	}
	Output *out = on ? on : output();
	if (!out) {
		return;
	}
	if (!maximized && snap == Snap::None) {
		restore = wlr_box{x, y, content_width(), content_height()};
	}
	if (maximized) {
		maximized = false;
		if (ready()) {
			send_maximized(false);
		}
	}
	snap = Snap::Zone;
	zone = z;
	tile_output = out->name();
	invalidate_frame();
	sync_tiled();
	refit();
}

void View::snap_to(Snap where, Output *on) {
	if (fullscreen) {
		return;
	}
	if (where == Snap::None) {
		if (snap != Snap::None) {
			snap = Snap::None;
			tile_output.clear();
			sync_tiled();
			ensure_restore_box();
			configure_frame(restore.x, restore.y, restore.width, restore.height);
		}
		return;
	}
	tile_to(where == Snap::Left ? Zone{0, 0, 0.5, 1} : Zone{0.5, 0, 0.5, 1}, on);
	snap = where;
}

// A title bar button's place on screen (empty if it isn't shown).
wlr_box View::button_box(Region which) const {
	if (!decorated()) {
		return wlr_box{};
	}
	const wlr_box f = frame_box();
	const int tw = f.width - 2 * frame_line();
	for (const Button &b : buttons(tw)) {
		if (b.kind == which) {
			return wlr_box{f.x + frame_line() + b.x, f.y + frame_line(), button_slot(),
				theme::title_height};
		}
	}
	return wlr_box{};
}

// Dragging a maximised or snapped window by its title bar gives it back its
// old size, keeping the pointer at the same relative spot on the title bar.
void View::unsnap_for_drag(double cursor_x, double) {
	if (!maximized && snap == Snap::None) {
		return;
	}
	cancel_animation();
	const wlr_box f = frame_box();
	const double fraction = f.width > 0 ? (cursor_x - f.x) / f.width : 0.5;
	ensure_restore_box();
	const int new_frame_w = restore.width + deco_width();

	if (maximized && ready()) {
		send_maximized(false);
	}
	maximized = false;
	snap = Snap::None;
	tile_output.clear();
	sync_tiled();
	invalidate_frame();

	move_to(static_cast<int>(std::lround(cursor_x - fraction * new_frame_w)), f.y);
	request_content_size(restore.width, restore.height);
}

void View::minimize() {
	if (minimized) {
		return;
	}
	cancel_animation();
	const Config &c = server->config;
	const bool animate = c.animate(c.anim_minimize) && mapped;
	GhostRect from;
	if (animate) {
		take_ghost();
		from = ghost_->current.alpha > 0 ? ghost_->current : frame_rect(1.0);
	}
	minimized = true;
	if (server->focused == this) {
		server->focus_next_after(this);
	}
	server->views_changed();
	wlr_scene_node_set_enabled(&tree->node, false);
	if (!animate) {
		return;
	}
	// The window's copy shrinks into its button on the panel (or just
	// drops and fades), above the window that now has focus.
	ghost_->raise();
	GhostRect to;
	if (c.anim_minimize_style != "panel" || !task_rect(&to)) {
		to = zoomed(from, 0.85, 0.0);
		to.y += 36;
	}
	run_ghost(from, to, c.ms(300), [this] { drop_ghost(); });
}

void View::restore_from_minimize() {
	if (minimized) {
		minimized = false;
		const Config &c = server->config;
		if (!c.animate(c.anim_minimize) || !mapped) {
			drop_ghost();
			sync_enabled();
			hide_real(false);
		} else {
			// Out of the panel button again (from wherever it got to).
			GhostRect from;
			if (ghost_) {
				take_ghost();
				from = ghost_->current;
			} else {
				if (c.anim_minimize_style != "panel" || !task_rect(&from)) {
					from = zoomed(frame_rect(1.0), 0.85, 0.0);
					from.y += 36;
				}
				take_ghost();
			}
			const GhostRect to = frame_rect(1.0);
			run_ghost(from, to, c.ms(300), [this] {
				sync_enabled();
				hide_real(false);
				drop_ghost();
			});
		}
	}
	server->focus(this);
	if (ghost_) {
		ghost_->place_above(&tree->node);   // focusing raised the window
	}
}

// ---- lifecycle, called by XdgView / XwaylandView -----------------------------

void View::did_map() {
	mapped = true;
	if (!placed) {
		if (!maximized && !fullscreen && snap == Snap::None) {
			server->place_new_view(this);
		}
		placed = true;
	}
	sync_enabled();
	update_positions();
	animate_open();
	server->focus(this);
	server->views_changed();
	server->normalize_workspaces();   // a window on the spare workspace: there's a new spare
	// Opened while the overview is up (started from its search, or by
	// itself): the overview makes way and the new window shows, as in GNOME.
	if (server->overview && server->overview->is_open()) server->overview->close(this);
}

void View::did_unmap() {
	if (server->overview) server->overview->view_gone(this);
	mapped = false;
	// No commit can arrive for a gone surface: drop any move it was waiting
	// for, or a re-map of the same view would jump to the stale position.
	move_pending_ = false;
	wlr_scene_node_set_enabled(&tree->node, false);
	server->workspace_leaving.erase(std::remove(server->workspace_leaving.begin(),
		server->workspace_leaving.end(), this), server->workspace_leaving.end());
	if (server->grab_view == this) {
		server->grab_view = nullptr;
		server->cursor_mode = CursorMode::Passthrough;
	}
	if (server->focused == this) {
		server->focus_next_after(this);
	}
	server->views_changed();
	server->normalize_workspaces();
}

void View::did_commit() {
	// A move that was waiting for the app to draw its new size.
	if (move_pending_) {
		const bool acked = static_cast<int32_t>(acked_serial() - pending_serial_) >= 0;
		if (acked || now_ms() - pending_since_ > 500) {
			move_to(pending_x_, pending_y_);
			try_finish_morph();
		}
	}
	// New surfaces (e.g. a subsurface) appearing mid-animation stay hidden
	// like the rest of the window.
	if (hidden_) {
		set_tree_opacity(&tree->node, 0.0f);
	}
	// While resizing from the left or top edge, keep the opposite edge still.
	if (server->cursor_mode == CursorMode::Resize && server->grab_view == this) {
		const wlr_box f = frame_box();
		const wlr_box &g = server->grab_box;
		int nx = x, ny = y;
		if (server->resize_edges & WLR_EDGE_LEFT) {
			nx = g.x + g.width - f.width;
		}
		if (server->resize_edges & WLR_EDGE_TOP) {
			ny = g.y + g.height - f.height;
		}
		if (nx != x || ny != y) {
			move_to(nx, ny);
		}
	}
	// Only when something changed: redoing the frame and the rounded corners
	// on every frame an app draws (a browser playing a video, a game) was
	// needless work for each picture.
	wlr_surface *surf = surface();
	const int sw = surf ? surf->current.width : 0, sh = surf ? surf->current.height : 0;
	const int cw = content_width(), ch = content_height();
	if (cw != seen_cw_ || ch != seen_ch_ || sw != seen_sw_ || sh != seen_sh_ || geometry_x() != seen_gx_ ||
			geometry_y() != seen_gy_ || hidden_) {
		seen_cw_ = cw;
		seen_ch_ = ch;
		seen_sw_ = sw;
		seen_sh_ = sh;
		seen_gx_ = geometry_x();
		seen_gy_ = geometry_y();
		update_positions();
	}
}

void View::did_destroy() {
	Server *s = server;
	if (s->overview) s->overview->view_gone(this);
	if (s->snap_layouts) s->snap_layouts->view_gone(this);
	if (s->max_hover_view == this) s->max_hover_view = nullptr;
	s->views.remove(this);
	if (s->focused == this) s->focused = nullptr;
	if (s->hover_view == this) s->hover_view = nullptr;
	if (s->pressed_view == this) s->pressed_view = nullptr;
	if (s->last_title_click_view == this) s->last_title_click_view = nullptr;
	if (s->grab_view == this) {
		s->grab_view = nullptr;
		s->cursor_mode = CursorMode::Passthrough;
	}
	s->switcher.erase(std::remove(s->switcher.begin(), s->switcher.end(), this),
		s->switcher.end());
	s->workspace_leaving.erase(std::remove(s->workspace_leaving.begin(), s->workspace_leaving.end(), this),
		s->workspace_leaving.end());
	s->views_changed();
	s->normalize_workspaces();
}

} // namespace dusk
