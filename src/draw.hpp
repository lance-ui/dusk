// Drawing helpers: cairo canvases that become wlroots scene buffers,
// text via pango, and the shared colour theme.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "wlr.hpp"

namespace dusk {

struct Color {
	double r, g, b, a = 1.0;
};

constexpr Color rgb(unsigned hex, double alpha = 1.0) {
	return Color{((hex >> 16) & 0xff) / 255.0, ((hex >> 8) & 0xff) / 255.0,
		(hex & 0xff) / 255.0, alpha};
}

constexpr Color with_alpha(const Color &c, double alpha) {
	return Color{c.r, c.g, c.b, alpha};
}

// The look of the desktop in one place: a normal light or dark theme, plus
// the outline, shadow and texture styles chosen in Settings. The "ink" style
// borrows from anime line art: soft dark contour lines, flat colour, a thin
// light rim on top edges, and shadows that sit just below things.
namespace theme {
inline bool dark = true;
inline bool oled = false;            // dark mode in pure black (lit pixels only where there's content)
inline bool high_contrast = false;   // accessibility: full-strength lines and text
inline bool own_icons = true;        // Dusk's drawn icons rather than the icon theme's
inline int line_width = 2;           // window outline: 0, 1 or 2 px
inline int border_width = 2;         // same as line_width, for window frames
inline int shadow_kind = 3;          // windows: 0 none, 1 soft, 2 hard, 3 ink
inline int title_height = 34;
inline bool accent_border = false;   // active window outline in the accent colour
inline bool buttons_left = false;    // titlebar buttons on the left (macOS style)
inline bool title_left = false;      // left-aligned window titles
inline bool show_titles = false;     // the window's title in our title bar
inline bool button_gtk = true;       // round GTK-style titlebar buttons with symbolic icons
inline bool button_sketch = false;   // drawn corner brackets and a red X
inline bool show_minimize = true;
inline bool show_maximize = true;

// Panels, menus, cards.
inline int ui_line = 2;              // 0 none, 1 thin, 2 ink
inline int ui_shadow = 1;            // 0 none, 1 soft, 2 ink
inline bool ui_halftone = false;     // screentone dots on cards
inline int ui_radius = 22;           // outer corner radius of cards and popups

// The small corners (Windows-look buttons, chips) scaled with Roundness.
inline double small_radius(double at22 = 6) { return std::max(1.5, at22 * ui_radius / 22.0); }
inline Color accent = rgb(0x4d8df6);
inline Color on_accent = rgb(0xffffff);
inline Color ink;                    // line-art outline colour
inline Color rim;                    // thin light edge along the top of shapes
// Status colours, one set for the whole desktop (they're a touch deeper on
// light backgrounds so they still read).
inline Color danger = rgb(0xe5484d);   // low battery, recording, close, errors
inline Color warning = rgb(0xf0b429);  // getting low, attention
inline Color success = rgb(0x3fb950);  // charging, done
inline Color line, line_soft;        // window outline colours (strong / inactive)
inline Color ui_line_color;          // outline of panels and cards
inline Color bg, bg_alt;             // surfaces
inline Color title_active, title_inactive, title_text, title_text_inactive;
inline Color border_active, border_inactive;
inline Color close_hover, button_hover;
inline Color panel_bg, panel_hover, panel_active;
inline Color text, text_dim, menu_bg, field_bg, divider;
inline Color card_bg;                // cards inside the control centre
inline double ui_opacity = 0.75;     // panels, menus, cards: how solid their background is
inline double ui_blur = 1.0;         // how much what's behind them is blurred (0 = off)
inline Color tile_off;               // control centre tile, switched off
inline Color button_bg;              // GTK-style titlebar button background
inline Color shadow;                 // window shadow
inline Color ui_shadow_color;        // shadow under panels and popups

inline void set_ui(const std::string &outline, const std::string &shadow_style,
		const std::string &texture, int radius) {
	ui_line = outline == "none" ? 0 : outline == "thin" ? 1 : 2;
	ui_shadow = shadow_style == "none" ? 0 : shadow_style == "ink" ? 2 : 1;
	(void)texture;
	ui_halftone = false;   // screentone was removed in 0.7 (too busy)
	ui_radius = radius;
	ui_line_color = ui_line == 2 ? with_alpha(ink, dark ? 0.85 : 0.70)
		: dark ? rgb(0xffffff, oled ? 0.16 : 0.09) : rgb(0x1e1914, 0.10);
	if (high_contrast && ui_line == 2) {
		ui_line_color = with_alpha(ink, 1.0);   // outlines that read on any screen
	}
	if (oled && dark && ui_line == 0) {
		// Black on black needs something to show where a panel or menu ends.
		ui_line_color = rgb(0xffffff, 0.10);
	}
	if (ui_shadow == 2) {
		ui_shadow_color = dark ? rgb(0x000000, 0.42) : with_alpha(ink, 0.22);
	} else {
		// Soft shadows look best faint, wide and slightly tinted.
		ui_shadow_color = dark ? rgb(0x000000, 0.22) : rgb(0x3c2d1e, 0.08);
	}
}

inline void set_theme(bool use_dark, const std::string &border, const std::string &shadow_style,
		int titlebar_height, bool accent_outline, bool pure_black = false) {
	dark = use_dark;
	oled = use_dark && pure_black;
	line_width = border == "none" ? 0 : border == "thin" ? 1 : 2;
	border_width = line_width;
	shadow_kind = shadow_style == "none" ? 0 : shadow_style == "soft" ? 1
		: shadow_style == "hard" ? 2 : 3;
	title_height = titlebar_height;
	accent_border = accent_outline;

	if (!dark) {
		// 0.35, calm: ivory and white, hairlines, colour only where it means something.
		// 0.35.5: libadwaita's own light greys, so Dusk sits with GNOME apps.
		bg = rgb(0xffffff);
		bg_alt = rgb(0xfafafb);
		text = rgb(0x2e2e33);
		text_dim = rgb(0x6e6e73);
		ink = rgb(0x2a2433, 0.92);
		rim = rgb(0xffffff, 0.75);
		line = border == "thin" ? rgb(0x000006, 0.12) : with_alpha(ink, 0.70);
		line_soft = border == "thin" ? rgb(0x000006, 0.07) : with_alpha(ink, 0.35);
		panel_bg = rgb(0xfafafb, 0.92);
		menu_bg = rgb(0xffffff);
		field_bg = rgb(0x000006, 0.06);
		button_hover = rgb(0x000000, 0.07);
		panel_hover = rgb(0x000000, 0.05);
		divider = rgb(0x000006, 0.08);
		shadow = shadow_kind == 2 ? rgb(0x1c1c21, 0.45) : shadow_kind == 3 ? with_alpha(ink, 0.34)
			: rgb(0x000006, 0.12);
		card_bg = rgb(0xffffff);
		tile_off = rgb(0x000006, 0.07);
		button_bg = rgb(0x000000, 0.07);
	} else if (oled) {
		// OLED: true black surfaces (those pixels stay off), with raised
		// cards a hair above black and soft grey lines so edges still read.
		bg = rgb(0x000000);
		bg_alt = rgb(0x000000);
		text = rgb(0xf2f2f5);
		text_dim = rgb(0x9a99a3);
		ink = rgb(0x3a3940, 0.95);   // "ink" lines are light grey here, or they'd vanish
		rim = rgb(0xffffff, 0.05);
		line = border == "thin" ? rgb(0xffffff, 0.16) : with_alpha(ink, 0.95);
		line_soft = border == "thin" ? rgb(0xffffff, 0.09) : with_alpha(ink, 0.55);
		panel_bg = rgb(0x000000);
		menu_bg = rgb(0x000000);
		field_bg = rgb(0xffffff, 0.08);
		button_hover = rgb(0xffffff, 0.11);
		panel_hover = rgb(0xffffff, 0.08);
		divider = rgb(0xffffff, 0.10);
		shadow = rgb(0x000000, 0.60);
		card_bg = rgb(0x121214);
		tile_off = rgb(0xffffff, 0.09);
		button_bg = rgb(0xffffff, 0.10);
	} else {
		// 0.35, calm: warm charcoal (not black), surfaces stepping up gently.
		// 0.35.5: libadwaita's dark greys (window #222226, header and sidebar
		// #2e2e32, popovers #36363a), so Dusk matches GNOME apps beside it.
		bg = rgb(0x2e2e32);       // header bar
		bg_alt = rgb(0x222226);   // window background, unfocused header bar
		text = rgb(0xffffff);
		text_dim = rgb(0xa0a0a4);
		ink = rgb(0x0a0a0c, 0.95);
		rim = rgb(0xffffff, 0.05);
		line = border == "thin" ? rgb(0xffffff, 0.10) : with_alpha(ink, 0.85);
		line_soft = border == "thin" ? rgb(0xffffff, 0.06) : with_alpha(ink, 0.5);
		panel_bg = rgb(0x1d1d20, 0.92);
		menu_bg = rgb(0x36363a);  // popovers
		field_bg = rgb(0xffffff, 0.07);
		button_hover = rgb(0xffffff, 0.09);
		panel_hover = rgb(0xffffff, 0.06);
		divider = rgb(0xffffff, 0.07);
		shadow = shadow_kind == 2 ? rgb(0x000000, 0.50) : rgb(0x000000, shadow_kind == 3 ? 0.50 : 0.28);
		card_bg = rgb(0x38383c);
		tile_off = rgb(0xffffff, 0.08);
		button_bg = rgb(0xffffff, 0.09);
	}
	// Title bars in the window colour (the grey of Dusk Settings' page and of
	// GNOME apps' windows), not the brighter header colour.
	title_active = bg_alt;
	title_inactive = bg_alt;
	title_text = text;
	title_text_inactive = text_dim;
	border_active = accent_border ? accent : line;
	border_inactive = line_soft;
	close_hover = rgb(0xd7263d);
	danger = dark ? rgb(0xe5484d) : rgb(0xd13438);
	warning = dark ? rgb(0xf0b429) : rgb(0xb7791f);
	success = dark ? rgb(0x3fb950) : rgb(0x1a7f37);
	panel_active = with_alpha(accent, dark ? 0.30 : 0.22);
	if (high_contrast) {
		// Accessibility: stronger window outlines and text that keeps its
		// contrast against any wallpaper.
		ink.a = 1.0;
		if (border != "none") {
			line_width = border_width = std::min(3, line_width + 1);
		}
		line = with_alpha(line, 1.0);
		line_soft = with_alpha(line_soft, std::max(0.65, line_soft.a));
		text = dark ? rgb(0xffffff) : rgb(0x000000);
		text_dim = dark ? rgb(0xdadae0) : rgb(0x414147);
	}
}

// The accent colour is chosen in Settings, so it can change at runtime.
inline void set_accent(const Color &c) {
	accent = c;
	panel_active = with_alpha(accent, dark ? 0.30 : 0.22);
	if (accent_border) {
		border_active = accent;
	}
	// White text on light accents is hard to read; use dark text there.
	// Whichever of white or near-black has more contrast on it (WCAG's
	// relative luminance): a light grey accent from a grey wallpaper gets
	// dark text, not white at 2:1.
	auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
	const double L = 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
	on_accent = 1.05 / (L + 0.05) >= (L + 0.05) / 0.0625 ? rgb(0xffffff) : rgb(0x1c1c21);
}

// Fonts, in the family chosen in Settings.
inline std::string font_store[7];
inline const char *font = "Sans 10";
inline const char *font_bold = "Sans Bold 10";
inline const char *font_small = "Sans 8";
inline const char *font_large = "Sans 11";
inline const char *font_title = "Sans Semibold 10";
inline const char *font_huge = "Sans Light 22";
inline const char *font_heading = "Sans Bold 12.5";   // a card's heading (the day, a page title)
// `base`: the size of ordinary text (10 = Dusk's usual; Settings › Fonts ›
// Text size moves it), the others keep their place round it.
inline void set_font(const std::string &family, double base = 10) {
	const std::string f = family.empty() ? "Sans" : family;
	auto pt = [base](double at10) {
		char buf[16];
		std::snprintf(buf, sizeof buf, "%.1f", std::max(6.0, at10 + (base - 10)));
		return std::string(buf);
	};
	// 0.35.1: one small type scale (14 px text, 12 px captions at 100%,
	// as GNOME and Material use), nothing under 12 px: the 10-11 px
	// captions were what read as blurry.
	font_store[0] = f + " " + pt(10.5);
	font_store[1] = f + " Bold " + pt(10.5);
	font_store[2] = f + " " + pt(9);
	font_store[3] = f + " " + pt(11.5);
	font_store[4] = f + " Semibold " + pt(10.5);
	font_store[5] = f + " Light " + pt(22);
	font_store[6] = f + " Bold " + pt(13);
	font = font_store[0].c_str();
	font_bold = font_store[1].c_str();
	font_small = font_store[2].c_str();
	font_large = font_store[3].c_str();
	font_title = font_store[4].c_str();
	font_huge = font_store[5].c_str();
	font_heading = font_store[6].c_str();
}
} // namespace theme

enum class Align { Left, Center, Right };

// A cairo image surface you draw on, then hand to a wlr_scene_buffer.
// Drawing happens in logical pixels; `scale` makes it crisp on HiDPI.
class Canvas {
public:
	Canvas(int width, int height, float scale = 1.0f);
	// Takes over a finished picture (pixel size) shown at width x height.
	Canvas(cairo_surface_t *adopt, int width, int height);
	~Canvas();
	Canvas(const Canvas &) = delete;
	Canvas &operator=(const Canvas &) = delete;

	cairo_t *cr() { return cr_; }
	int width() const { return w_; }
	int height() const { return h_; }

	// Hands the pixels to `node` (the canvas cannot be drawn on afterwards).
	void present(wlr_scene_buffer *node);
	// Same, and the picture stays known to the node after it's on the GPU
	// (the wallpaper: transitions and the overview use it again).
	void present(wlr_scene_buffer *node, bool keep);

private:
	cairo_surface_t *surface_ = nullptr;
	cairo_t *cr_ = nullptr;
	int w_, h_;
};

// Shows `buffer` on `node` and keeps it alive while the node shows it (see draw.cpp).
void set_kept_buffer(wlr_scene_buffer *node, wlr_buffer *buffer);

void set_color(cairo_t *cr, const Color &c);
void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r);
void fill_rounded(cairo_t *cr, double x, double y, double w, double h, double r,
	const Color &c);
// A card: filled rounded rectangle with the theme outline and, for the
// hard shadow style, a comic-style offset shadow.
void ink_card(cairo_t *cr, double x, double y, double w, double h, double r,
	const Color &fill, double shadow_offset);
// A card in the chosen interface style: flat fill, optional screentone,
// a light rim along the top, and the outline. (Shadows are drawn by the
// GPU underneath; see UiLayer.)
void ui_card(cairo_t *cr, double x, double y, double w, double h, double r, const Color &fill);
// Corner radius for something inside a rounded container: outer radius
// minus the gap, so the curves run parallel.
inline double inner_radius(double outer, double gap) {
	return outer - gap > 2 ? outer - gap : 2;
}
// Manga screentone: dots that grow toward (fx, fy), within the box.
void halftone(cairo_t *cr, double x, double y, double w, double h,
	double fx, double fy, double reach, double spacing, const Color &color);

// A row of slanted LED segments along `length` (the cockpit gauge the OSD's
// volume and brightness bars and the panel's workspace pager share): `lit`
// of `count` on, the last one brightest. Without `per_segment`, the lit ones
// use `color` and the rest stay faint; with it, each segment gets its own
// colour in order (bottom to top when vertical).
void led_segments(cairo_t *cr, double x, double y, double length, double thick,
	bool vertical, int count, int lit, const Color &color, bool off = false,
	const Color *per_segment = nullptr);

// Draws `text` inside the box (x, y, w, h), vertically centred and
// ellipsised if it doesn't fit.
void draw_text(cairo_t *cr, const std::string &text, double x, double y,
	double w, double h, const char *font, const Color &color,
	Align align = Align::Left);

// Wrapped text, at most `max_lines` lines (ellipsised after that). Returns
// the height it took. With `draw` false it only measures.
int draw_paragraph(cairo_t *cr, const std::string &text, double x, double y, double w,
	int max_lines, const char *font, const Color &color, bool draw = true);

// Width in logical pixels that `text` would take.
int text_width(const std::string &text, const char *font);

// Placeholder app icon until real icons are added: a coloured circle
// (colour picked from `key`) with the first letter of `label`.
void draw_app_badge(cairo_t *cr, double cx, double cy, double radius,
	const std::string &key, const std::string &label);

void to_float(const Color &c, float out[4]);

// Mixes two colours (t = 0 gives a, 1 gives b).
Color mix(const Color &a, const Color &b, double t);

// Loads a PNG/JPEG/WebP/... image into a cairo surface (nullptr on failure).
// Photos are rotated according to their EXIF orientation.
cairo_surface_t *load_image(const std::string &path);
// Same, scaled to fit width x height (keeps the aspect ratio; SVGs render sharp).
cairo_surface_t *load_image_sized(const std::string &path, int width, int height);
// A picture held in memory (e.g. PNG bytes from the clipboard), scaled down
// to fit max_w x max_h. Its full size goes to *full_w / *full_h if given.
cairo_surface_t *load_image_bytes(const std::string &bytes, int max_w, int max_h,
	int *full_w = nullptr, int *full_h = nullptr);

} // namespace dusk
