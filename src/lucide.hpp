// Draws Lucide icons (https://lucide.dev, ISC licence) with cairo, from the
// table tools/lucide2cpp.py makes (lucide_icons.hpp). Used by Dusk and by
// Dusk Settings; it needs only cairo.
#pragma once

#include <cairo.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "lucide_icons.hpp"

namespace lucide {

inline const Icon *find(const char *name) {
	size_t lo = 0, hi = sizeof kIcons / sizeof kIcons[0];
	while (lo < hi) {
		const size_t mid = (lo + hi) / 2;
		const int c = std::strcmp(kIcons[mid].name, name);
		if (c == 0) return &kIcons[mid];
		if (c < 0) lo = mid + 1;
		else hi = mid;
	}
	return nullptr;
}

inline bool exists(const char *name) {
	return find(name) != nullptr;
}

// The icon's outline as the current path (in its 24 x 24 grid), one
// element at a time: `each(filled)` is called after every element.
template <class F>
void trace(cairo_t *cr, const Icon &icon, F each) {
	const char *p = icon.ops;
	auto number = [&p]() {
		char *end = nullptr;
		const double v = std::strtod(p, &end);
		p = end;
		return v;
	};
	while (*p) {
		cairo_new_path(cr);
		bool filled = false;
		while (*p == ' ') p++;
		if (*p == 'F' && p[1] == ' ') {
			filled = true;
			p += 2;
		}
		while (*p && *p != ';') {
			const char op = *p++;
			switch (op) {
			case 'M': { const double x = number(), y = number(); cairo_move_to(cr, x, y); break; }
			case 'L': { const double x = number(), y = number(); cairo_line_to(cr, x, y); break; }
			case 'C': {
				const double x1 = number(), y1 = number(), x2 = number(), y2 = number(), x = number(), y = number();
				cairo_curve_to(cr, x1, y1, x2, y2, x, y);
				break;
			}
			case 'Z': cairo_close_path(cr); break;
			case 'O': {
				const double cx = number(), cy = number(), r = number();
				cairo_new_sub_path(cr);
				cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
				cairo_close_path(cr);
				break;
			}
			case 'E': {
				const double cx = number(), cy = number(), rx = number(), ry = number();
				if (rx > 0 && ry > 0) {
					cairo_save(cr);
					cairo_translate(cr, cx, cy);
					cairo_scale(cr, rx, ry);
					cairo_new_sub_path(cr);
					cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
					cairo_close_path(cr);
					cairo_restore(cr);
				}
				break;
			}
			case 'R': {
				const double x = number(), y = number(), w = number(), h = number();
				double r = number();
				r = std::fmin(r, std::fmin(w, h) / 2);
				cairo_new_sub_path(cr);
				if (r <= 0) {
					cairo_rectangle(cr, x, y, w, h);
				} else {
					cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
					cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
					cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
					cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
					cairo_close_path(cr);
				}
				break;
			}
			default: break;   // spaces
			}
		}
		if (*p == ';') p++;
		each(filled);
	}
}

// Draws `name` into the square (x, y, size) in the cairo source colour set
// by the caller. `line` is the stroke width on the 24 px grid (Lucide's own
// is 2). `fill_all` fills every closed shape too (solid media buttons).
inline bool draw(cairo_t *cr, const char *name, double x, double y, double size, double line = 2.0,
		bool fill_all = false) {
	const Icon *icon = find(name);
	if (!icon) return false;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, size / 24.0, size / 24.0);
	cairo_set_line_width(cr, line);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	trace(cr, *icon, [cr, fill_all](bool filled) {
		if (filled || fill_all) cairo_fill_preserve(cr);
		cairo_stroke(cr);
	});
	cairo_restore(cr);
	return true;
}

} // namespace lucide
