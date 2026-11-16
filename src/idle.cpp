// Doing nothing for a while: the screens turn off, then (if set) the
// computer goes to sleep. Videos and games can ask to keep the screen on
// (the idle-inhibit protocol), and so does the control centre's caffeine.
// Also warns when the battery runs low.
#include "server.hpp"
#include "lock.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <set>

#include "anim.hpp"
#include "notify.hpp"
#include "system.hpp"

namespace dusk {

// ---- Keeping awake: D-Bus inhibitors and gamepads ----------------------------

struct AwakeKeepers {
	Server *server;
	sd_bus *bus = nullptr;
	sd_bus_slot *filter = nullptr;
	wl_event_source *fd_source = nullptr, *timeout_source = nullptr;
	uint32_t next_cookie = 1;
	std::map<uint32_t, std::pair<std::string, std::string>> cookies;   // -> sender, app (why)
	// Gamepads (/dev/input/js*): any movement counts as using the computer.
	std::map<std::string, std::pair<int, wl_event_source *>> pads;
	double pads_scanned = 0;

	explicit AwakeKeepers(Server *s) : server(s) {
		if (sd_bus_open_user(&bus) < 0) {
			bus = nullptr;
			return;
		}
		if (sd_bus_add_filter(bus, &filter, &AwakeKeepers::handle_message, this) < 0) {
			wlr_log(WLR_ERROR, "ScreenSaver: can't install the bus filter");
		}
		if (sd_bus_match_signal(bus, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
			"NameOwnerChanged", &AwakeKeepers::handle_owner, this) < 0) {
			wlr_log(WLR_ERROR, "ScreenSaver: can't match NameOwnerChanged");
		}
		// Fine if someone else has them: then that program does this job.
		sd_bus_request_name(bus, "org.freedesktop.ScreenSaver", 0);
		sd_bus_request_name(bus, "org.freedesktop.PowerManagement.Inhibit", 0);
		sd_bus_request_name(bus, "org.freedesktop.PowerManagement", 0);
		// GNOME's session manager, the part apps use: Software's "Restart &
		// Upgrade", "Restart to update", log out / shut down from apps, and
		// keeping the screen on. Under GNOME it's GNOME's (it has the name).
		sd_bus_request_name(bus, "org.gnome.SessionManager", 0);
		const int bus_fd = sd_bus_get_fd(bus);
		if (bus_fd >= 0) {
			fd_source = wl_event_loop_add_fd(s->loop, bus_fd, WL_EVENT_READABLE,
				[](int, uint32_t, void *d) { static_cast<AwakeKeepers *>(d)->process(); return 0; }, this);
		} else {
			wlr_log(WLR_ERROR, "ScreenSaver: the bus has no file descriptor");
		}
		timeout_source = wl_event_loop_add_timer(s->loop,
			[](void *d) { static_cast<AwakeKeepers *>(d)->process(); return 0; }, this);
		process();
	}
	~AwakeKeepers() {
		for (auto &[path, pad] : pads) {
			wl_event_source_remove(pad.second);
			close(pad.first);
		}
		if (fd_source) wl_event_source_remove(fd_source);
		if (timeout_source) wl_event_source_remove(timeout_source);
		if (filter) sd_bus_slot_unref(filter);
		if (bus) sd_bus_flush_close_unref(bus);
	}

	bool inhibited() const { return !cookies.empty(); }

	void process() {
		if (!bus) return;
		while (sd_bus_process(bus, nullptr) > 0) {
		}
		const int events = sd_bus_get_events(bus);
		uint32_t mask = 0;
		if (events & POLLIN) mask |= WL_EVENT_READABLE;
		if (events & POLLOUT) mask |= WL_EVENT_WRITABLE;
		if (fd_source) {
			wl_event_source_fd_update(fd_source, mask);
		}
		uint64_t usec = 0;
		if (sd_bus_get_timeout(bus, &usec) >= 0 && usec != UINT64_MAX) {
			timespec ts{};
			clock_gettime(CLOCK_MONOTONIC, &ts);
			const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
			wl_event_source_timer_update(timeout_source,
				usec > now ? std::max(5, static_cast<int>((usec - now) / 1000) + 1) : 5);
		} else {
			wl_event_source_timer_update(timeout_source, 0);
		}
	}

	static int handle_owner(sd_bus_message *m, void *data, sd_bus_error *) {
		auto *self = static_cast<AwakeKeepers *>(data);
		const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
		if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || (new_owner && *new_owner)) return 0;
		// An app that asked went away without saying so: its requests go too.
		for (auto it = self->cookies.begin(); it != self->cookies.end();) {
			if (it->second.first == (name ? name : "")) it = self->cookies.erase(it);
			else ++it;
		}
		return 0;
	}

	static int handle_message(sd_bus_message *m, void *data, sd_bus_error *) {
		auto *self = static_cast<AwakeKeepers *>(data);
		if (!sd_bus_message_is_method_call(m, nullptr, nullptr)) return 0;
		const char *iface = sd_bus_message_get_interface(m);
		const char *member = sd_bus_message_get_member(m);
		if (!iface || !member) return 0;
		const std::string i = iface, mem = member;
		const bool saver = i == "org.freedesktop.ScreenSaver";
		const bool power = i == "org.freedesktop.PowerManagement.Inhibit";
		if (i == "org.gnome.SessionManager") return self->session_manager(m, mem);
		if (!saver && !power) return 0;
		if (mem == "Inhibit") {
			const char *app = nullptr, *reason = nullptr;
			sd_bus_message_read(m, "ss", &app, &reason);
			const uint32_t cookie = self->next_cookie++;
			const char *sender = sd_bus_message_get_sender(m);
			self->cookies[cookie] = {sender ? sender : "", std::string(app ? app : "") + ": " + (reason ? reason : "")};
			self->server->note_activity();
			sd_bus_reply_method_return(m, "u", cookie);
			return 1;
		}
		if (mem == "UnInhibit") {
			uint32_t cookie = 0;
			sd_bus_message_read(m, "u", &cookie);
			self->cookies.erase(cookie);
			sd_bus_reply_method_return(m, "");
			return 1;
		}
		if (mem == "HasInhibit") {
			sd_bus_reply_method_return(m, "b", !self->cookies.empty());
			return 1;
		}
		if (mem == "SimulateUserActivity") {
			self->server->note_activity();
			sd_bus_reply_method_return(m, "");
			return 1;
		}
		if (mem == "GetActive") {
			sd_bus_reply_method_return(m, "b", self->server->screens_off);
			return 1;
		}
		if (mem == "GetActiveTime" || mem == "GetSessionIdleTime") {
			const double idle_s = (now_ms() - self->server->last_activity) / 1000.0;
			sd_bus_reply_method_return(m, "u", static_cast<uint32_t>(std::max(0.0, idle_s)));
			return 1;
		}
		return 0;
	}

	// org.gnome.SessionManager: restart / shut down / log out when an app asks
	// (Software's "Restart & Upgrade" and "Restart & Update" ask this way; it
	// has already asked you), and its inhibit calls (flag 8: keep awake).
	int session_manager(sd_bus_message *m, const std::string &mem) {
		Server *srv = server;
		auto later = [srv](const char *action) {
			// After the reply has gone out.
			srv->run_then("sleep 0.3", [srv, a = std::string(action)](bool) { srv->power_action(a); });
		};
		if (mem == "Reboot" || mem == "RequestReboot") {
			sd_bus_reply_method_return(m, "");
			later("restart");
			return 1;
		}
		if (mem == "Shutdown" || mem == "RequestShutdown") {
			sd_bus_reply_method_return(m, "");
			later("shutdown");
			return 1;
		}
		if (mem == "Logout") {
			sd_bus_reply_method_return(m, "");
			later("logout");
			return 1;
		}
		if (mem == "CanShutdown" || mem == "CanRebootToFirmwareSetup" || mem == "IsSessionRunning") {
			sd_bus_reply_method_return(m, "b", true);
			return 1;
		}
		if (mem == "Inhibit") {
			const char *app = nullptr, *reason = nullptr;
			uint32_t xid = 0, flags = 0;
			sd_bus_message_read(m, "susu", &app, &xid, &reason, &flags);
			const uint32_t cookie = next_cookie++;
			if (flags & 8) {   // idle (the others, log-out and switching user, aren't needed here)
				const char *sender = sd_bus_message_get_sender(m);
				cookies[cookie] = {sender ? sender : "", std::string(app ? app : "") + ": " + (reason ? reason : "")};
				server->note_activity();
			}
			sd_bus_reply_method_return(m, "u", cookie);
			return 1;
		}
		if (mem == "Uninhibit") {
			uint32_t cookie = 0;
			sd_bus_message_read(m, "u", &cookie);
			cookies.erase(cookie);
			sd_bus_reply_method_return(m, "");
			return 1;
		}
		if (mem == "IsInhibited") {
			uint32_t flags = 0;
			sd_bus_message_read(m, "u", &flags);
			sd_bus_reply_method_return(m, "b", (flags & 8) && !cookies.empty());
			return 1;
		}
		return 0;
	}

	// New gamepads (they come and go): watched for input.
	void scan_pads() {
		pads_scanned = now_ms();
		DIR *dir = opendir("/dev/input");
		if (!dir) return;
		std::set<std::string> seen;
		while (dirent *e = readdir(dir)) {
			if (std::strncmp(e->d_name, "js", 2) != 0) continue;
			const std::string path = std::string("/dev/input/") + e->d_name;
			seen.insert(path);
			if (pads.count(path)) continue;
			const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0) continue;
			wl_event_source *src = wl_event_loop_add_fd(server->loop, fd, WL_EVENT_READABLE,
				[](int fd, uint32_t mask, void *d) {
					auto *self = static_cast<AwakeKeepers *>(d);
					char buf[256];
					ssize_t n = 0, got = 0;
					while ((n = read(fd, buf, sizeof buf)) > 0) got += n;
					if (got > 0) {
						self->server->last_activity = now_ms();   // not a full wake: screens stay as they are
						if (self->server->screens_off) self->server->note_activity();
					}
					if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR) || n == 0) {
						for (auto it = self->pads.begin(); it != self->pads.end(); ++it) {
							if (it->second.first == fd) {
								wl_event_source_remove(it->second.second);
								close(fd);
								self->pads.erase(it);
								break;
							}
						}
					}
					return 0;
				}, this);
			pads[path] = {fd, src};
		}
		closedir(dir);
	}
};

void Server::start_idle() {
	awake = std::unique_ptr<AwakeKeepers, void (*)(AwakeKeepers *)>(new AwakeKeepers(this),
		[](AwakeKeepers *a) { delete a; });
	awake->scan_pads();
	idle_notifier = wlr_idle_notifier_v1_create(display);
	idle_inhibit = wlr_idle_inhibit_v1_create(display);
	last_activity = now_ms();
	idle_timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), handle_idle_timer, this);
	wl_event_source_timer_update(idle_timer, 2000);
}

void Server::stop_idle() {
	awake.reset();
	if (idle_timer) {
		wl_event_source_remove(idle_timer);
		idle_timer = nullptr;
	}
}

int Server::handle_idle_timer(void *data) {
	auto *s = static_cast<Server *>(data);
	s->idle_tick();
	wl_event_source_timer_update(s->idle_timer, 2000);
	return 0;
}

void Server::note_activity() {
	last_activity = now_ms();
	slept = false;
	idle_locked = false;
	if (idle_notifier) {
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	}
	if (screens_off) {
		wake_screens();
	}
}

void Server::screens_off_now() {
	screens_off = true;
	for (Output *o : outputs) {
		if (!o->enabled()) continue;
		wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, false);
		wlr_output_commit_state(o->output, &state);
		wlr_output_state_finish(&state);
	}
}

void Server::wake_screens() {
	screens_off = false;
	for (Output *o : outputs) {
		if (!o->enabled()) continue;
		o->applied.clear();   // set its mode again
		o->configure();
		o->apply_night_light();
		wlr_output_schedule_frame(o->output);
	}
	arrange();
}

void Server::idle_tick() {
	const double now = now_ms();
	// Battery times only when the charger is really out (by the charger,
	// when the computer reports one; else by the battery's state).
	const bool on_battery = status && status->has_battery
		&& (status->on_mains >= 0 ? status->on_mains == 0 : !status->charging);

	// Kept awake by caffeine or by an app (a video, a game, a call): over
	// Wayland (idle-inhibit), D-Bus (ScreenSaver / PowerManagement, what SDL
	// games and Steam use) or logind ("systemd-inhibit --what=idle").
	bool inhibited = status && status->caffeine;
	if (idle_inhibit && !wl_list_empty(&idle_inhibit->inhibitors)) {
		inhibited = true;
	}
	if (awake && awake->inhibited()) {
		inhibited = true;
	}
	if (logind_idle_blocked) {
		inhibited = true;
	}
	// A full-screen window (a game, a film) on a screen that's on.
	if (config.idle_fullscreen) {
		for (View *v : views) {
			if (v->mapped && v->fullscreen && !v->minimized) {
				inhibited = true;
				break;
			}
		}
	}
	if (now - logind_checked > 20000) {
		logind_checked = now;
		run_capture("busctl --system get-property org.freedesktop.login1 /org/freedesktop/login1 "
			"org.freedesktop.login1.Manager BlockInhibited 2>/dev/null", [this](const std::string &out) {
				// s "handle-lid-switch:idle:sleep" (Dusk's own caffeine shows here too)
				logind_idle_blocked = out.find("idle") != std::string::npos;
			});
	}
	if (awake && now - awake->pads_scanned > 10000) {
		awake->scan_pads();
	}
	if (idle_notifier) {
		wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
	}
	if (inhibited) {
		last_activity = now;
	}
	const double idle_min = (now - last_activity) / 60000.0;
	const int screen_off = on_battery ? config.screen_off_battery : config.screen_off_ac;
	const int sleep_after = on_battery ? config.sleep_battery : config.sleep_ac;
	// Each has its own time (Settings › Power): lock, screen off, sleep.
	const int lock_after = on_battery ? config.lock_after_battery : config.lock_after_ac;
	if (lock_after > 0 && !idle_locked && idle_min >= lock_after) {
		idle_locked = true;
		if (lock && config.lock_command.empty()) lock->lock();
		else if (!config.lock_command.empty()) spawn(config.lock_command);
	}
	if (screen_off > 0 && !screens_off && idle_min >= screen_off) {
		// Locked first (Settings › Lock screen), so it's locked when
		// you come back.
		if (config.lock_screen_off && lock && config.lock_command.empty()) lock->lock();
		screens_off_now();
	}
	if (sleep_after > 0 && !slept && idle_min >= sleep_after) {
		slept = true;
		if (config.lock_sleep && lock && config.lock_command.empty()) lock->lock();
		spawn("systemctl suspend");
	}

	// Low battery.
	if (status && status->has_battery && config.battery_warnings) {
		if (status->charging) {
			battery_warned = 101;
		} else {
			struct Level { int percent; int urgency; const char *title, *body; };
			static const Level kLevels[] = {
				{5, 2, "Battery almost empty", "Plug in now, or the computer will turn off."},
				{10, 2, "Battery very low", "Plug in soon."},
				{20, 1, "Battery low", "You may want to plug in, or turn on power saver."},
			};
			for (const Level &l : kLevels) {
				if (status->battery <= l.percent && battery_warned > l.percent) {
					battery_warned = l.percent;
					const std::string body = std::to_string(status->battery) + "% left. " + l.body;
					if (notifications) {
						battery_note = notifications->post(l.title, body, "battery-caution-symbolic",
							l.urgency, battery_note);
					} else {
						spawn("notify-send -u " + std::string(l.urgency == 2 ? "critical" : "normal") +
							" -i battery-caution-symbolic " + shell_quote(l.title) + " " + shell_quote(body));
					}
					break;
				}
			}
		}
	}
}

} // namespace dusk
