// Touchpad gestures (Settings > Mouse & touchpad > Gestures). Swipes with 3,
// 4 or 5 fingers can each do something per direction; by default three
// fingers up open the overview, down close it, and left/right switch
// workspaces. Two fingers always scroll. Gestures nothing is set for go to
// the app under the pointer (pinch to zoom in a browser, for example).
#include "server.hpp"

#include <cmath>

#include "ccentre.hpp"
#include "clipboard.hpp"
#include "launcher.hpp"
#include "overview.hpp"
#include "popover.hpp"

namespace dusk {

namespace {
constexpr double kTrigger = 70;   // how far the fingers go (touchpad units, ~px) before it acts
}

void Server::setup_gestures() {
	pointer_gestures = wlr_pointer_gestures_v1_create(display);
	on_swipe_begin.connect<&Server::handle_swipe_begin>(&cursor->events.swipe_begin, this);
	on_swipe_update.connect<&Server::handle_swipe_update>(&cursor->events.swipe_update, this);
	on_swipe_end.connect<&Server::handle_swipe_end>(&cursor->events.swipe_end, this);
	on_pinch_begin.connect<&Server::handle_pinch_begin>(&cursor->events.pinch_begin, this);
	on_pinch_update.connect<&Server::handle_pinch_update>(&cursor->events.pinch_update, this);
	on_pinch_end.connect<&Server::handle_pinch_end>(&cursor->events.pinch_end, this);
	on_hold_begin.connect<&Server::handle_hold_begin>(&cursor->events.hold_begin, this);
	on_hold_end.connect<&Server::handle_hold_end>(&cursor->events.hold_end, this);
}

void Server::run_action(const std::string &a) {
	Output *here = output_at(cursor->x, cursor->y);
	if (a.empty() || a == "none") return;
	if (a == "overview") {
		if (overview && !overview->is_open()) overview->open(here);
	} else if (a == "overview-close") {
		if (overview && overview->is_open()) overview->close();
	} else if (a == "overview-toggle") {
		if (overview) overview->toggle(here);
	} else if (a == "workspace-next") {
		switch_workspace(workspace + 1);
	} else if (a == "workspace-prev") {
		switch_workspace(workspace - 1);
	} else if (a == "app-menu") {
		launcher->toggle(here);
	} else if (a == "control-centre") {
		launcher->close();
		control_centre->toggle(here);
	} else if (a == "show-desktop") {
		toggle_show_desktop();
	} else if (a == "clipboard") {
		if (clipboard_card->is_open()) {
			clipboard_card->close();
		} else if (here) {
			close_popups();
			clipboard_card->open(here, wlr_box{static_cast<int>(cursor->x), static_cast<int>(cursor->y), 1, 1}, "point");
		}
	} else if (a == "screenshot") {
		screenshot("tool");
	} else if (focused) {
		View *v = focused;
		if (a == "close-window") v->close();
		else if (a == "maximize") v->toggle_maximize();
		else if (a == "minimize") v->minimize();
		else if (a == "fullscreen") v->set_fullscreen(!v->fullscreen);
		else if (a == "snap-left") v->snap_to(v->snap == View::Snap::Left ? View::Snap::None : View::Snap::Left, v->tile_target());
		else if (a == "snap-right") v->snap_to(v->snap == View::Snap::Right ? View::Snap::None : View::Snap::Right, v->tile_target());
	}
	if (a == "switch-window") {
		// The window before this one (like a quick Alt+Tab).
		View *next = nullptr;
		bool seen = false;
		for (View *v : views) {
			if (!v->visible()) continue;
			if (seen) {
				next = v;
				break;
			}
			seen = v == focused;
		}
		if (next) focus(next);
	}
}

void Server::handle_swipe_begin(void *data) {
	if (locked()) return;   // nothing behind the lock screen
	auto *e = static_cast<wlr_pointer_swipe_begin_event *>(data);
	note_activity();
	swipe_fingers = e->fingers;
	swipe_dx = swipe_dy = 0;
	swipe_fired = false;
	// Ours if something is set for this many fingers.
	swipe_ours = false;
	for (const char *dir : {"up", "down", "left", "right"}) {
		auto it = config.gestures.find(std::to_string(e->fingers) + "_" + dir);
		if (it != config.gestures.end() && it->second != "none" && !it->second.empty()) swipe_ours = true;
	}
	if (!swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_begin(pointer_gestures, seat, e->time_msec, e->fingers);
	}
}

void Server::handle_swipe_update(void *data) {
	if (locked()) return;   // nothing behind the lock screen
	auto *e = static_cast<wlr_pointer_swipe_update_event *>(data);
	note_activity();
	if (!swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_update(pointer_gestures, seat, e->time_msec, e->dx, e->dy);
		return;
	}
	swipe_dx += e->dx;
	swipe_dy += e->dy;
	if (swipe_fired || std::max(std::fabs(swipe_dx), std::fabs(swipe_dy)) < kTrigger) {
		return;
	}
	// Acts as soon as the fingers have gone far enough (feels quicker than
	// waiting for them to lift).
	swipe_fired = true;
	const bool horizontal = std::fabs(swipe_dx) > std::fabs(swipe_dy);
	const char *dir = horizontal ? (swipe_dx < 0 ? "left" : "right") : (swipe_dy < 0 ? "up" : "down");
	auto it = config.gestures.find(std::to_string(swipe_fingers) + "_" + dir);
	if (it != config.gestures.end()) run_action(it->second);
}

void Server::handle_swipe_end(void *data) {
	if (locked()) return;   // nothing behind the lock screen
	auto *e = static_cast<wlr_pointer_swipe_end_event *>(data);
	if (!swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_end(pointer_gestures, seat, e->time_msec, e->cancelled);
	}
	swipe_ours = swipe_fired = false;
	swipe_fingers = 0;
}

// Pinches and holds aren't used by Dusk: the app gets them.
void Server::handle_pinch_begin(void *data) {
	if (locked()) return;   // nothing behind the lock screen
	auto *e = static_cast<wlr_pointer_pinch_begin_event *>(data);
	wlr_pointer_gestures_v1_send_pinch_begin(pointer_gestures, seat, e->time_msec, e->fingers);
}

void Server::handle_pinch_update(void *data) {
	if (locked()) return;   // nothing behind the lock screen
	auto *e = static_cast<wlr_pointer_pinch_update_event *>(data);
	wlr_pointer_gestures_v1_send_pinch_update(pointer_gestures, seat, e->time_msec, e->dx, e->dy, e->scale, e->rotation);
}

void Server::handle_pinch_end(void *data) {
	if (locked()) return;   // nothing behind the lock screen
	auto *e = static_cast<wlr_pointer_pinch_end_event *>(data);
	wlr_pointer_gestures_v1_send_pinch_end(pointer_gestures, seat, e->time_msec, e->cancelled);
}

void Server::handle_hold_begin(void *data) {
	if (locked()) return;   // nothing behind the lock screen
	auto *e = static_cast<wlr_pointer_hold_begin_event *>(data);
	wlr_pointer_gestures_v1_send_hold_begin(pointer_gestures, seat, e->time_msec, e->fingers);
}

void Server::handle_hold_end(void *data) {
	if (locked()) return;   // nothing behind the lock screen
	auto *e = static_cast<wlr_pointer_hold_end_event *>(data);
	wlr_pointer_gestures_v1_send_hold_end(pointer_gestures, seat, e->time_msec, e->cancelled);
}

} // namespace dusk
