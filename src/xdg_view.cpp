// Wayland app windows (xdg-shell) and their popups.
#include <algorithm>

#include "server.hpp"

namespace dusk {

XdgView::XdgView(Server *server, wlr_xdg_toplevel *toplevel)
		: View(server), toplevel(toplevel) {
	content = wlr_scene_xdg_surface_create(tree, toplevel->base);
	toplevel->base->data = this;
	toplevel->base->surface->data = content;   // popups attach under this

	wlr_surface *s = toplevel->base->surface;
	on_map_.connect<&XdgView::handle_map>(&s->events.map, this);
	on_unmap_.connect<&XdgView::handle_unmap>(&s->events.unmap, this);
	on_commit_.connect<&XdgView::handle_commit>(&s->events.commit, this);
	on_destroy_.connect<&XdgView::handle_destroy>(&toplevel->events.destroy, this);
	on_request_move_.connect<&XdgView::handle_request_move>(
		&toplevel->events.request_move, this);
	on_request_resize_.connect<&XdgView::handle_request_resize>(
		&toplevel->events.request_resize, this);
	on_request_maximize_.connect<&XdgView::handle_request_maximize>(
		&toplevel->events.request_maximize, this);
	on_request_minimize_.connect<&XdgView::handle_request_minimize>(
		&toplevel->events.request_minimize, this);
	on_request_fullscreen_.connect<&XdgView::handle_request_fullscreen>(
		&toplevel->events.request_fullscreen, this);
	on_set_title_.connect<&XdgView::handle_set_title>(&toplevel->events.set_title, this);
	on_new_popup_.connect<&XdgView::handle_new_popup>(
		&toplevel->base->events.new_popup, this);
}

std::string XdgView::title() const {
	if (toplevel->title && *toplevel->title) {
		return toplevel->title;
	}
	if (toplevel->app_id && *toplevel->app_id) {
		return toplevel->app_id;
	}
	return "Window";
}

std::string XdgView::app_id() const {
	return toplevel->app_id ? toplevel->app_id : "";
}

int XdgView::content_width() const {
	const int w = toplevel->base->geometry.width;
	return w > 0 ? w : toplevel->base->surface->current.width;
}

int XdgView::content_height() const {
	const int h = toplevel->base->geometry.height;
	return h > 0 ? h : toplevel->base->surface->current.height;
}

uint32_t XdgView::request_content_size(int w, int h) {
	if (!ready()) {
		return 0;
	}
	return wlr_xdg_toplevel_set_size(toplevel, std::max(1, w), std::max(1, h));
}

void XdgView::send_tiled(bool on) {
	wlr_xdg_toplevel_set_tiled(toplevel, on
		? WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM : WLR_EDGE_NONE);
}

void XdgView::send_activated(bool on) {
	wlr_xdg_toplevel_set_activated(toplevel, on);
}

void XdgView::send_maximized(bool on) {
	wlr_xdg_toplevel_set_maximized(toplevel, on);
}

void XdgView::send_fullscreen(bool on) {
	wlr_xdg_toplevel_set_fullscreen(toplevel, on);
}

void XdgView::send_resizing(bool on) {
	wlr_xdg_toplevel_set_resizing(toplevel, on);
}

void XdgView::close() {
	wlr_xdg_toplevel_send_close(toplevel);
}

void XdgView::handle_map(void *) {
	did_map();
}

void XdgView::handle_unmap(void *) {
	if (mapped && !minimized) {
		snapshot_and_fade_out();
	}
	did_unmap();
}

void XdgView::handle_commit(void *) {
	if (toplevel->base->initial_commit) {
		// First commit: tell the app its starting state and who draws the
		// title bar. Everything goes out together in the first configure.
		if (decoration) {
			wlr_xdg_toplevel_decoration_v1_set_mode(decoration, decoration_mode);
		}
		if (toplevel->requested.fullscreen) {
			set_fullscreen(true);
		} else if (toplevel->requested.maximized) {
			set_maximized(true);
		} else {
			wlr_xdg_toplevel_set_size(toplevel, 0, 0);   // app picks its size
		}
		return;
	}
	did_commit();
}

void XdgView::handle_destroy(void *) {
	did_destroy();
	delete this;
}

void XdgView::handle_request_move(void *) {
	if (mapped && !fullscreen) {
		server->begin_move(this);
	}
}

void XdgView::handle_request_resize(void *data) {
	auto *event = static_cast<wlr_xdg_toplevel_resize_event *>(data);
	if (mapped && !fullscreen) {
		server->begin_resize(this, event->edges);
	}
}

void XdgView::handle_request_maximize(void *) {
	if (!ready()) {
		return;   // handled at the first commit
	}
	if (toplevel->requested.maximized != maximized) {
		set_maximized(toplevel->requested.maximized);
	}
	// The protocol wants a configure in reply even if nothing changed.
	wlr_xdg_surface_schedule_configure(toplevel->base);
}

void XdgView::handle_request_minimize(void *) {
	minimize();
}

void XdgView::handle_request_fullscreen(void *) {
	if (!ready()) {
		return;   // handled at the first commit
	}
	set_fullscreen(toplevel->requested.fullscreen);
	wlr_xdg_surface_schedule_configure(toplevel->base);
}

void XdgView::handle_set_title(void *) {
	redraw_frame();
	server->views_changed();
}

void XdgView::handle_new_popup(void *data) {
	new Popup(this, static_cast<wlr_xdg_popup *>(data));
}

// ---- server-side decoration negotiation -----------------------------------

void XdgView::attach_decoration(wlr_xdg_toplevel_decoration_v1 *deco) {
	decoration = deco;
	on_deco_request_mode_.connect<&XdgView::handle_decoration_request_mode>(
		&deco->events.request_mode, this);
	on_deco_destroy_.connect<&XdgView::handle_decoration_destroy>(
		&deco->events.destroy, this);
	handle_decoration_request_mode(nullptr);
}

void XdgView::handle_decoration_request_mode(void *) {
	if (decoration->requested_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
		decoration_mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	} else {
		decoration_mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	}
	apply_decoration_mode();
}

void XdgView::apply_decoration_mode() {
	if (decoration && ready()) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration, decoration_mode);
	}
	set_server_side(decoration_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void XdgView::handle_decoration_destroy(void *) {
	decoration = nullptr;
	on_deco_request_mode_.disconnect();
	on_deco_destroy_.disconnect();
}

// ---- popups ---------------------------------------------------------------

Popup::Popup(View *view, wlr_xdg_popup *popup) : view(view), popup(popup) {
	wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	auto *parent_tree = parent ? static_cast<wlr_scene_tree *>(parent->surface->data)
		: view->content;
	popup->base->surface->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);

	on_commit.connect<&Popup::handle_commit>(&popup->base->surface->events.commit, this);
	on_new_popup.connect<&Popup::handle_new_popup>(&popup->base->events.new_popup, this);
	on_reposition.connect<&Popup::handle_reposition>(&popup->events.reposition, this);
	on_destroy.connect<&Popup::handle_destroy>(&popup->events.destroy, this);
}

// Keep menus on screen: give wlroots the screen area in the coordinate
// space of the window's content.
void Popup::unconstrain() {
	Output *out = view->output();
	if (!out) {
		return;
	}
	wlr_box box = view->fullscreen ? out->box() : out->usable();
	box.x -= view->x + view->deco_left();
	box.y -= view->y + view->deco_top();
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

void Popup::handle_commit(void *) {
	if (popup->base->initial_commit) {
		unconstrain();
		wlr_xdg_surface_schedule_configure(popup->base);
	}
}

void Popup::handle_new_popup(void *data) {
	new Popup(view, static_cast<wlr_xdg_popup *>(data));
}

void Popup::handle_reposition(void *) {
	unconstrain();
}

void Popup::handle_destroy(void *) {
	delete this;
}

} // namespace dusk
