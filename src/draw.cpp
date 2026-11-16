#include "draw.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include <gdk-pixbuf/gdk-pixbuf.h>

namespace dusk {

namespace {

// A wlr_buffer whose pixels live in a cairo image surface.
struct CairoBuffer {
	wlr_buffer base;   // must stay the first member
	cairo_surface_t *surface;
};

void buffer_destroy(wlr_buffer *buffer) {
	auto *cb = reinterpret_cast<CairoBuffer *>(buffer);
	cairo_surface_destroy(cb->surface);
	wlr_buffer_finish(buffer);
	delete cb;
}

bool buffer_begin_access(wlr_buffer *buffer, uint32_t, void **data,
		uint32_t *format, size_t *stride) {
	auto *cb = reinterpret_cast<CairoBuffer *>(buffer);
	*data = cairo_image_surface_get_data(cb->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = static_cast<size_t>(cairo_image_surface_get_stride(cb->surface));
	return *data != nullptr;
}

void buffer_end_access(wlr_buffer *) {}

wlr_buffer_impl make_impl() {
	wlr_buffer_impl impl{};
	impl.destroy = buffer_destroy;
	impl.begin_data_ptr_access = buffer_begin_access;
	impl.end_data_ptr_access = buffer_end_access;
	return impl;
}

const wlr_buffer_impl cairo_buffer_impl = make_impl();

// The scene lets go of a picture once it's on the GPU, and then forgets it
// (wlr_scene_buffer.buffer becomes NULL). We keep each node's picture until
// the node gets another one or goes away: window animations copy it, the
// overview shows the wallpaper again, and a new wallpaper can only fade in
// over an old one that's still known. (Without this, wallpaper transitions
// never ran.)
struct Keeper {
	wl_listener destroy;   // must stay the first member
	wlr_scene_buffer *node = nullptr;
	wlr_buffer *buffer = nullptr;
};

std::unordered_map<wlr_scene_buffer *, Keeper *> &keepers() {
	static std::unordered_map<wlr_scene_buffer *, Keeper *> map;
	return map;
}

void keeper_node_destroyed(wl_listener *listener, void *) {
	auto *k = reinterpret_cast<Keeper *>(listener);
	if (k->buffer) wlr_buffer_unlock(k->buffer);
	wl_list_remove(&k->destroy.link);
	keepers().erase(k->node);
	delete k;
}

} // namespace

void set_kept_buffer(wlr_scene_buffer *node, wlr_buffer *buffer) {
	wlr_scene_buffer_set_buffer(node, buffer);
	Keeper *&k = keepers()[node];
	if (!k) {
		k = new Keeper{};
		k->node = node;
		k->destroy.notify = keeper_node_destroyed;
		wl_signal_add(&node->node.events.destroy, &k->destroy);
	}
	if (k->buffer) wlr_buffer_unlock(k->buffer);
	k->buffer = buffer ? wlr_buffer_lock(buffer) : nullptr;
}

Canvas::Canvas(int width, int height, float scale)
		: w_(width > 0 ? width : 1), h_(height > 0 ? height : 1) {
	if (scale <= 0.0f) {
		scale = 1.0f;
	}
	int pw = static_cast<int>(std::lround(w_ * scale));
	int ph = static_cast<int>(std::lround(h_ * scale));
	surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_surface_set_device_scale(surface_, scale, scale);
	cr_ = cairo_create(surface_);
	// Crisp text: lightly hinted to the pixel grid (unhinted text on an
	// image surface came out soft, worst at 100% scale).
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
	cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);   // (widths stay as measured)
	cairo_set_font_options(cr_, fo);
	cairo_font_options_destroy(fo);
}

Canvas::Canvas(cairo_surface_t *adopt, int width, int height)
		: w_(width > 0 ? width : 1), h_(height > 0 ? height : 1) {
	surface_ = adopt;
	cr_ = cairo_create(surface_);
}

Canvas::~Canvas() {
	if (cr_) {
		cairo_destroy(cr_);
	}
	if (surface_) {
		cairo_surface_destroy(surface_);
	}
}

void Canvas::present(wlr_scene_buffer *node) {
	present(node, false);
}

void Canvas::present(wlr_scene_buffer *node, bool keep) {
	if (!surface_) {
		return;
	}
	cairo_destroy(cr_);
	cr_ = nullptr;
	cairo_surface_flush(surface_);

	auto *cb = new CairoBuffer{};
	cb->surface = surface_;
	surface_ = nullptr;   // the buffer owns it now
	wlr_buffer_init(&cb->base, &cairo_buffer_impl,
		cairo_image_surface_get_width(cb->surface),
		cairo_image_surface_get_height(cb->surface));

	if (keep) {
		set_kept_buffer(node, &cb->base);
	} else {
		wlr_scene_buffer_set_buffer(node, &cb->base);
	}
	wlr_scene_buffer_set_dest_size(node, w_, h_);
	wlr_buffer_drop(&cb->base);
}

void set_color(cairo_t *cr, const Color &c) {
	cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	r = std::min(r, std::min(w, h) / 2.0);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void fill_rounded(cairo_t *cr, double x, double y, double w, double h, double r,
		const Color &c) {
	rounded_rect(cr, x, y, w, h, r);
	set_color(cr, c);
	cairo_fill(cr);
}

void ink_card(cairo_t *cr, double x, double y, double w, double h, double r,
		const Color &fill, double shadow_offset) {
	const double lw = theme::line_width;
	if (shadow_offset > 0) {
		rounded_rect(cr, x + shadow_offset, y + shadow_offset, w, h, r);
		set_color(cr, theme::shadow);
		cairo_fill(cr);
	}
	rounded_rect(cr, x + lw / 2, y + lw / 2, w - lw, h - lw, r);
	set_color(cr, fill);
	if (lw > 0) {
		cairo_fill_preserve(cr);
		set_color(cr, theme::line);
		cairo_set_line_width(cr, lw);
		cairo_stroke(cr);
	} else {
		cairo_fill(cr);
	}
}

void ui_card(cairo_t *cr, double x, double y, double w, double h, double r, const Color &fill) {
	const double lw = theme::ui_line == 2 ? 1.6 : theme::ui_line == 1 ? 1.0 : 0.0;
	const double inset = lw / 2;
	rounded_rect(cr, x + inset, y + inset, w - lw, h - lw, r - inset);
	set_color(cr, fill);
	cairo_fill_preserve(cr);
	cairo_save(cr);
	cairo_clip(cr);
	if (theme::ui_halftone) {
		// Screentone fading in from the bottom-right corner, like the
		// shading on a printed page.
		halftone(cr, x, y, w, h, x + w, y + h, std::hypot(w, h) * 0.55, 7,
			with_alpha(theme::text, theme::dark ? 0.07 : 0.06));
	}
	// The rim: a thin light line just inside the top edge, like the
	// highlight colourists put along the top of a shape.
	if (theme::ui_line > 0) {
		cairo_set_line_width(cr, 1.0);
		rounded_rect(cr, x + lw + 0.5, y + lw + 0.5, w - 2 * lw - 1, h - 2 * lw - 1, r - lw);
		cairo_pattern_t *fade = cairo_pattern_create_linear(0, y, 0, y + std::min(h, 2.5 * r));
		const Color &c = theme::rim;
		cairo_pattern_add_color_stop_rgba(fade, 0, c.r, c.g, c.b, c.a);
		cairo_pattern_add_color_stop_rgba(fade, 1, c.r, c.g, c.b, 0);
		cairo_set_source(cr, fade);
		cairo_stroke(cr);
		cairo_pattern_destroy(fade);
	}
	cairo_restore(cr);
	if (lw > 0) {
		rounded_rect(cr, x + inset, y + inset, w - lw, h - lw, r - inset);
		set_color(cr, theme::ui_line_color);
		cairo_set_line_width(cr, lw);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke(cr);
	} else {
		cairo_new_path(cr);
	}
}

// A row of slanted LED segments along `length`, thick `thick` across: the
// cockpit gauge the OSD's volume and brightness bars and the panel's
// workspace pager share, so the two always look alike.
void led_segments(cairo_t *cr, double x, double y, double length, double thick,
		bool vertical, int count, int lit, const Color &color, bool off,
		const Color *per_segment) {
	const Color unlit = with_alpha(theme::text, theme::dark ? 0.12 : 0.10);
	const double gap = 3, slant = 3.5;
	const double sw = (length - gap * (count - 1) - slant) / std::max(1, count);
	for (int i = 0; i < count; i++) {
		cairo_new_path(cr);
		if (vertical) {
			// Bottom to top, slanted the other way round.
			const double sy = y + length - slant - (i + 1) * sw - i * gap;
			cairo_move_to(cr, x, sy + slant);
			cairo_line_to(cr, x + thick, sy);
			cairo_line_to(cr, x + thick, sy + sw);
			cairo_line_to(cr, x, sy + sw + slant);
		} else {
			const double sx = x + i * (sw + gap);
			cairo_move_to(cr, sx + slant, y);
			cairo_line_to(cr, sx + slant + sw, y);
			cairo_line_to(cr, sx + sw, y + thick);
			cairo_line_to(cr, sx, y + thick);
		}
		cairo_close_path(cr);
		Color c = per_segment ? per_segment[i] : i < lit ? color : unlit;
		if (!per_segment && i < lit && !off) {
			if (i == lit - 1) {
				c = Color{std::min(1.0, c.r * 0.55 + 0.45), std::min(1.0, c.g * 0.55 + 0.45),
					std::min(1.0, c.b * 0.55 + 0.45), 1.0};
			} else {
				c.a = 0.55 + 0.45 * (i + 1.0) / std::max(1, lit);   // brighter towards the tip
			}
		}
		set_color(cr, c);
		cairo_fill(cr);
	}
}

void halftone(cairo_t *cr, double x, double y, double w, double h,
		double fx, double fy, double reach, double spacing, const Color &color) {
	set_color(cr, color);
	for (double py = y + spacing / 2; py < y + h; py += spacing) {
		// Offset every other row, like real screentone.
		const bool odd = static_cast<int>((py - y) / spacing) % 2;
		for (double px = x + (odd ? spacing : spacing / 2); px < x + w; px += spacing) {
			const double d = std::hypot(px - fx, py - fy);
			const double t = 1.0 - d / reach;
			if (t <= 0) {
				continue;
			}
			cairo_new_sub_path(cr);
			cairo_arc(cr, px, py, spacing * 0.48 * t, 0, 2 * M_PI);
		}
	}
	cairo_fill(cr);
}

void draw_text(cairo_t *cr, const std::string &text, double x, double y,
		double w, double h, const char *font, const Color &color, Align align) {
	if (text.empty() || w <= 0) {
		return;
	}
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text.c_str(), -1);
	pango_layout_set_single_paragraph_mode(layout, TRUE);
	pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	switch (align) {
	case Align::Left: pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT); break;
	case Align::Center: pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER); break;
	case Align::Right: pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT); break;
	}

	int tw = 0, th = 0;
	pango_layout_get_pixel_size(layout, &tw, &th);
	set_color(cr, color);
	cairo_move_to(cr, x, y + (h - th) / 2.0);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

int draw_paragraph(cairo_t *cr, const std::string &text, double x, double y, double w,
		int max_lines, const char *font, const Color &color, bool draw) {
	if (text.empty() || w <= 0) {
		return 0;
	}
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text.c_str(), -1);
	pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_height(layout, -std::max(1, max_lines));   // negative = lines
	int tw = 0, th = 0;
	pango_layout_get_pixel_size(layout, &tw, &th);
	if (draw) {
		set_color(cr, color);
		cairo_move_to(cr, x, y);
		pango_cairo_show_layout(cr, layout);
	}
	g_object_unref(layout);
	return th;
}

void draw_app_badge(cairo_t *cr, double cx, double cy, double radius,
		const std::string &key, const std::string &label) {
	// Pick a hue from the key so each app keeps its colour.
	uint32_t hash = 2166136261u;
	for (unsigned char ch : key) {
		hash = (hash ^ ch) * 16777619u;
	}
	// Flat, slightly muted colours, like screen-printed manga colour.
	double h = (hash % 360) / 60.0, s = 0.45, v = 0.70;
	double c = v * s, xx = c * (1 - std::fabs(std::fmod(h, 2.0) - 1)), m = v - c;
	double r = 0, g = 0, b = 0;
	switch (static_cast<int>(h)) {
	case 0: r = c; g = xx; break;
	case 1: r = xx; g = c; break;
	case 2: g = c; b = xx; break;
	case 3: g = xx; b = c; break;
	case 4: r = xx; b = c; break;
	default: r = c; b = xx; break;
	}
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, radius, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, r + m, g + m, b + m);
	cairo_fill(cr);

	std::string letter;
	if (!label.empty()) {
		// First UTF-8 character.
		size_t len = 1;
		unsigned char first = static_cast<unsigned char>(label[0]);
		if (first >= 0xf0) len = 4;
		else if (first >= 0xe0) len = 3;
		else if (first >= 0xc0) len = 2;
		letter = label.substr(0, len);
		if (len == 1) {
			letter[0] = static_cast<char>(std::toupper(first));
		}
	}
	draw_text(cr, letter, cx - radius, cy - radius, radius * 2, radius * 2,
		radius >= 12 ? theme::font_large : theme::font_bold, rgb(0xffffff),
		Align::Center);
}

namespace {

// RGB(A) pixbuf -> cairo surface. Takes ownership of `pixbuf`.
cairo_surface_t *pixbuf_to_surface(GdkPixbuf *pixbuf);

} // namespace

// gdk-pixbuf (and the image loaders behind it, glycin on newer Fedora) is
// used from the wallpaper thread too: one picture at a time.
std::mutex &pixbuf_lock() {
	static std::mutex m;
	return m;
}

cairo_surface_t *load_image(const std::string &path) {
	std::lock_guard<std::mutex> guard(pixbuf_lock());
	GError *error = nullptr;
	GdkPixbuf *loaded = gdk_pixbuf_new_from_file(path.c_str(), &error);
	if (!loaded) {
		wlr_log(WLR_ERROR, "Could not load image %s: %s", path.c_str(),
			error ? error->message : "unknown error");
		if (error) {
			g_error_free(error);
		}
		return nullptr;
	}
	GdkPixbuf *pixbuf = gdk_pixbuf_apply_embedded_orientation(loaded);
	g_object_unref(loaded);
	if (!pixbuf) {
		return nullptr;
	}
	return pixbuf_to_surface(pixbuf);
}

cairo_surface_t *load_image_sized(const std::string &path, int width, int height) {
	std::lock_guard<std::mutex> guard(pixbuf_lock());
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path.c_str(), width, height,
		TRUE, nullptr);
	return pixbuf ? pixbuf_to_surface(pixbuf) : nullptr;
}

cairo_surface_t *load_image_bytes(const std::string &bytes, int max_w, int max_h, int *full_w,
		int *full_h) {
	std::lock_guard<std::mutex> guard(pixbuf_lock());
	if (bytes.empty()) {
		return nullptr;
	}
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	const bool ok = gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar *>(bytes.data()),
		bytes.size(), nullptr);
	gdk_pixbuf_loader_close(loader, nullptr);
	GdkPixbuf *pixbuf = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;   // the loader's
	cairo_surface_t *out = nullptr;
	if (pixbuf) {
		const int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
		if (full_w) *full_w = w;
		if (full_h) *full_h = h;
		const double s = std::min({1.0, max_w / static_cast<double>(w), max_h / static_cast<double>(h)});
		const int tw = std::max(1, static_cast<int>(std::lround(w * s)));
		const int th = std::max(1, static_cast<int>(std::lround(h * s)));
		GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, tw, th, GDK_INTERP_BILINEAR);
		if (scaled) {
			out = pixbuf_to_surface(scaled);   // takes the scaled copy
		}
	}
	g_object_unref(loader);
	return out;
}

namespace {

cairo_surface_t *pixbuf_to_surface(GdkPixbuf *pixbuf) {

	const int w = gdk_pixbuf_get_width(pixbuf);
	const int h = gdk_pixbuf_get_height(pixbuf);
	const int channels = gdk_pixbuf_get_n_channels(pixbuf);
	const bool has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
	const int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
	const guchar *src = gdk_pixbuf_read_pixels(pixbuf);

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		g_object_unref(pixbuf);
		cairo_surface_destroy(surface);
		return nullptr;
	}
	cairo_surface_flush(surface);
	unsigned char *dst = cairo_image_surface_get_data(surface);
	const int dst_stride = cairo_image_surface_get_stride(surface);

	// RGB(A) bytes -> cairo's premultiplied native-endian ARGB32.
	for (int y = 0; y < h; y++) {
		const guchar *in = src + static_cast<size_t>(y) * src_stride;
		auto *out = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * dst_stride);
		for (int x = 0; x < w; x++, in += channels) {
			const uint32_t a = has_alpha ? in[3] : 255;
			const uint32_t r = in[0] * a / 255, g = in[1] * a / 255, b = in[2] * a / 255;
			out[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	cairo_surface_mark_dirty(surface);
	g_object_unref(pixbuf);
	return surface;
}

} // namespace

Color mix(const Color &a, const Color &b, double t) {
	return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
		a.a + (b.a - a.a) * t};
}

void to_float(const Color &c, float out[4]) {
	// wlroots wants premultiplied alpha.
	out[0] = static_cast<float>(c.r * c.a);
	out[1] = static_cast<float>(c.g * c.a);
	out[2] = static_cast<float>(c.b * c.a);
	out[3] = static_cast<float>(c.a);
}

// Measured once and remembered (the panels, menus and cards measure the same
// words again on every redraw), with one layout kept for measuring instead
// of a new picture, context and layout each time.
int text_width(const std::string &text, const char *font) {
	struct Measurer {
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		cairo_t *cr = cairo_create(surface);
		PangoLayout *layout = pango_cairo_create_layout(cr);
		std::string font;
		std::unordered_map<std::string, int> widths;
		~Measurer() {
			g_object_unref(layout);
			cairo_destroy(cr);
			cairo_surface_destroy(surface);
		}
	};
	thread_local Measurer m;
	std::string key = font ? font : "";
	key += '\x1f';
	key += text;
	if (auto it = m.widths.find(key); it != m.widths.end()) return it->second;
	if (m.font != (font ? font : "")) {
		m.font = font ? font : "";
		PangoFontDescription *desc = pango_font_description_from_string(m.font.c_str());
		pango_layout_set_font_description(m.layout, desc);
		pango_font_description_free(desc);
	}
	pango_layout_set_text(m.layout, text.c_str(), -1);
	int tw = 0, th = 0;
	pango_layout_get_pixel_size(m.layout, &tw, &th);
	if (m.widths.size() > 4000) m.widths.clear();   // (the clock and timers keep making new ones)
	m.widths.emplace(std::move(key), tw);
	return tw;
}

} // namespace dusk
