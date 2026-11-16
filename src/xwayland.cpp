// X11 apps through XWayland: normal windows (XwaylandView) and
// menus/tooltips (Unmanaged).
#ifdef DUSK_XWAYLAND

#include <unistd.h>
#include <algorithm>

#include "server.hpp"

namespace dusk {

// ---- starting XWayland -------------------------------------------------------

void Server::start_xwayland() {
	// "lazy": the X server only starts when the first X11 app connects.
	xwayland = wlr_xwayland_create(display, compositor, true);
	if (!xwayland) {
		wlr_log(WLR_ERROR, "XWayland could not be started; X11 apps won't work");
		unsetenv("DISPLAY");
		return;
	}
	on_xwayland_ready.connect<&Server::handle_xwayland_ready>(&xwayland->events.ready, this);
	on_xwayland_new_surface.connect<&Server::handle_xwayland_new_surface>(
		&xwayland->events.new_surface, this);
	setenv("DISPLAY", xwayland->display_name, 1);
	update_xwayland_cursor();
}

// The cursor X11 apps show over their windows until they set their own.
void Server::update_xwayland_cursor() {
	if (!xwayland || !cursor_manager) {
		return;
	}
	wlr_xcursor_manager_load(cursor_manager, 1);
	if (wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(cursor_manager, "default", 1)) {
		wlr_xcursor_image *image = xcursor->images[0];
		if (wlr_buffer *buffer = wlr_xcursor_image_get_buffer(image)) {
			wlr_xwayland_set_cursor(xwayland, buffer, image->hotspot_x, image->hotspot_y);
		}
	}
}

void Server::handle_xwayland_ready(void *) {
	wlr_xwayland_set_seat(xwayland, seat);
	// Old-style tray icons (Wine and Proton games) into the panel's tray,
	// instead of Wine's own little window (a separate helper: see src/xembed).
	static bool tray_started = false;
	if (!tray_started) {
		tray_started = true;
		// (Next to dusk itself: /usr/local/bin may not be on the PATH.)
		std::string helper = "dusk-xembed";
		char self[4096];
		const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
		if (n > 0) {
			self[n] = '\0';
			std::string dir(self);
			dir.resize(dir.find_last_of('/'));
			if (access((dir + "/dusk-xembed").c_str(), X_OK) == 0) helper = dir + "/dusk-xembed";
		}
		spawn("DISPLAY=" + std::string(xwayland->display_name) + " exec '" + helper + "'");
	}
}

void Server::handle_xwayland_new_surface(void *data) {
	auto *xsurface = static_cast<wlr_xwayland_surface *>(data);
	if (xsurface->override_redirect) {
		new Unmanaged(this, xsurface);
	} else {
		views.push_front(new XwaylandView(this, xsurface));
	}
}

// ---- normal X11 windows --------------------------------------------------------

XwaylandView::XwaylandView(Server *server, wlr_xwayland_surface *xsurface)
		: View(server), xsurface(xsurface) {
	xsurface->data = this;
	on_associate_.connect<&XwaylandView::handle_associate>(&xsurface->events.associate, this);
	on_dissociate_.connect<&XwaylandView::handle_dissociate>(
		&xsurface->events.dissociate, this);
	on_destroy_.connect<&XwaylandView::handle_destroy>(&xsurface->events.destroy, this);
	on_request_configure_.connect<&XwaylandView::handle_request_configure>(
		&xsurface->events.request_configure, this);
	on_request_move_.connect<&XwaylandView::handle_request_move>(
		&xsurface->events.request_move, this);
	on_request_resize_.connect<&XwaylandView::handle_request_resize>(
		&xsurface->events.request_resize, this);
	on_request_minimize_.connect<&XwaylandView::handle_request_minimize>(
		&xsurface->events.request_minimize, this);
	on_request_maximize_.connect<&XwaylandView::handle_request_maximize>(
		&xsurface->events.request_maximize, this);
	on_request_fullscreen_.connect<&XwaylandView::handle_request_fullscreen>(
		&xsurface->events.request_fullscreen, this);
	on_request_activate_.connect<&XwaylandView::handle_request_activate>(
		&xsurface->events.request_activate, this);
	on_set_title_.connect<&XwaylandView::handle_set_title>(&xsurface->events.set_title, this);
	on_set_class_.connect<&XwaylandView::handle_set_title>(&xsurface->events.set_class, this);
	on_set_decorations_.connect<&XwaylandView::handle_set_decorations>(
		&xsurface->events.set_decorations, this);

	server_side = xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
	if (xsurface->surface) {
		handle_associate(nullptr);
	}
}

std::string XwaylandView::title() const {
	if (xsurface->title && *xsurface->title) {
		return xsurface->title;
	}
	if (xsurface->class_ && *xsurface->class_) {
		return xsurface->class_;
	}
	return "Window";
}

std::string XwaylandView::app_id() const {
	// WM_CLASS "class" is the closest thing X11 has to an app id.
	return xsurface->class_ ? xsurface->class_ : "";
}

void XwaylandView::configure(int cx, int cy, int w, int h) {
	wlr_xwayland_surface_configure(xsurface, static_cast<int16_t>(cx),
		static_cast<int16_t>(cy), static_cast<uint16_t>(std::max(1, w)),
		static_cast<uint16_t>(std::max(1, h)));
}

uint32_t XwaylandView::request_content_size(int w, int h) {
	configure(x + deco_left(), y + deco_top(), w, h);
	return 0;   // X11 apps don't acknowledge sizes; the move happens right away
}

void XwaylandView::send_position() {
	configure(x + deco_left(), y + deco_top(), content_width(), content_height());
}

void XwaylandView::send_activated(bool on) {
	if (on && xsurface->minimized) {
		wlr_xwayland_surface_set_minimized(xsurface, false);
	}
	wlr_xwayland_surface_activate(xsurface, on);
}

void XwaylandView::send_maximized(bool on) {
	wlr_xwayland_surface_set_maximized(xsurface, on, on);
}

void XwaylandView::send_fullscreen(bool on) {
	wlr_xwayland_surface_set_fullscreen(xsurface, on);
}

void XwaylandView::close() {
	wlr_xwayland_surface_close(xsurface);
}

void XwaylandView::handle_associate(void *) {
	wlr_surface *s = xsurface->surface;
	on_map_.connect<&XwaylandView::handle_map>(&s->events.map, this);
	on_unmap_.connect<&XwaylandView::handle_unmap>(&s->events.unmap, this);
	on_commit_.connect<&XwaylandView::handle_commit>(&s->events.commit, this);
}

void XwaylandView::handle_dissociate(void *) {
	on_map_.disconnect();
	on_unmap_.disconnect();
	on_commit_.disconnect();
}

void XwaylandView::handle_map(void *) {
	// (For "games act up" reports: dusk-doctor / the journal show this.)
	wlr_log(WLR_INFO, "x11 map: class=%s title=%s %dx%d+%d+%d fullscreen=%d deco=%d parent=%d",
		xsurface->class_ ? xsurface->class_ : "", xsurface->title ? xsurface->title : "", xsurface->width,
		xsurface->height, xsurface->x, xsurface->y, xsurface->fullscreen, static_cast<int>(xsurface->decorations),
		xsurface->parent != nullptr);
	if (!content) {
		content = wlr_scene_subsurface_tree_create(tree, xsurface->surface);
	}
	if (!placed) {
		server_side = xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;

		// Respect a position the app asked for (e.g. splash screens, dialogs).
		const bool has_position = xsurface->size_hints &&
			(xsurface->size_hints->flags &
				(XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION));
		if (has_position) {
			int nx = xsurface->x - deco_left();
			int ny = xsurface->y - deco_top();
			if (Output *out = server->output_at(xsurface->x, xsurface->y)) {
				ny = std::max(ny, out->usable().y);   // keep the title bar on screen
			}
			placed = true;
			move_to(nx, ny);
		}

		// Start in the state the app asked for.
		if (xsurface->fullscreen) {
			set_fullscreen(true);
		} else if (fills_an_output(xsurface->x, xsurface->y, xsurface->width, xsurface->height)) {
			placed = true;
			move_to(xsurface->x, xsurface->y);   // (on that screen, for set_fullscreen)
			auto_fullscreen_ = true;
			set_fullscreen(true);
		} else if (xsurface->maximized_horz && xsurface->maximized_vert) {
			set_maximized(true);
		}
	}
	did_map();
}

void XwaylandView::handle_unmap(void *) {
	if (mapped && !minimized) {
		snapshot_and_fade_out();
	}
	did_unmap();
	if (content) {
		wlr_scene_node_destroy(&content->node);
		content = nullptr;
	}
}

void XwaylandView::handle_commit(void *) {
	did_commit();
}

void XwaylandView::handle_destroy(void *) {
	xsurface->data = nullptr;
	did_destroy();
	delete this;
}

void XwaylandView::handle_request_configure(void *data) {
	auto *event = static_cast<wlr_xwayland_surface_configure_event *>(data);
	if (!mapped) {
		configure(event->x, event->y, event->width, event->height);
		return;
	}
	if (fullscreen && auto_fullscreen_ &&
			!fills_an_output(event->x, event->y, event->width, event->height)) {
		// The game went back to a window (its own setting): let it.
		auto_fullscreen_ = false;
		set_fullscreen(false);
		x = event->x - deco_left();
		y = event->y - deco_top();
		wlr_scene_node_set_position(&tree->node, x, y);
		configure(event->x, event->y, event->width, event->height);
		return;
	}
	if (fullscreen || maximized || snap != Snap::None) {
		send_position();   // keep our size; just tell the app again
		return;
	}
	if (fills_an_output(event->x, event->y, event->width, event->height)) {
		x = event->x;
		y = event->y;
		wlr_scene_node_set_position(&tree->node, x, y);
		auto_fullscreen_ = true;
		set_fullscreen(true);
		server->focus(this);
		return;
	}
	// Floating windows may move and resize themselves.
	x = event->x - deco_left();
	y = event->y - deco_top();
	wlr_scene_node_set_position(&tree->node, x, y);
	configure(event->x, event->y, event->width, event->height);
}

Output *XwaylandView::fills_an_output(int fx, int fy, int fw, int fh) const {
	// Frameless only: an ordinary window maximised by hand has a frame.
	if (xsurface->override_redirect || xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL ||
		xsurface->parent || fw <= 0 || fh <= 0) {
		return nullptr;
	}
	for (Output *o : server->outputs) {
		const wlr_box b = o->box();
		if (b.width > 0 && fx == b.x && fy == b.y && fw == b.width && fh == b.height) return o;
	}
	return nullptr;
}

void XwaylandView::handle_request_move(void *) {
	if (mapped && !fullscreen) {
		server->begin_move(this);
	}
}

void XwaylandView::handle_request_resize(void *data) {
	auto *event = static_cast<wlr_xwayland_resize_event *>(data);
	if (mapped && !fullscreen) {
		server->begin_resize(this, event->edges);
	}
}

void XwaylandView::handle_request_minimize(void *data) {
	auto *event = static_cast<wlr_xwayland_minimize_event *>(data);
	if (!mapped) {
		return;
	}
	if (event->minimize) {
		minimize();
	} else {
		restore_from_minimize();
	}
}

void XwaylandView::handle_request_maximize(void *) {
	if (!mapped) {
		return;   // applied when it maps
	}
	const bool want = xsurface->maximized_horz && xsurface->maximized_vert;
	if (want != maximized) {
		set_maximized(want);
	} else {
		send_maximized(maximized);
	}
}

void XwaylandView::handle_request_fullscreen(void *) {
	if (!mapped) {
		return;   // applied when it maps
	}
	auto_fullscreen_ = false;
	set_fullscreen(xsurface->fullscreen);
	// A game going full screen wants the keyboard and controller: it gets them.
	if (xsurface->fullscreen) server->focus(this);
}

void XwaylandView::handle_request_activate(void *) {
	if (mapped) {
		server->focus(this);
	}
}

void XwaylandView::handle_set_title(void *) {
	redraw_frame();
	server->views_changed();
}

void XwaylandView::handle_set_decorations(void *) {
	set_server_side(xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL);
}

// ---- X11 menus and tooltips ------------------------------------------------------

Unmanaged::Unmanaged(Server *server, wlr_xwayland_surface *xsurface)
		: server(server), xsurface(xsurface) {
	on_associate_.connect<&Unmanaged::handle_associate>(&xsurface->events.associate, this);
	on_dissociate_.connect<&Unmanaged::handle_dissociate>(&xsurface->events.dissociate, this);
	on_destroy_.connect<&Unmanaged::handle_destroy>(&xsurface->events.destroy, this);
	on_request_configure_.connect<&Unmanaged::handle_request_configure>(
		&xsurface->events.request_configure, this);
	if (xsurface->surface) {
		handle_associate(nullptr);
	}
}

void Unmanaged::handle_associate(void *) {
	on_map_.connect<&Unmanaged::handle_map>(&xsurface->surface->events.map, this);
	on_unmap_.connect<&Unmanaged::handle_unmap>(&xsurface->surface->events.unmap, this);
}

void Unmanaged::handle_dissociate(void *) {
	on_map_.disconnect();
	on_unmap_.disconnect();
}

void Unmanaged::handle_map(void *) {
	wlr_log(WLR_INFO, "x11 popup map: class=%s title=%s %dx%d+%d+%d", xsurface->class_ ? xsurface->class_ : "",
		xsurface->title ? xsurface->title : "", xsurface->width, xsurface->height, xsurface->x, xsurface->y);
	tree = wlr_scene_tree_create(server->layer_unmanaged);
	tree->node.data = &tag;
	wlr_scene_subsurface_tree_create(tree, xsurface->surface);
	wlr_scene_node_set_position(&tree->node, xsurface->x, xsurface->y);
	on_set_geometry_.connect<&Unmanaged::handle_set_geometry>(
		&xsurface->events.set_geometry, this);
	// dusk-xembed's holders for tray icons: never shown, never given the
	// keyboard (each one took it from the game as its icon arrived).
	if (xsurface->class_ && std::string(xsurface->class_) == "dusk-xembed") {
		wlr_scene_node_set_enabled(&tree->node, false);
		return;
	}
	// Nothing off every screen takes the keyboard either.
	if (!server->output_at(xsurface->x + xsurface->width / 2.0, xsurface->y + xsurface->height / 2.0)) {
		return;
	}
	if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
		server->focus_surface(xsurface->surface);
	}
}

void Unmanaged::handle_unmap(void *) {
	on_set_geometry_.disconnect();
	if (tree) {
		wlr_scene_node_destroy(&tree->node);
		tree = nullptr;
	}
	// Give the keyboard back to the window if this menu had it.
	if (server->seat->keyboard_state.focused_surface == xsurface->surface) {
		server->focus_surface(server->focused ? server->focused->surface() : nullptr);
	}
}

void Unmanaged::handle_set_geometry(void *) {
	if (tree) {
		wlr_scene_node_set_position(&tree->node, xsurface->x, xsurface->y);
	}
}

void Unmanaged::handle_request_configure(void *data) {
	auto *event = static_cast<wlr_xwayland_surface_configure_event *>(data);
	wlr_xwayland_surface_configure(xsurface, event->x, event->y, event->width, event->height);
	if (tree) {
		wlr_scene_node_set_position(&tree->node, event->x, event->y);
	}
}

void Unmanaged::handle_destroy(void *) {
	if (tree) {
		wlr_scene_node_destroy(&tree->node);
	}
	delete this;
}

} // namespace dusk

#endif // DUSK_XWAYLAND
