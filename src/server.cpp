// Compositor setup, outputs, input handling and window management.
#include "server.hpp"

#include <wlr/types/wlr_damage_ring.h>
#include "lock.hpp"

#include <algorithm>
#include <filesystem>
#include <array>
#include <fstream>
#include <map>
#include <sstream>
#include <cmath>
#include <csignal>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audiolevel.hpp"
#include "ccentre.hpp"
#include "lyrics.hpp"
#include "recorder.hpp"
#include "clipboard.hpp"
#include "desktop.hpp"
#include "osk.hpp"
#include "overview.hpp"
#include "crash.hpp"
#include "trash.hpp"
#include "draw.hpp"
#include "launcher.hpp"
#include "notify.hpp"
#include "osd.hpp"
#include "panel.hpp"
#include "popover.hpp"
#include "shot.hpp"
#include "wallpaper.hpp"
#include "tray.hpp"
#include "system.hpp"
#include "ui.hpp"

namespace dusk {

namespace {

int handle_terminate_signal(int, void *data) {
	wl_display_terminate(static_cast<wl_display *>(data));
	return 0;
}

// Redraws the clock at the start of every minute.
int clock_tick(void *data) {
	auto *server = static_cast<Server *>(data);
	server->redraw_panels();
	if (server->control_centre) {
		server->control_centre->refresh();
	}
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	wl_event_source_timer_update(server->clock_timer, (60 - local.tm_sec) * 1000 + 50);
	return 0;
}

const char *cursor_for_edges(uint32_t edges) {
	const bool top = edges & WLR_EDGE_TOP, bottom = edges & WLR_EDGE_BOTTOM;
	const bool left = edges & WLR_EDGE_LEFT, right = edges & WLR_EDGE_RIGHT;
	if (top && left) return "nw-resize";
	if (top && right) return "ne-resize";
	if (bottom && left) return "sw-resize";
	if (bottom && right) return "se-resize";
	if (top) return "n-resize";
	if (bottom) return "s-resize";
	if (left) return "w-resize";
	if (right) return "e-resize";
	return "default";
}

} // namespace

// ============================================================================
// Startup and shutdown
// ============================================================================

bool Server::start() {
	// Writing clipboard data to an app that has gone must not end the session.
	signal(SIGPIPE, SIG_IGN);
	display = wl_display_create();
	loop = wl_display_get_event_loop(display);

	backend = wlr_backend_autocreate(loop, &session);
	if (!backend) {
		wlr_log(WLR_ERROR, "Could not create a backend");
		return false;
	}
	// Screens with no display behind them (a phone or tablet used as a
	// second screen, through VNC): made on request, see phone.cpp.
	if (wlr_backend_is_multi(backend)) {
		headless = wlr_headless_backend_create(loop);
		if (headless && !wlr_multi_backend_add(backend, headless)) {
			wlr_backend_destroy(headless);
			headless = nullptr;
		}
	}
	renderer = fx_renderer_create(backend);   // scenefx: rounded corners, shadows
	if (!renderer) {
		wlr_log(WLR_ERROR, "Could not create a renderer");
		return false;
	}
	wlr_renderer_init_wl_shm(renderer, display);
	wlr_linux_dmabuf_v1 *dmabuf = nullptr;
	if (wlr_renderer_get_drm_fd(renderer) >= 0 &&
			wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DMABUF) != nullptr) {
		dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(display, 5, renderer);
	}
	// Explicit sync: apps and XWayland tell the GPU when a picture is really
	// finished. Without it, Chromium browsers (Vivaldi, Chrome), Electron apps
	// and games can flicker or show half-drawn frames, above all on NVIDIA.
	if (wlr_renderer_get_drm_fd(renderer) >= 0 && renderer->features.timeline &&
			backend->features.timeline) {
		wlr_linux_drm_syncobj_manager_v1_create(display, 1, wlr_renderer_get_drm_fd(renderer));
		wlr_log(WLR_INFO, "Explicit sync: on");
	} else {
		wlr_log(WLR_INFO, "Explicit sync: off (renderer %s, backend %s)",
			renderer->features.timeline ? "yes" : "no", backend->features.timeline ? "yes" : "no");
	}
	allocator = wlr_allocator_autocreate(backend, renderer);
	if (!allocator) {
		wlr_log(WLR_ERROR, "Could not create an allocator");
		return false;
	}

	Config::migrate_from_floe();
	config.load();
	write_pending_settings();
	anim = std::make_unique<Animator>(loop);
	anim->wake = [this] { schedule_frames(); };
	apply_theme();
	apps.reload();

	compositor = wlr_compositor_create(display, 6, renderer);
	// Games may ask to show frames the moment they're ready (no vsync: less
	// input lag), as on KDE. Only for a focused full-screen window.
	tearing_control = wlr_tearing_control_manager_v1_create(display, 1);
	wlr_subcompositor_create(display);
	// Smooth video and games (frame timing), small things browsers and
	// toolkits use, dialogs over the right window (portals), and windows
	// asking to be brought forward (xdg-activation).
	wlr_presentation_create(display, backend, 2);
	wlr_single_pixel_buffer_manager_v1_create(display);
	wlr_alpha_modifier_v1_create(display);
	{
		wlr_xdg_foreign_registry *foreign = wlr_xdg_foreign_registry_create(display);
		wlr_xdg_foreign_v1_create(display, foreign);
		wlr_xdg_foreign_v2_create(display, foreign);
	}
	activation = wlr_xdg_activation_v1_create(display);
	on_request_activate.connect<&Server::handle_request_activate>(&activation->events.request_activate, this);
	// Games: the pointer locked or kept inside the window, and raw mouse movement.
	pointer_constraints = wlr_pointer_constraints_v1_create(display);
	on_new_constraint.connect<&Server::handle_new_constraint>(&pointer_constraints->events.new_constraint, this);
	relative_pointer = wlr_relative_pointer_manager_v1_create(display);
	// Games, virtual machines and remote desktops can take every key (Super too).
	shortcuts_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(display);
	on_new_inhibitor.connect<&Server::handle_new_inhibitor>(&shortcuts_inhibit->events.new_inhibitor, this);
	wlr_data_device_manager_create(display);
	wlr_primary_selection_v1_device_manager_create(display);
	wlr_viewporter_create(display);
	wlr_fractional_scale_manager_v1_create(display, 1);   // sharp text at 125%, 150%...

	layout = wlr_output_layout_create(display);
	on_layout_change.connect<&Server::handle_layout_change>(&layout->events.change, this);
	wlr_xdg_output_manager_v1_create(display, layout);
	on_new_output.connect<&Server::handle_new_output>(&backend->events.new_output, this);

	scene = wlr_scene_create();
	scene_layout = wlr_scene_attach_output_layout(scene, layout);
	if (dmabuf) {
		wlr_scene_set_linux_dmabuf_v1(scene, dmabuf);   // lets apps' pictures go straight to the screen
	}
	layer_background = wlr_scene_tree_create(&scene->tree);
	layer_desktop = wlr_scene_tree_create(&scene->tree);
	layer_panel_shadow = wlr_scene_tree_create(&scene->tree);
	layer_views = wlr_scene_tree_create(&scene->tree);
	layer_overview = wlr_scene_tree_create(&scene->tree);
	layer_panel = wlr_scene_tree_create(&scene->tree);
	layer_fullscreen = wlr_scene_tree_create(&scene->tree);
	layer_unmanaged = wlr_scene_tree_create(&scene->tree);
	layer_menu = wlr_scene_tree_create(&scene->tree);
	layer_notify = wlr_scene_tree_create(&scene->tree);
	layer_overlay = wlr_scene_tree_create(&scene->tree);
	layer_lock = wlr_scene_tree_create(&scene->tree);

	xdg_shell = wlr_xdg_shell_create(display, 3);
	on_new_toplevel.connect<&Server::handle_new_toplevel>(
		&xdg_shell->events.new_toplevel, this);

	decoration_manager = wlr_xdg_decoration_manager_v1_create(display);
	on_new_decoration.connect<&Server::handle_new_decoration>(
		&decoration_manager->events.new_toplevel_decoration, this);

	// Lets screenshot tools (grim) capture the screen, and clipboard tools
	// (wl-clipboard, clipboard managers) work.
	wlr_screencopy_manager_v1_create(display);
	// Keyboards and pointers from programs: remote desktop (wayvnc: a phone
	// or tablet controlling Dusk), on-screen keyboards.
	virtual_keyboards = wlr_virtual_keyboard_manager_v1_create(display);
	on_virtual_keyboard.connect<&Server::handle_virtual_keyboard>(&virtual_keyboards->events.new_virtual_keyboard, this);
	virtual_pointers = wlr_virtual_pointer_manager_v1_create(display);
	on_virtual_pointer.connect<&Server::handle_virtual_pointer>(&virtual_pointers->events.new_virtual_pointer, this);
	wlr_data_control_manager_v1_create(display);

	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, layout);
	load_cursor_theme();

	// Lets apps ask Dusk to draw a named cursor ("text", "pointer"...) from
	// Dusk's theme, so the cursor looks the same in every app.
	wlr_cursor_shape_manager_v1 *shapes = wlr_cursor_shape_manager_v1_create(display, 1);
	on_request_cursor_shape.connect<&Server::handle_request_cursor_shape>(
		&shapes->events.request_set_shape, this);
	on_cursor_motion.connect<&Server::handle_cursor_motion>(&cursor->events.motion, this);
	on_cursor_motion_absolute.connect<&Server::handle_cursor_motion_absolute>(
		&cursor->events.motion_absolute, this);
	on_cursor_button.connect<&Server::handle_cursor_button>(&cursor->events.button, this);
	on_cursor_axis.connect<&Server::handle_cursor_axis>(&cursor->events.axis, this);
	on_cursor_frame.connect<&Server::handle_cursor_frame>(&cursor->events.frame, this);
	setup_gestures();

	on_new_input.connect<&Server::handle_new_input>(&backend->events.new_input, this);
	seat = wlr_seat_create(display, "seat0");
	on_request_cursor.connect<&Server::handle_request_cursor>(
		&seat->events.request_set_cursor, this);
	on_request_selection.connect<&Server::handle_request_selection>(
		&seat->events.request_set_selection, this);
	on_request_primary_selection.connect<&Server::handle_request_primary_selection>(
		&seat->events.request_set_primary_selection, this);
	on_request_start_drag.connect<&Server::handle_request_start_drag>(
		&seat->events.request_start_drag, this);
	on_start_drag.connect<&Server::handle_start_drag>(&seat->events.start_drag, this);

	launcher = std::make_unique<Launcher>(this);
	osd = std::make_unique<Osd>(this);
	images = std::make_unique<ImageLoader>(loop);
	status = std::make_unique<SystemStatus>(this);
	control_centre = std::make_unique<ControlCentre>(this);
	calendar = std::make_unique<Calendar>(this);
	media_card = std::make_unique<MediaCard>(this);
	menu = std::make_unique<ContextMenu>(this);
	snap_layouts = std::make_unique<SnapLayouts>(this);
	clipboard = std::make_unique<Clipboard>(this);
	clipboard_card = std::make_unique<ClipboardCard>(this);
	osk = std::make_unique<Osk>(this);
	clipboard->changed = [this] { clipboard_card->update(); };
	trash = std::make_unique<Trash>(this);
	desktop = std::make_unique<Desktop>(this);
	overview = std::make_unique<Overview>(this);
	shot_tool = std::make_unique<ScreenshotTool>(this);
	audio_level = std::make_unique<AudioLevel>(this);
	lyrics = std::make_unique<Lyrics>(this);
	recorder = std::make_unique<Recorder>(this);
	lock = std::make_unique<LockScreen>(this);
	wall_worker = std::make_unique<WallpaperWorker>(this);
	wall_worker->done = [this](WallpaperWorker::Result &r) {
		for (Output *o : outputs) {
			if (o->name() == r.job.output && o->wall_generation == r.job.generation && o->enabled()) {
				Canvas canvas(r.surface, r.job.width, r.job.height);
				r.surface = nullptr;   // the canvas has it
				o->show_wallpaper(canvas, o->wall_wanted);
				if (lock && lock->active()) lock->refresh();   // the (login) screen shows it too
				// "Accent from wallpaper": the new accent comes after the
				// transition, so apps restyling don't make it stutter.
				if (r.has_accent && config.accent_auto && r.job.path == config.wallpaper) {
					struct stat st{};
					const std::string id = config.wallpaper + "|" +
						(stat(config.wallpaper.c_str(), &st) == 0 ? std::to_string(st.st_mtime) : "");
					if (id != auto_accent_for) {
						auto_accent_for = id;
						auto_accent = r.accent;
						if (!accent_timer) {
							accent_timer = wl_event_loop_add_timer(loop, [](void *data) {
								static_cast<Server *>(data)->reload_config();
								return 0;
							}, this);
						}
						// Dusk's own accent changes at once; other apps restyle
						// after the transition (see apply_config).
						wl_event_source_timer_update(accent_timer, 1);
					}
				}
				return;
			}
		}
	};
	max_hover_timer = wl_event_loop_add_timer(loop, handle_max_hover, this);
	drop_timer = wl_event_loop_add_timer(loop, handle_drop_timer, this);
	status->changed = [this] {
		if (lock) lock->status_changed();   // music and battery on the lock screen
		redraw_panels();
		control_centre->refresh();
		media_card->refresh();
		if (lyrics) lyrics->media_changed();
	};

#ifdef DUSK_XWAYLAND
	start_xwayland();
#endif

	const char *sock = wl_display_add_socket_auto(display);
	if (!sock) {
		wlr_log(WLR_ERROR, "Could not create a Wayland socket");
		return false;
	}
	socket = sock;

	if (!wlr_backend_start(backend)) {
		wlr_log(WLR_ERROR, "Could not start the backend");
		return false;
	}

	setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
	// "GNOME" second: apps that pick their password store or settings by
	// desktop (Chrome, Electron apps, VS Code, Qt) then use the GNOME keyring
	// and GNOME's settings, which Dusk keeps up to date. Portals and Dusk's
	// own autostart match "Dusk" first.
	setenv("XDG_CURRENT_DESKTOP", "Dusk:GNOME", 1);
	setenv("XDG_SESSION_DESKTOP", "Dusk", 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("MOZ_ENABLE_WAYLAND", "1", 1);
	setenv("QT_QPA_PLATFORM", "wayland;xcb", 1);
	// Qt apps take light/dark (and fonts, icons) from GTK's settings,
	// which Dusk keeps in step. Needs qt6-qtbase-gtk; Qt falls back
	// quietly without it.
	// With "Style other apps" on and qt6ct installed, Qt apps get Dusk's
	// colours from the qt6ct scheme dusk-apply-theme writes; otherwise
	// they follow GTK's settings.
	const bool qt6ct = access("/usr/bin/qt6ct", X_OK) == 0;
	if (config.style_apps && qt6ct) {
		setenv("QT_QPA_PLATFORMTHEME", "qt6ct", 0);
	} else {
		for (const char *dir : {"/usr/lib64/qt6/plugins", "/usr/lib/qt6/plugins"}) {
			if (access((std::string(dir) + "/platformthemes/libqgtk3.so").c_str(), R_OK) == 0) {
				setenv("QT_QPA_PLATFORMTHEME", "gtk3", 0);
				break;
			}
		}
	}
	// Installed GNOME (libadwaita) apps read light/dark and the accent
	// straight from GNOME's settings, so they follow Dusk even when the
	// desktop portal doesn't pass it on (an idea from Arqen shell).
	// Flatpak apps still use the portal.
	setenv("ADW_DISABLE_PORTAL", "1", 1);
	setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 1);

	// When running as the real desktop (not nested in a window), tell
	// D-Bus and systemd where apps they start should show up.
	if (session) {
		// Portals already running (started before we set the environment)
		// would not know this is Dusk; restart them so they pick up
		// dusk-portals.conf.
		spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY DISPLAY "
			"XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE QT_QPA_PLATFORMTHEME "
			"ADW_DISABLE_PORTAL XCURSOR_THEME XCURSOR_SIZE XCURSOR_PATH "
			"2>/dev/null; "
			// A graphical session is now running (the portal waits for that).
			"systemctl --user start dusk-session.target 2>/dev/null || "
			"{ systemctl --user daemon-reload; systemctl --user start dusk-session.target; }; "
			"systemctl --user try-restart xdg-desktop-portal.service "
			"xdg-desktop-portal-gtk.service 2>/dev/null");
	}

	// After the environment is set: D-Bus needs to know about us first.
	notifications = std::make_unique<Notifications>(this);
	notifications->changed = [this] {
		control_centre->refresh();
		redraw_panels();
		if (lock) lock->notifications_changed();
	};
	// Just updated: a note, once, with what's new (from the installer's list).
	{
		static wl_event_source *news_timer = nullptr;
		news_timer = wl_event_loop_add_timer(loop, [](void *data) {
			auto *srv = static_cast<Server *>(data);
			wl_event_source_remove(news_timer);
			news_timer = nullptr;
#ifdef DUSK_VERSION
			const std::string version = DUSK_VERSION;
#else
			const std::string version;
#endif
			const char *home = std::getenv("HOME");
			if (!home || version.empty() || !srv->notifications) return 0;
			const std::string seen_path = std::string(home) + "/.local/state/dusk/seen-version";
			std::string seen;
			if (FILE *f = std::fopen(seen_path.c_str(), "r")) {
				char buf[64] = {};
				if (std::fgets(buf, sizeof buf, f)) seen = buf;
				std::fclose(f);
				while (!seen.empty() && std::isspace(static_cast<unsigned char>(seen.back()))) seen.pop_back();
			}
			if (seen == version) return 0;
			std::vector<std::string> heads;
			if (FILE *f = std::fopen("/usr/local/share/dusk/whatsnew.txt", "r")) {
				char line[512];
				bool ours = false;
				while (std::fgets(line, sizeof line, f)) {
					std::string l = line;
					while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
					const size_t tab = l.find('\t');
					if (tab == std::string::npos) continue;
					if (heads.empty() && !ours) ours = l.substr(0, tab) == version;   // (the list is this version's)
					if (ours) heads.push_back(l.substr(tab + 1));
				}
				std::fclose(f);
			}
			if (!heads.empty()) {
				std::string body;
				for (size_t i = 0; i < heads.size() && i < 4; i++) body += (i ? "\n• " : "• ") + heads[i];
				if (heads.size() > 4) body += "\n…and " + std::to_string(heads.size() - 4) + " more";
				srv->notifications->post("Dusk was updated to " + version, body, "dusk", 1, 0,
					{{"default", "What's new"}}, [srv](const std::string &) { srv->open_settings("about"); });
			}
			std::filesystem::create_directories(std::string(home) + "/.local/state/dusk");
			if (FILE *f = std::fopen(seen_path.c_str(), "w")) {
				std::fputs(version.c_str(), f);
				std::fclose(f);
			}
			return 0;
		}, this);
		wl_event_source_timer_update(news_timer, 5000);
	}
	notifications->history_changed = [this] {
		// Arrived while the control centre is open: seen already.
		if (control_centre && control_centre->is_open() && (!config.separate_notes() || control_centre->notes_open())) notifications->unseen = 0;
		if (control_centre) control_centre->refresh();
		redraw_panels();   // the bell on the panel
	};
	tray = std::make_unique<Tray>(this);
	tray->changed = [this] {
		const size_t n = tray->items().size();
		if (n != tray_count) {
			tray_count = n;
			arrange();   // the panels make room for icons that came or went
		}
		redraw_panels();
	};

	apply_config();   // wallpaper image etc.
	watch_config();

	wl_event_loop_add_signal(loop, SIGTERM, handle_terminate_signal, display);
	wl_event_loop_add_signal(loop, SIGINT, handle_terminate_signal, display);

	clock_timer = wl_event_loop_add_timer(loop, clock_tick, this);
	clock_tick(this);
	start_idle();

	wlr_log(WLR_INFO, "Dusk is running on WAYLAND_DISPLAY=%s", socket.c_str());
	return true;
}

void Server::run() {
	wl_display_run(display);
}

void Server::shutdown() {
	if (session) {
		// The graphical session is over: services bound to it stop.
		// And take back what we told the user session, so a different desktop
		// logged into next isn't told it's Dusk (dusk-session does this too,
		// also after a crash).
		if (std::system("systemctl --user unset-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP "
				"XDG_SESSION_DESKTOP XDG_SESSION_TYPE QT_QPA_PLATFORMTHEME ADW_DISABLE_PORTAL >/dev/null 2>&1") != 0) {
			wlr_log(WLR_DEBUG, "Could not reset the session environment");
		}
		if (std::system("systemctl --user stop dusk-session.target >/dev/null 2>&1") != 0) {
			wlr_log(WLR_DEBUG, "Could not stop dusk-session.target");
		}
	}
	wl_display_destroy_clients(display);
#ifdef DUSK_XWAYLAND
	if (xwayland) {
		on_xwayland_ready.disconnect();
		on_xwayland_new_surface.disconnect();
		wlr_xwayland_destroy(xwayland);
		xwayland = nullptr;
	}
#endif
	shutting_down = true;
	if (config_watch) {
		wl_event_source_remove(config_watch);
		config_watch = nullptr;
	}
	if (inotify_fd >= 0) {
		::close(inotify_fd);
		inotify_fd = -1;
	}

	// Stop listening before the objects we listen to go away.
	on_new_output.disconnect();
	on_new_input.disconnect();
	on_layout_change.disconnect();
	on_new_toplevel.disconnect();
	on_new_decoration.disconnect();
	on_cursor_motion.disconnect();
	on_cursor_motion_absolute.disconnect();
	on_cursor_button.disconnect();
	on_cursor_axis.disconnect();
	on_cursor_frame.disconnect();
	on_request_cursor.disconnect();
	on_request_selection.disconnect();
	on_request_primary_selection.disconnect();
	on_request_cursor_shape.disconnect();
	// The seat and virtual-input listeners too: these signals are freed with
	// the display, and ~Listener would otherwise unlink from freed memory.
	on_virtual_keyboard.disconnect();
	on_virtual_pointer.disconnect();
	on_request_start_drag.disconnect();
	on_start_drag.disconnect();

	stop_idle();
	stop_phone_screen();
	if (accent_timer) {
		wl_event_source_remove(accent_timer);
		accent_timer = nullptr;
	}
	if (theme_timer) {
		wl_event_source_remove(theme_timer);
		theme_timer = nullptr;
	}
	if (clock_timer) {
		wl_event_source_remove(clock_timer);
		clock_timer = nullptr;
	}

	// Our scene nodes must go before the scene itself.
	notifications.reset();
	control_centre.reset();
	if (max_hover_timer) {
		wl_event_source_remove(max_hover_timer);
		max_hover_timer = nullptr;
	}
	if (drop_timer) {
		wl_event_source_remove(drop_timer);
		drop_timer = nullptr;
	}
	drop_finish = nullptr;
	snap_layouts.reset();
	lock.reset();
	recorder.reset();
	lyrics.reset();
	shot_tool.reset();
	wall_worker.reset();
	tray.reset();
	overview.reset();
	desktop.reset();
	trash.reset();
	on_request_activate.disconnect();
	on_new_constraint.disconnect();
	on_constraint_destroy.disconnect();
	on_new_inhibitor.disconnect();
	for (Listener *l : {&on_swipe_begin, &on_swipe_update, &on_swipe_end, &on_pinch_begin, &on_pinch_update,
			&on_pinch_end, &on_hold_begin, &on_hold_end}) {
		l->disconnect();
	}
	clipboard_card.reset();
	clipboard.reset();
	menu.reset();
	calendar.reset();
	media_card.reset();
	audio_level.reset();
	status.reset();
	launcher.reset();
	osd.reset();
	for (Output *output : outputs) {
		output->panels.clear();
		// The scene frees its outputs with it: let go of ours first, or the
		// output's own teardown (when the backend goes) frees it a second time.
		if (output->scene_output) {
			wlr_scene_output_destroy(output->scene_output);
			output->scene_output = nullptr;
		}
		output->finish_wallpaper_transition();
		if (output->wallpaper) {
			wlr_scene_node_destroy(&output->wallpaper->node);
			output->wallpaper = nullptr;
		}
		for (auto *&corner : output->corners) {
			if (corner) {
				wlr_scene_node_destroy(&corner->node);
				corner = nullptr;
			}
		}
	}
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_xcursor_manager_destroy(cursor_manager);
	wlr_cursor_destroy(cursor);
	wlr_allocator_destroy(allocator);
	wlr_renderer_destroy(renderer);
	wlr_backend_destroy(backend);   // also destroys outputs and keyboards
	anim.reset();
	wl_display_destroy(display);
}

// ============================================================================
// Settings (~/.config/dusk/dusk.conf), re-read whenever the file changes
// ============================================================================

namespace {

int handle_config_event(int fd, uint32_t, void *data) {
	auto *server = static_cast<Server *>(data);
	alignas(inotify_event) char buffer[4096];
	bool changed = false;
	for (;;) {
		const ssize_t len = read(fd, buffer, sizeof buffer);
		if (len <= 0) {
			break;
		}
		for (ssize_t i = 0; i < len;) {
			auto *event = reinterpret_cast<inotify_event *>(buffer + i);
			if (event->len > 0 && std::strcmp(event->name, "dusk.conf") == 0) {
				changed = true;
			}
			i += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
		}
	}
	if (changed) {
		server->reload_config();
	}
	return 0;
}

} // namespace

void Server::watch_config() {
	const std::string dir = Config::directory();
	// mkdir -p
	for (size_t pos = dir.find('/', 1);; pos = dir.find('/', pos + 1)) {
		mkdir(dir.substr(0, pos).c_str(), 0755);
		if (pos == std::string::npos) {
			break;
		}
	}

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd < 0) {
		wlr_log(WLR_ERROR, "Can't watch the settings file; changes apply after restart");
		return;
	}
	// The settings app replaces the file (write + rename), so watch the folder.
	if (inotify_add_watch(inotify_fd, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
		wlr_log(WLR_ERROR, "Can't watch %s", dir.c_str());
		::close(inotify_fd);
		inotify_fd = -1;
		return;
	}
	config_watch = wl_event_loop_add_fd(loop, inotify_fd, WL_EVENT_READABLE,
		handle_config_event, this);
}

void Server::reload_config() {
	// Dusk writes a setting and reloads straight away; the file watcher
	// then reports the same change again. Same text: nothing to do.
	std::string text;
	{
		std::ifstream f(Config::file_path());
		if (!f.is_open()) {
			// A missing or unreadable file (an editor moving it aside for a
			// moment, a read error) must not wipe the desktop's settings.
			// Keep what we have until the file really comes back.
			wlr_log(WLR_INFO, "Settings file unreadable, keeping current settings");
			return;
		}
		std::stringstream ss;
		ss << f.rdbuf();
		text = ss.str();
	}
	if (text.empty()) {
		// An empty file carries no settings either: same protection.
		wlr_log(WLR_INFO, "Settings file is empty, keeping current settings");
		return;
	}
	if (text == last_config_text) {
		return;
	}
	last_config_text = text;
	wlr_log(WLR_INFO, "Settings changed, applying");
	config.load();
	write_pending_settings();
	apply_config();
}

namespace {

// Reads a GNOME desktop setting (empty if unset).
std::string ask_gsettings_a(const char *key) {
	const std::string command =
		std::string("gsettings get org.gnome.desktop.interface ") + key + " 2>/dev/null";
	std::string out;
	if (FILE *pipe = popen(command.c_str(), "r")) {
		char buffer[256];
		while (fgets(buffer, sizeof buffer, pipe)) {
			out += buffer;
		}
		pclose(pipe);
	}
	std::string clean;
	for (char c : out) {
		if (c != '\'' && c != '\n') {
			clean += c;
		}
	}
	return clean;
}

std::string gnome_setting(const char *key) {
	// Asking gsettings starts a program and waits for it: remember answers
	// for a minute so settings changes stay quick.
	static std::map<std::string, std::pair<std::string, double>> cache;
	const double now = now_ms();
	if (auto it = cache.find(key); it != cache.end() && now - it->second.second < 60000) {
		return it->second.first;
	}
	std::string value = ask_gsettings_a(key);
	cache[key] = {value, now};
	return value;
}

// GNOME's accent colours (libadwaita apps use one of these): the one
// nearest Dusk's accent.
std::string gnome_accent(const Color &c) {
	struct Named { const char *name; uint32_t rgb; };
	static const Named kAccents[] = {
		{"blue", 0x3584e4}, {"teal", 0x2190a4}, {"green", 0x3a944a}, {"yellow", 0xc88800},
		{"orange", 0xed5b00}, {"red", 0xe62d42}, {"pink", 0xd56199}, {"purple", 0x9141ac},
		{"slate", 0x6f8396},
	};
	const char *best = "blue";
	double best_d = 1e9;
	for (const Named &n : kAccents) {
		const double r = ((n.rgb >> 16) & 0xff) / 255.0 - c.r;
		const double g = ((n.rgb >> 8) & 0xff) / 255.0 - c.g;
		const double b = (n.rgb & 0xff) / 255.0 - c.b;
		const double d = 2 * r * r + 4 * g * g + 3 * b * b;
		if (d < best_d) {
			best_d = d;
			best = n.name;
		}
	}
	return best;
}

} // namespace

void Server::apply_theme() {
	theme::high_contrast = config.high_contrast;
	theme::set_theme(config.dark, config.border, config.shadow, config.titlebar_height,
		config.accent_border, config.oled);
	theme::own_icons = config.own_icons;
	theme::set_accent(config.accent);
	theme::buttons_left = config.buttons_side == "left";
	theme::title_left = config.title_align == "left";
	theme::show_titles = config.show_titles;
	theme::set_ui(config.ui_outline, config.ui_shadow, config.ui_texture, config.ui_radius);
	// See-through and blurred: menus, cards, notifications, the OSD.
	theme::ui_opacity = config.ui_opacity;
	theme::ui_blur = config.ui_blur;
	theme::menu_bg.a = config.ui_opacity;
	if (scene) {
		wlr_scene_set_blur_data(scene, 3, 6, 0.02f, theme::dark ? 0.9f : 1.04f, 0.9f, 1.15f);
	}
	theme::set_font(config.font, config.font_size - 1);
	set_curve(config.anim_curve);
	Fades::enabled = config.animate(config.anim_hover);
	theme::button_gtk = config.button_style == "gtk";
	theme::button_sketch = config.button_style == "sketch";
	theme::show_minimize = config.show_minimize;
	theme::show_maximize = config.show_maximize;
}

// Apps that draw their own title bar get the same buttons as Dusk's, on the
// same side: GNOME/GTK/libadwaita apps read GNOME's button layout, KDE apps
// (and Qt apps using KDE's decorations) read kwinrc.
void Server::sync_app_buttons() {
	if (!config.apps_follow_buttons) {
		applied_buttons.clear();
		return;
	}
	// From the screen edge inwards, like Dusk's own title bar.
	std::vector<std::string> gnome = {"close"};
	std::string kde = "X";
	if (config.show_maximize) {
		gnome.push_back("maximize");
		kde += "A";
	}
	if (config.show_minimize) {
		gnome.push_back("minimize");
		kde += "I";
	}
	const bool left = config.buttons_side == "left";
	if (!left) {
		std::reverse(gnome.begin(), gnome.end());
		std::reverse(kde.begin(), kde.end());
	}
	std::string list;
	for (const std::string &b : gnome) {
		list += (list.empty() ? "" : ",") + b;
	}
	const std::string layout = left ? list + ":appmenu" : "appmenu:" + list;
	if (layout == applied_buttons) {
		return;
	}
	applied_buttons = layout;
	const std::string on_left = left ? kde : "", on_right = left ? "" : kde;
	spawn("command -v gsettings >/dev/null && [ \"$(gsettings get org.gnome.desktop.wm.preferences "
		"button-layout 2>/dev/null)\" != \"'" + layout + "'\" ] && gsettings set "
		"org.gnome.desktop.wm.preferences button-layout '" + layout + "' 2>/dev/null; "
		"for k in kwriteconfig6 kwriteconfig5; do command -v $k >/dev/null || continue; "
		"$k --file kwinrc --group org.kde.kdecoration2 --key ButtonsOnLeft '" + on_left + "'; "
		"$k --file kwinrc --group org.kde.kdecoration2 --key ButtonsOnRight '" + on_right + "'; "
		"break; done");
}

// The wallpaper's most colourful common colour, made bright enough to be an
// accent (the same as Settings' "From wallpaper"). Grey pictures give a
// silver grey. Runs on the wallpaper worker thread: the picture is already
// decoded there, and Dusk doesn't stall.
bool accent_from_surface(cairo_surface_t *src, Color &out) {
	if (!src || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
		return false;
	}
	const int sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src);
	if (sw <= 0 || sh <= 0) {
		return false;
	}
	const double k = std::min(1.0, 80.0 / std::max(sw, sh));
	const int tw = std::max(1, static_cast<int>(sw * k)), th = std::max(1, static_cast<int>(sh * k));
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
	{
		cairo_t *c = cairo_create(img);
		cairo_scale(c, k, k);
		cairo_set_source_surface(c, src, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_FAST);
		cairo_paint(c);
		cairo_destroy(c);
	}
	cairo_surface_flush(img);
	const int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
	const int stride = cairo_image_surface_get_stride(img);
	const unsigned char *data = cairo_image_surface_get_data(img);
	double weight[36] = {}, rs[36] = {}, gs[36] = {}, bs[36] = {};
	for (int y = 0; data && y < h; y++) {
		const auto *row = reinterpret_cast<const uint32_t *>(data + y * stride);
		for (int x = 0; x < w; x++) {
			const uint32_t px = row[x];
			const double a = ((px >> 24) & 0xff) / 255.0;
			if (a < 0.5) continue;
			const double r = ((px >> 16) & 0xff) / 255.0 / a, g = ((px >> 8) & 0xff) / 255.0 / a,
				b = (px & 0xff) / 255.0 / a;
			const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
			const double val = mx, sat = mx > 0 ? (mx - mn) / mx : 0;
			if (sat < 0.18 || val < 0.18) continue;
			double hue;
			if (mx == r) hue = std::fmod((g - b) / (mx - mn) + 6, 6);
			else if (mx == g) hue = (b - r) / (mx - mn) + 2;
			else hue = (r - g) / (mx - mn) + 4;
			const int bucket = std::clamp(static_cast<int>(hue / 6 * 36), 0, 35);
			const double wt = sat * sat * val;
			weight[bucket] += wt;
			rs[bucket] += r * wt;
			gs[bucket] += g * wt;
			bs[bucket] += b * wt;
		}
	}
	cairo_surface_destroy(img);
	auto score = [&](int i) { return weight[i] + 0.5 * (weight[(i + 35) % 36] + weight[(i + 1) % 36]); };
	int best = -1;
	for (int i = 0; i < 36; i++) {
		if (weight[i] > 0 && (best < 0 || score(i) > score(best))) best = i;
	}
	if (best < 0) {
		out = rgb(0x8e9399);
		return true;
	}
	double r = rs[best] / weight[best], g = gs[best] / weight[best], b = bs[best] / weight[best];
	// Saturation and brightness into a middle band: readable on light and dark.
	const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
	const double sat = mx > 0 ? (mx - mn) / mx : 0;
	const double want_v = std::clamp(mx, 0.62, 0.90), want_s = std::clamp(sat, 0.45, 0.85);
	auto fix = [&](double c) {
		// Same hue: rescale the distance from the top channel.
		const double rel = mx > mn ? (mx - c) / (mx - mn) : 0;   // 0 = top channel, 1 = bottom
		return want_v * (1 - want_s * rel);
	};
	out = Color{fix(r), fix(g), fix(b), 1.0};
	return true;
}

// "From wallpaper" stays on: a new wallpaper gives a new accent.
void Server::resolve_accent() {
	if (!config.accent_auto || config.wallpaper_type != "image" || config.wallpaper.empty()) {
		return;
	}
	struct stat st{};
	const std::string id = config.wallpaper + "|" +
		(stat(config.wallpaper.c_str(), &st) == 0 ? std::to_string(st.st_mtime) : "");
	if (id != auto_accent_for) {
		// Settings already picked it (from its thumbnail, at once): use that.
		if (config.accent_for == config.wallpaper) {
			auto_accent_for = id;
			auto_accent = config.accent;
			written_auto_accent.clear();
		} else {
			// Not worked out yet: the wallpaper worker does it while it reads
			// the picture.
			return;
		}
	}
	config.accent = auto_accent;
	// Into the file too, so Settings shows it (only when it changed).
	char hex[16];
	std::snprintf(hex, sizeof hex, "#%02x%02x%02x", static_cast<int>(std::lround(auto_accent.r * 255)),
		static_cast<int>(std::lround(auto_accent.g * 255)), static_cast<int>(std::lround(auto_accent.b * 255)));
	if (hex != written_auto_accent) {
		std::string now;
		{
			std::ifstream f(Config::file_path());
			std::string line;
			while (std::getline(f, line)) {
				const auto eq = line.find('=');
				if (eq == std::string::npos) continue;
				std::string k = line.substr(0, eq);
				k.erase(k.find_last_not_of(" \t") + 1);
				k.erase(0, k.find_first_not_of(" \t"));
				if (k == "accent") {
					now = line.substr(eq + 1);
					now.erase(0, now.find_first_not_of(" \t"));
					now.erase(now.find_last_not_of(" \t\r") + 1);
				}
			}
		}
		written_auto_accent = hex;
		if (now != hex) {
			Config::write_key("accent", hex);
		}
	}
}

void Server::write_pending_settings() {
	for (const auto &[key, value] : config.pending_writes) {
		Config::write_key(key, value);
	}
	config.pending_writes.clear();
}

void Server::write_setting(const std::string &key, const std::string &value) {
	Config::write_key(key, value);
	// The file watcher would get there too; this makes the change instant.
	reload_config();
}

void Server::apply_config() {
	crumb("settings applied");
	resolve_accent();
	if (lock) lock->refresh();   // its look follows the settings too
	const Config &c = config;
	auto hex = [](const Color &col) {
		char buf[32];
		std::snprintf(buf, sizeof buf, "%.3f,%.3f,%.3f", col.r, col.g, col.b);
		return std::string(buf);
	};
	// Groups of settings: only the groups that changed are redone.
	const std::string look = std::to_string(c.dark) + std::to_string(c.oled) + std::to_string(c.own_icons) +
		hex(c.accent) + c.border + c.shadow +
		std::to_string(c.accent_border) + std::to_string(c.window_radius) +
		std::to_string(c.titlebar_height) + c.title_align + std::to_string(c.show_titles) + c.buttons_side + c.button_style +
		std::to_string(c.show_minimize) + std::to_string(c.show_maximize) + c.font + std::to_string(c.font_size) + c.ui_outline +
		c.ui_shadow + std::to_string(c.ui_radius) + std::to_string(c.ui_opacity) + std::to_string(c.ui_blur) + std::to_string(c.panel_blur) +
		std::to_string(c.settings_opacity) + std::to_string(c.settings_blur) + std::to_string(c.csd_radius) +
		std::to_string(c.screen_corners);
	const bool look_changed = look != applied_look;
	applied_look = look;
	// (A picture doesn't change with the accent or light/dark: redoing it
	// would re-read the file and re-upload it for nothing.)
	const bool picture = c.wallpaper_type == "image";
	const std::string wall = c.wallpaper_type + "|" + c.wallpaper + "|" + c.wallpaper_mode + "|" +
		(picture ? std::string() : hex(c.wallpaper_color) + std::to_string(c.dark) + std::to_string(c.oled) + hex(c.accent));
	const bool wall_changed = wall != applied_wallpaper;
	applied_wallpaper = wall;
	const std::string kb = c.keyboard_layout + "|" + c.keyboard_variant + "|" + c.keyboard_options + "|" +
		std::to_string(c.repeat_rate) + "|" + std::to_string(c.repeat_delay);
	const bool kb_changed = kb != applied_keyboard;
	applied_keyboard = kb;

	apply_theme();
	load_cursor_theme();
	sync_phone_screen();
	sync_audio_sleep();

	// Light/dark, accent and font for other apps (GTK, libadwaita, Qt,
	// Chromium...), done by the dusk-apply-theme script when it's there.
	{
		char hex[16];
		std::snprintf(hex, sizeof hex, "#%02x%02x%02x",
			static_cast<int>(std::lround(config.accent.r * 255)),
			static_cast<int>(std::lround(config.accent.g * 255)),
			static_cast<int>(std::lround(config.accent.b * 255)));
		const std::string accent = gnome_accent(config.accent);
		const bool oled = config.dark && config.oled;
		// The icon theme picked in Settings goes to GNOME's setting first (GTK
		// apps read it), then the script passes it on to Qt apps.
		std::string icon_cmd;
		const std::string icon_theme = effective_icon_theme();
		if (!icon_theme.empty()) {
			std::string safe;
			for (char ch : icon_theme) if (ch != '\'' && ch != '\\') safe += ch;
			icon_cmd = "gsettings set org.gnome.desktop.interface icon-theme '" + safe + "' 2>/dev/null; ";
		}
		const std::string key = std::string(config.dark ? "1 " : "0 ") + hex + " " + config.font +
			(config.style_apps ? " 1" : " 0") + (oled ? " 1" : " 0") + " " + icon_theme + "|" + config.mono_font + "|" +
			std::to_string(config.font_size) + "|" + std::to_string(config.mono_font_size);
		if (key != color_scheme) {
			color_scheme = key;
			const std::string script = helper_path("dusk-apply-theme");
			if (!script.empty()) {
				// (The colour must be quoted: an unquoted "#4d8df6" starts a shell
				// comment, which left GNOME apps with no accent at all.)
				const std::string cmd = icon_cmd + shell_quote(script) + (config.dark ? " 1 " : " 0 ") + shell_quote(hex) + " " +
					shell_quote(accent) + " " +
					shell_quote(config.font) + (config.style_apps ? " 1" : " 0") + (oled ? " 1" : " 0") + " " +
					shell_quote(config.mono_font) + " " + std::to_string(config.font_size) + " " +
					std::to_string(config.mono_font_size);
				// Every app restyling at once is heavy: not while a new
				// wallpaper is coming in.
				// (Deferring it till after the transition made the accent feel
				// slow to change, so it runs at once now.)
				const bool transition = false;
				if (transition) {
					pending_theme_cmd = cmd;
					if (!theme_timer) {
						theme_timer = wl_event_loop_add_timer(loop, [](void *data) {
							auto *self = static_cast<Server *>(data);
							if (!self->pending_theme_cmd.empty()) self->spawn(self->pending_theme_cmd);
							self->pending_theme_cmd.clear();
							return 0;
						}, this);
					}
					wl_event_source_timer_update(theme_timer, static_cast<int>(config.ms(config.wallpaper_transition_ms)) + 150);
				} else {
					pending_theme_cmd.clear();
					spawn(cmd);
				}
			} else {
				spawn(icon_cmd + std::string("gsettings set org.gnome.desktop.interface color-scheme '") +
					(config.dark ? "prefer-dark" : "default") + "'; gsettings set "
					"org.gnome.desktop.interface accent-color '" + accent + "' 2>/dev/null");
			}
		}
	}
	sync_app_buttons();
	// Middle-click paste: GTK apps also paste their own selection, unless
	// GNOME's setting says not to.
	{
		const std::string paste = config.middle_click_paste ? "true" : "false";
		if (paste != applied_paste) {
			applied_paste = paste;
			if (!config.middle_click_paste && seat) {
				wlr_seat_set_primary_selection(seat, nullptr, wl_display_next_serial(display));
			}
			spawn("command -v gsettings >/dev/null && [ \"$(gsettings get org.gnome.desktop.interface "
				"gtk-enable-primary-paste 2>/dev/null)\" != " + paste + " ] && gsettings set "
				"org.gnome.desktop.interface gtk-enable-primary-paste " + paste + " 2>/dev/null");
		}
	}
	if (clipboard) clipboard->settings_changed();
	{
		const std::string mine = effective_icon_theme();
		icons.set_theme(mine.empty() ? gnome_setting("icon-theme") : mine);
	}

	if (kb_changed) {
		for (Keyboard *keyboard : keyboards) {
			keyboard->apply_settings();   // compiles a keymap: not for every change
		}
	}
	for (Pointer *pointer : pointers) {
		pointer->apply_settings();
	}
	// Turning notifications on/off in Settings.
	if (notifications && config.notifications != notifications->running()) {
		notifications.reset();
		notifications = std::make_unique<Notifications>(this);
		notifications->changed = [this] {
			control_centre->refresh();
			redraw_panels();
			if (lock) lock->notifications_changed();
		};
		notifications->history_changed = [this] {
		// Arrived while the control centre is open: seen already.
		if (control_centre && control_centre->is_open() && (!config.separate_notes() || control_centre->notes_open())) notifications->unseen = 0;
		if (control_centre) control_centre->refresh();
		redraw_panels();   // the bell on the panel
	};
	}

	for (Output *output : outputs) {
		output->configure();
	}
	rescue_views();
	for (Output *output : outputs) {
		if (output->enabled()) bring_back(output);
	}
	wlr_cursor_warp_closest(cursor, nullptr, cursor->x, cursor->y);
	write_outputs_info();
	for (Output *output : outputs) {
		output->apply_night_light();
		output->update_panels();
		if (wall_changed) {
			output->wallpaper_w = 0;   // repaint the wallpaper (it's big: only when needed)
		}
		if (look_changed) {
			output->corner_radius = -1;
		}
	}
	if (look_changed) {
		for (View *view : views) {
			view->invalidate_frame();
			view->update_positions();   // border width / radius may have changed
		}
	}
	arrange();
	if (look_changed && launcher->is_open()) {
		launcher->close();
	}
	if (desktop) desktop->settings_changed(look_changed);
	// Workspaces: dynamic or a fixed number (windows past the end come back).
	normalize_workspaces();
	for (View *v : views) v->sync_enabled();
	clipboard_card->update();
	control_centre->refresh();
	if (lyrics) lyrics->apply_config();
	views_changed();
}

// ============================================================================
// Outputs (monitors)
// ============================================================================

void Server::handle_new_output(void *data) {
	auto *wlr_out = static_cast<wlr_output *>(data);
	wlr_output_init_render(wlr_out, allocator, renderer);

	auto *output = new Output(this, wlr_out);
	outputs.push_back(output);
	output->configure();
	output->apply_night_light();
	output->update_panels();
	arrange();
	bring_back(output);
	write_outputs_info();
}

Output::Output(Server *server, wlr_output *output) : server(server), output(output) {
	output->data = this;
	on_frame.connect<&Output::handle_frame>(&output->events.frame, this);
	on_request_state.connect<&Output::handle_request_state>(
		&output->events.request_state, this);
	on_destroy.connect<&Output::handle_destroy>(&output->events.destroy, this);
}

Output::~Output() {
	if (server->lock) {
		server->lock->output_gone(this);
	}
	if (server->launcher) {
		server->launcher->output_gone(this);
	}
	if (server->control_centre) {
		server->control_centre->output_gone(this);
	}
	if (server->calendar) {
		server->calendar->output_gone(this);
		server->media_card->output_gone(this);
		server->menu->output_gone(this);
		server->snap_layouts->output_gone(this);
	}
	if (server->clipboard_card) {
		server->clipboard_card->output_gone(this);
	}
	if (server->desktop) {
		server->desktop->output_gone(this);
	}
	if (server->overview) {
		server->overview->output_gone(this);
	}
	if (server->shot_tool) {
		server->shot_tool->output_gone(this);
	}
	if (server->lyrics) {
		server->lyrics->output_gone(this);
	}
	panels.clear();
	for (auto *&corner : corners) {
		if (corner) {
			wlr_scene_node_destroy(&corner->node);
			corner = nullptr;
		}
	}
	finish_wallpaper_transition();
	if (wallpaper) {
		wlr_scene_node_destroy(&wallpaper->node);
	}
}

wlr_box Output::box() const {
	wlr_box b{};
	wlr_output_layout_get_box(server->layout, output, &b);
	return b;
}

wlr_box Output::usable() const {
	wlr_box b = box();
	b.x += reserved[3];
	b.y += reserved[0];
	b.width = std::max(0, b.width - reserved[1] - reserved[3]);
	b.height = std::max(0, b.height - reserved[0] - reserved[2]);
	return b;
}

// The built-in laptop screen, or else the first screen.
bool Output::is_primary() const {
	const std::string &want = server->config.primary_output;
	if (!want.empty()) {
		for (Output *o : server->outputs) {
			if (o->name() == want && o->enabled()) {
				return o == this;
			}
		}
	}
	for (Output *o : server->outputs) {
		if (!o->enabled()) {
			continue;
		}
		const std::string n = o->name();
		if (n.rfind("eDP", 0) == 0 || n.rfind("LVDS", 0) == 0 || n.rfind("DSI", 0) == 0) {
			return o == this;
		}
	}
	for (Output *o : server->outputs) {
		if (o->enabled()) {
			return o == this;
		}
	}
	return false;
}

// Makes this screen's panels match the settings: new ones are created,
// removed ones go, the rest are updated in place.
void Output::update_panels() {
	std::vector<std::unique_ptr<Panel>> next;
	if (!enabled()) {
		panels.clear();
		return;
	}
	for (const PanelConfig &pc : server->config.panels) {
		const bool here = pc.output == "all" || pc.output == name() ||
			(pc.output == "primary" && is_primary());
		if (!here) {
			continue;
		}
		auto it = std::find_if(panels.begin(), panels.end(),
			[&](const std::unique_ptr<Panel> &p) { return p && p->config().id == pc.id; });
		if (it != panels.end()) {
			(*it)->set_config(pc);
			next.push_back(std::move(*it));
		} else {
			next.push_back(std::make_unique<Panel>(server, this, pc));
		}
	}
	panels = std::move(next);
}

// Night light: warmer colours through the screen's gamma table.
void Output::apply_night_light() {
	const Config &c = server->config;
	const int want = c.night_light ? c.night_light_temp : 0;
	if (want == gamma_temp || !enabled()) {
		return;
	}
	const size_t size = wlr_output_get_gamma_size(output);
	if (size < 2) {
		gamma_temp = want;   // not supported (e.g. running in a window)
		return;
	}
	wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_color_transform *transform = nullptr;
	if (want > 0) {
		// Colour of a light at this temperature (Tanner Helland's fit of
		// black-body colours), relative to daylight white.
		const double t = want / 100.0;
		const double r = 1.0;
		const double g = std::clamp((99.4708025861 * std::log(t) - 161.1195681661) / 255.0, 0.0, 1.0);
		const double b = t <= 19 ? 0.0
			: std::clamp((138.5177312231 * std::log(t - 10) - 305.0447927307) / 255.0, 0.0, 1.0);
		std::vector<uint16_t> lut(size * 3);
		for (size_t i = 0; i < size; i++) {
			const double v = static_cast<double>(i) / static_cast<double>(size - 1) * 65535.0;
			lut[i] = static_cast<uint16_t>(v * r);
			lut[size + i] = static_cast<uint16_t>(v * g);
			lut[2 * size + i] = static_cast<uint16_t>(v * b);
		}
		transform = wlr_color_transform_init_lut_3x1d(size, lut.data(), lut.data() + size,
			lut.data() + 2 * size);
	}
	wlr_output_state_set_color_transform(&state, transform);
	if (wlr_output_commit_state(output, &state)) {
		gamma_temp = want;
	} else {
		wlr_log(WLR_INFO, "Night light isn't supported on %s", name().c_str());
		gamma_temp = want;   // don't retry every time
	}
	wlr_output_state_finish(&state);
	if (transform) {
		wlr_color_transform_unref(transform);
	}
}

namespace {

std::string mode_string(int w, int h, int mhz) {
	char buf[64];
	std::snprintf(buf, sizeof buf, "%dx%d@%.3f", w, h, mhz / 1000.0);
	return buf;
}

wl_output_transform rotation_transform(int degrees) {
	switch (degrees) {
	case 90: return WL_OUTPUT_TRANSFORM_90;
	case 180: return WL_OUTPUT_TRANSFORM_180;
	case 270: return WL_OUTPUT_TRANSFORM_270;
	default: return WL_OUTPUT_TRANSFORM_NORMAL;
	}
}

int transform_rotation(int t) {
	switch (t) {
	case WL_OUTPUT_TRANSFORM_90: return 90;
	case WL_OUTPUT_TRANSFORM_180: return 180;
	case WL_OUTPUT_TRANSFORM_270: return 270;
	default: return 0;
	}
}

bool is_builtin(const std::string &n) {
	return n.rfind("eDP", 0) == 0 || n.rfind("LVDS", 0) == 0 || n.rfind("DSI", 0) == 0;
}

// The mode for the wanted size and rate, like sway does it: that size at
// the closest rate (the highest if no rate was asked for), else the
// screen's preferred mode.
wlr_output_mode *pick_mode(wlr_output *out, int w, int h, double hz) {
	if (w <= 0 || h <= 0) {
		return wlr_output_preferred_mode(out);
	}
	wlr_output_mode *best = nullptr;
	const int want = static_cast<int>(hz * 1000.0 + 0.5);
	wlr_output_mode *m;
	wl_list_for_each(m, &out->modes, link) {
		if (m->width != w || m->height != h) {
			continue;
		}
		if (!best) {
			best = m;
		} else if (want > 0) {
			if (std::abs(m->refresh - want) < std::abs(best->refresh - want)) {
				best = m;
			}
		} else if (m->refresh > best->refresh) {
			best = m;
		}
	}
	return best ? best : wlr_output_preferred_mode(out);
}

} // namespace

// The scale that suits a screen, for one with none chosen yet (Settings ›
// Displays marks the same one "recommended"). Ordinary screens: from the
// pixels per inch (laptops are looked at from closer). Big screens (a TV, a
// 32"+ monitor), looked at from across a room or desk: by how many pixels
// tall they are, so a 4K TV is laid out like a 1080p one (200%).
double recommended_output_scale(int px_w, int px_h, int mm_w, int mm_h, bool builtin) {
	if (px_w <= 0 || px_h <= 0) return 1.0;
	auto quarter = [](double v) { return std::clamp(std::round(v * 4.0) / 4.0, 1.0, 2.0); };
	const double diag = mm_w > 0 && mm_h > 0 ? std::hypot(mm_w, mm_h) / 25.4 : 0;
	const double ppi = mm_w > 0 ? px_w / (mm_w / 25.4) : 0;
	const bool size_known = ppi >= 30 && ppi <= 700;
	if (!builtin && (!size_known || diag >= 32)) {
		return quarter(std::min(px_w, px_h) / 1080.0 * 0.95);   // (a touch under, so 1440p stays 125%)
	}
	if (!size_known) return 1.0;
	return quarter(ppi / (builtin ? 135.0 : 110.0));
}

void Output::configure() {
	const Config &c = server->config;
	OutputSettings s;
	if (auto it = c.outputs.find(name()); it != c.outputs.end()) {
		s = it->second;
	}
	auto sc = c.output_scale.find(name());
	float scale = 1.0f;
	if (sc != c.output_scale.end()) {
		scale = sc->second;
	} else {
		// None chosen: the one that suits it (a 4K TV isn't left at 100%).
		int pw = s.width, ph = s.height;
		if (pw <= 0 || ph <= 0) {
			if (wlr_output_mode *m = wlr_output_preferred_mode(output)) {
				pw = m->width;
				ph = m->height;
			}
		}
		const std::string n = name();
		const bool builtin = n.rfind("eDP", 0) == 0 || n.rfind("LVDS", 0) == 0 || n.rfind("DSI", 0) == 0;
		scale = static_cast<float>(recommended_output_scale(pw, ph, output->phys_width, output->phys_height, builtin));
	}

	// Never turn off the last screen that's on.
	bool on = s.enabled;
	if (!on) {
		bool others = false;
		for (Output *o : server->outputs) {
			others = others || (o != this && o->enabled());
		}
		on = !others;
	}

	char key[256];
	std::snprintf(key, sizeof key, "%d %dx%d@%.3f %d,%d,%d r%d s%.3f v%d", on, s.width, s.height,
		s.refresh, s.has_position, s.x, s.y, s.rotation, scale, s.vrr);
	if (applied == key) {
		return;
	}

	if (!on) {
		if (enabled()) server->evacuate(this);
		if (server->launcher) server->launcher->output_gone(this);
		if (server->control_centre) server->control_centre->output_gone(this);
		if (server->calendar) {
			server->calendar->output_gone(this);
			server->media_card->output_gone(this);
			server->menu->output_gone(this);
			server->snap_layouts->output_gone(this);
		}
		if (server->clipboard_card) server->clipboard_card->output_gone(this);
		if (server->desktop) server->desktop->output_gone(this);
		if (server->overview) server->overview->output_gone(this);
		if (server->shot_tool) server->shot_tool->output_gone(this);
		panels.clear();
		if (scene_output) {
			wlr_scene_output_destroy(scene_output);
			scene_output = nullptr;
		}
		wlr_output_layout_remove(server->layout, output);
		finish_wallpaper_transition();
		if (wallpaper) {
			wlr_scene_node_set_enabled(&wallpaper->node, false);
		}
		for (auto *corner : corners) {
			if (corner) {
				wlr_scene_node_set_enabled(&corner->node, false);
			}
		}
		wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, false);
		// Success is recorded only when the commit really went through, so a
		// failed disable stays retryable on the next configure.
		if (wlr_output_commit_state(output, &state)) {
			applied = key;
		} else {
			wlr_log(WLR_ERROR, "%s: could not disable the output", name().c_str());
		}
		wlr_output_state_finish(&state);
		return;
	}

	wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_mode *mode = pick_mode(output, s.width, s.height, s.refresh);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	} else if (s.width > 0 && s.height > 0) {
		// No mode list (e.g. running in a window): ask for the size.
		wlr_output_state_set_custom_mode(&state, s.width, s.height,
			static_cast<int>(s.refresh * 1000.0));
	}
	wlr_output_state_set_transform(&state, rotation_transform(s.rotation));
	wlr_output_state_set_scale(&state, scale);
	if (output->adaptive_sync_supported) {
		wlr_output_state_set_adaptive_sync_enabled(&state, s.vrr);
	}
	if (!wlr_output_test_state(output, &state)) {
		// Something in there isn't possible: fall back to the preferred
		// mode and no variable refresh, keeping rotation and scale.
		wlr_log(WLR_INFO, "%s: those display settings aren't possible, using the preferred mode",
			name().c_str());
		wlr_output_state_finish(&state);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		if (wlr_output_mode *pref = wlr_output_preferred_mode(output)) {
			wlr_output_state_set_mode(&state, pref);
		}
		wlr_output_state_set_transform(&state, rotation_transform(s.rotation));
		wlr_output_state_set_scale(&state, scale);
	}
	// Success is recorded only after the commit really went through: a passed
	// wlr_output_test_state does not guarantee the commit will, and a failed
	// commit must stay retryable (a screen that stayed black gets another try
	// the next time settings are applied).
	if (wlr_output_commit_state(output, &state)) {
		applied = key;
	} else {
		wlr_log(WLR_ERROR, "%s: display settings commit failed", name().c_str());
		wlr_output_state_finish(&state);
		return;
	}
	wlr_output_state_finish(&state);

	wlr_output_layout_output *lo = s.has_position
		? wlr_output_layout_add(server->layout, output, s.x, s.y)
		: wlr_output_layout_add_auto(server->layout, output);
	const bool coming_back = !scene_output && lo;
	if (coming_back) {
		scene_output = wlr_scene_output_create(server->scene, output);
		wlr_scene_output_layout_add_output(server->scene_layout, lo, scene_output);
	}
	if (wallpaper) {
		wlr_scene_node_set_enabled(&wallpaper->node, true);
	}
	wallpaper_w = 0;      // repaint at the new size
	corner_radius = -1;
	gamma_temp = -1;      // a mode change can drop the night light
}

void Server::write_outputs_info() {
	const char *rt = std::getenv("XDG_RUNTIME_DIR");
	if (!rt || !*rt) {
		return;
	}
	const std::string dir = std::string(rt) + "/dusk";
	mkdir(dir.c_str(), 0700);
	std::string text;
	auto line = [&text](const char *k, const std::string &v) {
		text += k;
		text += '=';
		text += v;
		text += '\n';
	};
	for (Output *o : outputs) {
		wlr_output *w = o->output;
		text += "[" + o->name() + "]\n";
		line("description", w->description ? w->description : "");
		line("make", w->make ? w->make : "");
		line("model", w->model ? w->model : "");
		line("enabled", o->enabled() ? "true" : "false");
		line("builtin", is_builtin(o->name()) ? "true" : "false");
		line("primary", o->is_primary() ? "true" : "false");
		line("mode", mode_string(w->width, w->height, w->refresh));
		const wlr_box b = o->box();
		line("position", std::to_string(b.x) + "," + std::to_string(b.y));
		line("logical", std::to_string(b.width) + "x" + std::to_string(b.height));
		char sc[32];
		std::snprintf(sc, sizeof sc, "%.3f", w->scale);
		line("scale", sc);
		line("rotation", std::to_string(transform_rotation(w->transform)));
		line("physical", std::to_string(w->phys_width) + "x" + std::to_string(w->phys_height));
		line("vrr_supported", w->adaptive_sync_supported ? "true" : "false");
		line("vrr", w->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED ? "true" : "false");
		std::string modes, preferred;
		wlr_output_mode *m;
		wl_list_for_each(m, &w->modes, link) {
			const std::string ms = mode_string(m->width, m->height, m->refresh);
			if (m->preferred) {
				preferred = ms;
			}
			if (!modes.empty()) {
				modes += ' ';
			}
			modes += ms;
		}
		line("preferred", preferred);
		line("modes", modes);
		text += '\n';
	}
	const std::string path = dir + "/outputs";
	const std::string tmp = path + ".tmp";
	if (FILE *f = std::fopen(tmp.c_str(), "w")) {
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
		std::rename(tmp.c_str(), path.c_str());
	}
}

void Server::edge_resist(double &dx, double &dy) {
	if (config.edge_resistance == "off" || outputs.size() < 2) {
		return;
	}
	const bool strong = config.edge_resistance == "strong";
	const bool moving = cursor_mode == CursorMode::Move;
	// A little more while dragging a window, so it can be dropped on the edge
	// to snap; but it gets across by pushing on a bit, or by holding it
	// against the edge for a moment (on a touchpad you can't push far in one go).
	// The pointer alone takes a firmer push than before (it was too easy to
	// slip onto the other screen by accident); a window carried across keeps
	// the lighter one, so it isn't a struggle.
	const double needed = moving ? (strong ? 140 : 70) : (strong ? 260 : 130);
	const double hold_ms = moving ? (strong ? 700 : 400) : (strong ? 1100 : 650);
	const double now = now_ms();
	if (now - push_at > (moving ? 1200 : 400)) {
		push_x = push_y = 0;   // stopped pushing: start again
		push_since = now;
	}
	if (push_x == 0 && push_y == 0) push_since = now;
	const bool held = now - push_since >= hold_ms;
	const double x = cursor->x, y = cursor->y;
	wlr_output *here = wlr_output_layout_output_at(layout, x, y);
	if (!here) {
		return;
	}
	wlr_box b{};
	wlr_output_layout_get_box(layout, here, &b);
	auto other_screen = [&](double px, double py) {
		wlr_output *o = wlr_output_layout_output_at(layout, px, py);
		return o && o != here;
	};
	const double nx = x + dx, ny = y + dy;
	if (nx < b.x || nx >= b.x + b.width) {
		const double cy = std::clamp(ny, static_cast<double>(b.y), b.y + b.height - 1.0);
		if (other_screen(nx, cy)) {
			if ((dx < 0) != (push_x < 0)) push_x = 0;   // changed direction
			push_x += dx;
			push_at = now;
			if (std::fabs(push_x) < needed && !held) {
				dx = dx < 0 ? b.x - x : b.x + b.width - 1 - x;   // stay on the edge
			} else {
				push_x = 0;
			}
		}
	}
	if (ny < b.y || ny >= b.y + b.height) {
		const double cx = std::clamp(nx, static_cast<double>(b.x), b.x + b.width - 1.0);
		if (other_screen(cx, ny)) {
			if ((dy < 0) != (push_y < 0)) push_y = 0;
			push_y += dy;
			push_at = now;
			if (std::fabs(push_y) < needed && !held) {
				dy = dy < 0 ? b.y - y : b.y + b.height - 1 - y;
			} else {
				push_y = 0;
			}
		}
	}
}

// Super+Shift+Left/Right: the window goes to the screen on that side (or
// round to the other end), same place relative to the screen, same tiling.
void Server::move_to_next_screen(View *view, int direction) {
	std::vector<Output *> on;
	for (Output *o : outputs) {
		if (o->enabled()) on.push_back(o);
	}
	if (on.size() < 2) {
		return;
	}
	std::sort(on.begin(), on.end(), [](Output *a, Output *b) { return a->box().x < b->box().x; });
	Output *from = view->tile_target();
	auto it = std::find(on.begin(), on.end(), from);
	const long i = it == on.end() ? 0 : it - on.begin();
	Output *to = on[static_cast<size_t>((i + direction + static_cast<long>(on.size())) %
		static_cast<long>(on.size()))];
	if (to == from) {
		return;
	}
	move_to_screen(view, to);
}

// A window onto another screen: where it was on its own (as a share of the
// screen), or maximised / tiled / full screen there the same way.
void Server::move_to_screen(View *view, Output *to, bool focus_it) {
	Output *from = view->tile_target();
	if (!to || to == from) return;
	const wlr_box a = from ? from->usable() : wlr_box{0, 0, 1, 1};
	const wlr_box b = to->usable();
	const wlr_box f = view->frame_box();
	const double rx = a.width > 0 ? double(f.x - a.x) / a.width : 0;
	const double ry = a.height > 0 ? double(f.y - a.y) / a.height : 0;
	view->home_output.clear();
	if (view->fullscreen) {
		view->set_fullscreen(false);
		const wlr_box o = to->box();
		view->move_to(o.x + o.width / 4, o.y + o.height / 4);
		view->set_fullscreen(true);
	} else if (view->maximized) {
		view->set_maximized(true, to);
	} else if (view->snap != View::Snap::None) {
		const View::Snap kind = view->snap;
		view->tile_to(view->zone, to);
		view->snap = kind;
	} else {
		view->move_to(b.x + static_cast<int>(std::lround(rx * b.width)),
			b.y + static_cast<int>(std::lround(ry * b.height)));
		rescue_views();   // too big to fit where it was: centred instead
	}
	if (focus_it) focus(view);
}

void Server::evacuate(Output *output) {
	const wlr_box b = output->box();
	if (b.width <= 0) {
		return;
	}
	for (View *v : views) {
		const wlr_box f = v->frame_box();
		const double cx = f.x + f.width / 2.0, cy = f.y + f.height / 2.0;
		if (cx >= b.x && cx < b.x + b.width && cy >= b.y && cy < b.y + b.height) {
			v->home_output = output->name();
			v->home_x = f.x - b.x;
			v->home_y = f.y - b.y;
		}
	}
}

void Server::bring_back(Output *output) {
	const wlr_box b = output->box();
	if (b.width <= 0) {
		return;
	}
	for (View *v : views) {
		if (v->home_output != output->name()) {
			continue;
		}
		v->home_output.clear();
		const wlr_box f = v->frame_box();
		// move_to, not just x/y: the window's picture (and where it takes
		// clicks) has to move too.
		v->move_to(b.x + std::clamp(v->home_x, 0, std::max(0, b.width - std::min(f.width, b.width))),
			b.y + std::clamp(v->home_y, 0, std::max(0, b.height - std::min(f.height, b.height))));
		v->update_positions();
		v->refit();   // maximised and snapped windows take the screen's size
	}
}

void Server::rescue_views() {
	Output *home = nullptr;
	for (Output *o : outputs) {
		if (o->enabled() && (!home || o->is_primary())) {
			home = o;
		}
	}
	if (!home) {
		return;
	}
	for (View *view : views) {
		const wlr_box f = view->frame_box();
		const double cx = f.x + f.width / 2.0, cy = f.y + f.height / 2.0;
		if (wlr_output_layout_output_at(layout, cx, cy)) {
			continue;
		}
		const wlr_box u = home->usable();
		// Moves the window's picture too. (Only changing x/y left it drawn,
		// and taking clicks, where the screen used to be: such windows
		// couldn't be used any more.)
		view->cancel_animation();
		view->move_to(u.x + std::max(0, (u.width - f.width) / 2), u.y + std::max(0, (u.height - f.height) / 2));
		// (A tiled or maximised window keeps tile_output: while that screen is
		// gone it fits the one it's on now, and goes back when it returns.)
		view->update_positions();
		view->refit();
	}
}

namespace {

bool never_accepts_input(wlr_scene_buffer *, double *, double *) {
	return false;
}

} // namespace

// Rounded screen corners: four small black masks on top of everything,
// which clicks pass straight through.
void Output::update_corners() {
	const int r = server->config.screen_corners;
	const wlr_box b = box();
	for (int i = 0; i < 4; i++) {
		if (!corners[i]) {
			corners[i] = wlr_scene_buffer_create(server->layer_overlay, nullptr);
			corners[i]->point_accepts_input = never_accepts_input;
		}
		wlr_scene_node_set_enabled(&corners[i]->node, r > 0 && b.width > 0 && !game);
	}
	if (r <= 0 || b.width <= 0) {
		return;
	}
	if (r != corner_radius) {
		corner_radius = r;
		for (int i = 0; i < 4; i++) {
			// 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left
			Canvas canvas(r, r, output->scale);
			cairo_t *cr = canvas.cr();
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_paint(cr);
			const double cx = (i == 1 || i == 2) ? 0 : r;
			const double cy = (i >= 2) ? 0 : r;
			cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
			cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
			cairo_fill(cr);
			canvas.present(corners[i]);
		}
	}
	wlr_scene_node_set_position(&corners[0]->node, b.x, b.y);
	wlr_scene_node_set_position(&corners[1]->node, b.x + b.width - r, b.y);
	wlr_scene_node_set_position(&corners[2]->node, b.x + b.width - r, b.y + b.height - r);
	wlr_scene_node_set_position(&corners[3]->node, b.x, b.y + b.height - r);
}

void Output::handle_frame(void *) {
	wlr_scene_output *scene_output = wlr_scene_get_scene_output(server->scene, output);
	if (!scene_output) {
		return;
	}
	// Animations move exactly once per refresh, right before drawing.
	server->anim->step();
	// Blur over something that changes, redrawn only where it changed, left
	// stale squares round the edges (the overview's "glitchy, hollow" look,
	// and smudges when windows are dragged under the panel). While the
	// overview is up or a window is being moved, the whole screen is drawn
	// each frame instead: a little more work, only for those moments.
	if (!game && ((server->overview && server->overview->shown()) || server->cursor_mode != CursorMode::Passthrough)) {
		const wlr_box whole{0, 0, output->width, output->height};   // buffer coordinates
		wlr_damage_ring_add_box(&scene_output->damage_ring, &whole);
	}
	// A full-screen game or video here, focused: nothing of Dusk's on top
	// of it (so it can go straight to the screen), and no vsync if it asked.
	View *f = server->focused;
	const bool game_here = f && f->mapped && f->fullscreen && !f->minimized && server->output_for_view(f) == this &&
		!(server->overview && server->overview->is_open()) && !(server->lock && server->lock->active());
	if (game_here != game) {
		game = game_here;
		const bool corners_on = !game && server->config.screen_corners > 0;
		for (wlr_scene_buffer *c : corners) {
			if (c) wlr_scene_node_set_enabled(&c->node, corners_on);
		}
		bool any = false;
		for (Output *o : server->outputs) any = any || o->game;
		if (any != server->game_mode) {
			server->game_mode = any;
			crumb(any ? "game mode on" : "game mode off");
		}
	}
	bool tear = false;
	if (game && server->config.allow_tearing && server->tearing_control && f->surface()) {
		tear = wlr_tearing_control_manager_v1_surface_hint_from_surface(server->tearing_control, f->surface()) ==
			WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
	}
	if (tear) {
		wlr_output_state state;
		wlr_output_state_init(&state);
		if (wlr_scene_output_build_state(scene_output, &state, nullptr)) {
			state.tearing_page_flip = true;
			if (!wlr_output_test_state(output, &state)) {
				state.tearing_page_flip = false;   // the screen can't: with vsync then
			}
			tearing = state.tearing_page_flip;
			wlr_output_commit_state(output, &state);
		}
		wlr_output_state_finish(&state);
	} else {
		tearing = false;
		wlr_scene_output_commit(scene_output, nullptr);
	}
	if (server->anim->active()) {
		wlr_output_schedule_frame(output);
	}
	timespec now{};
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

void Output::handle_request_state(void *data) {
	// e.g. the window was resized when running nested inside another desktop.
	auto *event = static_cast<wlr_output_event_request_state *>(data);
	wlr_output_commit_state(output, event->state);
	server->arrange();
	server->write_outputs_info();
}

void Output::handle_destroy(void *) {
	Server *s = server;
	if (s->phone_output == output) {
		s->phone_output = nullptr;
	}
	const wlr_box gone = box();
	const bool pointer_here = gone.width > 0 && s->cursor->x >= gone.x && s->cursor->y >= gone.y &&
		s->cursor->x < gone.x + gone.width && s->cursor->y < gone.y + gone.height;
	if (!s->shutting_down && enabled()) {
		s->evacuate(this);   // its windows come back if it's plugged in again
	}
	s->outputs.erase(std::remove(s->outputs.begin(), s->outputs.end(), this), s->outputs.end());
	const bool was_on = enabled();
	if (scene_output) {
		wlr_scene_output_destroy(scene_output);
		scene_output = nullptr;
	}
	// Out of the layout now: wlroots would do it too, but only after us,
	// and until then its windows still looked like they were on a screen.
	wlr_output_layout_remove(s->layout, output);
	delete this;
	if (!s->shutting_down) {
		// If that was the only screen that was on, turn another one on.
		bool any = false;
		for (Output *o : s->outputs) {
			any = any || o->enabled();
		}
		if (was_on && !any) {
			for (Output *o : s->outputs) {
				o->applied.clear();
				o->configure();
				o->update_panels();
			}
		}
		s->screen_gone(pointer_here);
	}
}

// A screen was unplugged or turned off: nothing may stay where it was.
void Server::screen_gone(bool pointer_was_there) {
	// A drag or resize in progress could be on the screen that went.
	if (cursor_mode != CursorMode::Passthrough) {
		if (grab_view && cursor_mode == CursorMode::Resize && grab_view->ready()) {
			grab_view->send_resizing(false);
		}
		cursor_mode = CursorMode::Passthrough;
		grab_view = nullptr;
		hide_snap_preview();
		snap_layouts->drag_end();
	}
	implicit_grab_surface = nullptr;
	buttons_down = 0;
	rescue_views();
	arrange();
	// The pointer: onto the main screen's middle if it was on the one that
	// went (otherwise it would sit in a corner of the next one).
	Output *home = nullptr;
	for (Output *o : outputs) {
		if (o->enabled() && (!home || o->is_primary())) home = o;
	}
	if (pointer_was_there && home) {
		const wlr_box b = home->box();
		wlr_cursor_warp(cursor, nullptr, b.x + b.width / 2.0, b.y + b.height / 2.0);
	} else {
		wlr_cursor_warp_closest(cursor, nullptr, cursor->x, cursor->y);
	}
	process_motion(static_cast<uint32_t>(now_ms()));   // whatever is under the pointer now
	schedule_frames();
	write_outputs_info();
}

void Server::handle_layout_change(void *) {
	// Screens moved, came or went: no window may be left where no screen is.
	rescue_views();
	arrange();
	if (lock) lock->refresh();   // covers every screen there is now
}

void Server::arrange() {
	for (Output *output : outputs) {
		// Panels along the top and bottom first; the side ones fit between
		// them. Several panels on one edge stack inwards.
		const wlr_box b = output->box();
		auto edge_index = [](const std::string &e) {
			return e == "top" ? 0 : e == "right" ? 1 : e == "bottom" ? 2 : 3;
		};
		int offset[4] = {0, 0, 0, 0};    // top, right, bottom, left
		int reserve[4] = {0, 0, 0, 0};
		for (int pass = 0; pass < 2; pass++) {
			for (auto &panel : output->panels) {
				if (panel->config().vertical() != (pass == 1)) {
					continue;
				}
				const int i = edge_index(panel->config().edge);
				panel->place(offset[i], b.y + reserve[0], b.y + b.height - reserve[2]);
				offset[i] += panel->thickness();
				reserve[i] += panel->reserved();
			}
		}
		for (int i = 0; i < 4; i++) {
			output->reserved[i] = reserve[i];
		}
		output->update_wallpaper();
		output->update_corners();
	}
	for (View *view : views) {
		view->refit();   // maximised/snapped windows follow screen size changes
	}
	if (notifications) {
		notifications->relayout();
	}
	if (desktop) {
		desktop->relayout();
	}
	if (control_centre) {
		control_centre->refresh();
	}
}

void Server::schedule_frames() {
	for (Output *output : outputs) {
		wlr_output_schedule_frame(output->output);
	}
}

// The screen with the focused window, else the one with the pointer.
Output *Server::focused_output() {
	if (focused && focused->mapped) {
		return output_for_view(focused);
	}
	return output_at(cursor->x, cursor->y);
}

Output *Server::output_at(double lx, double ly) {
	wlr_output *wlr_out = wlr_output_layout_output_at(layout, lx, ly);
	for (Output *output : outputs) {
		if (output->output == wlr_out) {
			return output;
		}
	}
	for (Output *output : outputs) {
		if (output->is_primary()) {
			return output;
		}
	}
	return outputs.empty() ? nullptr : outputs.front();
}

Output *Server::output_for_view(const View *view) {
	const wlr_box f = view->frame_box();
	double cx = 0, cy = 0;
	wlr_output_layout_closest_point(layout, nullptr, f.x + f.width / 2.0, f.y + 10, &cx, &cy);
	return output_at(cx, cy);
}

float Server::max_scale() const {
	float scale = 1.0f;
	for (Output *output : outputs) {
		scale = std::max(scale, output->output->scale);
	}
	return scale;
}

// ============================================================================
// Windows
// ============================================================================

void Server::handle_new_toplevel(void *data) {
	auto *toplevel = static_cast<wlr_xdg_toplevel *>(data);
	views.push_front(new XdgView(this, toplevel));
}

void Server::handle_new_decoration(void *data) {
	auto *deco = static_cast<wlr_xdg_toplevel_decoration_v1 *>(data);
	if (auto *view = static_cast<XdgView *>(deco->toplevel->base->data)) {
		view->attach_decoration(deco);
	}
}

void Server::raise(View *view) {
	wlr_scene_node_raise_to_top(&view->tree->node);
	views.remove(view);
	views.push_front(view);
	for (View *v : views) {
		if (v->fullscreen && v != view) v->update_layer();   // raised over a full-screen one: it goes under the panels
	}
}

// Gives keyboard input to a surface without changing the active window
// (used for X11 menus that need typing).
void Server::focus_surface(wlr_surface *surface) {
	if (surface && locked()) {
		return;   // a window opening meanwhile gets the keyboard after unlocking
	}
	if (!surface) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		update_constraint();
		return;
	}
	if (wlr_keyboard *kb = wlr_seat_get_keyboard(seat)) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes,
			&kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
	}
	update_constraint();
}

void Server::focus(View *view) {
	if (!view) {
		if (focused) {
			View *prev = focused;
			focused = nullptr;
			prev->set_activated(false);
		}
		wlr_seat_keyboard_notify_clear_focus(seat);
		views_changed();
		return;
	}
	if (view->minimized) {
		view->restore_from_minimize();   // animates it back, then focuses it
		return;
	}
	if (!view->mapped) {
		// Not on screen yet (an X11 restore or a self-fullscreen request can
		// arrive before the window maps): flag it as needing attention and
		// wait, instead of giving the keyboard to a surface that cannot draw.
		view->urgent = true;
		views_changed();
		return;
	}
	if (!shown_desktop.empty()) {
		shown_desktop.clear();   // "show desktop" is over once a window is used
	}
	if (!view->sticky && view->workspace != workspace) {
		switch_workspace(view->workspace);   // it's on another workspace: go there
	}
	view->urgent = false;

	View *prev = focused;
	focused = view;
	if (prev && prev != view) {
		prev->set_activated(false);   // also moves a fullscreen window back down
	}
	if (desktop) {
		desktop->focus_lost();   // keys go to the window again
	}
	view->set_activated(true);
	raise(view);

	focus_surface(view->surface());
	views_changed();
}

void Server::focus_next_after(View *gone) {
	for (View *view : views) {
		if (view != gone && view->visible()) {
			focus(view);
			return;
		}
	}
	if (focused == gone) {
		focused = nullptr;
	}
	focus(nullptr);
}

void Server::views_changed() {
	// Full-screen windows over or under the panels, by what's on top now.
	for (View *v : views) {
		if (v->fullscreen) v->update_layer();
	}
	redraw_panels();
}

// Once, when the event loop is next free, however many things asked for it
// before then (a status update, a notification and a menu opening in the
// same moment each redrew every panel straight away).
void Server::redraw_panels() {
	if (panels_idle_) return;
	panels_idle_ = wl_event_loop_add_idle(loop, [](void *data) {
		auto *self = static_cast<Server *>(data);
		self->panels_idle_ = nullptr;
		for (Output *output : self->outputs) {
			for (auto &panel : output->panels) {
				panel->redraw();
			}
		}
	}, this);
}

// Right-click on the wallpaper.
// The screen locker to run: yours (lock_command), or the first one found.
std::string Server::lock_command() const {
	if (!config.lock_command.empty()) {
		return config.lock_command;
	}
	const char *path = getenv("PATH");
	const std::string dirs = path ? path : "/usr/local/bin:/usr/bin";
	static const std::pair<const char *, const char *> kLockers[] = {
		{"hyprlock", "hyprlock"}, {"gtklock", "gtklock -d"}, {"swaylock", "swaylock -f"},
		{"waylock", "waylock"},
	};
	for (const auto &[bin, cmd] : kLockers) {
		size_t start = 0;
		while (start <= dirs.size()) {
			const size_t end = std::min(dirs.find(':', start), dirs.size());
			const std::string file = dirs.substr(start, end - start) + "/" + bin;
			if (access(file.c_str(), X_OK) == 0) {
				return cmd;
			}
			start = end + 1;
		}
	}
	return "";
}

void Server::power_action(const std::string &action) {
	close_popups();
	menu->close();
	if (action == "lock") {
		// Your own locker if you set one (Settings: lock_command), otherwise Dusk's.
		if (!config.lock_command.empty()) spawn(config.lock_command);
		else if (lock) lock->lock();
	} else if (action == "sleep") {
		spawn("systemctl suspend");
	} else if (action == "hibernate") {
		spawn("systemctl hibernate");
	} else if (action == "logout") {
		wl_display_terminate(display);
	} else if (action == "restart") {
		spawn("systemctl reboot");
	} else if (action == "shutdown") {
		spawn("systemctl poweroff");
	} else if (action == "firmware") {
		spawn("systemctl reboot --firmware-setup");
	}
}

void Server::show_power_menu(Output *output, wlr_box anchor, const std::string &edge) {
	struct Action { const char *id, *label, *icon, *question, *yes; };
	static const Action kActions[] = {
		{"lock", "Lock", "lock", nullptr, nullptr},
		{"sleep", "Sleep", "suspend", nullptr, nullptr},
		{"hibernate", "Hibernate", "moon", nullptr, nullptr},
		{"logout", "Log out", "logout", "Log out?", "Log out now"},
		{"restart", "Restart", "restart", "Restart?", "Restart now"},
		{"shutdown", "Shut down", "power", "Shut down?", "Shut down now"},
		{"firmware", "Restart to firmware setup", "gear", "Restart to firmware setup?", "Restart now"},
	};
	std::vector<MenuItem> items;
	for (const std::string &id : config.power_actions) {
		const Action *a = nullptr;
		for (const Action &x : kActions) if (id == x.id) a = &x;
		if (!a) continue;
		const std::string action = a->id;
		if (a->question && config.power_confirm) {
			const std::string question = a->question, yes_label = a->yes;
			items.push_back(MenuItem::action(std::string(a->label) + "…",
				[this, output, anchor, edge, action, question, yes_label] {
					std::vector<MenuItem> ask;
					ask.push_back(MenuItem::header(question));
					MenuItem yes = MenuItem::action(yes_label, [this, action] { power_action(action); });
					yes.danger = true;
					ask.push_back(yes);
					ask.push_back(MenuItem::action("Cancel", {}));
					menu->show(output, anchor, edge, std::move(ask));
				}, a->icon));
		} else {
			items.push_back(MenuItem::action(a->label, [this, action] { power_action(action); }, a->icon));
		}
	}
	if (items.empty()) {
		items.push_back(MenuItem::header("No power options"));
	}
	items.push_back(MenuItem::separator());
	items.push_back(MenuItem::action("Power options…", [this] { open_settings("power"); }, "gear"));
	menu->show(output, anchor, edge, std::move(items));
}

void Server::show_desktop_menu(double x, double y) {
	Output *out = output_at(x, y);
	if (!out) {
		return;
	}
	std::vector<MenuItem> items;
	if (desktop) {
		desktop->add_menu_items(items, x, y);   // New folder, Paste...
	}
	items.push_back(MenuItem::action("Open terminal", [this] { spawn_terminal(); }, "terminal"));
	items.push_back(MenuItem::action("Show desktop", [this] { toggle_show_desktop(); }, "desktop"));
	items.push_back(MenuItem::separator());
	items.push_back(MenuItem::radio("Dark", config.dark, [this] { write_setting("theme", "dark"); }));
	items.push_back(MenuItem::radio("Light", !config.dark, [this] { write_setting("theme", "light"); }));
	items.push_back(MenuItem::separator());
	items.push_back(MenuItem::action("Change wallpaper…", [this] { open_settings("appearance"); }, "image"));
	items.push_back(MenuItem::action("Display settings…", [this] { open_settings("displays"); }, "display"));
	items.push_back(MenuItem::action("Settings", [this] { open_settings(); }, "gear"));
	menu->show(out, wlr_box{static_cast<int>(x), static_cast<int>(y), 1, 1}, "point", std::move(items));
}

void Server::close_popups() {
	launcher->close();
	control_centre->close();
	calendar->close();
	media_card->close();
	menu->close();
	snap_layouts->close();
	clipboard_card->close();
}

std::vector<Popover *> Server::popovers() const {
	return {calendar.get(), media_card.get(), menu.get(), snap_layouts.get(), clipboard_card.get(), osk.get()};
}

// Minimises every window; doing it again brings them back.
void Server::toggle_show_desktop() {
	if (!shown_desktop.empty()) {
		std::vector<View *> back = shown_desktop;
		shown_desktop.clear();
		for (auto it = back.rbegin(); it != back.rend(); ++it) {
			if (std::find(views.begin(), views.end(), *it) != views.end() && (*it)->minimized) {
				(*it)->restore_from_minimize();
			}
		}
		return;
	}
	std::vector<View *> hide;
	for (View *v : views) {
		if (v->visible()) {
			hide.push_back(v);
		}
	}
	for (View *v : hide) {
		v->minimize();
	}
	shown_desktop = hide;
	redraw_panels();
}

void Server::place_new_view(View *view) {
	Output *output = output_at(cursor->x, cursor->y);
	if (!output) {
		view->move_to(0, 0);
		return;
	}
	const wlr_box u = output->usable();
	const int fw = view->content_width() + view->deco_width();
	const int fh = view->content_height() + view->deco_height();

	int nx, ny;
	if (config.placement == "cursor") {
		// Centred on the pointer.
		nx = static_cast<int>(cursor->x) - fw / 2;
		ny = static_cast<int>(cursor->y) - fh / 2;
	} else {
		nx = u.x + (u.width - fw) / 2;
		ny = u.y + (u.height - fh) / 2;
		if (config.placement == "cascade") {
			// Step down and right from the top-left for each open window.
			int open = 0;
			for (View *v : views) {
				if (v != view && v->mapped) {
					open++;
				}
			}
			const int step = (open % 8) * 32;
			nx = u.x + 40 + step;
			ny = u.y + 30 + step;
		}
	}
	nx = std::max(u.x, std::min(nx, u.x + u.width - fw));
	ny = std::max(u.y, std::min(ny, u.y + u.height - fh));
	view->move_to(nx, ny);
}

void Server::begin_move(View *view) {
	if (view->fullscreen) {
		return;
	}
	view->cancel_animation();
	focus(view);
	cursor_mode = CursorMode::Move;
	grab_view = view;
	grab_x = cursor->x - view->x;
	grab_y = cursor->y - view->y;
	press_x = cursor->x;
	press_y = cursor->y;
	wlr_seat_pointer_notify_clear_focus(seat);
	set_cursor("grabbing");
}

void Server::begin_resize(View *view, uint32_t edges) {
	if (view->fullscreen) {
		return;
	}
	view->cancel_animation();
	focus(view);
	if (view->maximized || view->snap != View::Snap::None) {
		// Keep the current size, but it is no longer maximised/snapped.
		if (view->maximized && view->ready()) {
			view->send_maximized(false);
		}
		view->maximized = false;
		view->snap = View::Snap::None;
		view->invalidate_frame();
		view->redraw_frame();
	}
	cursor_mode = CursorMode::Resize;
	grab_view = view;
	grab_box = view->frame_box();
	grab_x = cursor->x;
	grab_y = cursor->y;
	resize_edges = edges;
	if (view->ready()) {
		view->send_resizing(true);
	}
	wlr_seat_pointer_notify_clear_focus(seat);
	set_cursor(cursor_for_edges(edges));
}

// ============================================================================
// Pointer
// ============================================================================

void Server::set_cursor(const char *name) {
	if (cursor_image == name) {
		return;
	}
	cursor_image = name;
	wlr_cursor_set_xcursor(cursor, cursor_manager, name);
}

Hit Server::hit_test(double lx, double ly) {
	Hit hit;
	// An open right-click menu is above everything Dusk draws.
	if (menu && menu->contains(lx, ly)) {
		hit.popover = menu.get();
		return hit;
	}
	double sx = 0, sy = 0;
	// The picture being dragged sits under the pointer: look past it.
	const bool hide_icon = drag_icon && drag_icon->node.enabled;
	if (hide_icon) wlr_scene_node_set_enabled(&drag_icon->node, false);
	wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node, lx, ly, &sx, &sy);
	if (hide_icon) wlr_scene_node_set_enabled(&drag_icon->node, true);

	// Walk up to the nearest tree we tagged (a window, the panel, the menu).
	NodeTag *tag = nullptr;
	if (node) {
		for (wlr_scene_tree *tree = node->parent; tree; tree = tree->node.parent) {
			if (tree->node.data) {
				tag = static_cast<NodeTag *>(tree->node.data);
				break;
			}
		}
	}
	if (tag && tag->kind == NodeTag::Kind::Panel) {
		hit.panel = static_cast<Panel *>(tag->object);
		return hit;
	}
	if (tag && tag->kind == NodeTag::Kind::Launcher) {
		hit.launcher = true;
		return hit;
	}
	if (tag && tag->kind == NodeTag::Kind::ControlCentre) {
		hit.control_centre = true;
		return hit;
	}
	if (tag && tag->kind == NodeTag::Kind::Toast) {
		hit.toast = static_cast<Toast *>(tag->object);
		return hit;
	}
	if (tag && tag->kind == NodeTag::Kind::Popover) {
		hit.popover = static_cast<Popover *>(tag->object);
		return hit;
	}
	if (tag && tag->kind == NodeTag::Kind::Desktop) {
		hit.desktop = true;   // an icon on the desktop
		return hit;
	}
	if (tag && tag->kind == NodeTag::Kind::Overview) {
		hit.overview = true;
		return hit;
	}
	if (tag && tag->kind == NodeTag::Kind::Osd) {
		// The OSD sits on the top layer: it is nothing to the hit test
		// (the press, scroll and drag handlers check it by position first),
		// and windows' resize margins do not poke through it.
		return hit;
	}
	if (tag && tag->kind == NodeTag::Kind::Unmanaged) {
		// X11 menus and tooltips: no frame, just the surface.
		if (node->type == WLR_SCENE_NODE_BUFFER) {
			if (wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
					wlr_scene_buffer_from_node(node))) {
				hit.region = Region::Content;
				hit.surface = ss->surface;
				hit.sx = sx;
				hit.sy = sy;
			}
		}
		return hit;
	}
	View *owner = (tag && tag->kind == NodeTag::Kind::View)
		? static_cast<View *>(tag->object) : nullptr;

	// The invisible resize margin around windows above whatever we hit.
	for (View *view : views) {
		if (view == owner) {
			break;
		}
		if (!view->visible() || !view->server_side) {
			continue;
		}
		uint32_t edges = 0;
		if (view->region_at(lx, ly, &edges) == Region::Edge) {
			hit.view = view;
			hit.region = Region::Edge;
			hit.edges = edges;
			return hit;
		}
	}

	if (!owner) {
		return hit;   // the desktop
	}
	hit.view = owner;
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		if (wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer)) {
			hit.region = Region::Content;
			hit.surface = scene_surface->surface;
			hit.sx = sx;
			hit.sy = sy;
			return hit;
		}
	}
	hit.region = owner->region_at(lx, ly, &hit.edges);
	if (hit.region == Region::Content || hit.region == Region::None) {
		hit.region = Region::Titlebar;   // our frame, but not a button or edge
	}
	return hit;
}

bool Server::locked() const {
	return lock && lock->active();
}

void Server::process_motion(uint32_t time) {
	const double cx = cursor->x, cy = cursor->y;
	if (locked()) {
		// Apps get no pointer while it's locked.
		cursor_mode = CursorMode::Passthrough;
		grab_view = nullptr;
		wlr_seat_pointer_notify_clear_focus(seat);
		lock->motion(cx, cy);
		return;
	}
	if (shot_tool && shot_tool->active()) {
		shot_tool->motion(cx, cy);
		return;
	}
	if (panel_drag) {
		if (Panel *p = panel_drag()) p->pin_drag(cx, cy);
		set_cursor("grabbing");
		return;
	}
	if (osd_grab) {
		// Dragging along the OSD's bar: keep setting the level as it moves.
		osd->pointer_set(cx, cy);
		return;
	}

	if (cursor_mode == CursorMode::Move && grab_view) {
		View *view = grab_view;
		if (view->maximized || view->snap != View::Snap::None) {
			// Don't unmaximise on a plain click; wait for a real drag.
			if (std::hypot(cx - press_x, cy - press_y) < 8) {
				return;
			}
			view->unsnap_for_drag(cx, cy);
			grab_x = cx - view->x;
			grab_y = cy - view->y;
		}
		int nx = static_cast<int>(std::lround(cx - grab_x));
		int ny = static_cast<int>(std::lround(cy - grab_y));
		// The title bar never goes under a panel: it stays between the top
		// panel and the bottom one of the screen the pointer is on.
		if (Output *here = output_at(cx, cy)) {
			const wlr_box u = here->usable();
			const int bar = view->decorated() ? view->deco_top() : 40;
			ny = std::clamp(ny, u.y, std::max(u.y, u.y + u.height - bar));
		}
		view->move_to(nx, ny);
		update_drop_target(view, cx, cy);
		return;
	}

	if (cursor_mode == CursorMode::Resize && grab_view) {
		View *view = grab_view;
		const double dx = cx - grab_x, dy = cy - grab_y;
		const wlr_box &b = grab_box;
		// Never smaller than the app says it can be drawn.
		const int min_w = std::max(160, view->min_width() + view->deco_width());
		const int min_h = std::max(view->deco_height() + 80, view->min_height() + view->deco_height());
		int left = b.x, right = b.x + b.width, top = b.y, bottom = b.y + b.height;
		if (resize_edges & WLR_EDGE_LEFT) {
			left = std::min(static_cast<int>(std::lround(b.x + dx)), right - min_w);
		} else if (resize_edges & WLR_EDGE_RIGHT) {
			right = std::max(static_cast<int>(std::lround(b.x + b.width + dx)), left + min_w);
		}
		if (resize_edges & WLR_EDGE_TOP) {
			top = std::min(static_cast<int>(std::lround(b.y + dy)), bottom - min_h);
			if (Output *here = output_at(cx, cy)) {
				top = std::max(top, here->usable().y);   // not up under the top panel
			}
		} else if (resize_edges & WLR_EDGE_BOTTOM) {
			bottom = std::max(static_cast<int>(std::lround(b.y + b.height + dy)), top + min_h);
		}
		view->request_content_size(right - left - view->deco_width(),
			bottom - top - view->deco_height());
		return;
	}

	// Dragging a control centre slider, or the media position.
	if (cc_grab) {
		control_centre->motion(cx, cy);
		return;
	}
	if (popover_grab) {
		popover_grab->motion(cx, cy);
		return;
	}
	if (desktop_grab) {
		desktop->motion(cx, cy, true);   // pressing or dragging a desktop icon
		return;
	}
	if (launcher->dragging_bar()) {
		launcher->drag_bar(cx, cy);
		return;
	}

	// Auto-hiding panels show when the pointer touches their edge.
	if (Output *here = output_at(cx, cy)) {
		for (auto &panel : here->panels) {
			panel->pointer_moved(cx, cy);
		}
	}
	if (cursor_mode == CursorMode::Passthrough && buttons_down == 0 && !seat->drag) {
		check_hot_corner(cx, cy);
	}

	// Dragging something between apps: whatever is under the pointer gets
	// to see it (and can accept the drop).
	if (seat->drag) {
		if (drag_icon) {
			wlr_scene_node_set_position(&drag_icon->node, static_cast<int>(cx), static_cast<int>(cy));
		}
		if (own_drag_picture) {
			wlr_scene_node_set_position(&own_drag_picture->node, static_cast<int>(cx) - own_drag_dx,
				static_cast<int>(cy) - own_drag_dy);
		}
		const Hit hit = hit_test(cx, cy);
		if (hit.surface) {
			wlr_seat_pointer_notify_enter(seat, hit.surface, hit.sx, hit.sy);
			wlr_seat_pointer_notify_motion(seat, time, hit.sx, hit.sy);
		} else {
			wlr_seat_pointer_notify_clear_focus(seat);
		}
		return;
	}

	// While a button is held on a window, it keeps getting the pointer.
	if (buttons_down > 0 && implicit_grab_surface) {
		wlr_seat_pointer_notify_motion(seat, time, cx - implicit_origin_x,
			cy - implicit_origin_y);
		return;
	}

	const Hit hit = hit_test(cx, cy);

	// Titlebar button hover.
	const bool on_button = hit.region == Region::Close || hit.region == Region::Maximize ||
		hit.region == Region::Minimize;
	View *new_hover = on_button ? hit.view : nullptr;
	if (hover_view && (hover_view != new_hover || hover_view->hover != hit.region)) {
		hover_view->hover = Region::None;
		hover_view->redraw_frame();
	}
	if (new_hover && new_hover->hover != hit.region) {
		new_hover->hover = hit.region;
		new_hover->redraw_frame();
	}
	hover_view = new_hover;

	// Resting on the maximise button opens the snap layouts (like Windows 11).
	View *on_max = hit.region == Region::Maximize ? hit.view : nullptr;
	if (on_max != max_hover_view) {
		max_hover_view = on_max;
		wl_event_source_timer_update(max_hover_timer, on_max && !config.snap_layouts.empty() ? 450 : 0);
	}
	if (snap_layouts->menu_open() && !on_max && hit.popover != snap_layouts.get()) {
		// Moved away from both the button and the layouts: close them.
		snap_layouts->close();
	}

	for (Output *output : outputs) {
		for (auto &panel : output->panels) {
			if (panel.get() == hit.panel) {
				panel->motion(cx, cy);
			} else {
				panel->leave();
			}
		}
	}
	if (hit.launcher) {
		launcher->motion(cx, cy);
	} else {
		launcher->leave();
	}
	if (hit.control_centre) {
		control_centre->motion(cx, cy);
	} else {
		control_centre->leave();
	}
	for (Popover *p : popovers()) {
		if (hit.popover == p) {
			p->motion(cx, cy);
		} else {
			p->leave();
		}
	}
	if (notifications) {
		if (hit.toast) {
			notifications->toast_motion(hit.toast, cx, cy);
		} else {
			notifications->toast_leave();
		}
	}
	desktop->motion(cx, cy, hit.desktop);
	if (overview && overview->is_open()) {
		if (hit.overview) overview->motion(cx, cy);
		if (!hit.surface) set_cursor("default");
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (config.focus_follows_mouse && hit.view && hit.view != focused &&
			cursor_mode == CursorMode::Passthrough && !launcher->is_open() &&
			!control_centre->is_open()) {
		// Focus without raising, so overlapping windows stay put.
		View *prev = focused;
		focused = hit.view;
		if (prev) {
			prev->set_activated(false);
		}
		hit.view->set_activated(true);
		views.remove(hit.view);
		views.push_front(hit.view);
		focus_surface(hit.view->surface());
		views_changed();
	}

	if (hit.surface) {
		cursor_image.clear();   // the app picks its own cursor
		wlr_seat_pointer_notify_enter(seat, hit.surface, hit.sx, hit.sy);
		wlr_seat_pointer_notify_motion(seat, time, hit.sx, hit.sy);
	} else {
		set_cursor(hit.region == Region::Edge ? cursor_for_edges(hit.edges) : "default");
		wlr_seat_pointer_notify_clear_focus(seat);
	}
}

void Server::handle_cursor_motion(void *data) {
	auto *event = static_cast<wlr_pointer_motion_event *>(data);
	note_activity();
	double dx = event->delta_x, dy = event->delta_y;
	// Games read the raw movement (mouse look), whatever the pointer does.
	if (relative_pointer) {
		wlr_relative_pointer_manager_v1_send_relative_motion(relative_pointer, seat,
			static_cast<uint64_t>(event->time_msec) * 1000, dx, dy, event->unaccel_dx, event->unaccel_dy);
	}
	// A game holding the pointer: it stays put (locked) or inside its area (confined).
	if (active_constraint && cursor_mode == CursorMode::Passthrough && !(shot_tool && shot_tool->active())) {
		const Hit hit = hit_test(cursor->x, cursor->y);
		if (hit.surface == active_constraint->surface) {
			if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
				return;
			}
			double nx = 0, ny = 0;
			if (!wlr_region_confine(&active_constraint->region, hit.sx, hit.sy, hit.sx + dx, hit.sy + dy, &nx, &ny)) {
				return;
			}
			dx = nx - hit.sx;
			dy = ny - hit.sy;
		}
	}
	edge_resist(dx, dy);
	wlr_cursor_move(cursor, &event->pointer->base, dx, dy);
	process_motion(event->time_msec);
}

void Server::handle_cursor_motion_absolute(void *data) {
	auto *event = static_cast<wlr_pointer_motion_absolute_event *>(data);
	note_activity();
	wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
	process_motion(event->time_msec);
}

void Server::handle_cursor_button(void *data) {
	auto *event = static_cast<wlr_pointer_button_event *>(data);
	note_activity();
	wl_event_source_timer_update(max_hover_timer, 0);   // clicked: no layouts flyout
	const bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
	const double cx = cursor->x, cy = cursor->y;

	if (locked()) {
		if (pressed) lock->press(cx, cy, event->button);
		buttons_down = 0;
		return;
	}
	if (shot_tool && shot_tool->active()) {
		if (!pressed && buttons_down > 0) buttons_down--;
		else if (pressed) buttons_down++;
		shot_tool->button(cx, cy, event->button, pressed);
		return;
	}

	if (!pressed) {
		if (buttons_down > 0) {
			buttons_down--;
		}
		if (osd_grab) {
			osd_grab = false;
			return;
		}
		if (panel_drag) {
			auto find = std::move(panel_drag);
			panel_drag = nullptr;
			if (Panel *p = find()) p->pin_release(cx, cy);
			return;
		}
		if (desktop_grab) {
			wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
			desktop->release(cx, cy);
			return;
		}
		if (launcher->dragging_bar()) {
			launcher->end_drag();
			return;
		}
		if (overview && overview->is_open() && !cc_grab && !popover_grab) {
			overview->release(cx, cy, event->button);
			process_motion(event->time_msec);
			return;
		}
		if (seat->drag && drop_on_desktop(event->time_msec, event->button)) {
			return;   // the drag ends once the files have been read
		}
		if (cc_grab) {
			control_centre->release();
			return;
		}
		if (popover_grab) {
			popover_grab->release();
			return;
		}
		if (cursor_mode != CursorMode::Passthrough) {
			View *view = grab_view;
			const CursorMode mode = cursor_mode;
			cursor_mode = CursorMode::Passthrough;
			grab_view = nullptr;
			if (view && mode == CursorMode::Resize && view->ready()) {
				view->send_resizing(false);
			}
			// Let go over a snap layout zone or a screen edge: tile it there,
			// on the screen the pointer is on (only after a real drag).
			const bool dragged = std::hypot(cx - press_x, cy - press_y) >= 8;
			if (view && mode == CursorMode::Move && dragged) {
				apply_drop(view);
			}
			drop_kind = Drop::None;
			hide_snap_preview();
			snap_layouts->drag_end();
		}
		if (pressed_view) {
			View *view = pressed_view;
			const Region button = view->pressed;
			view->pressed = Region::None;
			pressed_view = nullptr;
			const Hit hit = hit_test(cx, cy);
			if (hit.view == view && hit.region == button) {
				if (button == Region::Close) {
					view->close();
				} else if (button == Region::Maximize) {
					view->toggle_maximize();
				} else if (button == Region::Minimize) {
					view->minimize();
				}
			}
			view->redraw_frame();
		}
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
		if (buttons_down == 0) {
			implicit_grab_surface = nullptr;
		}
		process_motion(event->time_msec);
		return;
	}

	buttons_down++;
	const Hit hit = hit_test(cx, cy);
	// An open right-click menu goes first, by where it is on the screen: it
	// can be over the control centre (the media card's menu), and nothing
	// under it should get the click.
	if (menu->is_open() && menu->contains(cx, cy)) {
		menu->press(cx, cy, event->button);
		return;
	}
	// A click anywhere but the menu closes it.
	if (menu->is_open() && hit.popover != menu.get()) {
		menu->close();
	}
	// The volume and brightness OSD is drawn on the top layer: while a bar
	// is up, pressing on it sets the level (and the drag continues until the
	// button is let go, wherever the pointer goes).
	if (osd && event->button == BTN_LEFT && osd->grabs_input(cx, cy)) {
		osd_grab = true;
		osd->pointer_set(cx, cy);
		return;
	}

	if (launcher->is_open()) {
		if (hit.launcher) {
			launcher->click(cx, cy, event->button);
			return;
		}
		if (!hit.panel && hit.popover != menu.get()) {
			launcher->close();   // clicking elsewhere closes the menu (not its power menu)
		}
	}
	if (control_centre->is_open()) {
		if (hit.control_centre) {
			control_centre->press(cx, cy, event->button);
			return;
		}
		if (!hit.panel && !hit.toast && !hit.popover) {
			control_centre->close();
		}
	}
	if (hit.popover) {
		hit.popover->press(cx, cy, event->button);
		return;
	}
	if (overview && overview->shown() && !hit.panel && !hit.toast) {
		calendar->close();
		media_card->close();
		clipboard_card->close();
		overview->press(cx, cy, event->button);
		return;
	}
	if (!hit.panel) {
		calendar->close();
		media_card->close();
		clipboard_card->close();
	}
	if (hit.toast) {
		notifications->toast_click(hit.toast, cx, cy, event->button);
		return;
	}
	if (hit.panel) {
		if (event->button == BTN_LEFT && hit.panel->pin_press(cx, cy)) {
			panel_drag = hit.panel->finder();   // a click or a drag: known when it's let go
			return;
		}
		hit.panel->click(cx, cy, event->button);
		return;
	}
	if (hit.desktop) {
		// An icon on the desktop: windows lose focus, like clicking any desktop.
		focus(nullptr);
		wlr_keyboard *kbd = wlr_seat_get_keyboard(seat);
		const bool ctrl = kbd && (wlr_keyboard_get_modifiers(kbd) & WLR_MODIFIER_CTRL);
		// The seat counts the button too: a drag into an app started from
		// here ends when it's let go.
		wlr_seat_pointer_notify_clear_focus(seat);
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
		desktop->press(cx, cy, event->button, event->time_msec, ctrl);
		return;
	}
	if (!hit.view) {
		if (hit.surface) {   // an X11 menu
			wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
				event->state);
			implicit_grab_surface = hit.surface;
			implicit_origin_x = cx - hit.sx;
			implicit_origin_y = cy - hit.sy;
			return;
		}
		// The bare desktop.
		if (desktop->enabled()) {
			focus(nullptr);
			desktop->click_empty();
		}
		if (event->button == BTN_RIGHT) {
			show_desktop_menu(cx, cy);
		}
		return;
	}

	View *view = hit.view;
	focus(view);

	// Super + drag moves (left button) or resizes (right button) from anywhere.
	wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	const bool super = kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO);
	if (super && !view->fullscreen) {
		if (event->button == BTN_LEFT) {
			begin_move(view);
			return;
		}
		if (event->button == BTN_RIGHT) {
			const wlr_box f = view->frame_box();
			uint32_t edges = 0;
			edges |= cx < f.x + f.width / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
			edges |= cy < f.y + f.height / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
			begin_resize(view, edges);
			return;
		}
	}

	switch (hit.region) {
	case Region::Content:
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
		implicit_grab_surface = hit.surface;
		implicit_origin_x = cx - hit.sx;
		implicit_origin_y = cy - hit.sy;
		break;
	case Region::Titlebar:
		if (event->button == BTN_LEFT) {
			const bool double_click = last_title_click_view == view &&
				event->time_msec - last_title_click_time < 400;
			if (double_click) {
				last_title_click_view = nullptr;
				if (config.double_click == "maximize") {
					view->toggle_maximize();
				} else if (config.double_click == "minimize") {
					view->minimize();
				}
			} else {
				last_title_click_view = view;
				last_title_click_time = event->time_msec;
				begin_move(view);
			}
		} else if (event->button == BTN_MIDDLE && config.middle_click == "minimize") {
			view->minimize();
		}
		break;
	case Region::Close:
	case Region::Maximize:
	case Region::Minimize:
		if (event->button == BTN_LEFT) {
			view->pressed = hit.region;
			pressed_view = view;
			view->redraw_frame();
		}
		break;
	case Region::Edge:
		if (event->button == BTN_LEFT) {
			begin_resize(view, hit.edges);
		}
		break;
	case Region::None:
		break;
	}
}

void Server::handle_cursor_axis(void *data) {
	auto *event = static_cast<wlr_pointer_axis_event *>(data);
	note_activity();
	if (locked()) return;
	// Scrolling on the OSD's bar steps it, like the keyboard does.
	if (osd && osd->grabs_input(cursor->x, cursor->y)) {
		osd->pointer_scroll(event->delta);
		return;
	}
	if (launcher->contains(cursor->x, cursor->y)) {
		if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
			launcher->scroll(event->delta, event->source == WL_POINTER_AXIS_SOURCE_WHEEL ||
				event->source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT);
		}
		return;
	}
	if (control_centre->contains(cursor->x, cursor->y)) {
		if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
			control_centre->scroll(cursor->x, cursor->y, event->delta);
		}
		return;
	}
	for (Popover *p : popovers()) {
		if (p->contains(cursor->x, cursor->y)) {
			if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
				p->scroll(cursor->x, cursor->y, event->delta);
			}
			return;
		}
	}
	if (overview && overview->is_open()) {
		overview->scroll(event->delta, event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL);
		return;
	}
	// Scrolling on a panel's overview button switches workspaces.
	{
		const Hit hit = hit_test(cursor->x, cursor->y);
		if (hit.panel && hit.panel->scroll(cursor->x, cursor->y, event->delta)) {
			return;
		}
	}
	wlr_seat_pointer_notify_axis(seat, event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
}

void Server::handle_cursor_frame(void *) {
	wlr_seat_pointer_notify_frame(seat);
}

void Server::handle_request_cursor(void *data) {
	auto *event = static_cast<wlr_seat_pointer_request_set_cursor_event *>(data);
	if (cursor_mode == CursorMode::Passthrough &&
			seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

void Server::handle_request_cursor_shape(void *data) {
	auto *event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event *>(data);
	if (cursor_mode != CursorMode::Passthrough ||
			event->seat_client != seat->pointer_state.focused_client) {
		return;
	}
	cursor_image.clear();   // the app chose this one
	wlr_cursor_set_xcursor(cursor, cursor_manager, wlr_cursor_shape_v1_name(event->shape));
}

namespace {

// Reads a GNOME desktop setting, which GTK apps follow (empty if unset).
std::string ask_gsettings_b(const char *key) {
	const std::string command =
		std::string("gsettings get org.gnome.desktop.interface ") + key + " 2>/dev/null";
	std::string out;
	if (FILE *pipe = popen(command.c_str(), "r")) {
		char buffer[256];
		while (fgets(buffer, sizeof buffer, pipe)) {
			out += buffer;
		}
		pclose(pipe);
	}
	// "'Adwaita'\n" -> "Adwaita";  "uint32 24" / "24" -> "24"
	std::string clean;
	for (char c : out) {
		if (c != '\'' && c != '\n') {
			clean += c;
		}
	}
	const auto space = clean.rfind(' ');
	return space == std::string::npos ? clean : clean.substr(space + 1);
}

std::string read_gnome_setting(const char *key) {
	// Asking gsettings starts a program and waits for it: remember answers
	// for a minute so settings changes stay quick.
	static std::map<std::string, std::pair<std::string, double>> cache;
	const double now = now_ms();
	if (auto it = cache.find(key); it != cache.end() && now - it->second.second < 60000) {
		return it->second.first;
	}
	std::string value = ask_gsettings_b(key);
	cache[key] = {value, now};
	return value;
}

} // namespace

// One cursor theme for everything: Dusk's own cursor, apps that let Dusk draw
// their cursor, X11 apps, and (through the environment and GNOME settings)
// apps that draw their own.
// Is the icon or cursor theme `name` installed? (`probe`: "cursors" for a
// cursor theme, "index.theme" for icons.)
bool theme_installed(const std::string &name, const char *probe) {
	if (name.empty()) return false;
	std::vector<std::string> dirs;
	if (const char *home = getenv("HOME")) {
		dirs.push_back(std::string(home) + "/.local/share/icons");
		dirs.push_back(std::string(home) + "/.icons");
	}
	const char *xdg = getenv("XDG_DATA_DIRS");
	std::string list = xdg && *xdg ? xdg : "/usr/local/share:/usr/share";
	size_t at = 0;
	while (at <= list.size()) {
		const size_t end = list.find(':', at);
		const std::string d = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
		if (!d.empty()) dirs.push_back(d + "/icons");
		if (end == std::string::npos) break;
		at = end + 1;
	}
	for (const std::string &d : dirs) {
		if (access((d + "/" + name + "/" + probe).c_str(), F_OK) == 0) return true;
	}
	return false;
}

// The icon theme to use: the one picked (Papirus follows light/dark), if
// it's installed; otherwise GNOME's.
std::string Server::effective_icon_theme() const {
	std::string t = config.icon_theme;
	if (t == "Papirus" && config.dark && theme_installed("Papirus-Dark", "index.theme")) t = "Papirus-Dark";
	return theme_installed(t, "index.theme") ? t : std::string();
}

void Server::load_cursor_theme() {
	std::string theme = theme_installed(config.cursor_theme, "cursors") ? config.cursor_theme : std::string();
	int size = config.cursor_size;
	if (theme.empty()) {
		theme = read_gnome_setting("cursor-theme");
	}
	if (size <= 0) {
		size = std::atoi(read_gnome_setting("cursor-size").c_str());
	}
	if (theme.empty()) {
		const char *env = getenv("XCURSOR_THEME");
		theme = env && *env ? env : "Adwaita";
	}
	if (size <= 0) {
		const char *env = getenv("XCURSOR_SIZE");
		size = env && std::atoi(env) > 0 ? std::atoi(env) : 24;
	}
	if (cursor_manager && theme == cursor_theme && size == cursor_size) {
		return;
	}
	cursor_theme = theme;
	cursor_size = size;
	setenv("XCURSOR_THEME", theme.c_str(), 1);
	setenv("XCURSOR_SIZE", std::to_string(size).c_str(), 1);
	// Where to find cursor themes, for apps that bring their own (older)
	// libraries, like AppImages: yours first, then the system's.
	if (const char *home = getenv("HOME"); home && *home) {
		const std::string h = home;
		setenv("XCURSOR_PATH", (h + "/.local/share/icons:" + h + "/.icons:/usr/share/icons:/usr/share/pixmaps").c_str(), 1);
	}

	if (cursor_manager) {
		wlr_cursor_unset_image(cursor);   // it still points into the old theme
		wlr_xcursor_manager_destroy(cursor_manager);
	}
	cursor_manager = wlr_xcursor_manager_create(theme.c_str(), static_cast<uint32_t>(size));
	cursor_image.clear();
	set_cursor("default");
#ifdef DUSK_XWAYLAND
	update_xwayland_cursor();
#endif

	// Chosen in Dusk Settings: tell GTK apps too (they read GNOME's settings).
	std::string safe;
	for (char c : theme) {
		if (c != '\'' && c != '\\') {
			safe += c;
		}
	}
	// X11 apps (Steam, older games, Wine) pick their cursor themselves: from
	// XCURSOR_THEME when they start, else the "default" theme in ~/.icons, and
	// the X resources. All three say this theme; and a theme only in
	// ~/.local/share/icons gets a link in ~/.icons, where older X libraries
	// (Steam's own) look.
	if (session) {
		const char *home = getenv("HOME");
		if (home && *home && safe == theme) {
			const std::string h = home;
			const std::string script =
				"mkdir -p '" + h + "/.icons/default' && "
				"printf '[Icon Theme]\\nName=Default\\nInherits=%s\\n' '" + safe + "' > '" + h + "/.icons/default/index.theme'; "
				"mkdir -p '" + h + "/.local/share/icons/default' && "
				"cp '" + h + "/.icons/default/index.theme' '" + h + "/.local/share/icons/default/index.theme'; "
				"[ -d '" + h + "/.local/share/icons/" + safe + "' ] && [ ! -e '" + h + "/.icons/" + safe + "' ] && "
				"ln -s '" + h + "/.local/share/icons/" + safe + "' '" + h + "/.icons/" + safe + "'; "
				"command -v xrdb >/dev/null && [ -n \"$DISPLAY\" ] && printf 'Xcursor.theme: %s\\nXcursor.size: %s\\n' '" +
				safe + "' '" + std::to_string(size) + "' | xrdb -merge; "
				// GTK apps that can't read GNOME's settings (AppImages and
				// Flatpaks with their own GTK) read settings.ini.
				"for v in 3.0 4.0; do f='" + h + "/.config/gtk-'$v/settings.ini; mkdir -p \"$(dirname \"$f\")\"; "
				"[ -f \"$f\" ] || printf '[Settings]\\n' > \"$f\"; "
				"grep -q '^\\[Settings\\]' \"$f\" || sed -i '1i [Settings]' \"$f\"; "
				"sed -i '/^gtk-cursor-theme-name=/d;/^gtk-cursor-theme-size=/d' \"$f\"; "
				"sed -i '/^\\[Settings\\]/a gtk-cursor-theme-name=" + safe + "\\ngtk-cursor-theme-size=" + std::to_string(size) + "' \"$f\"; done; "
				"dbus-update-activation-environment --systemd XCURSOR_THEME XCURSOR_SIZE XCURSOR_PATH 2>/dev/null; true";
			spawn(script);
		}
	}
	if (theme_installed(config.cursor_theme, "cursors")) {
		spawn("gsettings set org.gnome.desktop.interface cursor-theme '" + safe + "'");
	}
	if (config.cursor_size > 0) {
		spawn("gsettings set org.gnome.desktop.interface cursor-size " + std::to_string(size));
	}
}

void Server::handle_request_selection(void *data) {
	auto *event = static_cast<wlr_seat_request_set_selection_event *>(data);
	wlr_seat_set_selection(seat, event->source, event->serial);
}

// Selected text becomes the "primary selection", which a middle-click
// pastes. Settings can turn that off: then nothing is ever selected for it.
void Server::handle_request_primary_selection(void *data) {
	auto *event = static_cast<wlr_seat_request_set_primary_selection_event *>(data);
	if (!config.middle_click_paste) {
		if (getenv("DUSK_DEBUG")) std::fprintf(stderr, "dusk: primary selection refused (turned off)\n");
		return;
	}
	if (getenv("DUSK_DEBUG")) {
		std::fprintf(stderr, "dusk: primary selection set (%s)\n", event->source ? "text" : "cleared");
	}
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

// An app wants to start dragging something (a file, text, a picture): only
// if it really is holding the mouse button down on itself.
void Server::handle_request_start_drag(void *data) {
	auto *event = static_cast<wlr_seat_request_start_drag_event *>(data);
	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	} else {
		wlr_data_source_destroy(event->drag->source);
	}
}

// Files dragged from an app and let go over the desktop go into the Desktop
// folder. The app is asked for them first; the drag only ends (as "not
// taken", for the app) once they've been read, so the app is still there to
// hand them over.
bool Server::drop_on_desktop(uint32_t time, uint32_t button) {
	wlr_drag *drag = seat->drag;
	if (!desktop || !clipboard || drop_waiting || !drag || !drag->source) {
		return false;
	}
	if (drag->source == own_drag_source) {
		return false;   // desktop icons brought back: the drag just ends
	}
	const double cx = cursor->x, cy = cursor->y;
	if (!desktop->takes_drop(cx, cy)) {
		return false;
	}
	const Hit hit = hit_test(cx, cy);
	if (hit.surface || hit.view || hit.panel || hit.popover || hit.launcher || hit.control_centre || hit.toast) {
		return false;   // over an app or Dusk's own things: not the desktop
	}
	bool files = false;
	char **types = static_cast<char **>(drag->source->mime_types.data);
	for (size_t i = 0; i < drag->source->mime_types.size / sizeof(char *); i++) {
		files = files || (types[i] && std::strcmp(types[i], "text/uri-list") == 0);
	}
	if (!files) {
		return false;
	}
	wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	const bool copy = kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_CTRL);
	drop_waiting = true;
	drop_finish = [this, time, button] {
		drop_waiting = false;
		wl_event_source_timer_update(drop_timer, 0);
		wlr_seat_pointer_notify_button(seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
		process_motion(time);
	};
	clipboard->read(drag->source, "text/uri-list", 256 * 1024, [this, copy, cx, cy](std::string text) {
		desktop->drop_files(parse_uri_list(text), copy, cx, cy);
		if (drop_finish) {
			auto finish = std::move(drop_finish);
			drop_finish = nullptr;
			finish();
		}
	});
	wl_event_source_timer_update(drop_timer, 1500);   // in case the app never answers
	return true;
}

// Desktop icons dragged onto an app: from here on it's a normal Wayland
// drag, as if a file manager had started it, so the app can take the files.
// wlroots needs a client to own a drag: the app under the pointer does.
bool Server::start_file_drag(const std::vector<std::string> &paths, wlr_buffer *picture, int grab_x,
		int grab_y, wlr_surface *over) {
	if (seat->drag || !over || !over->resource || !clipboard || paths.empty()) {
		return false;
	}
	wlr_seat_client *origin = wlr_seat_client_for_wl_client(seat, wl_resource_get_client(over->resource));
	if (!origin) {
		return false;
	}
	wlr_data_source *source = clipboard->drag_source(paths);
	wlr_drag *drag = wlr_drag_create(origin, source, nullptr);
	if (!drag) {
		wlr_data_source_destroy(source);
		return false;
	}
	own_drag_source = source;
	on_own_drag_destroy.connect<&Server::handle_own_drag_destroy>(&drag->events.destroy, this);
	on_own_drag_origin_gone.connect<&Server::handle_own_drag_origin_gone>(&origin->events.destroy, this);
	if (picture) {
		own_drag_picture = wlr_scene_buffer_create(layer_overlay, picture);
		own_drag_picture->point_accepts_input = [](wlr_scene_buffer *, double *, double *) { return false; };
		wlr_scene_buffer_set_opacity(own_drag_picture, 0.85f);
		own_drag_dx = grab_x;
		own_drag_dy = grab_y;
		wlr_scene_node_set_position(&own_drag_picture->node, static_cast<int>(cursor->x) - grab_x,
			static_cast<int>(cursor->y) - grab_y);
	}
	wlr_seat_start_pointer_drag(seat, drag, wl_display_next_serial(display));
	process_motion(0);   // the app under the pointer sees it at once
	return true;
}

void Server::handle_own_drag_destroy(void *) {
	on_own_drag_destroy.disconnect();
	on_own_drag_origin_gone.disconnect();
	own_drag_source = nullptr;   // wlroots frees it after the drop
	if (own_drag_picture) {
		wlr_scene_node_destroy(&own_drag_picture->node);
		own_drag_picture = nullptr;
	}
}

// The app that stood in as the drag's owner quit mid-drag: end the drag.
void Server::handle_own_drag_origin_gone(void *) {
	on_own_drag_origin_gone.disconnect();
	if (seat->drag) {
		wlr_seat_pointer_end_grab(seat);
	}
}

int Server::handle_drop_timer(void *data) {
	auto *self = static_cast<Server *>(data);
	if (self->drop_finish) {
		auto finish = std::move(self->drop_finish);
		self->drop_finish = nullptr;
		finish();
	}
	return 0;
}

// The drag started: its picture follows the pointer.
void Server::handle_start_drag(void *data) {
	auto *drag = static_cast<wlr_drag *>(data);
	implicit_grab_surface = nullptr;   // the drag decides who gets the pointer now
	if (!drag->icon) {
		return;
	}
	drag_icon = wlr_scene_drag_icon_create(layer_overlay, drag->icon);
	wlr_scene_node_set_position(&drag_icon->node, static_cast<int>(cursor->x),
		static_cast<int>(cursor->y));
	on_drag_icon_destroy.disconnect();
	on_drag_icon_destroy.connect<&Server::handle_drag_icon_destroy>(&drag->icon->events.destroy, this);
}

void Server::handle_drag_icon_destroy(void *) {
	on_drag_icon_destroy.disconnect();
	drag_icon = nullptr;   // its scene tree goes away with it
}

// ============================================================================
// Keyboards and shortcuts
// ============================================================================

void Server::handle_virtual_keyboard(void *data) {
	auto *kbd = static_cast<wlr_virtual_keyboard_v1 *>(data);
	keyboards.push_back(new Keyboard(this, &kbd->keyboard, true));
	wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

void Server::handle_virtual_pointer(void *data) {
	auto *event = static_cast<wlr_virtual_pointer_v1_new_pointer_event *>(data);
	wlr_input_device *device = &event->new_pointer->pointer.base;
	pointers.push_back(new Pointer(this, device));
	wlr_cursor_attach_input_device(cursor, device);
	if (event->suggested_output) {
		wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
	}
}

void Server::handle_new_input(void *data) {
	auto *device = static_cast<wlr_input_device *>(data);
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		keyboards.push_back(new Keyboard(this, wlr_keyboard_from_input_device(device)));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		pointers.push_back(new Pointer(this, device));
		wlr_cursor_attach_input_device(cursor, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!keyboards.empty()) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(seat, caps);
}

Pointer::Pointer(Server *server, wlr_input_device *device)
		: server(server), device(device) {
	on_destroy.connect<&Pointer::handle_destroy>(&device->events.destroy, this);
	apply_settings();
}

// Tap to click, natural scrolling and speed, from Settings.
void Pointer::apply_settings() {
	if (!wlr_input_device_is_libinput(device)) {
		return;   // e.g. running in a window inside another desktop
	}
	libinput_device *li = wlr_libinput_get_device_handle(device);
	const Config &c = server->config;
	const bool touchpad = libinput_device_config_tap_get_finger_count(li) > 0;
	if (touchpad) {
		libinput_device_config_tap_set_enabled(li, c.tap_to_click
			? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	}
	if (libinput_device_config_scroll_has_natural_scroll(li)) {
		libinput_device_config_scroll_set_natural_scroll_enabled(li,
			touchpad ? c.natural_scroll_touchpad : c.natural_scroll_mouse);
	}
	if (libinput_device_config_accel_is_available(li)) {
		libinput_device_config_accel_set_speed(li, c.pointer_speed);
		const uint32_t profiles = libinput_device_config_accel_get_profiles(li);
		const auto want = c.accel_profile == "flat" ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
			: LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
		if (profiles & want) {
			libinput_device_config_accel_set_profile(li, want);
		}
	}
	if (libinput_device_config_left_handed_is_available(li)) {
		libinput_device_config_left_handed_set(li, c.left_handed);
	}
	if (libinput_device_config_dwt_is_available(li)) {
		libinput_device_config_dwt_set_enabled(li, c.disable_while_typing
			? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	}
	if (c.click_method != "default") {
		const uint32_t methods = libinput_device_config_click_get_methods(li);
		const auto want = c.click_method == "fingers" ? LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
			: LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
		if (methods & want) {
			libinput_device_config_click_set_method(li, want);
		}
	} else {
		libinput_device_config_click_set_method(li, libinput_device_config_click_get_default_method(li));
	}
	if (libinput_device_config_middle_emulation_is_available(li)) {
		libinput_device_config_middle_emulation_set_enabled(li, c.middle_emulation
			? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
	}
}

void Pointer::handle_destroy(void *) {
	Server *s = server;
	s->pointers.erase(std::remove(s->pointers.begin(), s->pointers.end(), this),
		s->pointers.end());
	delete this;
}

Keyboard::Keyboard(Server *server, wlr_keyboard *keyboard, bool is_virtual)
		: server(server), keyboard(keyboard), is_virtual(is_virtual) {
	apply_settings();
	// Num Lock / Caps Lock when it starts (Settings › Keyboard), and for a
	// keyboard plugged in later.
	if (!is_virtual) {
		const Config &c = server->config;
		auto want = [](const std::string &v) { return v == "on" ? 1 : v == "off" ? 0 : -1; };
		int num = want(c.numlock_on_start), caps = want(c.capslock_on_start);
		// One plugged in later matches the others (what you've switched since).
		for (Keyboard *k : server->keyboards) {
			if (k != this && !k->is_virtual && k->keyboard->xkb_state) {
				num = k->num ? 1 : 0;
				caps = k->caps ? 1 : 0;
				break;
			}
		}
		set_locks(num, caps);
	}

	on_modifiers.connect<&Keyboard::handle_modifiers>(&keyboard->events.modifiers, this);
	on_key.connect<&Keyboard::handle_key>(&keyboard->events.key, this);
	on_destroy.connect<&Keyboard::handle_destroy>(&keyboard->base.events.destroy, this);

	wlr_seat_set_keyboard(server->seat, keyboard);
}

// Layout and key repeat, from Settings.
void Keyboard::apply_settings() {
	const Config &c = server->config;
	if (is_virtual) {
		return;   // a program's keyboard brings its own layout
	}
	auto or_null = [](const std::string &s) { return s.empty() ? nullptr : s.c_str(); };
	xkb_rule_names names{};
	names.layout = or_null(c.keyboard_layout);
	names.variant = or_null(c.keyboard_variant);
	names.options = or_null(c.keyboard_options);

	xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "Unknown keyboard layout \"%s\", using the default",
			c.keyboard_layout.c_str());
		keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	if (keymap) {
		// A new layout starts with the locks off: keep them as they were.
		const bool had_state = keyboard->xkb_state != nullptr;
		const bool was_num = num, was_caps = caps;
		wlr_keyboard_set_keymap(keyboard, keymap);
		xkb_keymap_unref(keymap);
		if (had_state) set_locks(was_num ? 1 : 0, was_caps ? 1 : 0);
	}
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(keyboard, c.repeat_rate, c.repeat_delay);
	if (keyboard->xkb_state) {
		caps = xkb_state_led_name_is_active(keyboard->xkb_state, XKB_LED_NAME_CAPS) > 0;
		num = xkb_state_led_name_is_active(keyboard->xkb_state, XKB_LED_NAME_NUM) > 0;
	}
	layout = keyboard->modifiers.group;
}

void Keyboard::set_locks(int num_lock, int caps_lock) {
	if (!keyboard->keymap || (num_lock < 0 && caps_lock < 0)) return;
	const xkb_mod_index_t num_i = xkb_keymap_mod_get_index(keyboard->keymap, XKB_MOD_NAME_NUM);
	const xkb_mod_index_t caps_i = xkb_keymap_mod_get_index(keyboard->keymap, XKB_MOD_NAME_CAPS);
	uint32_t locked = keyboard->modifiers.locked;
	auto put = [&](xkb_mod_index_t i, int on) {
		if (i == XKB_MOD_INVALID || on < 0) return;
		if (on) locked |= 1u << i;
		else locked &= ~(1u << i);
	};
	put(num_i, num_lock);
	put(caps_i, caps_lock);
	if (locked == keyboard->modifiers.locked) return;
	// Quietly: no on-screen "Num Lock on" for this.
	if (num_lock >= 0) num = num_lock != 0;
	if (caps_lock >= 0) caps = caps_lock != 0;
	wlr_keyboard_notify_modifiers(keyboard, keyboard->modifiers.depressed, keyboard->modifiers.latched, locked,
		keyboard->modifiers.group);
}

void Keyboard::handle_modifiers(void *) {
	wlr_seat_set_keyboard(server->seat, keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->modifiers);

	// Show Caps Lock / Num Lock / layout changes on screen.
	xkb_state *state = keyboard->xkb_state;
	if (!state) {
		return;
	}
	const bool now_caps = xkb_state_led_name_is_active(state, XKB_LED_NAME_CAPS) > 0;
	// The lock screen's "Caps Lock is on", straight away (not at the next key).
	if (server->lock) server->lock->set_caps_lock(now_caps);
	const bool now_num = xkb_state_led_name_is_active(state, XKB_LED_NAME_NUM) > 0;
	const xkb_layout_index_t now_layout = keyboard->modifiers.group;
	const Config &c = server->config;
	if (c.osd_lock_keys && now_caps != caps) {
		server->osd->show(Osd::Kind::CapsLock, -1, !now_caps);
	} else if (c.osd_lock_keys && now_num != num) {
		server->osd->show(Osd::Kind::NumLock, -1, !now_num);
	} else if (c.osd_layout && now_layout != layout && keyboard->keymap &&
			xkb_keymap_num_layouts(keyboard->keymap) > 1) {
		const char *name = xkb_keymap_layout_get_name(keyboard->keymap, now_layout);
		server->osd->show(Osd::Kind::Layout, -1, false, name ? name : "");
	}
	caps = now_caps;
	num = now_num;
	layout = now_layout;
}

void Keyboard::handle_key(void *data) {
	auto *event = static_cast<wlr_keyboard_key_event *>(data);
	server->note_activity();
	const uint32_t keycode = event->keycode + 8;   // libinput -> xkb
	const bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;

	const xkb_keysym_t *syms = nullptr;
	const int count = xkb_state_key_get_syms(keyboard->xkb_state, keycode, &syms);
	const uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard);

	bool handled = false;
	for (int i = 0; i < count; i++) {
		const xkb_keysym_t sym = syms[i];
		const bool is_super = sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R;

		if (server->locked()) {
			// Locked: the keys are for the lock screen. Switching terminals,
			// volume and brightness still work, and Ctrl+Alt+Backspace (which
			// ends the session, so nothing is left open).
			if (pressed) {
				const bool ctrl = modifiers & WLR_MODIFIER_CTRL, alt = modifiers & WLR_MODIFIER_ALT;
				const bool system = (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) ||
					sym == XKB_KEY_XF86AudioRaiseVolume || sym == XKB_KEY_XF86AudioLowerVolume ||
					sym == XKB_KEY_XF86AudioMute || sym == XKB_KEY_XF86AudioMicMute ||
					sym == XKB_KEY_XF86MonBrightnessUp || sym == XKB_KEY_XF86MonBrightnessDown ||
					(ctrl && alt && sym == XKB_KEY_BackSpace);
				if (system) {
					server->handle_keybinding(sym, modifiers);
				} else {
					char text[16] = {};
					xkb_state_key_get_utf8(keyboard->xkb_state, keycode, text, sizeof text);
					server->lock->key(sym, text, modifiers);
				}
			}
			server->super_tapped = false;
			handled = true;
			continue;
		}

		// Tapping Super on its own opens the overview (or the app menu:
		// Settings > Workspaces), unless the app asked for every key.
		if (is_super) {
			if (pressed) {
				server->super_tapped = !server->shortcuts_inhibited();
			} else if (server->super_tapped) {
				server->super_tapped = false;
				Output *here = server->output_at(server->cursor->x, server->cursor->y);
				if (server->overview && (server->overview->is_open() || server->config.super_key == "overview")) {
					server->launcher->close();
					server->overview->toggle(here);
				} else {
					server->launcher->toggle(here);
				}
			}
		} else if (pressed) {
			server->super_tapped = false;
		}

		if (!pressed && (sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R)) {
			server->end_switcher();
		}

		if (server->shot_tool && server->shot_tool->active()) {
			// The screenshot tool has the keyboard while it's open.
			server->super_tapped = false;
			if (pressed) server->shot_tool->key(sym);
			handled = true;
			continue;
		}
		if (pressed && !is_super) {
			handled = server->handle_keybinding(sym, modifiers);
			if (!handled && sym == XKB_KEY_Escape &&
					(server->calendar->is_open() || server->media_card->is_open() ||
						server->menu->is_open() || server->snap_layouts->is_open() ||
						server->clipboard_card->is_open())) {
				server->close_popups();
				handled = true;
			}
			if (!handled && server->clipboard_card && server->clipboard_card->is_open()) {
				handled = server->clipboard_card->handle_key(sym);
			}
			if (!handled && server->overview && server->overview->is_open() && !server->launcher->is_open() &&
					!server->control_centre->is_open()) {
				char text[16] = {};
				xkb_state_key_get_utf8(keyboard->xkb_state, keycode, text, sizeof text);
				handled = server->overview->handle_key(sym, modifiers, text);
			}
			if (!handled && !server->launcher->is_open() && !server->control_centre->is_open() &&
					!(server->overview && server->overview->is_open()) &&
					server->desktop && server->desktop->wants_keyboard()) {
				// The desktop was clicked (no window active): Delete, F2, Ctrl+C...
				char text[16] = {};
				xkb_state_key_get_utf8(keyboard->xkb_state, keycode, text, sizeof text);
				handled = server->desktop->handle_key(sym, modifiers, text);
			}
			if (!handled && (server->launcher->is_open() || server->control_centre->is_open())) {
				char text[16] = {};
				xkb_state_key_get_utf8(keyboard->xkb_state, keycode, text, sizeof text);
				if (server->control_centre->is_open()) {
					handled = server->control_centre->handle_key(sym, text);
				} else {
					handled = server->launcher->handle_key(sym, text);
				}
			}
			if (handled) {
				break;
			}
		}
	}

	if (pressed && handled) {
		swallowed.push_back(event->keycode);
		return;
	}
	if (!pressed) {
		auto it = std::find(swallowed.begin(), swallowed.end(), event->keycode);
		if (it != swallowed.end()) {
			swallowed.erase(it);
			return;   // the app never saw this key go down
		}
	}
	wlr_seat_set_keyboard(server->seat, keyboard);
	wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
}

void Keyboard::handle_destroy(void *) {
	Server *s = server;
	s->keyboards.erase(std::remove(s->keyboards.begin(), s->keyboards.end(), this),
		s->keyboards.end());
	delete this;
}

// Puts things right when a game left the desktop stuck: keys an app kept
// for itself come back, menus and the overview close, the window in front
// gets the keyboard again and every screen is drawn afresh.
void Server::unstick() {
	wlr_log(WLR_INFO, "Super+Escape: taking the keys back, redrawing");
	// What everything was doing, for a report (journalctl --user -b | grep unstick).
	wlr_log(WLR_INFO, "unstick: game_mode=%d overview=%d/%d launcher=%d cc=%d grab=%d cursor_mode=%d inhibitors=%zu "
		"layers views=%d fullscreen=%d desktop=%d",
		game_mode, overview && overview->is_open(), overview && overview->shown(), launcher && launcher->is_open(),
		control_centre && control_centre->is_open(), cc_grab, static_cast<int>(cursor_mode), inhibitors.size(),
		layer_views->node.enabled, layer_fullscreen->node.enabled, layer_desktop->node.enabled);
	for (View *v : views) {
		const wlr_box b = v->frame_box();
		wlr_log(WLR_INFO, "unstick: view app=%s title=%s %dx%d+%d+%d mapped=%d visible=%d fullscreen=%d maximized=%d "
			"minimized=%d focused=%d ws=%d layer=%s", v->app_id().c_str(), v->title().c_str(), b.width, b.height, b.x, b.y,
			v->mapped, v->visible(), v->fullscreen, v->maximized, v->minimized, v == focused, v->workspace,
			v->tree->node.parent == layer_fullscreen ? "fullscreen" : "views");
	}
	{
		int count = 0;
		wlr_scene_node *child;
		wl_list_for_each(child, &layer_unmanaged->children, link) count++;
		wlr_log(WLR_INFO, "unstick: x11 popups on screen: %d", count);
	}
	// Whatever was hidden for a moment (the overview, a full-screen app menu) shows again.
	if (!(overview && overview->is_open())) {
		wlr_scene_node_set_enabled(&layer_views->node, true);
		wlr_scene_node_set_enabled(&layer_fullscreen->node, true);
		wlr_scene_node_set_enabled(&layer_desktop->node, true);
		wlr_scene_node_set_enabled(&layer_panel_shadow->node, true);
	}
	cursor_mode = CursorMode::Passthrough;
	cc_grab = false;
	for (ShortcutInhibitor *i : inhibitors) inhibitor_release(i);
	close_popups();
	if (overview && overview->is_open()) overview->close(nullptr);
	View *front = nullptr;
	for (View *v : views) {
		if (v->mapped && v->visible()) {
			front = v;
			break;
		}
	}
	if (front) focus(front);
	for (View *v : views) v->update_layer();
	for (Output *o : outputs) wlr_output_schedule_frame(o->output);
	redraw_panels();
}

bool Server::handle_keybinding(xkb_keysym_t sym, uint32_t modifiers) {
	const bool super = modifiers & WLR_MODIFIER_LOGO;
	const bool alt = modifiers & WLR_MODIFIER_ALT;
	const bool ctrl = modifiers & WLR_MODIFIER_CTRL;
	const bool shift = modifiers & WLR_MODIFIER_SHIFT;
	View *view = focused;

	// Ctrl+Alt+F1..F12: switch virtual terminal.
	if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (session) {
			wlr_session_change_vt(session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
		}
		return true;
	}
	// Super+Escape: get the desktop back, whatever a game did (as in GNOME,
	// it also takes back the keys an app asked to keep for itself).
	if (super && sym == XKB_KEY_Escape) {
		unstick();
		return true;
	}
	// An app that asked for every key (a virtual machine, a remote desktop)
	// gets them, apart from the emergency exit.
	if (shortcuts_inhibited() && !(ctrl && alt && sym == XKB_KEY_BackSpace)) {
		return false;
	}
	// Your own shortcuts (Settings > Keyboard shortcuts) come first.
	{
		const uint32_t mods = modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL |
			WLR_MODIFIER_SHIFT);
		const xkb_keysym_t lower = xkb_keysym_to_lower(sym);
		for (const Shortcut &sc : config.shortcuts) {
			if (sc.sym == lower && sc.mods == mods) {
				spawn(sc.command);
				return true;
			}
		}
	}
	// Screenshots: Print = this screen, Shift+Print = every screen,
	// Alt+Print = the active window.
	// Shift+Ctrl+Alt+R (as in GNOME): record this screen, or stop recording.
	if (ctrl && alt && shift && !super && xkb_keysym_to_lower(sym) == XKB_KEY_r) {
		if (recorder) recorder->toggle();
		return true;
	}
	if (sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req) {
		screenshot(alt || sym == XKB_KEY_Sys_Req ? "window" : shift ? "all"
			: config.screenshot_print == "tool" ? "tool" : "screen");
		return true;
	}
	// Volume, microphone and brightness keys (no modifier needed).
	switch (sym) {
	case XKB_KEY_XF86AudioRaiseVolume:
		volume_key("up", false);
		return true;
	case XKB_KEY_XF86AudioLowerVolume:
		volume_key("down", false);
		return true;
	case XKB_KEY_XF86AudioMute:
		volume_key("mute", false);
		return true;
	case XKB_KEY_XF86AudioMicMute:
		volume_key("mute", true);
		return true;
	case XKB_KEY_XF86MonBrightnessUp:
		brightness_key(true);
		return true;
	case XKB_KEY_XF86MonBrightnessDown:
		brightness_key(false);
		return true;
	// Play / pause, next, previous, stop: the player the control centre
	// shows (MPRIS, through playerctl), whichever window is in front.
	case XKB_KEY_XF86AudioPlay:
	case XKB_KEY_XF86AudioPause:
		if (status) status->media("play-pause");
		return true;
	case XKB_KEY_XF86AudioNext:
		if (status) status->media("next");
		return true;
	case XKB_KEY_XF86AudioPrev:
		if (status) status->media("previous");
		return true;
	case XKB_KEY_XF86AudioStop:
		if (status) status->media("stop");
		return true;
	default:
		break;
	}
	// Ctrl+Alt+Backspace: quit Dusk (emergency exit).
	if (ctrl && alt && sym == XKB_KEY_BackSpace) {
		wl_display_terminate(display);
		return true;
	}
	// Workspaces: Super+Page Up/Down, Ctrl+Alt+Left/Right, Super+Alt+Left/Right;
	// with Shift the active window goes along.
	{
		int dir = 0;
		if (super && (sym == XKB_KEY_Page_Up || sym == XKB_KEY_Page_Down)) dir = sym == XKB_KEY_Page_Down ? 1 : -1;
		if (((ctrl && alt) || (super && alt)) && (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) dir = sym == XKB_KEY_Right ? 1 : -1;
		if (dir != 0) {
			if (shift && view && !(overview && overview->is_open())) {
				int to = workspace + dir;
				if (to < 0) return true;
				if (config.workspaces_dynamic && to >= workspace_count) to = workspace_count - 1;
				move_to_workspace(view, to, true);
			} else {
				switch_workspace(workspace + dir);
			}
			return true;
		}
		// Super+1…9, 0 (10): that workspace, if there is one; with Shift
		// the active window goes there instead.
		if (super && !ctrl && !alt) {
			static const xkb_keysym_t kPlain[] = {XKB_KEY_1, XKB_KEY_2, XKB_KEY_3, XKB_KEY_4, XKB_KEY_5,
				XKB_KEY_6, XKB_KEY_7, XKB_KEY_8, XKB_KEY_9, XKB_KEY_0};
			static const xkb_keysym_t kPad[] = {XKB_KEY_KP_1, XKB_KEY_KP_2, XKB_KEY_KP_3, XKB_KEY_KP_4, XKB_KEY_KP_5,
				XKB_KEY_KP_6, XKB_KEY_KP_7, XKB_KEY_KP_8, XKB_KEY_KP_9, XKB_KEY_KP_0};
			// (With Shift the key gives its symbol, on a US layout these.)
			static const xkb_keysym_t kShifted[] = {XKB_KEY_exclam, XKB_KEY_at, XKB_KEY_numbersign, XKB_KEY_dollar,
				XKB_KEY_percent, XKB_KEY_asciicircum, XKB_KEY_ampersand, XKB_KEY_asterisk, XKB_KEY_parenleft,
				XKB_KEY_parenright};
			int n = -1;
			for (int i = 0; i < 10 && n < 0; i++) {
				if (sym == kPlain[i] || sym == kPad[i] || (shift && sym == kShifted[i])) n = i;
			}
			if (n >= 0) {
				if (n < workspace_count) {
					if (shift && view && !(overview && overview->is_open())) move_to_workspace(view, n, true);
					else if (n != workspace) switch_workspace(n);
				}
				return true;   // (not one yet: nothing, and the key isn't typed into the app)
			}
		}
		// Super+Home / Super+End: the first / last workspace.
		if (super && (sym == XKB_KEY_Home || sym == XKB_KEY_End)) {
			switch_workspace(sym == XKB_KEY_Home ? 0 : workspace_count - 1);
			return true;
		}
		// Super+Tab: the overview (again: back out of it).
		if (super && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
			if (overview) overview->toggle(output_at(cursor->x, cursor->y));
			return true;
		}
		// Super+S or Super+W: the overview.
		if (super && !shift && (sym == XKB_KEY_s || sym == XKB_KEY_S || sym == XKB_KEY_w || sym == XKB_KEY_W)) {
			if (overview) overview->toggle(output_at(cursor->x, cursor->y));
			return true;
		}
	}
	// Alt+Tab / Alt+Shift+Tab: switch windows (on this workspace).
	if (alt && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
		if (overview && overview->is_open()) overview->close();
		if (switcher.empty()) {
			for (View *v : views) {
				if (v->mapped && (v->sticky || v->workspace == workspace)) {
					switcher.push_back(v);
				}
			}
			switcher_index = 0;
		}
		if (switcher.size() > 1) {
			const bool back = shift || sym == XKB_KEY_ISO_Left_Tab;
			const size_t n = switcher.size();
			switcher_index = back ? (switcher_index + n - 1) % n : (switcher_index + 1) % n;
			focus(switcher[switcher_index]);
		}
		return true;
	}
	// Alt+F1: app menu (like KDE).
	if (alt && sym == XKB_KEY_F1) {
		launcher->toggle(output_at(cursor->x, cursor->y));
		return true;
	}
	// Alt+F4 or Super+Q: close window.
	if ((alt && sym == XKB_KEY_F4) || (super && (sym == XKB_KEY_q || sym == XKB_KEY_Q))) {
		if (view) {
			view->close();
		}
		return true;
	}
	if (!super) {
		return false;
	}
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		spawn_terminal();
		return true;
	case XKB_KEY_i:
	case XKB_KEY_I:
		open_settings();
		return true;
	case XKB_KEY_z:
	case XKB_KEY_Z:
		if (view) {
			if (snap_layouts->menu_open()) {
				snap_layouts->close();
			} else {
				open_snap_menu(view);
			}
		}
		return true;
	case XKB_KEY_e:
	case XKB_KEY_E:
		spawn("xdg-open \"$HOME\"");   // your files, in the default file manager
		return true;
	case XKB_KEY_a:
	case XKB_KEY_A:
	case XKB_KEY_n:
	case XKB_KEY_N:
		launcher->close();
		control_centre->toggle(output_at(cursor->x, cursor->y));
		return true;
	case XKB_KEY_d:
	case XKB_KEY_D:
		toggle_show_desktop();
		return true;
	case XKB_KEY_k:
	case XKB_KEY_K:
		osk->toggle(focused_output());   // Super+K: the on-screen keyboard
		return true;
	case XKB_KEY_l:
	case XKB_KEY_L:
		power_action("lock");   // Super+L: lock the screen
		return true;
	case XKB_KEY_v:
	case XKB_KEY_V:
		// The clipboard history, at the pointer.
		if (clipboard_card->is_open()) {
			clipboard_card->close();
		} else if (Output *o = output_at(cursor->x, cursor->y)) {
			close_popups();
			clipboard_card->open(o, wlr_box{static_cast<int>(cursor->x), static_cast<int>(cursor->y), 1, 1},
				"point");
		}
		return true;
	case XKB_KEY_f:
	case XKB_KEY_F:
		if (view) {
			view->set_fullscreen(!view->fullscreen);
		}
		return true;
	case XKB_KEY_Up:
		if (view) {
			view->set_maximized(true, view->tile_target());
		}
		return true;
	case XKB_KEY_Down:
		if (view) {
			if (view->maximized) {
				view->set_maximized(false);
			} else if (view->snap != View::Snap::None) {
				view->snap_to(View::Snap::None);
			} else {
				view->minimize();
			}
		}
		return true;
	case XKB_KEY_Left:
		if (view && shift) {
			move_to_next_screen(view, -1);
		} else if (view) {
			view->snap_to(view->snap == View::Snap::Left ? View::Snap::None : View::Snap::Left,
				view->tile_target());
		}
		return true;
	case XKB_KEY_Right:
		if (view && shift) {
			move_to_next_screen(view, 1);
		} else if (view) {
			view->snap_to(view->snap == View::Snap::Right ? View::Snap::None : View::Snap::Right,
				view->tile_target());
		}
		return true;
	default:
		return false;
	}
}

void Server::end_switcher() {
	switcher.clear();
	switcher_index = 0;
}

// ============================================================================
// Launching apps
// ============================================================================

// ---- commands whose output we need (volume, brightness) --------------------

namespace {

struct Capture {
	Server *server;
	pid_t pid;
	int fd;
	wl_event_source *source = nullptr;
	std::string output;
	std::function<void(std::string)> done;
};

int handle_capture_readable(int fd, uint32_t mask, void *data) {
	auto *capture = static_cast<Capture *>(data);
	char buffer[512];
	for (;;) {
		const ssize_t n = read(fd, buffer, sizeof buffer);
		if (n > 0) {
			capture->output.append(buffer, static_cast<size_t>(n));
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;   // interrupted: just try again, the child may still be running
		}
		if (n < 0 && errno == EAGAIN && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
			return 0;   // more to come
		}
		break;   // end of output
	}
	wl_event_source_remove(capture->source);
	close(fd);
	waitpid(capture->pid, nullptr, 0);
	if (capture->done) {
		capture->done(capture->output);
	}
	delete capture;
	return 0;
}

// "Volume: 0.45" or "Volume: 0.45 [MUTED]" -> 45, muted
bool parse_wpctl(const std::string &out, int &percent, bool &muted) {
	const auto pos = out.find("Volume:");
	if (pos == std::string::npos) {
		return false;
	}
	percent = static_cast<int>(std::lround(std::strtod(out.c_str() + pos + 7, nullptr) * 100));
	muted = out.find("MUTED") != std::string::npos;
	return true;
}

} // namespace

void Server::run_capture(const std::string &command, std::function<void(std::string)> done) {
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		return;
	}
	const pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		signal(SIGPIPE, SIG_DFL);   // Dusk ignores it; the apps it starts must not
		dup2(fds[1], STDOUT_FILENO);
		execl("/bin/sh", "/bin/sh", "-c", command.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	auto *capture = new Capture{this, pid, fds[0], nullptr, "", std::move(done)};
	capture->source = wl_event_loop_add_fd(loop, fds[0], WL_EVENT_READABLE,
		handle_capture_readable, capture);
}

// The click in headphones every so often: the sound card powering down
// when nothing plays for a few seconds and back up when something does (a
// notification, the media wave starting to listen). WirePlumber is told
// not to let it sleep (Settings › Media & sound); restarted only when the
// setting changes.
void Server::sync_audio_sleep() {
	const char *xdg = std::getenv("XDG_CONFIG_HOME");
	const char *home = std::getenv("HOME");
	const std::string dir = std::string(xdg && *xdg ? xdg : std::string(home ? home : "") + "/.config") +
		"/wireplumber/wireplumber.conf.d";
	const std::string file = dir + "/51-dusk-keep-awake.conf";
	std::error_code ec;
	const bool there = std::filesystem::exists(file, ec);
	if (config.audio_keep_awake == there) return;
	if (config.audio_keep_awake) {
		std::filesystem::create_directories(dir, ec);
		std::ofstream f(file);
		f << "# Written by Dusk (Settings > Media & sound > Keep the sound card awake).\n"
			"# The sound card isn't put to sleep when quiet, so headphones don't click.\n"
			"monitor.alsa.rules = [\n"
			"  {\n"
			"    matches = [\n"
			"      { node.name = \"~alsa_output.*\" }\n"
			"      { node.name = \"~alsa_input.*\" }\n"
			"    ]\n"
			"    actions = {\n"
			"      update-props = {\n"
			"        session.suspend-timeout-seconds = 0\n"
			"      }\n"
			"    }\n"
			"  }\n"
			"]\n";
		if (!f) return;
	} else {
		std::filesystem::remove(file, ec);
	}
	wlr_log(WLR_INFO, "Sound card sleep %s: restarting WirePlumber", config.audio_keep_awake ? "off" : "on");
	spawn("systemctl --user restart wireplumber");
}

// Volume through PipeWire's wpctl (Fedora's default sound system).
void Server::volume_key(const char *action, bool mic) {
	const std::string device = mic ? "@DEFAULT_AUDIO_SOURCE@" : "@DEFAULT_AUDIO_SINK@";
	const std::string step = std::to_string(config.volume_step) + "%";
	std::string command;
	if (std::string(action) == "up") {
		command = "wpctl set-mute " + device + " 0; wpctl set-volume -l 1.0 " + device + " " +
			step + "+";
	} else if (std::string(action) == "down") {
		command = "wpctl set-volume " + device + " " + step + "-";
	} else {
		command = "wpctl set-mute " + device + " toggle";
	}
	command += "; wpctl get-volume " + device;
	run_capture(command, [this, mic](const std::string &out) {
		int percent = 0;
		bool muted = false;
		if (parse_wpctl(out, percent, muted)) {
			osd->show(mic ? Osd::Kind::Mic : Osd::Kind::Volume, mic ? -1 : percent, muted);
			if (mic) {
				status->mic_muted = muted;
			} else {
				status->volume = percent;
				status->muted = muted;
			}
			if (status->changed) status->changed();
		}
	});
}

// Screen brightness through brightnessctl.
void Server::brightness_key(bool up) {
	const std::string step = std::to_string(config.brightness_step) + "%";
	run_capture("brightnessctl -m --min-value=1 set " + step + (up ? "+" : "-"),
		[this](const std::string &out) {
			// "intel_backlight,backlight,12345,45%,30000"
			const auto pct = out.find('%');
			if (pct == std::string::npos) {
				return;
			}
			auto comma = out.rfind(',', pct);
			const int value = std::atoi(out.c_str() + (comma == std::string::npos ? 0 : comma + 1));
			osd->show(Osd::Kind::Brightness, value, false);
			status->brightness = value;
			status->has_brightness = true;
			if (status->changed) status->changed();
		});
}

void Server::spawn(const std::string &command) {
	// Fork twice so the app is not our child and never becomes a zombie.
	pid_t child = fork();
	if (child == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		signal(SIGPIPE, SIG_DFL);   // Dusk ignores it; the apps it starts must not
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", command.c_str(), static_cast<char *>(nullptr));
			_exit(127);
		}
		_exit(0);
	} else if (child > 0) {
		waitpid(child, nullptr, 0);
	}
}

void Server::spawn_terminal() {
	if (!config.terminal.empty()) {
		spawn(config.terminal);
		return;
	}
	const char *preferred = getenv("DUSK_TERMINAL");
	if (preferred && *preferred) {
		spawn(preferred);
		return;
	}
	spawn("for t in ptyxis konsole kitty alacritty wezterm gnome-terminal foot "
		"xfce4-terminal xterm; do command -v \"$t\" >/dev/null 2>&1 && exec \"$t\"; done");
}

// Dusk Settings: use the copy next to the dusk binary if there is one (so it
// works straight from the build folder), otherwise the installed one.
// A program installed next to dusk (or in data/ when running from the
// build folder), else found on PATH. Empty if there's none.
std::string Server::helper_path(const std::string &name) const {
	char self[4096];
	const ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
	if (len > 0) {
		self[len] = '\0';
		const std::string dir = std::string(self).substr(0, std::string(self).rfind('/') + 1);
		for (const std::string &p : {dir + name, dir + "../data/" + name}) {
			if (access(p.c_str(), X_OK) == 0) return p;
		}
	}
	for (const char *d : {"/usr/local/bin/", "/usr/bin/"}) {
		if (access((std::string(d) + name).c_str(), X_OK) == 0) return std::string(d) + name;
	}
	return "";
}

void Server::open_settings(const std::string &page) {
	const std::string args = page.empty() ? "" : " --page " + shell_quote(page);
	// Open already: that window comes forward (from another workspace too);
	// a page asked for is passed on to it (a second Settings started with
	// --page hands it to the first one and goes).
	for (View *v : views) {
		if (v->mapped && v->app_id() == "org.dusk.Settings") {
			focus(v);
			if (page.empty()) return;
			break;
		}
	}
	char self[4096];
	const ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
	if (len > 0) {
		self[len] = '\0';
		std::string path(self);
		path = path.substr(0, path.rfind('/') + 1) + "dusk-settings";
		if (access(path.c_str(), X_OK) == 0) {
			spawn(shell_quote(path) + args);
			return;
		}
	}
	spawn("dusk-settings" + args);
}

} // namespace dusk
