#include <algorithm>
#include <cairo.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include "audiolevel.hpp"
#include "ccentre.hpp"
#include "lyrics.hpp"
#include "recorder.hpp"
#include "clipboard.hpp"
#include "desktop.hpp"
#include "shot.hpp"
#include "wallpaper.hpp"
#include "tray.hpp"
#include "overview.hpp"
#include "trash.hpp"
#include "launcher.hpp"
#include "notify.hpp"
#include "osd.hpp"
#include "panel.hpp"
#include "popover.hpp"
#include "system.hpp"
#include "server.hpp"
#include "session.hpp"
#include "crash.hpp"
#include "lock.hpp"
#include "osk.hpp"

namespace {

void print_help() {
	std::puts(
		"Dusk - a floating desktop\n"
		"\n"
		"Usage: dusk [options]\n"
		"  -s, --startup <command>  run a command once Dusk has started\n"
		"  -d, --debug              verbose logging\n"
		"  -h, --help               show this help\n"
		"\n"
		"Dusk also runs ~/.config/dusk/autostart (if it exists and is executable).");
}

} // namespace

// "dusk --scale-png FILE N": enlarges a screenshot N times with sharp
// (blocky, not blurred) pixels, so text stays crisp. Used by screenshots'
// "HD for sharing"; runs without starting the desktop.
static int scale_png(const char *path, int n) {
	n = std::clamp(n, 1, 4);
	cairo_surface_t *src = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(src);
		return 1;
	}
	const int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
	cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * n, h * n);
	cairo_t *cr = cairo_create(dst);
	cairo_scale(cr, n, n);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);
	const std::string tmp = std::string(path) + ".scaled";
	const bool ok = cairo_surface_write_to_png(dst, tmp.c_str()) == CAIRO_STATUS_SUCCESS &&
		std::rename(tmp.c_str(), path) == 0;
	cairo_surface_destroy(dst);
	cairo_surface_destroy(src);
	return ok ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc == 4 && std::string(argv[1]) == "--scale-png") {
		return scale_png(argv[2], std::atoi(argv[3]));
	}
	std::string startup;
	bool debug = false;
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		if ((arg == "-s" || arg == "--startup") && i + 1 < argc) {
			startup = argv[++i];
		} else if (arg == "-d" || arg == "--debug") {
			debug = true;
		} else if (arg == "-h" || arg == "--help") {
			print_help();
			return 0;
		} else {
			std::fprintf(stderr, "Unknown option: %s\n\n", arg.c_str());
			print_help();
			return 1;
		}
	}

	dusk::install_crash_log();
	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, nullptr);

	dusk::Server server;
	if (!server.start()) {
		return 1;
	}

	// As the real desktop (not in a window): the keyring, the admin
	// password prompt and the apps set to start at login.
	if (server.session) {
		dusk::start_keyring(server);
		dusk::start_polkit_agent(server);
		if (server.config.autostart) {
			// A moment after starting: the tray, the portals and the rest are
			// ready by then (apps like Steam look for the tray once, at start).
			wl_event_source *later = wl_event_loop_add_timer(server.loop, [](void *data) {
				dusk::run_autostart(*static_cast<dusk::Server *>(data));
				return 0;
			}, &server);
			wl_event_source_timer_update(later, 1500);
		}
	}
	if (!startup.empty()) {
		server.spawn(startup);
	}
	if (const char *home = getenv("HOME"); home) {
		const char *config = getenv("XDG_CONFIG_HOME");
		std::string autostart = (config && *config) ? std::string(config)
			: std::string(home) + "/.config";
		autostart += "/dusk/autostart";
		if (access(autostart.c_str(), X_OK) == 0) {
			server.spawn(dusk::shell_quote(autostart));
		}
	}

	server.run();
	server.shutdown();
	return 0;
}
