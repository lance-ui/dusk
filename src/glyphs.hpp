// Small icons for the panel, control centre, menus and notifications. Dusk
// draws its own set (Settings > Appearance > Icons); app icons always come
// from the icon theme. With "From the icon theme" the theme's symbolic
// icons are used where it has them.
#pragma once

#include <string>

#include "apps.hpp"
#include "draw.hpp"

namespace dusk {

// Named icons: wifi-0..4, wired, offline, airplane, speaker-0..3, speaker-muted,
// speaker-on, battery-<0..100>, battery-<n>-charging, bluetooth, bluetooth-off,
// moon (dark mode), sun, nightlight, bell, bell-off, coffee, gauge-<profile>, mic,
// mic-off, gear, power, play, pause, next, previous, chevron-right/left/up/down,
// lock, check, close, desktop, display, logout, restart, suspend, refresh, edit,
// screenshot, plus, minus, clipboard, paste, copy, trash, folder, folder-new, file,
// terminal, open, info, image, apps, search, area, window, pointer, timer.
void draw_named_icon(cairo_t *cr, IconLoader &icons, const std::string &name,
	double x, double y, double size, const Color &color, float scale);

// Just the drawn version (no icon theme).
void draw_glyph(cairo_t *cr, const std::string &name, double cx, double cy, double size,
	const Color &color);

// Icon names for the current state of things.
std::string wifi_icon(int signal);              // signal 0..100
std::string speaker_icon(int volume, bool muted);
std::string battery_icon(int percent, bool charging);

} // namespace dusk
