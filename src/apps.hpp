// Installed apps (.desktop files) and their icons from the icon theme.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "draw.hpp"
#include "wlr.hpp"

namespace dusk {

struct AppInfo {
	std::string id;          // desktop file id, e.g. "org.gnome.Nautilus.desktop"
	std::string name;
	std::string exec;        // ready to run with /bin/sh -c
	std::string icon;        // icon name or absolute path
	std::string wm_class;    // StartupWMClass
	std::string comment;
	bool visible = true;     // shown in the app menu
};

class AppDatabase {
public:
	void reload();
	// Reads them again only if an app was added or removed since (the app
	// folders' times changed): opening the app menu re-read every .desktop
	// file each time.
	void reload_if_changed();
	const std::vector<AppInfo> &apps() const { return apps_; }
	// Best-guess icon name for a window's app id (or X11 class).
	std::string icon_for(const std::string &app_id);
	// The same, and when the app id matches nothing installed (games under
	// Wine/Proton: "rocketleague.exe", Steam's "steam_app_252950"), the
	// app whose name matches the window's title or the program's name.
	std::string icon_for(const std::string &app_id, const std::string &title);

private:
	std::vector<AppInfo> apps_;
	std::map<std::string, std::string> icon_cache_;
	std::map<std::string, std::string> title_cache_;
	std::map<std::string, bool> id_found_;   // app id -> a desktop file matched it
	std::string folders_seen_;   // the app folders and their times, at the last reload
	std::string folders_now() const;
};

class IconLoader {
public:
	~IconLoader();
	// A square icon of `px` device pixels, or nullptr if none was found.
	// The loader owns the surface.
	cairo_surface_t *get(const std::string &name, int px);
	// A symbolic (single-colour) icon such as "window-close-symbolic", for
	// use as a mask. nullptr if the theme doesn't have it.
	cairo_surface_t *get_symbolic(const std::string &name, int px);
	void set_theme(const std::string &theme);
	void clear();

private:
	std::string find_file(const std::string &name, bool symbolic, int px) const;
	const std::vector<std::pair<std::string, int>> &theme_dirs(const std::string &root) const;
	mutable std::map<std::string, std::vector<std::pair<std::string, int>>> dirs_;

	std::string theme_ = "Adwaita";
	std::map<std::string, cairo_surface_t *> cache_;
};

// Draws an app icon into the square (x, y, size), falling back to a
// coloured letter badge when the app has no icon.
void draw_icon(cairo_t *cr, IconLoader &icons, const std::string &icon_name,
	const std::string &label, double x, double y, double size, float scale);

// Draws the first symbolic icon of `names` that the theme has, in `color`,
// into the square (x, y, size). Returns false if none was found (the caller
// then draws its own fallback).
bool draw_symbolic(cairo_t *cr, IconLoader &icons, std::initializer_list<const char *> names,
	double x, double y, double size, const Color &color, float scale);

} // namespace dusk
