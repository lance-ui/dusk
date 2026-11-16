// Workspaces, like GNOME's: a row of desktops. By default there are as many
// as you use plus an empty one at the end (an empty one in the middle goes
// away once you leave it), or a fixed number (Settings > Workspaces).
// Switching slides the windows sideways; the wallpaper stays.
#include "server.hpp"

#include <algorithm>
#include <cmath>

#include "overview.hpp"

namespace dusk {

bool Server::workspace_used(int index) const {
	for (const View *v : views) {
		if (v->mapped && !v->sticky && v->workspace == index) {
			return true;
		}
	}
	return false;
}

void Server::normalize_workspaces() {
	const int before_count = workspace_count, before_ws = workspace;
	if (config.workspaces_dynamic) {
		// Empty ones go, apart from the one you're on and the last.
		for (int i = workspace_count - 2; i >= 0; i--) {
			if (i == workspace || workspace_used(i)) {
				continue;
			}
			for (View *v : views) {
				if (v->workspace > i) v->workspace--;
			}
			if (workspace > i) workspace--;
			workspace_count--;
		}
		int last_used = -1;
		for (const View *v : views) {
			if (v->mapped && !v->sticky) last_used = std::max(last_used, v->workspace);
		}
		// Always one empty workspace after the last one in use.
		workspace_count = std::max({1, last_used + 2, workspace + 1});
	} else {
		workspace_count = std::clamp(config.workspace_count, 1, 12);
		for (View *v : views) {
			v->workspace = std::clamp(v->workspace, 0, workspace_count - 1);
		}
		workspace = std::clamp(workspace, 0, workspace_count - 1);
	}
	if (before_count != workspace_count || before_ws != workspace) {
		if (overview) overview->workspaces_changed();
		redraw_panels();
	}
}

void Server::finish_workspace_switch() {
	if (workspace_anim) {
		const uint64_t id = workspace_anim;
		workspace_anim = 0;
		anim->cancel(id);
	}
	const std::vector<View *> leaving = workspace_leaving;
	workspace_leaving.clear();
	for (View *v : views) {
		if (v->slide_x != 0 || std::find(leaving.begin(), leaving.end(), v) != leaving.end()) {
			v->set_slide(0, 1.0);
		}
		v->sync_enabled();
	}
}

void Server::switch_workspace(int index, bool animate) {
	if (config.workspaces_dynamic) {
		index = std::clamp(index, 0, workspace_count - 1);
	} else if (config.workspace_wrap && workspace_count > 1) {
		index = (index % workspace_count + workspace_count) % workspace_count;
	} else {
		index = std::clamp(index, 0, workspace_count - 1);
	}
	if (index == workspace) {
		return;
	}
	finish_workspace_switch();
	const int dir = index > workspace ? 1 : -1;
	const int old = workspace;
	for (View *v : views) {
		if (v->mapped && !v->minimized && !v->sticky && v->workspace == old) {
			workspace_leaving.push_back(v);
		}
	}
	workspace = index;
	end_switcher();

	const bool slide = animate && config.animate(config.anim_workspaces) && !(overview && overview->is_open());
	if (!slide) {
		workspace_leaving.clear();
	}
	for (View *v : views) {
		v->sync_enabled();
	}

	// The keyboard goes to the top window here (or nowhere).
	View *top = nullptr;
	for (View *v : views) {
		if (v->visible()) {
			top = v;
			break;
		}
	}
	if (focused && !focused->visible()) {
		View *prev = focused;
		focused = nullptr;
		prev->set_activated(false);
	}
	if (top) {
		if (top != focused) focus(top);
	} else {
		focus(nullptr);
	}

	if (slide) {
		// How far each window travels: its screen's width, or (with a screen
		// next to it in that direction) a short way while fading.
		struct Move {
			View *view;
			bool in;
			int distance;
			bool fade;
		};
		auto moves = std::make_shared<std::vector<Move>>();
		for (View *v : views) {
			const bool out = std::find(workspace_leaving.begin(), workspace_leaving.end(), v) != workspace_leaving.end();
			const bool in = !out && v->visible() && !v->sticky;
			if (!out && !in) continue;
			Output *o = output_for_view(v);
			const wlr_box b = o ? o->box() : wlr_box{0, 0, 1920, 1080};
			const double probe_x = dir > 0 ? b.x - 4 : b.x + b.width + 4;
			const bool neighbour = output_at(probe_x, b.y + b.height / 2.0) != nullptr ||
				output_at(dir > 0 ? b.x + b.width + 4 : b.x - 4, b.y + b.height / 2.0) != nullptr;
			moves->push_back(Move{v, in, neighbour ? std::max(60, b.width / 8) : b.width, neighbour});
		}
		for (const Move &m : *moves) {
			m.view->set_slide(m.in ? dir * m.distance : 0, m.in && m.fade ? 0.0 : 1.0);
		}
		workspace_anim = anim->start(config.ms(320), [this, moves, dir](double t) {
			for (const Move &m : *moves) {
				if (std::find(views.begin(), views.end(), m.view) == views.end()) continue;
				const double p = std::clamp(t, -0.2, 1.2);
				const int dx = static_cast<int>(std::lround(m.in ? dir * m.distance * (1.0 - p)
					: -dir * m.distance * p));
				const double a = !m.fade ? 1.0 : m.in ? p : 1.0 - p;
				m.view->set_slide(dx, a);
			}
		}, [this] {
			workspace_anim = 0;
			finish_workspace_switch();
		});
	}
	normalize_workspaces();
	views_changed();
	if (overview) overview->workspaces_changed();
}

void Server::move_to_workspace(View *view, int index, bool follow) {
	if (!view) {
		return;
	}
	index = std::clamp(index, 0, std::max(0, workspace_count - 1));
	view->sticky = false;
	if (view->workspace == index) {
		return;
	}
	view->workspace = index;
	if (follow) {
		switch_workspace(index);
		focus(view);
	} else {
		view->sync_enabled();
		if (focused == view) {
			focus_next_after(view);
		}
	}
	normalize_workspaces();
	views_changed();
	if (overview) overview->workspaces_changed();
}

// GNOME's hot corner: pushing the pointer into the corner opens the overview.
void Server::check_hot_corner(double x, double y) {
	const std::string &corner = config.hot_corner;
	if (corner == "none" || !overview) {
		return;
	}
	Output *o = output_at(x, y);
	if (!o) {
		return;
	}
	const wlr_box b = o->box();
	const double cx = corner.find("right") != std::string::npos ? b.x + b.width - 1 : b.x;
	const double cy = corner.find("bottom") != std::string::npos ? b.y + b.height - 1 : b.y;
	const double d = std::hypot(x - cx, y - cy);
	if (d > 40) {
		hot_corner_armed = true;
		return;
	}
	if (d > 1.5 || !hot_corner_armed || buttons_down > 0 || cursor_mode != CursorMode::Passthrough) {
		return;
	}
	// Not over a full-screen game or video.
	if (focused && focused->fullscreen && output_for_view(focused) == o) {
		return;
	}
	hot_corner_armed = false;
	overview->toggle(o);
}

} // namespace dusk

namespace dusk {

// "New workspace": one more at the end (written to the settings, so it's
// still there next time), and you're taken there.
void Server::add_workspace() {
	if (config.workspaces_dynamic) {
		switch_workspace(workspace_count - 1, true);   // the empty one at the end
		return;
	}
	if (workspace_count >= 12) {
		return;
	}
	write_setting("workspace_count", std::to_string(workspace_count + 1));
	switch_workspace(workspace_count - 1, true);
}

// Its windows move to the workspace before it (or the next, for the first).
void Server::remove_workspace(int index) {
	if (workspace_count <= 1 || index < 0 || index >= workspace_count) {
		return;
	}
	finish_workspace_switch();
	for (View *v : views) {
		if (v->workspace == index) {
			v->workspace = std::max(0, index - 1);
		} else if (v->workspace > index) {
			v->workspace--;
		}
	}
	if (workspace >= index && workspace > 0) {
		workspace--;
	}
	if (!config.workspaces_dynamic) {
		write_setting("workspace_count", std::to_string(workspace_count - 1));
	}
	normalize_workspaces();
	for (View *v : views) {
		v->sync_enabled();
	}
	views_changed();
	if (overview) overview->workspaces_changed();
}

} // namespace dusk
