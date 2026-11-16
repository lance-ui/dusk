#include "overview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <linux/input-event-codes.h>

#include "apps.hpp"
#include "draw.hpp"
#include "glyphs.hpp"

namespace dusk {

namespace {

constexpr int kCardRadius = 18;
constexpr int kGap = 30;          // between windows in the overview
constexpr int kBadge = 44;        // app icon under each window
constexpr int kSearchW = 380, kSearchH = 40;
constexpr int kAppCell = 96, kAppsH = 104;

bool no_input(wlr_scene_buffer *, double *, double *) {
	return false;
}

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

int ri(double v) {
	return static_cast<int>(std::lround(v));
}

bool inside(const wlr_box &b, double x, double y) {
	return b.width > 0 && x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height;
}

bool inside(const GhostRect &r, double x, double y) {
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void set_rect_color(wlr_scene_rect *rect, const Color &c) {
	// Scene colours are premultiplied.
	const float f[4] = {static_cast<float>(c.r * c.a), static_cast<float>(c.g * c.a), static_cast<float>(c.b * c.a),
		static_cast<float>(c.a)};
	wlr_scene_rect_set_color(rect, f);
}

// Dark like GNOME's in the dark theme; a pale frosted version in the light one.
Color kText() { return theme::dark ? rgb(0xffffff) : rgb(0x1c1c21); }
Color kTextDim() { return theme::dark ? rgb(0xffffff, 0.62) : rgb(0x1c1c21, 0.62); }
Color kField() { return theme::dark ? rgb(0xffffff, 0.11) : rgb(0x000000, 0.07); }
Color kDim() { return theme::dark ? rgb(0x161619) : rgb(0xeeeef2); }

} // namespace

Overview::Overview(Server *server) : server_(server) {}

Overview::~Overview() {
	if (rebuild_) wl_event_source_remove(rebuild_);
	server_->anim->cancel(anim_);
	server_->anim->cancel(glide_anim_);
	windows_.clear();
	for (auto &s : screens_) {
		s->thumbs.clear();
		wlr_scene_node_destroy(&s->tree->node);
	}
	screens_.clear();
}

Overview::Screen *Overview::screen_for(Output *output) {
	for (auto &s : screens_) {
		if (s->output == output) return s.get();
	}
	return nullptr;
}

// ---- opening and closing ------------------------------------------------------------

void Overview::toggle(Output *from) {
	if (open_) {
		close();
	} else {
		open(from);
	}
}

void Overview::open(Output *from) {
	if (open_) {
		return;
	}
	const Config &c = server_->config;
	const double ms = c.animate(c.anim_overview) ? c.ms(260) : 0;
	if (!screens_.empty()) {
		// Still closing: turn round.
		open_ = true;
		pick_ = nullptr;
		server_->anim->cancel(anim_);
		anim_ = server_->anim->tween(t_, 1.0, ms, [this] { apply(t_); }, [this] { anim_ = 0; });
		return;
	}
	server_->close_popups();
	server_->end_switcher();
	server_->finish_workspace_switch();
	if (!from) {
		from = server_->output_at(server_->cursor->x, server_->cursor->y);
	}
	open_ = true;
	pick_ = nullptr;
	shown_ws_ = server_->workspace;
	query_.clear();
	selected_ = -1;
	hover_ = nullptr;
	pressed_ = nullptr;
	dragging_ = false;
	Output *primary = from;
	for (Output *o : server_->outputs) {
		if (o->enabled()) {
			if (!primary) primary = o;
			build_screen(o, o == primary);
		}
	}
	if (screens_.empty()) {
		open_ = false;
		return;
	}
	// A copy of every window (on every workspace: search finds them all).
	for (auto it = server_->views.rbegin(); it != server_->views.rend(); ++it) {
		View *v = *it;
		if (!v->mapped) continue;
		Screen *s = screen_for(server_->output_for_view(v));
		if (!s) s = screens_.front().get();
		auto w = std::make_unique<Window>();
		w->view = v;
		w->screen = s;
		const wlr_box f = v->frame_box();
		w->real = GhostRect{static_cast<double>(f.x), static_cast<double>(f.y), static_cast<double>(f.width),
			static_cast<double>(f.height), 1};
		const float none[4] = {0, 0, 0, 0};
		w->glow = wlr_scene_rect_create(s->windows, 1, 1, none);
		w->glow->accepts_input = false;
		wlr_scene_node_set_enabled(&w->glow->node, false);
		// A minimized window's whole tree is disabled, and Ghost::copy needs
		// live coordinates to place the parts. Flash the root back on just
		// for this synchronous copy (single-threaded: nothing renders in
		// between), then leave it exactly as it was.
		const bool tree_was_enabled = v->tree->node.enabled;
		if (!tree_was_enabled) {
			wlr_scene_node_set_enabled(&v->tree->node, true);
		}
		w->ghost = std::make_unique<Ghost>(s->windows, &v->tree->node, f.x, f.y, std::max(1, f.width),
			std::max(1, f.height));
		if (!tree_was_enabled) {
			wlr_scene_node_set_enabled(&v->tree->node, false);
		}
		w->badge = wlr_scene_buffer_create(s->windows, nullptr);
		w->badge->point_accepts_input = no_input;
		{
			Canvas canvas(kBadge, kBadge, s->output->output->scale);
			draw_icon(canvas.cr(), server_->icons, server_->apps.icon_for(v->app_id(), v->title()), v->title(), 0, 0, kBadge,
				s->output->output->scale);
			canvas.present(w->badge);
		}
		w->label = wlr_scene_buffer_create(s->windows, nullptr);
		w->label->point_accepts_input = no_input;
		w->close = wlr_scene_buffer_create(s->windows, nullptr);
		w->close->point_accepts_input = no_input;
		wlr_scene_node_set_enabled(&w->label->node, false);
		wlr_scene_node_set_enabled(&w->close->node, false);
		windows_.push_back(std::move(w));
	}
	// The real windows (and the desktop icons) step aside while it's open.
	wlr_scene_node_set_enabled(&server_->layer_views->node, false);
	wlr_scene_node_set_enabled(&server_->layer_fullscreen->node, false);
	wlr_scene_node_set_enabled(&server_->layer_desktop->node, false);
	layers_hidden_ = true;
	wlr_seat_pointer_notify_clear_focus(server_->seat);

	layout();
	t_ = 0;
	apply(0);
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(t_, 1.0, ms, [this] { apply(t_); }, [this] { anim_ = 0; });
	server_->redraw_panels();
}

void Overview::close(View *pick) {
	if (!open_) {
		return;
	}
	open_ = false;
	pick_ = pick;
	pressed_ = nullptr;
	dragging_ = false;
	set_hover(nullptr);
	select(-1);
	if (pick && !pick->sticky && pick->workspace != server_->workspace) {
		server_->switch_workspace(pick->workspace, false);
	}
	if (pick && pick->minimized) {
		pick->minimized = false;   // it comes back with the others
		pick->sync_enabled();
		server_->views_changed();
	}
	// Windows of the workspace go back where they are; the rest fade.
	for (auto &w : windows_) {
		const wlr_box f = w->view->frame_box();
		w->real = GhostRect{static_cast<double>(f.x), static_cast<double>(f.y), static_cast<double>(f.width),
			static_cast<double>(f.height), 1};
		if (w->in_layout) w->place = w->ghost->current;   // from wherever a glide got to
	}
	server_->anim->cancel(glide_anim_);
	glide_anim_ = 0;
	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(t_, 0.0, c.animate(c.anim_overview) ? c.ms(240) : 0, [this] { apply(t_); },
		[this] {
			anim_ = 0;
			if (!open_) finish_close();
		});
}

void Overview::finish_close() {
	View *pick = pick_;
	pick_ = nullptr;
	destroy_all();
	if (pick && std::find(server_->views.begin(), server_->views.end(), pick) != server_->views.end() &&
			pick->mapped) {
		server_->focus(pick);
	}
	server_->process_motion(0);
	server_->redraw_panels();
}

void Overview::destroy_all() {
	if (rebuild_) {
		wl_event_source_remove(rebuild_);
		rebuild_ = nullptr;
	}
	server_->anim->cancel(anim_);
	server_->anim->cancel(glide_anim_);
	anim_ = glide_anim_ = 0;
	windows_.clear();
	for (auto &s : screens_) {
		s->thumbs.clear();
		wlr_scene_node_destroy(&s->tree->node);
	}
	screens_.clear();
	hover_ = nullptr;
	pressed_ = nullptr;
	if (layers_hidden_) {
		layers_hidden_ = false;
		wlr_scene_node_set_enabled(&server_->layer_views->node, true);
		wlr_scene_node_set_enabled(&server_->layer_fullscreen->node, true);
		wlr_scene_node_set_enabled(&server_->layer_desktop->node, true);
		for (View *v : server_->views) {
			v->sync_enabled();
		}
	}
}

void Overview::view_gone(View *view) {
	if (hover_ && hover_->view == view) hover_ = nullptr;
	if (pressed_ && pressed_->view == view) {
		pressed_ = nullptr;
		dragging_ = false;
	}
	if (pick_ == view) pick_ = nullptr;
	const size_t before = windows_.size();
	windows_.erase(std::remove_if(windows_.begin(), windows_.end(),
		[view](const std::unique_ptr<Window> &w) { return w->view == view; }), windows_.end());
	if (windows_.size() != before && open_) {
		layout();
		glide();
	}
}

void Overview::output_gone(Output *output) {
	if (screen_for(output)) {
		open_ = false;
		pick_ = nullptr;
		destroy_all();
	}
}

void Overview::workspaces_changed() {
	if (!open_ || screens_.empty()) {
		return;
	}
	if (!rebuild_) {
		rebuild_ = wl_event_loop_add_idle(server_->loop, &Overview::handle_rebuild, this);
	}
}

void Overview::handle_rebuild(void *data) {
	auto *self = static_cast<Overview *>(data);
	self->rebuild_ = nullptr;
	self->rebuild();
}

void Overview::rebuild() {
	if (!open_ || screens_.empty()) {
		return;
	}
	const int ws = server_->workspace;
	slide_dir_ = ws > shown_ws_ ? 1 : ws < shown_ws_ ? -1 : 0;
	shown_ws_ = ws;
	for (auto &s : screens_) {
		build_strip(*s);
	}
	layout();
	glide();
	slide_dir_ = 0;
}

// ---- building ---------------------------------------------------------------------------

void Overview::build_screen(Output *output, bool primary) {
	auto s = std::make_unique<Screen>();
	s->output = output;
	s->primary = primary;
	s->tree = wlr_scene_tree_create(server_->layer_overview);
	s->tree->node.data = &tag;
	const wlr_box o = output->box();
	// A little past the screen's edges, so the blur doesn't darken them.
	int ml, mt, mr, mb;
	blur_margins(server_->outputs, s->output, 96, &ml, &mt, &mr, &mb);
	s->blur = wlr_scene_blur_create(s->tree, o.width + ml + mr, o.height + mt + mb);
	wlr_scene_node_set_position(&s->blur->node, o.x - ml, o.y - mt);
	wlr_scene_blur_set_strength(s->blur, 1.0f);
	const float dim[4] = {0, 0, 0, 0};
	s->dim = wlr_scene_rect_create(s->tree, o.width, o.height, dim);
	wlr_scene_node_set_position(&s->dim->node, o.x, o.y);
	// (No shadow under the card: drawn over the blur it showed as a dark
	// smudge down one side.)
	for (auto &p : s->peek) {
		p = wlr_scene_buffer_create(s->tree, nullptr);
		wlr_scene_node_set_enabled(&p->node, false);
	}
	s->card = wlr_scene_buffer_create(s->tree, nullptr);
	if (output->wallpaper && output->wallpaper->buffer) {
		wlr_scene_buffer_set_buffer(s->card, output->wallpaper->buffer);
		for (auto &p : s->peek) wlr_scene_buffer_set_buffer(p, output->wallpaper->buffer);
	}
	wlr_scene_buffer_set_corner_radii(s->card, corner_radii_all(kCardRadius));
	s->strip = wlr_scene_tree_create(s->tree);
	s->search = wlr_scene_buffer_create(s->tree, nullptr);
	s->apps = wlr_scene_buffer_create(s->tree, nullptr);
	s->add = wlr_scene_buffer_create(s->tree, nullptr);
	wlr_scene_node_set_enabled(&s->add->node, false);
	s->label = wlr_scene_buffer_create(s->tree, nullptr);
	s->label->point_accepts_input = no_input;
	wlr_scene_node_set_enabled(&s->label->node, false);
	wlr_scene_node_set_enabled(&s->search->node, primary);
	// Clipped to their rounded shapes, so nothing square shows round them
	// (the field's corners were drawing as a dark box over the blur).
	wlr_scene_buffer_set_corner_radii(s->search, corner_radii_all(kSearchH / 2));
	wlr_scene_buffer_set_corner_radii(s->label, corner_radii_all(13));
	wlr_scene_buffer_set_corner_radii(s->add, corner_radii_all(kCardRadius));
	wlr_scene_node_set_enabled(&s->apps->node, false);
	s->windows = wlr_scene_tree_create(s->tree);
	screens_.push_back(std::move(s));
	build_strip(*screens_.back());
}

// The workspaces, small, in a row (or a column) at the edge chosen in
// Settings › Workspaces (top, under the search; bottom; left; right): each
// one's wallpaper and its windows, the one shown ringed in the accent with a
// ✕ to remove it, and a "+" after them. Drag a window onto one to move it
// there; click one to go to it. On the main screen.
void Overview::build_strip(Screen &s) {
	s.thumbs.clear();
	// (Their scene nodes live in s.strip: start it afresh.)
	wlr_scene_node_destroy(&s.strip->node);
	s.strip = wlr_scene_tree_create(s.tree);
	wlr_scene_node_place_below(&s.strip->node, &s.search->node);
	s.strip_box = wlr_box{};
	s.strip_add = wlr_box{};
	s.remove_box = wlr_box{};
	s.strip_bits.clear();
	const std::string &where = server_->config.overview_strip;
	if (!s.primary || where == "off") {
		return;
	}
	const bool column = where == "left" || where == "right";
	const wlr_box o = s.output->box();
	const wlr_box u = s.output->usable();
	const int count = std::max(1, server_->workspace_count);
	const bool can_add = count < 12;
	const int slots = count + (can_add ? 1 : 0);
	const double aspect = o.height > 0 ? static_cast<double>(o.width) / o.height : 16.0 / 9.0;
	const int gap = 12;
	int th, tw;
	if (column) {
		// Down the side, under the search, as many as fit.
		tw = std::clamp(static_cast<int>(u.width * 0.095), 90, 190);
		th = std::max(1, static_cast<int>(tw / aspect));
		const int room = u.height - (18 + kSearchH + 28) - 30;
		while (slots * th + (slots - 1) * gap > room && tw > 48) {
			tw -= 6;
			th = std::max(1, static_cast<int>(tw / aspect));
		}
	} else {
		th = std::clamp(static_cast<int>(u.height * 0.085), 44, 90);
		tw = std::max(1, static_cast<int>(th * aspect));
		while (slots * tw + (slots - 1) * gap > u.width * 0.9 && th > 24) {
			th -= 4;
			tw = std::max(1, static_cast<int>(th * aspect));
		}
	}
	const int total = column ? slots * th + (slots - 1) * gap : slots * tw + (slots - 1) * gap;
	int x0, y0;
	if (column) {
		const int top = u.y + 18 + kSearchH + 28, room = u.y + u.height - 30 - top;
		y0 = top + std::max(0, (room - total) / 2);
		x0 = where == "left" ? u.x + 24 : u.x + u.width - 24 - tw;
		s.strip_box = wlr_box{x0, y0, tw, total};
	} else {
		x0 = u.x + (u.width - total) / 2;
		y0 = where == "bottom" ? u.y + u.height - 24 - th : u.y + 18 + kSearchH + 16;
		s.strip_box = wlr_box{x0, y0, total, th};
	}
	auto place = [&](int i) {
		return column ? wlr_box{x0, y0 + i * (th + gap), tw, th} : wlr_box{x0 + i * (tw + gap), y0, tw, th};
	};
	const double k = static_cast<double>(tw) / std::max(1, o.width);
	const float none[4] = {0, 0, 0, 0};
	for (int i = 0; i < count; i++) {
		auto t = std::make_unique<WsThumb>();
		t->index = i;
		t->box = place(i);
		const bool shown = i == server_->workspace;
		t->ring = wlr_scene_rect_create(s.strip, tw + 6, th + 6, none);
		wlr_scene_node_set_position(&t->ring->node, t->box.x - 3, t->box.y - 3);
		wlr_scene_rect_set_corner_radius(t->ring, 11);
		t->ring->accepts_input = false;
		set_rect_color(t->ring, shown ? with_alpha(theme::accent, alpha_) : rgb(0xffffff, 0.0));
		t->wall = wlr_scene_buffer_create(s.strip, s.output->wallpaper ? s.output->wallpaper->buffer : nullptr);
		wlr_scene_node_set_position(&t->wall->node, t->box.x, t->box.y);
		wlr_scene_buffer_set_dest_size(t->wall, tw, th);
		wlr_scene_buffer_set_corner_radii(t->wall, corner_radii_all(8));
		wlr_scene_buffer_set_opacity(t->wall, static_cast<float>(alpha_));
		t->wall->point_accepts_input = no_input;
		// Its windows, tiny, where they are.
		for (auto it = server_->views.rbegin(); it != server_->views.rend(); ++it) {
			View *v = *it;
			if (!v->mapped || v->minimized || (v->workspace != i && !v->sticky) ||
					server_->output_for_view(v) != s.output) {
				continue;
			}
			const wlr_box f = v->frame_box();
			auto g = std::make_unique<Ghost>(s.strip, &v->tree->node, f.x, f.y, std::max(1, f.width),
				std::max(1, f.height));
			g->apply(GhostRect{t->box.x + (f.x - o.x) * k, t->box.y + (f.y - o.y) * k, std::max(1.0, f.width * k),
				std::max(1.0, f.height * k), alpha_});
			t->minis.push_back(std::move(g));
		}
		// Its number, small, in the corner (so "Super+2" has a picture to it).
		{
			const std::string num = std::to_string(i + 1);
			Canvas canvas(20, 18, s.output->output->scale);
			fill_rounded(canvas.cr(), 0, 0, 20, 18, 9, rgb(0x000000, 0.45));
			draw_text(canvas.cr(), num, 0, 0, 20, 18, theme::font_small, rgb(0xffffff), Align::Center);
			wlr_scene_buffer *b = wlr_scene_buffer_create(s.strip, nullptr);
			canvas.present(b);
			b->point_accepts_input = no_input;
			wlr_scene_node_set_position(&b->node, t->box.x + 5, t->box.y + 5);
			s.strip_bits.push_back(b);
		}
		// The shown one (when there's more than one): a ✕ that removes it.
		if (shown && count > 1) {
			Canvas canvas(20, 20, s.output->output->scale);
			cairo_t *cr = canvas.cr();
			cairo_arc(cr, 10, 10, 9.5, 0, 2 * M_PI);
			set_color(cr, rgb(0x000000, 0.55));
			cairo_fill(cr);
			draw_glyph(cr, "close", 10, 10, 10, rgb(0xffffff));
			wlr_scene_buffer *b = wlr_scene_buffer_create(s.strip, nullptr);
			canvas.present(b);
			b->point_accepts_input = no_input;
			wlr_scene_node_set_position(&b->node, t->box.x + tw - 25, t->box.y + 5);
			s.strip_bits.push_back(b);
			s.remove_box = wlr_box{t->box.x + tw - 27, t->box.y + 3, 24, 24};
		}
		s.thumbs.push_back(std::move(t));
	}
	// "+": a new workspace (or drop a window on it: a new one with that window).
	if (can_add) {
		const wlr_box b = place(count);
		Canvas canvas(tw, th, s.output->output->scale);
		cairo_t *cr = canvas.cr();
		rounded_rect(cr, 1, 1, tw - 2, th - 2, 8);
		set_color(cr, with_alpha(kText(), 0.06));
		cairo_fill_preserve(cr);
		const double dash[] = {5, 4};
		cairo_set_dash(cr, dash, 2, 0);
		cairo_set_line_width(cr, 1.5);
		set_color(cr, with_alpha(kText(), 0.35));
		cairo_stroke(cr);
		cairo_set_dash(cr, nullptr, 0, 0);
		draw_glyph(cr, "plus", tw / 2.0, th / 2.0, std::min(22.0, th * 0.4), kTextDim());
		wlr_scene_buffer *add = wlr_scene_buffer_create(s.strip, nullptr);
		canvas.present(add);
		add->point_accepts_input = no_input;
		wlr_scene_node_set_position(&add->node, b.x, b.y);
		s.strip_bits.push_back(add);
		s.strip_add = b;
	}
}

bool Overview::matches(const View *view) const {
	if (query_.empty()) return true;
	const std::string q = lower(query_);
	if (lower(view->title()).find(q) != std::string::npos) return true;
	if (lower(view->app_id()).find(q) != std::string::npos) return true;
	for (const AppInfo &a : server_->apps.apps()) {
		if (!a.icon.empty() && a.icon == server_->apps.icon_for(view->app_id(), view->title())) {
			return lower(a.name).find(q) != std::string::npos;
		}
	}
	return false;
}

std::vector<int> Overview::app_results() const {
	std::vector<int> out;
	if (query_.empty() || !server_->config.overview_apps) return out;
	const std::string q = lower(query_);
	const auto &apps = server_->apps.apps();
	std::vector<int> starts, contains;
	for (int i = 0; i < static_cast<int>(apps.size()); i++) {
		const AppInfo &a = apps[static_cast<size_t>(i)];
		if (!a.visible) continue;
		const std::string name = lower(a.name);
		if (name.rfind(q, 0) == 0) starts.push_back(i);
		else if (name.find(q) != std::string::npos || lower(a.comment).find(q) != std::string::npos) contains.push_back(i);
	}
	out = starts;
	out.insert(out.end(), contains.begin(), contains.end());
	if (out.size() > 8) out.resize(8);
	return out;
}

// ---- layout -----------------------------------------------------------------------------

void Overview::layout() {
	for (auto &w : windows_) {
		w->was_in_layout = w->in_layout;
		w->in_layout = false;
	}
	for (auto &s : screens_) {
		layout_screen(*s);
	}
	// A keyboard selection that went away.
	const int n = static_cast<int>(selectable().size()) + (screens_.empty() ? 0 : static_cast<int>(app_results().size()));
	if (selected_ >= n) selected_ = n - 1;
	if (!query_.empty() && selected_ < 0 && n > 0) selected_ = 0;
	select(selected_);
}

void Overview::layout_screen(Screen &s) {
	const wlr_box o = s.output->box();
	const wlr_box u = s.output->usable();
	int top = u.y + 22;
	if (s.primary) {
		s.search_box = wlr_box{u.x + (u.width - kSearchW) / 2, u.y + 18, kSearchW, kSearchH};
		draw_search(s);
		top = s.search_box.y + kSearchH + 14;
		// "Workspace 2 of 3", when there's more than one (and no pictures of them).
		const int count = std::max(1, server_->workspace_count);
		if (s.strip_box.width > 0) {
			wlr_scene_node_set_enabled(&s.label->node, false);
			if (server_->config.overview_strip == "top") top = s.strip_box.y + s.strip_box.height + 18;
		} else if (count > 1) {
			// "Workspace 2 of 3" and a ✕ that removes it (its windows go to
			// the one before).
			const std::string text = "Workspace " + std::to_string(server_->workspace + 1) + " of " + std::to_string(count);
			const int text_w = text_width(text, theme::font);
			const int tw = text_w + 24 + 30;
			Canvas canvas(tw, 26, s.output->output->scale);
			cairo_t *cr = canvas.cr();
			fill_rounded(cr, 0, 0, tw, 26, 13, kField());
			draw_text(cr, text, 12, 0, text_w + 4, 26, theme::font, kTextDim());
			cairo_new_path(cr);
			cairo_arc(cr, tw - 17, 13, 10, 0, 2 * M_PI);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
			cairo_fill(cr);
			draw_glyph(cr, "close", tw - 17, 13, 11, kText());
			canvas.present(s.label);
			const int lx = u.x + (u.width - tw) / 2;
			wlr_scene_node_set_position(&s.label->node, lx, top);
			wlr_scene_node_set_enabled(&s.label->node, true);
			s.remove_box = wlr_box{lx + tw - 30, top, 30, 26};
			top += 32;
		} else {
			wlr_scene_node_set_enabled(&s.label->node, false);
			s.remove_box = wlr_box{};
		}
		top += 8;
	}
	int bottom = u.y + u.height - 30;
	if (s.strip_box.width > 0 && server_->config.overview_strip == "bottom") bottom = s.strip_box.y - 18;
	// A column of pictures at a side: the card keeps clear of it.
	int area_x = u.x, area_w = u.width;
	if (s.strip_box.width > 0 && server_->config.overview_strip == "left") {
		area_x = s.strip_box.x + s.strip_box.width + 16;
		area_w = u.x + u.width - area_x;
	} else if (s.strip_box.width > 0 && server_->config.overview_strip == "right") {
		area_w = s.strip_box.x - 16 - u.x;
	}
	const std::vector<int> apps = s.primary ? app_results() : std::vector<int>{};
	if (!apps.empty()) {
		s.apps_box = wlr_box{u.x + (u.width - static_cast<int>(apps.size()) * kAppCell) / 2, bottom - kAppsH,
			static_cast<int>(apps.size()) * kAppCell, kAppsH};
		bottom -= kAppsH + 14;
		draw_apps(s);
		wlr_scene_node_set_enabled(&s.apps->node, true);
	} else {
		s.apps_box = wlr_box{};
		s.app_hits.clear();
		wlr_scene_node_set_enabled(&s.apps->node, false);
	}
	// The big card: the screen, smaller, with room for the neighbours to peek in.
	const double avail_w = area_w * (area_w < u.width ? 0.9 : 0.84), avail_h = std::max(40, bottom - top);
	const double k = std::min(avail_w / std::max(1, o.width), avail_h / std::max(1, o.height));
	const int cw = std::max(1, ri(o.width * k)), ch = std::max(1, ri(o.height * k));
	s.card_box = wlr_box{area_x + (area_w - cw) / 2, top + ri((avail_h - ch) / 2), cw, ch};
	wlr_scene_node_set_position(&s.card->node, s.card_box.x, s.card_box.y);
	wlr_scene_buffer_set_dest_size(s.card, cw, ch);
	// Workspaces either side, peeking in from the screen's edges.
	for (int side = 0; side < 2; side++) {
		wlr_scene_buffer *p = s.peek[side];
		bool exists = side == 0 ? server_->workspace > 0 : server_->workspace < server_->workspace_count - 1;
		// (Not on the side the pictures are on.)
		const std::string &strip_at = server_->config.overview_strip;
		const bool strip = s.strip_box.width > 0;
		if (strip && ((side == 0 && strip_at == "left") || (side == 1 && strip_at == "right"))) exists = false;
		const int gap = 26;
		int x, vis;
		if (side == 1) {
			x = s.card_box.x + cw + gap;
			vis = u.x + u.width - x;
		} else {
			vis = s.card_box.x - gap - u.x;
			x = u.x;
		}
		vis = std::min(vis, cw);
		if (side == 1) {
			// After the last workspace: a card that makes a new one.
			const bool can_add = !exists && s.primary && !strip && vis >= 56 && server_->workspace_count < 12;
			wlr_scene_node_set_enabled(&s.add->node, can_add);
			s.add_box = wlr_box{};
			if (can_add) {
				const int ph = ri(ch * 0.92), aw = std::min(vis - 16, 220);
				s.add_box = wlr_box{x, s.card_box.y + (ch - ph) / 2, aw, ph};
				Canvas canvas(aw, ph, s.output->output->scale);
				cairo_t *cr = canvas.cr();
				rounded_rect(cr, 1.5, 1.5, aw - 3, ph - 3, kCardRadius);
				cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
				cairo_fill_preserve(cr);
				const double dash[] = {8, 6};
				cairo_set_dash(cr, dash, 2, 0);
				cairo_set_line_width(cr, 2);
				cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
				cairo_stroke(cr);
				cairo_set_dash(cr, nullptr, 0, 0);
				const double cx = std::min(aw / 2.0, 60.0), cy = ph / 2.0;
				draw_glyph(cr, "plus", cx, cy - 12, 30, kText());
				draw_text(cr, "New", 0, cy + 10, cx * 2, 22, theme::font_bold, kTextDim(), Align::Center);
				canvas.present(s.add);
				wlr_scene_node_set_position(&s.add->node, s.add_box.x, s.add_box.y);
				wlr_scene_buffer_set_opacity(s.add, static_cast<float>(alpha_));
			}
		}
		if (!exists || vis < 10 || !p->buffer) {
			s.peek_box[side] = wlr_box{};
			wlr_scene_node_set_enabled(&p->node, false);
			continue;
		}
		const int ph = ri(ch * 0.92);
		s.peek_box[side] = wlr_box{x, s.card_box.y + (ch - ph) / 2, vis, ph};
		const double bw = p->buffer->width, bh = p->buffer->height;
		const double frac = static_cast<double>(vis) / cw;
		const double fx = side == 1 ? 0.0 : std::max(0.0, bw * (1 - frac));
		const wlr_fbox src{fx, 0, std::min(bw - fx, std::max(1.0, bw * frac)), bh};
		wlr_scene_buffer_set_source_box(p, &src);
		wlr_scene_buffer_set_dest_size(p, vis, ph);
		wlr_scene_node_set_position(&p->node, x, s.card_box.y + (ch - ph) / 2);
		wlr_scene_buffer_set_corner_radii(p, side == 1 ? corner_radii_new(kCardRadius, 0, 0, kCardRadius)
			: corner_radii_new(0, kCardRadius, kCardRadius, 0));
		wlr_scene_buffer_set_opacity(p, static_cast<float>(0.55 * alpha_));
		wlr_scene_node_set_enabled(&p->node, true);
	}

	// The windows to show here: this workspace's, or what the search found.
	std::vector<Window *> wins;
	for (auto &w : windows_) {
		if (w->screen != &s) continue;
		View *v = w->view;
		const bool here = query_.empty()
			? ((!v->minimized || server_->config.overview_minimized) && (v->sticky || v->workspace == server_->workspace))
			: matches(v);
		if (here) {
			w->in_layout = true;
			wins.push_back(w.get());
		}
	}
	const wlr_box area{s.card_box.x + 26, s.card_box.y + 26, std::max(1, cw - 52), std::max(1, ch - 26 - 44)};
	place_windows(s, wins, area, k);
}

// Rows of windows, as big as fits (never bigger than on the card), centred.
void Overview::place_windows(Screen &s, std::vector<Window *> &wins, const wlr_box &area, double max_scale) {
	(void)s;
	const size_t n = wins.size();
	if (n == 0) return;
	std::sort(wins.begin(), wins.end(), [](const Window *a, const Window *b) {
		const double ay = a->real.y + a->real.h / 2, by = b->real.y + b->real.h / 2;
		if (std::fabs(ay - by) > 40) return ay < by;
		return a->real.x < b->real.x;
	});
	double best = -1;
	std::vector<std::vector<Window *>> best_rows;
	for (size_t rows = 1; rows <= n; rows++) {
		std::vector<std::vector<Window *>> r(rows);
		const size_t per = (n + rows - 1) / rows;
		for (size_t i = 0; i < n; i++) r[std::min(rows - 1, i / per)].push_back(wins[i]);
		r.erase(std::remove_if(r.begin(), r.end(), [](const auto &row) { return row.empty(); }), r.end());
		double scale = max_scale, total_h = 0;
		for (auto &row : r) {
			std::sort(row.begin(), row.end(), [](const Window *a, const Window *b) { return a->real.x < b->real.x; });
			double sum_w = 0, max_h = 0;
			for (const Window *w : row) {
				sum_w += w->real.w;
				max_h = std::max(max_h, w->real.h);
			}
			scale = std::min(scale, (area.width - kGap * (row.size() - 1.0)) / std::max(1.0, sum_w));
			total_h += max_h;
		}
		scale = std::min(scale, (area.height - kGap * (r.size() - 1.0)) / std::max(1.0, total_h));
		if (scale > best + 1e-4) {
			best = scale;
			best_rows = r;
		}
	}
	const double sc = std::max(0.02, best);
	double total_h = 0;
	for (const auto &row : best_rows) {
		double max_h = 0;
		for (const Window *w : row) max_h = std::max(max_h, w->real.h);
		total_h += max_h * sc;
	}
	total_h += kGap * (best_rows.size() - 1.0);
	double y = area.y + (area.height - total_h) / 2;
	for (const auto &row : best_rows) {
		double row_w = 0, max_h = 0;
		for (const Window *w : row) {
			row_w += w->real.w * sc;
			max_h = std::max(max_h, w->real.h * sc);
		}
		row_w += kGap * (row.size() - 1.0);
		double x = area.x + (area.width - row_w) / 2;
		for (Window *w : row) {
			const double ww = w->real.w * sc, wh = w->real.h * sc;
			w->place = GhostRect{std::round(x), std::round(y + (max_h - wh) / 2), std::round(ww), std::round(wh), 1};
			x += ww + kGap;
		}
		y += max_h + kGap;
	}
}

// ---- drawing ----------------------------------------------------------------------------

void Overview::draw_search(Screen &s) {
	const float scale = s.output->output->scale;
	Canvas canvas(kSearchW, kSearchH, scale);
	cairo_t *cr = canvas.cr();
	fill_rounded(cr, 0, 0, kSearchW, kSearchH, kSearchH / 2.0, kField());
	draw_glyph(cr, "search", 22, kSearchH / 2.0, 17, kTextDim());
	const int tx = 42;
	if (query_.empty()) {
		draw_text(cr, "Type to search", tx, 0, kSearchW - tx - 16, kSearchH, theme::font_large, kTextDim());
	} else {
		draw_text(cr, query_, tx, 0, kSearchW - tx - 16, kSearchH, theme::font_large, kText());
		const int cx = std::min(kSearchW - 18, tx + text_width(query_, theme::font_large) + 2);
		cairo_rectangle(cr, cx, 11, 2, kSearchH - 22);
		set_color(cr, theme::accent);
		cairo_fill(cr);
	}
	canvas.present(s.search);
	wlr_scene_node_set_position(&s.search->node, s.search_box.x, s.search_box.y);
}

void Overview::draw_apps(Screen &s) {
	const std::vector<int> apps = app_results();
	s.app_hits.clear();
	if (apps.empty()) return;
	const float scale = s.output->output->scale;
	const int w = static_cast<int>(apps.size()) * kAppCell;
	Canvas canvas(w, kAppsH, scale);
	cairo_t *cr = canvas.cr();
	const int n_windows = static_cast<int>(selectable().size());
	const auto &all = server_->apps.apps();
	for (size_t i = 0; i < apps.size(); i++) {
		const AppInfo &a = all[static_cast<size_t>(apps[i])];
		const double x = i * kAppCell;
		if (selected_ == n_windows + static_cast<int>(i)) {
			fill_rounded(cr, x + 4, 2, kAppCell - 8, kAppsH - 4, 16, with_alpha(theme::accent, 0.45));
		}
		draw_icon(cr, server_->icons, a.icon, a.name, x + (kAppCell - 52) / 2.0, 10, 52, scale);
		draw_text(cr, a.name, x + 4, 66, kAppCell - 8, 30, theme::font, kText(), Align::Center);
		s.app_hits.push_back(AppHit{apps[i], wlr_box{s.apps_box.x + static_cast<int>(x), s.apps_box.y, kAppCell, kAppsH}});
	}
	canvas.present(s.apps);
	wlr_scene_node_set_position(&s.apps->node, s.apps_box.x, s.apps_box.y);
}

// The icon under a window, and on hover its title and ✕.
void Overview::draw_window_extras(Window &w) {
	const GhostRect &r = w.ghost->current;
	const double a = std::clamp((t_ - 0.55) / 0.45, 0.0, 1.0) * (w.in_layout ? r.alpha : 0.0);
	wlr_scene_node_set_enabled(&w.badge->node, a > 0.01 && !(dragging_ && pressed_ == &w));
	wlr_scene_node_set_position(&w.badge->node, ri(r.x + r.w / 2 - kBadge / 2.0), ri(r.y + r.h - kBadge * 0.6));
	wlr_scene_buffer_set_opacity(w.badge, static_cast<float>(a));

	const bool hovered = hover_ == &w && open_ && !dragging_;
	const std::vector<Window *> sel = selectable();
	const bool chosen = selected_ >= 0 && selected_ < static_cast<int>(sel.size()) && sel[static_cast<size_t>(selected_)] == &w;
	const bool lit = (hovered || chosen) && a > 0.5;
	wlr_scene_node_set_enabled(&w.glow->node, lit);
	if (lit) {
		wlr_scene_rect_set_size(w.glow, std::max(1, ri(r.w + 12)), std::max(1, ri(r.h + 12)));
		wlr_scene_node_set_position(&w.glow->node, ri(r.x - 6), ri(r.y - 6));
		wlr_scene_rect_set_corner_radius(w.glow, 16);
		set_rect_color(w.glow, chosen ? with_alpha(theme::accent, 0.55) : rgb(0xffffff, 0.20));
	}
	wlr_scene_node_set_enabled(&w.label->node, lit);
	wlr_scene_node_set_enabled(&w.close->node, hovered && a > 0.5);
	if (lit) {
		const std::string title = w.view->title();
		if (title != w.label_drawn) {
			w.label_drawn = title;
			w.label_w = std::min(360, text_width(title, theme::font) + 28);
			Canvas canvas(w.label_w, 28, w.screen->output->output->scale);
			fill_rounded(canvas.cr(), 0, 0, w.label_w, 28, 14, rgb(0x000000, 0.62));
			draw_text(canvas.cr(), title, 14, 0, w.label_w - 28, 28, theme::font, rgb(0xffffff), Align::Center);
			canvas.present(w.label);
		}
		wlr_scene_node_set_position(&w.label->node, ri(r.x + r.w / 2 - w.label_w / 2.0), ri(r.y + r.h + kBadge * 0.45));
	}
	if (hovered && a > 0.5) {
		const int red = hover_close_ ? 1 : 0;
		if (red != w.close_drawn) {
			w.close_drawn = red;
			Canvas canvas(28, 28, w.screen->output->output->scale);
			cairo_t *cr = canvas.cr();
			cairo_arc(cr, 14, 14, 13, 0, 2 * M_PI);
			set_color(cr, red ? theme::danger : (theme::dark ? rgb(0x3a3a40, 0.95) : rgb(0xffffff, 0.95)));
			cairo_fill(cr);
			draw_glyph(cr, "close", 14, 14, 14, red ? rgb(0xffffff) : kText());
			canvas.present(w.close);
		}
		wlr_scene_node_set_position(&w.close->node, ri(r.x + r.w - 16), ri(r.y - 12));
	}
}

void Overview::set_alpha(double a) {
	alpha_ = std::clamp(a, 0.0, 1.0);
	for (auto &s : screens_) {
		wlr_scene_blur_set_alpha(s->blur, static_cast<float>(alpha_));
		set_rect_color(s->dim, with_alpha(kDim(), (theme::dark ? 0.72 : 0.62) * alpha_));
		wlr_scene_buffer_set_opacity(s->card, static_cast<float>(alpha_));
		wlr_scene_buffer_set_opacity(s->search, static_cast<float>(alpha_));
		wlr_scene_buffer_set_opacity(s->apps, static_cast<float>(alpha_));
		wlr_scene_buffer_set_opacity(s->add, static_cast<float>(alpha_));
		wlr_scene_buffer_set_opacity(s->label, static_cast<float>(alpha_));
		for (auto *p : s->peek) wlr_scene_buffer_set_opacity(p, static_cast<float>(0.55 * alpha_));
		wlr_scene_node_set_enabled(&s->strip->node, alpha_ > 0.02);
		// The pictures fade in with everything (all their parts), sliding in a
		// little from their edge.
		const std::string &at = server_->config.overview_strip;
		const int off = static_cast<int>(std::lround((1.0 - alpha_) * 14));
		wlr_scene_node_set_position(&s->strip->node, at == "left" ? -off : at == "right" ? off : 0,
			at == "bottom" ? off : at == "top" ? -off : 0);
		for (auto &t : s->thumbs) {
			wlr_scene_buffer_set_opacity(t->wall, static_cast<float>(alpha_));
			if (t->index == server_->workspace && !dragging_) set_rect_color(t->ring, with_alpha(theme::accent, alpha_));
			for (auto &g : t->minis) {
				GhostRect r = g->current;
				r.alpha = alpha_;
				g->apply(r);
			}
		}
		for (wlr_scene_buffer *b : s->strip_bits) wlr_scene_buffer_set_opacity(b, static_cast<float>(alpha_));
	}
}

// The open/close movement: windows between their real place (t = 0) and the
// overview (t = 1); everything else fades.
void Overview::apply(double t) {
	const double tt = std::clamp(t, 0.0, 1.0);
	set_alpha(tt);
	for (auto &w : windows_) {
		const bool here = w->view->visible();   // on the workspace shown
		GhostRect r;
		if (w->in_layout && here) {
			r = lerp(w->real, w->place, t);
			r.alpha = 1;
		} else if (w->in_layout) {
			// (A minimised one fades in at its place, a little faded.)
			r = w->place;
			r.alpha = tt * (w->view->minimized ? 0.65 : 1.0);
		} else if (here && !open_) {
			r = w->real;
			r.alpha = 1 - tt;
		} else {
			r = w->place.w > 1 ? w->place : w->real;
			r.alpha = 0;
		}
		w->ghost->apply(r);
		draw_window_extras(*w);
	}
}

// From where the windows are now to their new places (search, workspace switch).
void Overview::glide() {
	if (!open_) return;
	// Switching workspace: this one's windows slide out one way as the next
	// one's slide in from the other (a quarter of the card), fading.
	for (auto &w : windows_) {
		w->from = w->ghost->current;
		w->away = 0;
		const double shift = slide_dir_ * (w->screen ? w->screen->card_box.width : 600) * 0.25;
		if (w->in_layout && !w->was_in_layout) {
			w->from = w->place;
			w->from.x += shift;
			w->from.alpha = 0;
		} else if (!w->in_layout && w->was_in_layout) {
			w->away = -shift;
		}
	}
	server_->anim->cancel(glide_anim_);
	const Config &c = server_->config;
	glide_anim_ = server_->anim->start(c.animate(c.anim_overview) ? c.ms(200) : 0, [this](double t) {
		const double tt = std::clamp(t, 0.0, 1.2);
		for (auto &w : windows_) {
			if (dragging_ && pressed_ == w.get()) continue;
			GhostRect to = w->in_layout ? w->place : w->from;
			if (!w->in_layout) to.x += w->away;
			to.alpha = w->in_layout ? (w->view->minimized ? 0.65 : 1.0) : 0.0;
			GhostRect r = lerp(w->from, to, tt);
			r.alpha = std::clamp(r.alpha, 0.0, 1.0);
			w->ghost->apply(r);
			draw_window_extras(*w);
		}
	}, [this] { glide_anim_ = 0; });
}

// ---- selection ----------------------------------------------------------------------------

std::vector<Overview::Window *> Overview::selectable() {
	std::vector<Window *> out;
	for (auto &s : screens_) {
		std::vector<Window *> mine;
		for (auto &w : windows_) {
			if (w->screen == s.get() && w->in_layout) mine.push_back(w.get());
		}
		std::sort(mine.begin(), mine.end(), [](const Window *a, const Window *b) {
			if (std::fabs(a->place.y - b->place.y) > 2) return a->place.y < b->place.y;
			return a->place.x < b->place.x;
		});
		if (s->primary) {
			out.insert(out.begin(), mine.begin(), mine.end());
		} else {
			out.insert(out.end(), mine.begin(), mine.end());
		}
	}
	return out;
}

void Overview::select(int index) {
	selected_ = index;
	for (auto &w : windows_) draw_window_extras(*w);
	for (auto &s : screens_) {
		if (s->primary && s->apps_box.width > 0) draw_apps(*s);
	}
}

void Overview::activate_selection() {
	std::vector<Window *> wins = selectable();
	const int n = static_cast<int>(wins.size());
	int i = selected_ >= 0 ? selected_ : 0;
	if (i < n) {
		close(wins[static_cast<size_t>(i)]->view);
		return;
	}
	const std::vector<int> apps = app_results();
	const int ai = i - n;
	if (ai >= 0 && ai < static_cast<int>(apps.size())) {
		server_->spawn(server_->apps.apps()[static_cast<size_t>(apps[static_cast<size_t>(ai)])].exec);
		close();
	}
}

// ---- pointer ------------------------------------------------------------------------------

Overview::Window *Overview::window_at(double lx, double ly, bool *on_close) {
	if (on_close) *on_close = false;
	// Topmost first (the end of the list was made last).
	for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
		Window *w = it->get();
		if (!w->in_layout) continue;
		const GhostRect &r = w->ghost->current;
		if (hover_ == w && on_close && std::hypot(lx - (r.x + r.w - 2), ly - (r.y + 2)) <= 15) {
			*on_close = true;
			return w;
		}
		if (inside(r, lx, ly)) return w;
		// The icon under it counts too.
		if (std::fabs(lx - (r.x + r.w / 2)) <= kBadge / 2.0 && ly >= r.y + r.h && ly <= r.y + r.h + kBadge * 0.45) return w;
	}
	return nullptr;
}

Overview::WsThumb *Overview::thumb_at(double lx, double ly, Screen **screen) {
	for (auto &s : screens_) {
		for (auto &t : s->thumbs) {
			if (inside(t->box, lx, ly)) {
				if (screen) *screen = s.get();
				return t.get();
			}
		}
	}
	return nullptr;
}

int Overview::peek_at(double lx, double ly) const {
	for (const auto &s : screens_) {
		if (inside(s->peek_box[0], lx, ly)) return -1;
		if (inside(s->peek_box[1], lx, ly)) return 1;
	}
	return 0;
}

int Overview::app_at(double lx, double ly) const {
	for (const auto &s : screens_) {
		for (const AppHit &h : s->app_hits) {
			if (inside(h.box, lx, ly)) return h.app;
		}
	}
	return -1;
}

void Overview::set_hover(Window *w) {
	Window *before = hover_;
	hover_ = w;
	if (before && before != w) draw_window_extras(*before);
	if (w) draw_window_extras(*w);
}

void Overview::motion(double lx, double ly) {
	if (!open_) return;
	if (pressed_) {
		if (!dragging_ && std::hypot(lx - press_x_, ly - press_y_) >= 8) {
			dragging_ = true;
			set_hover(nullptr);
			server_->set_cursor("grabbing");
		}
		if (dragging_) {
			// The window follows the pointer, small, until it's dropped on a workspace.
			const GhostRect &p = pressed_->place;
			const double k = std::min(1.0, 220.0 / std::max(1.0, std::max(p.w, p.h)));
			pressed_->ghost->apply(GhostRect{lx - p.w * k / 2, ly - p.h * k / 2, p.w * k, p.h * k, 0.9});
			draw_window_extras(*pressed_);
			for (auto &s : screens_) {
				for (auto &t : s->thumbs) {
					const bool over = inside(t->box, lx, ly);
					set_rect_color(t->ring, t->index == server_->workspace ? theme::accent
						: over ? with_alpha(kText(), 0.55) : rgb(0xffffff, 0.0));
				}
			}
			return;
		}
	}
	bool on_close = false;
	Window *w = window_at(lx, ly, &on_close);
	if (w != hover_ || on_close != hover_close_) {
		hover_close_ = on_close;
		set_hover(w);
	}
	server_->set_cursor("default");
}

void Overview::press(double lx, double ly, uint32_t button) {
	if (!open_) return;
	bool on_close = false;
	if (Window *w = window_at(lx, ly, &on_close)) {
		if (on_close || button == BTN_MIDDLE) {
			w->view->close();
			return;
		}
		if (button == BTN_LEFT) {
			pressed_ = w;
			press_x_ = lx;
			press_y_ = ly;
			dragging_ = false;
		}
		return;
	}
	if (button != BTN_LEFT) return;
	// (The ✕ on the shown workspace's picture before the picture itself.)
	for (const auto &s : screens_) {
		if (inside(s->remove_box, lx, ly)) {
			server_->remove_workspace(server_->workspace);
			return;
		}
	}
	if (WsThumb *t = thumb_at(lx, ly)) {
		// The ring moves at once (the rest follows as the workspace changes).
		for (auto &sc : screens_) {
			for (auto &o : sc->thumbs) {
				set_rect_color(o->ring, o->index == t->index ? with_alpha(theme::accent, alpha_) : rgb(0xffffff, 0.0));
			}
		}
		server_->switch_workspace(t->index, false);
		return;
	}
	if (const int side = peek_at(lx, ly)) {
		server_->switch_workspace(server_->workspace + side, false);
		return;
	}
	for (const auto &s : screens_) {
		if (inside(s->add_box, lx, ly) || inside(s->strip_add, lx, ly)) {
			server_->add_workspace();
			return;
		}
		if (inside(s->remove_box, lx, ly)) {
			server_->remove_workspace(server_->workspace);
			return;
		}
	}
	if (const int app = app_at(lx, ly); app >= 0) {
		server_->spawn(server_->apps.apps()[static_cast<size_t>(app)].exec);
		close();
		return;
	}
	for (const auto &s : screens_) {
		if (inside(s->search_box, lx, ly)) return;   // (typing goes there anyway)
	}
	close();   // the empty background: back to the desktop
}

void Overview::release(double lx, double ly, uint32_t button) {
	if (button != BTN_LEFT || !pressed_) return;
	Window *w = pressed_;
	const bool was_drag = dragging_;
	pressed_ = nullptr;
	dragging_ = false;
	server_->set_cursor("default");
	if (!was_drag) {
		close(w->view);
		return;
	}
	if (WsThumb *t = thumb_at(lx, ly)) {
		server_->move_to_workspace(w->view, t->index, false);
	} else if (const int side = peek_at(lx, ly)) {
		server_->move_to_workspace(w->view, server_->workspace + side, false);
	} else {
		bool done = false;
		for (const auto &sc : screens_) {
			if (inside(sc->add_box, lx, ly) || inside(sc->strip_add, lx, ly)) {
				View *v = w->view;
				server_->add_workspace();   // a new one, with this window on it
				server_->move_to_workspace(v, server_->workspace_count - 1, false);
				done = true;
				break;
			}
		}
		// Dropped on another screen: the window goes over to it.
		if (!done) {
			for (const auto &sc : screens_) {
				const wlr_box ob = sc->output->box();
				if (sc.get() != w->screen && inside(ob, lx, ly)) {
					server_->move_to_screen(w->view, sc->output, false);
					// It's laid out with that screen's windows now.
					w->screen = sc.get();
					const wlr_box f = w->view->frame_box();
					w->real = GhostRect{static_cast<double>(f.x), static_cast<double>(f.y), static_cast<double>(f.width),
						static_cast<double>(f.height), 1};
					break;
				}
			}
		}
	}
	for (auto &s : screens_) build_strip(*s);
	layout();
	glide();
}

void Overview::scroll(double delta, bool horizontal) {
	if (!open_) return;
	(void)horizontal;
	scroll_accum_ += delta;
	const uint32_t now = static_cast<uint32_t>(now_ms());
	if (std::fabs(scroll_accum_) < 12 || now - last_scroll_ms_ < 180) return;
	const int dir = scroll_accum_ > 0 ? 1 : -1;
	scroll_accum_ = 0;
	last_scroll_ms_ = now;
	server_->switch_workspace(server_->workspace + dir, false);
}

// ---- keys ----------------------------------------------------------------------------------

bool Overview::handle_key(xkb_keysym_t sym, uint32_t modifiers, const std::string &text) {
	if (!open_) return false;
	const bool ctrl = modifiers & WLR_MODIFIER_CTRL;
	const bool alt = modifiers & WLR_MODIFIER_ALT;
	const bool super = modifiers & WLR_MODIFIER_LOGO;
	const int n = static_cast<int>(selectable().size()) + static_cast<int>(app_results().size());
	switch (sym) {
	case XKB_KEY_Escape:
		if (!query_.empty()) {
			query_.clear();
			selected_ = -1;
			layout();
			glide();
		} else {
			close();
		}
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		activate_selection();
		return true;
	case XKB_KEY_Page_Up:
	case XKB_KEY_Page_Down:
		server_->switch_workspace(server_->workspace + (sym == XKB_KEY_Page_Down ? 1 : -1), false);
		return true;
	case XKB_KEY_Left:
	case XKB_KEY_Right:
		if ((ctrl && alt) || super) {
			server_->switch_workspace(server_->workspace + (sym == XKB_KEY_Right ? 1 : -1), false);
			return true;
		}
		[[fallthrough]];
	case XKB_KEY_Up:
	case XKB_KEY_Down:
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab: {
		if (n == 0) return true;
		const bool back = sym == XKB_KEY_Left || sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab;
		int i = selected_ < 0 ? (back ? n - 1 : 0) : (selected_ + (back ? n - 1 : 1)) % n;
		select(i);
		return true;
	}
	case XKB_KEY_BackSpace:
		if (!query_.empty()) {
			size_t cut = query_.size() - 1;
			while (cut > 0 && (static_cast<unsigned char>(query_[cut]) & 0xc0) == 0x80) cut--;
			query_.erase(cut);
			selected_ = query_.empty() ? -1 : 0;
			layout();
			glide();
		}
		return true;
	default:
		break;
	}
	if (!ctrl && !super && !text.empty() && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f) {
		query_ += text;
		selected_ = 0;
		layout();
		glide();
	}
	return true;   // nothing else reaches the apps while it's open
}

} // namespace dusk
