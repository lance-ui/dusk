#include "wallpaper.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "draw.hpp"
#include "crash.hpp"
#include "server.hpp"

namespace dusk {

// ============================================================================
// The worker thread
// ============================================================================

namespace {

// `image` scaled into a new w x h (pixels) picture, the way `mode` says.
cairo_surface_t *compose(cairo_surface_t *image, const std::string &mode, int w, int h) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(1, w), std::max(1, h));
	cairo_t *cr = cairo_create(out);
	cairo_set_source_rgb(cr, 0.06, 0.07, 0.09);
	cairo_paint(cr);
	if (image) {
		const double iw = cairo_image_surface_get_width(image);
		const double ih = cairo_image_surface_get_height(image);
		if (iw > 0 && ih > 0) {
			cairo_save(cr);
			if (mode == "stretch") {
				cairo_scale(cr, w / iw, h / ih);
			} else {
				double s = 1.0;
				if (mode == "fit") s = std::min(w / iw, h / ih);
				else if (mode != "center") s = std::max(w / iw, h / ih);   // fill
				cairo_translate(cr, (w - iw * s) / 2.0, (h - ih * s) / 2.0);
				cairo_scale(cr, s, s);
			}
			cairo_set_source_surface(cr, image, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	cairo_destroy(cr);
	cairo_surface_flush(out);
	return out;
}

} // namespace

WallpaperWorker::WallpaperWorker(Server *server) : server_(server) {
	if (pipe2(pipe_, O_CLOEXEC | O_NONBLOCK) == 0) {
		source_ = wl_event_loop_add_fd(server->loop, pipe_[0], WL_EVENT_READABLE, &WallpaperWorker::handle_ready,
			this);
	}
	thread_ = std::thread([this] { run(); });
}

WallpaperWorker::~WallpaperWorker() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable()) thread_.join();
	if (source_) wl_event_source_remove(source_);
	for (int fd : pipe_) {
		if (fd >= 0) close(fd);
	}
	for (Result &r : results_) {
		if (r.surface) cairo_surface_destroy(r.surface);
	}
	if (cached_) cairo_surface_destroy(cached_);
}

void WallpaperWorker::request(const Job &job) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
			[&job](const Job &j) { return j.output == job.output; }), jobs_.end());
		jobs_.push_back(job);
	}
	wake_.notify_one();
}

void WallpaperWorker::run() {
	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
			if (stop_) return;
			job = jobs_.front();
			jobs_.pop_front();
		}
		if (job.path != cached_path_) {
			if (cached_) cairo_surface_destroy(cached_);
			cached_ = load_image(job.path);
			cached_path_ = job.path;
		}
		const int pw = std::max(1, static_cast<int>(std::lround(job.width * job.scale)));
		const int ph = std::max(1, static_cast<int>(std::lround(job.height * job.scale)));
		crumb("wallpaper worker: %s %dx%d", job.path.c_str(), pw, ph);
		Result r{job, compose(cached_, job.mode, pw, ph)};
		if (job.accent && cached_) {
			r.has_accent = accent_from_surface(cached_, r.accent);
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (stop_) {
				cairo_surface_destroy(r.surface);
				return;
			}
			results_.push_back(r);
		}
		const char one = 1;
		if (pipe_[1] >= 0) {
			ssize_t ignored = write(pipe_[1], &one, 1);
			(void) ignored;
		}
	}
}

int WallpaperWorker::handle_ready(int fd, uint32_t, void *data) {
	auto *self = static_cast<WallpaperWorker *>(data);
	char buf[64];
	while (read(fd, buf, sizeof buf) > 0) {
	}
	std::deque<Result> ready;
	{
		std::lock_guard<std::mutex> lock(self->mutex_);
		ready.swap(self->results_);
	}
	for (Result &r : ready) {
		if (self->done) self->done(r);
		if (r.surface) cairo_surface_destroy(r.surface);   // nobody wanted it any more
	}
	return 0;
}

// ============================================================================
// An output's wallpaper
// ============================================================================

namespace {

// Dusk's own wallpapers, drawn straight away (they're quick).
void draw_generated(cairo_t *cr, const Config &c, double w, double h) {
	if (c.wallpaper_type == "solid") {
		set_color(cr, c.wallpaper_color);
		cairo_paint(cr);
	} else if (c.wallpaper_type == "halftone") {
		// Screentone dots in the accent colour sweeping in from a corner.
		set_color(cr, theme::bg_alt);
		cairo_paint(cr);
		halftone(cr, 0, 0, w, h, w, h, std::hypot(w, h) * 0.62, 16,
			with_alpha(theme::accent, theme::dark ? 0.50 : 0.55));
	} else {
		// "Dusk": a deep blue evening sky with a soft violet horizon.
		cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, h);
		cairo_pattern_add_color_stop_rgb(sky, 0.00, 0.10, 0.09, 0.19);
		cairo_pattern_add_color_stop_rgb(sky, 0.42, 0.20, 0.18, 0.31);
		cairo_pattern_add_color_stop_rgb(sky, 0.57, 0.36, 0.33, 0.45);
		cairo_pattern_add_color_stop_rgb(sky, 0.62, 0.40, 0.37, 0.48);
		cairo_pattern_add_color_stop_rgb(sky, 0.75, 0.22, 0.20, 0.32);
		cairo_pattern_add_color_stop_rgb(sky, 1.00, 0.12, 0.11, 0.20);
		cairo_set_source(cr, sky);
		cairo_paint(cr);
		cairo_pattern_destroy(sky);
		// A faint glow in the accent colour along the horizon.
		cairo_pattern_t *glow = cairo_pattern_create_radial(w * 0.5, h * 0.6, 0, w * 0.5, h * 0.6, w * 0.55);
		cairo_pattern_add_color_stop_rgba(glow, 0, theme::accent.r, theme::accent.g, theme::accent.b, 0.16);
		cairo_pattern_add_color_stop_rgba(glow, 1, theme::accent.r, theme::accent.g, theme::accent.b, 0.0);
		cairo_set_source(cr, glow);
		cairo_paint(cr);
		cairo_pattern_destroy(glow);
	}
}

std::string color_key(const Color &c) {
	char buf[48];
	std::snprintf(buf, sizeof buf, "%.3f,%.3f,%.3f", c.r, c.g, c.b);
	return buf;
}

// wlroots aborts on a crop box that starts before the picture, and rounding
// (1.14 - 0.14 is 0.99999...) can put it a hair below zero: keep it inside.
void set_source(wlr_scene_buffer *b, double x, double y, double w, double h) {
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)) {
		wlr_scene_buffer_set_source_box(b, nullptr);
		return;
	}
	x = std::max(0.0, x);
	y = std::max(0.0, y);
	w = std::max(1.0, w);
	h = std::max(1.0, h);
	if (b->buffer) {
		x = std::min(x, std::max(0.0, b->buffer->width - 1.0));
		y = std::min(y, std::max(0.0, b->buffer->height - 1.0));
		w = std::min(w, b->buffer->width - x);
		h = std::min(h, b->buffer->height - y);
	}
	const wlr_fbox box{x, y, w, h};
	wlr_scene_buffer_set_source_box(b, &box);
}

} // namespace

void Output::update_wallpaper() {
	const wlr_box b = box();
	if (b.width <= 0 || b.height <= 0) {
		return;
	}
	if (!wallpaper) {
		wallpaper = wlr_scene_buffer_create(server->layer_background, nullptr);
	}
	if (!wall_next) {
		wlr_scene_node_set_position(&wallpaper->node, b.x, b.y);
	}
	if (b.width == wallpaper_w && b.height == wallpaper_h && output->scale == wallpaper_scale) {
		return;
	}
	wallpaper_w = b.width;
	wallpaper_h = b.height;
	wallpaper_scale = output->scale;

	const Config &c = server->config;
	if (c.wallpaper_type == "image" && !c.wallpaper.empty()) {
		// The picture: decoded and scaled on the worker; the old one stays
		// until it's ready.
		wall_wanted = "image|" + c.wallpaper + "|" + c.wallpaper_mode;
		WallpaperWorker::Job job;
		job.output = name();
		job.generation = ++wall_generation;
		job.path = c.wallpaper;
		job.mode = c.wallpaper_mode;
		job.width = b.width;
		job.height = b.height;
		job.scale = output->scale;
		job.accent = c.accent_auto;
		if (server->wall_worker) server->wall_worker->request(job);
		return;
	}
	++wall_generation;   // a picture still on its way isn't wanted any more
	wall_wanted = c.wallpaper_type + "|" + color_key(c.wallpaper_color) + "|" + color_key(theme::accent) + "|" +
		std::to_string(theme::dark);
	Canvas canvas(b.width, b.height, output->scale);
	draw_generated(canvas.cr(), c, b.width, b.height);
	show_wallpaper(canvas, wall_wanted);
}

void Output::finish_wallpaper_transition() {
	if (wall_anim) {
		const uint64_t id = wall_anim;
		wall_anim = 0;
		server->anim->cancel(id);
	}
	for (wlr_scene_buffer *strip : wall_strips) wlr_scene_node_destroy(&strip->node);
	wall_strips.clear();
	if (!wall_next) {
		return;
	}
	crumb("wallpaper transition done on %s", name().c_str());
	// The new picture's node becomes the wallpaper (rather than handing its
	// picture over, which would send it to the GPU a second time and hitch
	// right at the end).
	const wlr_box b = box();
	wlr_scene_buffer *next = wall_next;
	wall_next = nullptr;
	wlr_scene_buffer_set_dest_size(next, b.width, b.height);
	wlr_scene_buffer_set_source_box(next, nullptr);
	wlr_scene_buffer_set_opacity(next, 1.0f);
	wlr_scene_buffer_set_corner_radii(next, corner_radii_none());
	wlr_scene_node_set_position(&next->node, b.x, b.y);
	wlr_scene_node_set_enabled(&next->node, true);
	if (wallpaper) wlr_scene_node_place_above(&next->node, &wallpaper->node);   // where the old one was
	if (wallpaper) {
		wlr_scene_node_destroy(&wallpaper->node);
	}
	wallpaper = next;
}

void Output::show_wallpaper(Canvas &canvas, const std::string &content) {
	const Config &c = server->config;
	if (!wallpaper) {
		wallpaper = wlr_scene_buffer_create(server->layer_background, nullptr);
	}
	const bool same = content == wall_shown;
	const bool had = wallpaper && wallpaper->buffer;
	// The small soft copy (see wall_soft).
	{
		cairo_surface_t *full = cairo_get_target(canvas.cr());
		cairo_surface_flush(full);
		const int fw = cairo_image_surface_get_width(full), fh = cairo_image_surface_get_height(full);
		// Halved again and again (each step averages 2×2 pixels: a smooth
		// blur, as strong as the overview's) down to 1/64 of the size.
		double dx = 1, dy = 1;   // (the picture's pixels per unit)
		cairo_surface_get_device_scale(full, &dx, &dy);
		cairo_surface_t *small = nullptr;
		{
			int cw = static_cast<int>(fw / dx), ch = static_cast<int>(fh / dy);
			cairo_surface_t *cur = full;
			const int goal_w = std::max(4, cw / 64), goal_h = std::max(4, ch / 64);
			while (cw > goal_w || ch > goal_h) {
				const int nw = std::max(goal_w, cw / 2), nh = std::max(goal_h, ch / 2);
				cairo_surface_t *next = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
				cairo_t *cr = cairo_create(next);
				cairo_scale(cr, static_cast<double>(nw) / cw, static_cast<double>(nh) / ch);
				cairo_set_source_surface(cr, cur, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
				cairo_paint(cr);
				cairo_destroy(cr);
				if (cur != full) cairo_surface_destroy(cur);
				cur = next;
				cw = nw;
				ch = nh;
			}
			small = cur == full ? cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4) : cur;
		}
		if (wall_soft) cairo_surface_destroy(wall_soft);
		wall_soft = small;
		wall_soft_gen++;
	}
	// Pictures changed quickly one after another: the one in the middle of
	// coming in jumps to the end, and the newest simply replaces it (no pile
	// of transitions).
	const bool busy = wall_next != nullptr;
	finish_wallpaper_transition();
	wall_shown = content;
	std::string style = c.wallpaper_transition;
	if (style == "random") {
		static const char *kStyles[] = {"fade", "slide", "push", "zoom", "wipe", "grow", "circle", "pointer", "iris"};
		static std::mt19937 rng{std::random_device{}()};
		style = kStyles[std::uniform_int_distribution<int>(0, 8)(rng)];
	}
	const double ms = c.ms(c.wallpaper_transition_ms);
	crumb("wallpaper %s: %s (style %s, same %d had %d busy %d)", name().c_str(), content.c_str(), style.c_str(),
		same, had, busy);
	wlr_log(WLR_INFO, "wallpaper on %s: %s (transition %s, %.0f ms%s)", name().c_str(), content.c_str(),
		same || !had ? "none: same picture or the first one" : style.c_str(), ms, c.animations ? "" : ", animations off");
	if (same || !had || busy || style == "none" || !c.animations || ms < 1 || !enabled()) {
		canvas.present(wallpaper, true);   // a resize, the first one, or no transition
		return;
	}

	// The new one goes on a node over the old and comes in; at the end it
	// replaces the old one.
	const wlr_box b = box();
	wall_next = wlr_scene_buffer_create(server->layer_background, nullptr);
	wlr_scene_node_place_above(&wall_next->node, &wallpaper->node);
	if (style == "wipe") {
		// The diagonal wipe: the new picture in thin strips across the screen,
		// each one uncovered a little later than the one above it, so the
		// edge between old and new runs corner to corner. (Each strip is its
		// own small picture: the whole one per strip would go to the GPU
		// dozens of times.)
		cairo_surface_t *full = cairo_get_target(canvas.cr());
		cairo_surface_flush(full);
		// The picture's own units per screen unit (its pixels, unless it says
		// its scale itself).
		double dsx = 1, dsy = 1;
		cairo_surface_get_device_scale(full, &dsx, &dsy);
		const double fx = cairo_image_surface_get_height(full) / dsy / std::max(1, b.height);
		const int n = std::clamp(b.height / 18, 12, 90);
		for (int i = 0; i < n; i++) {
			const int y0 = b.height * i / n, y1 = b.height * (i + 1) / n;
			Canvas strip(b.width, std::max(1, y1 - y0), output->scale);
			cairo_scale(strip.cr(), 1 / fx, 1 / fx);
			cairo_set_source_surface(strip.cr(), full, 0, -y0 * fx);
			cairo_paint(strip.cr());
			wlr_scene_buffer *node = wlr_scene_buffer_create(server->layer_background, nullptr);
			strip.present(node, true);
			wlr_scene_node_place_above(&node->node, &wallpaper->node);
			wlr_scene_node_set_position(&node->node, b.x, b.y + y0);
			wlr_scene_node_set_enabled(&node->node, false);
			wall_strips.push_back(node);
		}
	}
	canvas.present(wall_next, true);
	if (style == "wipe") {
		wlr_scene_node_set_enabled(&wall_next->node, false);   // shown at the end, in one piece
	} else if (style == "iris") {
		// The old one closes in a shrinking circle over the new one.
		wlr_scene_node_place_below(&wall_next->node, &wallpaper->node);
	}
	// Circles from the pointer start where it is (on this screen).
	double ox = b.width / 2.0, oy = b.height / 2.0;
	if (style == "pointer") {
		ox = std::clamp(server->cursor->x - b.x, 0.0, static_cast<double>(b.width));
		oy = std::clamp(server->cursor->y - b.y, 0.0, static_cast<double>(b.height));
	}
	const double pw = wall_next->buffer ? wall_next->buffer->width : b.width;
	const double ph = wall_next->buffer ? wall_next->buffer->height : b.height;
	const double opw = wallpaper->buffer ? wallpaper->buffer->width : pw;
	const double oph = wallpaper->buffer ? wallpaper->buffer->height : ph;

	// A circle round (cx, cy) with diameter d on `n` (a picture pw x ph
	// covering the screen): only the part on the screen is shown (nothing
	// spills onto a screen next to it), rounded to the circle; its corners
	// square up at the end so it ends as the plain picture.
	auto circle = [b](wlr_scene_buffer *n, double cx, double cy, double d, double pw, double ph, double t) {
		const double w = b.width, h = b.height;
		const double x0 = std::clamp(cx - d / 2, 0.0, w), x1 = std::clamp(cx + d / 2, 0.0, w);
		const double y0 = std::clamp(cy - d / 2, 0.0, h), y1 = std::clamp(cy + d / 2, 0.0, h);
		const double dw = std::max(1.0, x1 - x0), dh = std::max(1.0, y1 - y0);
		wlr_scene_node_set_position(&n->node, b.x + static_cast<int>(std::lround(x0)),
			b.y + static_cast<int>(std::lround(y0)));
		set_source(n, pw * x0 / w, ph * y0 / h, pw * dw / w, ph * dh / h);
		wlr_scene_buffer_set_dest_size(n, static_cast<int>(std::lround(dw)), static_cast<int>(std::lround(dh)));
		const double square_up = std::clamp((t - 0.82) / 0.18, 0.0, 1.0);
		const double r = std::min({d / 2, dw / 2, dh / 2}) * (1.0 - square_up);
		wlr_scene_buffer_set_corner_radius(n, static_cast<int>(std::lround(r)));
	};
	// Far enough to cover the whole screen from (ox, oy).
	const double reach = 2 * std::max({std::hypot(ox, oy), std::hypot(b.width - ox, oy),
		std::hypot(ox, b.height - oy), std::hypot(b.width - ox, b.height - oy)});

	auto step = [this, style, b, pw, ph, opw, oph, circle, ox, oy, reach](double raw) {
		if (!wall_next) return;
		const double t = std::clamp(raw, 0.0, 1.0);   // already eased (Settings > Animations)
		const double w = b.width, h = b.height;
		wlr_scene_buffer *n = wall_next;
		if (style == "fade") {
			wlr_scene_buffer_set_opacity(n, static_cast<float>(t));
		} else if (style == "slide" || style == "push") {
			// In from the right: its left part shows first, at the right edge.
			const double vis = std::max(1.0, w * t);
			wlr_scene_node_set_position(&n->node, b.x + static_cast<int>(std::lround(w - vis)), b.y);
			set_source(n, 0, 0, pw * vis / w, ph);
			wlr_scene_buffer_set_dest_size(n, static_cast<int>(std::lround(vis)), b.height);
			if (style == "push" && wallpaper) {
				// The old one leaves to the left.
				const double left = std::max(1.0, w - vis);
				set_source(wallpaper, opw * (1 - left / w), 0, opw * left / w, oph);
				wlr_scene_buffer_set_dest_size(wallpaper, static_cast<int>(std::lround(left)), b.height);
				wlr_scene_node_set_position(&wallpaper->node, b.x, b.y);
			}
		} else if (style == "wipe") {
			// Diagonal: from the top left corner to the bottom right. Each strip
			// shows as far as the slanted edge has got at its height.
			const int count = static_cast<int>(wall_strips.size());
			const double slope = 0.6;   // how far the edge leans
			for (int i = 0; i < count; i++) {
				wlr_scene_buffer *s = wall_strips[static_cast<size_t>(i)];
				const double ymid = h * (i + 0.5) / count;
				const double vis = std::clamp(t * (w + h * slope) - ymid * slope, 0.0, w);
				if (vis < 1) {
					wlr_scene_node_set_enabled(&s->node, false);
					continue;
				}
				const double sw = s->buffer ? s->buffer->width : w, sh = s->buffer ? s->buffer->height : 1;
				const int y0 = static_cast<int>(h * i / count), y1 = static_cast<int>(h * (i + 1) / count);
				set_source(s, 0, 0, sw * vis / w, sh);
				wlr_scene_buffer_set_dest_size(s, static_cast<int>(std::lround(vis)), std::max(1, y1 - y0));
				wlr_scene_node_set_enabled(&s->node, true);
			}
		} else if (style == "circle" || style == "pointer") {
			// A circle opening out, from the middle or from the pointer.
			const double e = t * t * (3 - 2 * t);
			circle(n, ox, oy, std::max(2.0, reach * e), pw, ph, t);
		} else if (style == "iris") {
			// The old one closes into the middle, like a camera's iris.
			if (wallpaper) {
				const double e = t * t * (3 - 2 * t);
				circle(wallpaper, ox, oy, std::max(2.0, reach * (1 - e)), opw, oph, 0.0);
				if (t >= 0.999) wlr_scene_node_set_enabled(&wallpaper->node, false);
			}
		} else if (style == "zoom") {
			// Fades in while settling from a little closer.
			const double z = std::max(1.0, 1.14 - 0.14 * t);
			set_source(n, pw * (1 - 1 / z) / 2, ph * (1 - 1 / z) / 2, pw / z, ph / z);
			wlr_scene_buffer_set_opacity(n, static_cast<float>(std::min(1.0, t * 1.4)));
		} else {   // grow
			// From a small rounded shape in the middle out to the whole screen.
			const double k = std::max(0.002, t);
			const double dw = std::max(1.0, w * k), dh = std::max(1.0, h * k);
			wlr_scene_node_set_position(&n->node, b.x + static_cast<int>(std::lround((w - dw) / 2)),
				b.y + static_cast<int>(std::lround((h - dh) / 2)));
			set_source(n, pw * (1 - k) / 2, ph * (1 - k) / 2, pw * k, ph * k);
			wlr_scene_buffer_set_dest_size(n, static_cast<int>(std::lround(dw)), static_cast<int>(std::lround(dh)));
			wlr_scene_buffer_set_corner_radius(n, static_cast<int>(std::lround(std::min(dw, dh) / 2 * (1 - t * t))));
		}
	};
	step(0.0);
	wall_anim = server->anim->start(ms, step, [this] {
		wall_anim = 0;
		finish_wallpaper_transition();
	});
}

} // namespace dusk
