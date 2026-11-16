// Protocols apps use beyond drawing windows:
//   - xdg-activation: a window asking to be brought forward (a link opened
//     from another app, a notification clicked). Allowed when the request
//     comes from what you were using; otherwise its panel button lights up.
//   - pointer constraints + relative pointer: games and 3D apps lock the
//     pointer (mouse look) or keep it inside their window, and get the raw
//     mouse movement.
//   - keyboard shortcuts inhibit: virtual machines, remote desktops and some
//     games get every key, Super included, while they're focused.
#include <unistd.h>
#include "server.hpp"

#include <algorithm>

namespace dusk {

struct ShortcutInhibitor {
	Server *server;
	wlr_keyboard_shortcuts_inhibitor_v1 *raw;
	Listener on_destroy;

	void handle_destroy(void *) {
		auto &list = server->inhibitors;
		list.erase(std::remove(list.begin(), list.end(), this), list.end());
		delete this;
	}
};

View *Server::view_for_surface(wlr_surface *surface) const {
	if (!surface) {
		return nullptr;
	}
	wlr_surface *root = wlr_surface_get_root_surface(surface);
	for (View *v : views) {
		if (v->surface() == surface || v->surface() == root) {
			return v;
		}
	}
	return nullptr;
}

void Server::handle_request_activate(void *data) {
	auto *event = static_cast<wlr_xdg_activation_v1_request_activate_event *>(data);
	View *view = view_for_surface(event->surface);
	if (!view || !view->mapped) {
		return;
	}
	// A token handed over by an app you were using (it had the keyboard):
	// bring the window forward, from another workspace too.
	if (event->token && event->token->seat) {
		focus(view);
		return;
	}
	// Otherwise it waits for you: its panel button is highlighted.
	if (view != focused) {
		view->urgent = true;
		views_changed();
	}
}

void Server::handle_new_constraint(void *data) {
	auto *constraint = static_cast<wlr_pointer_constraint_v1 *>(data);
	if (constraint->surface && constraint->surface == seat->keyboard_state.focused_surface) {
		update_constraint();
	}
}

void Server::handle_constraint_destroy(void *) {
	on_constraint_destroy.disconnect();
	active_constraint = nullptr;
}

void Server::update_constraint() {
	if (!pointer_constraints) {
		return;
	}
	wlr_surface *s = seat->keyboard_state.focused_surface;
	wlr_pointer_constraint_v1 *want = s
		? wlr_pointer_constraints_v1_constraint_for_surface(pointer_constraints, s, seat) : nullptr;
	if (want == active_constraint) {
		return;
	}
	if (active_constraint) {
		// Where the app would like the pointer to be when it lets go.
		wlr_pointer_constraint_v1 *old = active_constraint;
		if (old->current.cursor_hint.enabled) {
			if (View *v = view_for_surface(old->surface)) {
				const double lx = v->x + v->deco_left() + old->current.cursor_hint.x;
				const double ly = v->y + v->deco_top() + old->current.cursor_hint.y;
				wlr_cursor_warp(cursor, nullptr, lx, ly);
			}
		}
		on_constraint_destroy.disconnect();
		wlr_pointer_constraint_v1_send_deactivated(old);
	}
	active_constraint = want;
	if (want) {
		on_constraint_destroy.connect<&Server::handle_constraint_destroy>(&want->events.destroy, this);
		wlr_pointer_constraint_v1_send_activated(want);
	}
}

void Server::handle_new_inhibitor(void *data) {
	auto *raw = static_cast<wlr_keyboard_shortcuts_inhibitor_v1 *>(data);
	auto *inhibitor = new ShortcutInhibitor{this, raw, {}};
	inhibitor->on_destroy.connect<&ShortcutInhibitor::handle_destroy>(&raw->events.destroy, inhibitor);
	inhibitors.push_back(inhibitor);
	wlr_keyboard_shortcuts_inhibitor_v1_activate(raw);
}

void Server::inhibitor_release(ShortcutInhibitor *i) {
	if (i->raw->active) wlr_keyboard_shortcuts_inhibitor_v1_deactivate(i->raw);
}

bool Server::shortcuts_inhibited() const {
	wlr_surface *s = seat->keyboard_state.focused_surface;
	if (!s) {
		return false;
	}
	for (const ShortcutInhibitor *i : inhibitors) {
		if (i->raw->surface == s && i->raw->active) {
			return true;
		}
	}
	// Dusk's Settings recording a shortcut (it leaves a note while it does):
	// every key goes to it, even ones Dusk itself uses, so any key or
	// combination can be picked.
	if (focused && focused->app_id() == "org.dusk.Settings") {
		const char *run = getenv("XDG_RUNTIME_DIR");
		if (run && access((std::string(run) + "/dusk-recording-keys").c_str(), F_OK) == 0) return true;
	}
	return false;
}

} // namespace dusk
