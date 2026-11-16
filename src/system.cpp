#include "system.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include "server.hpp"

namespace dusk {

namespace fs = std::filesystem;

namespace {

// Checked every 10 s: network state, volume, brightness. Every third
// time (30 s) also Wi-Fi signal, Bluetooth and the power mode, which are
// slower to ask. Nothing while the screens are off. Opening the control
// centre or changing something checks everything at once.
constexpr int kPollMs = 10000;
constexpr int kFullEvery = 3;
const char *kSep = "@@DUSK@@";

std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> out;
	std::stringstream ss(text);
	std::string line;
	while (std::getline(ss, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		out.push_back(line);
	}
	return out;
}

std::vector<std::string> sections(const std::string &text) {
	std::vector<std::string> out;
	size_t start = 0;
	for (;;) {
		const size_t pos = text.find(kSep, start);
		if (pos == std::string::npos) {
			out.push_back(text.substr(start));
			break;
		}
		out.push_back(text.substr(start, pos - start));
		start = pos + std::strlen(kSep);
	}
	return out;
}

// nmcli -t output: fields separated by ':', with ':' and '\' escaped by '\'.
std::vector<std::string> nm_fields(const std::string &line) {
	std::vector<std::string> out(1);
	for (size_t i = 0; i < line.size(); i++) {
		if (line[i] == '\\' && i + 1 < line.size()) {
			out.back() += line[++i];
		} else if (line[i] == ':') {
			out.emplace_back();
		} else {
			out.back() += line[i];
		}
	}
	return out;
}

std::string trim(const std::string &s) {
	const auto a = s.find_first_not_of(" \t\n\r");
	if (a == std::string::npos) {
		return "";
	}
	const auto b = s.find_last_not_of(" \t\n\r");
	return s.substr(a, b - a + 1);
}

// "Volume: 0.45 [MUTED]"
bool parse_wpctl(const std::string &out, int &percent, bool &muted) {
	const auto pos = out.find("Volume:");
	if (pos == std::string::npos) {
		return false;
	}
	percent = static_cast<int>(std::lround(std::strtod(out.c_str() + pos + 7, nullptr) * 100));
	muted = out.find("MUTED") != std::string::npos;
	return true;
}

std::string read_file(const fs::path &path) {
	std::ifstream in(path);
	std::string s;
	std::getline(in, s);
	return trim(s);
}

} // namespace

std::string shell_quote(const std::string &text) {
	std::string out = "'";
	for (char c : text) {
		if (c == '\'') {
			out += "'\\''";
		} else {
			out += c;
		}
	}
	return out + "'";
}

SystemStatus::SystemStatus(Server *server) : server_(server) {
	timer_ = wl_event_loop_add_timer(server->loop, &SystemStatus::handle_timer, this);
	read_battery();
	refresh();
	wl_event_source_timer_update(timer_, kPollMs);
	media_retry_ = wl_event_loop_add_timer(server->loop, &SystemStatus::handle_media_retry, this);
	start_media_watch();
	start_event_watch();
	privacy_timer_ = wl_event_loop_add_timer(server->loop, &SystemStatus::handle_privacy_timer, this);
	wl_event_source_timer_update(privacy_timer_, 1500);
}

// Every few seconds: is anything recording from the microphone (a stream on
// a PipeWire source; level meters left out), or holding a camera open?
int SystemStatus::handle_privacy_timer(void *data) {
	auto *self = static_cast<SystemStatus *>(data);
	// Not while the screen is off, nor while something is full screen (a
	// game or video: then nobody sees the panel, and the check costs time).
	if (!self->server_->screens_off && !self->server_->game_mode) self->poll_privacy();
	wl_event_source_timer_update(self->privacy_timer_, 5000);
	return 0;
}

void SystemStatus::poll_privacy() {
	if (privacy_busy_) return;
	privacy_busy_ = true;
	server_->run_capture(
		// Recordings from a microphone: every source output except level
		// meters (Dusk's own "dusk-levels" for the media wave, mixers' "peak
		// detect") and anything recording what's playing rather than a
		// microphone (a sink's ".monitor": SongRec listening to a song,
		// visualizers, screen recorders' system sound).
		"mon=$(pactl list short sources 2>/dev/null | awk '$2 ~ /\\.monitor$/ {printf \"%s \", $1}'); "
		"pactl list source-outputs 2>/dev/null | awk -v mon=\" $mon\" "
		"'/^Source Output #/{if(n&&!s)c++;n=1;s=0} "
		"/^[[:space:]]*Source: /{if(index(mon, \" \" $2 \" \"))s=1} "
		"/dusk-levels|[Pp]eak detect/{s=1} END{if(n&&!s)c++;print c+0}'; "
		// The camera: which of your programs have a camera device open. The
		// sound/video server (PipeWire, WirePlumber) keeps cameras open
		// whether anyone is watching or not, so for those it's only "in use"
		// while one of its camera streams is running. (The webcam driver's
		// own count, used before, stayed up after the camera was let go.)
		"cam=0; pw=0; "
		"for p in $(find /proc/[0-9]*/fd -maxdepth 1 -lname '/dev/video*' 2>/dev/null | cut -d/ -f3 | sort -u); do "
		"case \"$(cat /proc/$p/comm 2>/dev/null)\" in pipewire|wireplumber|pipewire-media-session) pw=1 ;; '') ;; *) cam=1 ;; esac; "
		"done; "
		"if [ $cam = 0 ] && [ $pw = 1 ] && command -v pw-dump >/dev/null && command -v python3 >/dev/null; then "
		"cam=$(timeout 3 pw-dump 2>/dev/null | python3 -c 'import json,sys\n"
		"try: d=json.load(sys.stdin)\nexcept Exception: d=[]\n"
		"print(int(any(isinstance(o,dict) and (o.get(\"info\") or {}).get(\"state\")==\"running\" and "
		"str(((o.get(\"info\") or {}).get(\"props\") or {}).get(\"media.class\",\"\")).startswith(\"Video/Source\") "
		"for o in d)))' 2>/dev/null || echo 0); fi; "
		"echo cam $cam",
		[this](const std::string &out) {
			privacy_busy_ = false;
			int mic = 0, cam = 0;
			char how[8] = {0};
			std::sscanf(out.c_str(), "%d %7s %d", &mic, how, &cam);
			if ((mic > 0) != mic_in_use || (cam > 0) != camera_in_use) {
				mic_in_use = mic > 0;
				camera_in_use = cam > 0;
				notify();
			}
		});
}

SystemStatus::~SystemStatus() {
	wl_event_source_remove(timer_);
	if (privacy_timer_) wl_event_source_remove(privacy_timer_);
	stop_media_watch();
	stop_event_watch();
	if (event_settle_) wl_event_source_remove(event_settle_);
	if (media_retry_) wl_event_source_remove(media_retry_);
	if (caffeine_pid_ > 0) {
		kill(-caffeine_pid_, SIGTERM);   // the whole group: systemd-inhibit and its sleep
		waitpid(caffeine_pid_, nullptr, 0);
	}
}

int SystemStatus::handle_timer(void *data) {
	auto *self = static_cast<SystemStatus *>(data);
	self->ticks_++;
	if (!self->server_->screens_off) {
		self->read_battery();
		// While a game is full screen nobody sees the panel: the full check
		// (Wi-Fi, Bluetooth, sound...) waits, so it costs the game nothing.
		self->refresh(!self->server_->game_mode && self->ticks_ % kFullEvery == 0);
	}
	// Told of changes as they happen: only a slow check as well, for what
	// the watchers don't cover.
	const int every = self->event_pid_ > 0 ? kPollMs * 3 : kPollMs;
	wl_event_source_timer_update(self->timer_, self->server_->game_mode ? kPollMs * 3 : every);
	return 0;
}

void SystemStatus::notify() {
	if (changed) {
		changed();
	}
}

namespace {
// Everything the panel and control centre show, in one string.
std::string state_of(const SystemStatus &s) {
	return std::to_string(s.has_battery) + std::to_string(s.battery) + std::to_string(s.charging) +
		"|" + s.net_kind + s.ssid + std::to_string(s.wifi_signal / 10) + std::to_string(s.wifi_enabled) +
		std::to_string(s.wifi_available) + "|" + std::to_string(s.volume) + std::to_string(s.muted) +
		std::to_string(s.mic_muted) + std::to_string(s.mic_volume) + std::to_string(s.has_audio) + "|" + std::to_string(s.brightness) +
		std::to_string(s.has_brightness) + "|" + std::to_string(s.bt_available) +
		std::to_string(s.bt_powered) + std::to_string(s.bt_service) + s.bt_device + "|" + s.power_profile +
		std::to_string(s.airplane) + "|" + s.audio_output;
}
} // namespace

void SystemStatus::read_battery() {
	// The computer's own batteries (BAT0, BAT1...): mice, keyboards, headsets
	// and game pads list theirs here too (scope "Device", often as 0%), and a
	// few laptops have an empty second slot. With several, they're added up.
	std::error_code ec;
	double now_total = 0, full_total = 0;
	int fallback = -1, cap_seen = -1;
	bool any = false, any_charging = false, any_discharging = false;
	bool any_full = false, any_held = false, any_filling = false;
	// The computer's own first (BAT*, scope System); anything else only when
	// there's none of those (a peripheral listed first used to make it 0%).
	std::vector<fs::path> own, other;
	for (const auto &entry : fs::directory_iterator("/sys/class/power_supply", ec)) {
		const auto &p = entry.path();
		if (read_file(p / "type") != "Battery") continue;
		if (read_file(p / "scope") == "Device" || read_file(p / "present") == "0") continue;
		const std::string name = p.filename().string();
		const bool system = name.rfind("BAT", 0) == 0 || name.rfind("CMB", 0) == 0 || read_file(p / "scope") == "System";
		(system ? own : other).push_back(p);
	}
	if (own.empty() && !other.empty()) own.push_back(other.front());
	for (const fs::path &p : own) {
		const std::string cap = read_file(p / "capacity");
		if (cap.empty() && read_file(p / "energy_full").empty() && read_file(p / "charge_full").empty()) continue;
		// An empty slot, or a battery that says it's full of nothing: skip it if there's another.
		if (own.size() > 1 && std::atoi(cap.c_str()) == 0 && std::atof(read_file(p / "energy_full").c_str()) <= 0 &&
				std::atof(read_file(p / "charge_full").c_str()) <= 0) continue;
		// Amounts (µWh or µAh), so two batteries add up properly.
		double now = std::atof(read_file(p / "energy_now").c_str()), full = std::atof(read_file(p / "energy_full").c_str());
		if (full <= 0) {
			now = std::atof(read_file(p / "charge_now").c_str());
			full = std::atof(read_file(p / "charge_full").c_str());
		}
		if (!cap.empty()) cap_seen = std::max(cap_seen, std::atoi(cap.c_str()));
		if (full > 0 && now <= 0 && std::atoi(cap.c_str()) > 0) now = full * std::atoi(cap.c_str()) / 100.0;   // no "now" amount
		if (full > 0) {
			now_total += now;
			full_total += full;
		} else if (!cap.empty()) {
			fallback = std::max(fallback, std::atoi(cap.c_str()));
		}
		any = true;
		const std::string st = read_file(p / "status");
		if (st == "Charging" || st == "Full" || st == "Not charging") any_charging = true;
		if (st == "Discharging") any_discharging = true;
		if (st == "Charging") any_filling = true;
		if (st == "Full") any_full = true;
		if (st == "Not charging") any_held = true;   // plugged in, but it isn't taking charge
	}
	// A dead battery (worn out: it stays at 0% while plugged in, "Not
	// charging"): the computer runs on the charger like a desktop, so it's
	// treated as one: no battery icon stuck at 0%, no battery tile, and the
	// "plugged in" times for the screen and sleep.
	// (It may even say "Charging". An empty battery that really is charging
	// gets past 0% within a couple of minutes, so it's only taken for dead
	// after two minutes of that.)
	// The charger itself (its "online"), not the battery's word for it: a
	// worn battery can say "Not charging" or "Discharging" on the charger.
	bool mains = false, mains_seen = false;
	for (const auto &entry : fs::directory_iterator("/sys/class/power_supply", ec)) {
		if (read_file(entry.path() / "type") != "Mains") continue;
		mains_seen = true;
		if (read_file(entry.path() / "online") == "1") mains = true;
	}
	on_mains = mains_seen ? (mains ? 1 : 0) : -1;
	if (any && cap_seen <= 0 && (full_total <= 0 || now_total <= 0) && (!any_discharging || mains)) {
		dead_reads_ = mains ? dead_reads_ + 1 : 0;
		if (dead_reads_ >= 12) any = false;
	} else {
		dead_reads_ = 0;
	}
	battery_dead = !any && dead_reads_ >= 12;
	has_battery = any;
	if (!any) return;
	if (full_total > 0) {
		battery = std::clamp(static_cast<int>(std::lround(100.0 * now_total / full_total)), 0, 100);
	} else if (fallback >= 0) {
		battery = std::clamp(fallback, 0, 100);
	}
	// Some batteries report no amounts (or zero) but do know their percentage.
	if (battery == 0 && cap_seen > 0) battery = std::clamp(cap_seen, 0, 100);
	charging = any_charging && !any_discharging;
	battery_state = any_discharging || !any_charging ? "Discharging" : any_filling ? "Charging"
		: any_full ? "Full" : any_held ? "Not charging" : "Charging";
}

void SystemStatus::refresh(bool full) {
	if (polling_) {
		// Don't drop it: a check that started before a change would show
		// the old state until the next timer.
		poll_again_ = std::max(poll_again_, full ? 2 : 1);
		return;
	}
	polling_ = true;
	const std::string sep = std::string("; echo ") + kSep + "; ";
	// Parts not asked for this time print nothing and keep their old value.
	const std::string skip = "true";
	const std::string command =
		"nmcli -t -f TYPE,STATE,CONNECTION dev 2>/dev/null" + sep +
		(full ? "nmcli -t -f WIFI g 2>/dev/null" : skip) + sep +
		(full ? "nmcli -t -f ACTIVE,SIGNAL,SSID dev wifi list --rescan no 2>/dev/null | grep '^yes'"
			: skip) + sep +
		"wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null" + sep +
		"wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null" + sep +
		"brightnessctl -m info 2>/dev/null" + sep +
		// Bluetooth: the adapter from the kernel (so it shows even when the
		// Bluetooth service isn't running), the service, then bluez's view.
		(full ? "ls /sys/class/bluetooth 2>/dev/null | grep -q '^hci' && echo ADAPTER; "
			"systemctl is-active --quiet bluetooth.service 2>/dev/null && echo SERVICE; "
			"command -v bluetoothctl >/dev/null || echo NOCTL; "
			"timeout 4 bluetoothctl show 2>/dev/null | grep -E 'Powered:|Controller '" : skip) + sep +
		(full ? "timeout 2 bluetoothctl devices Connected 2>/dev/null" : skip) + sep +
		// busctl instead of powerprofilesctl: that one is a Python script,
		// slow to start every time.
		(full ? "{ busctl --system get-property org.freedesktop.UPower.PowerProfiles "
			"/org/freedesktop/UPower/PowerProfiles org.freedesktop.UPower.PowerProfiles ActiveProfile "
			"2>/dev/null || busctl --system get-property net.hadess.PowerProfiles /net/hadess/PowerProfiles "
			"net.hadess.PowerProfiles ActiveProfile 2>/dev/null; } | sed 's/^s \"//; s/\"$//'" : skip) + sep +
		// Where sound goes: its name ("Speakers", "WH-1000XM4"...).
		(full ? "wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null | sed -n 's/.*node.description = \"\\(.*\\)\"/\\1/p' | head -n 1"
			: skip);
	server_->run_capture(command, [this, full](const std::string &out) {
		polling_ = false;
		parse_poll(out, full);
		if (poll_again_) {
			const bool again_full = poll_again_ == 2;
			poll_again_ = 0;
			refresh(again_full);
		}
	});
}

void SystemStatus::parse_poll(const std::string &out, bool full) {
	const std::vector<std::string> s = sections(out);
	auto part = [&](size_t i) { return i < s.size() ? s[i] : std::string(); };

	// Network
	net_kind = "none";
	wifi_available = false;
	for (const std::string &line : lines_of(part(0))) {
		const auto f = nm_fields(line);
		if (f.size() < 3) {
			continue;
		}
		if (f[0] == "wifi") {
			wifi_available = true;
		}
		if (f[1] == "connected") {
			if (f[0] == "ethernet" && net_kind != "ethernet") {
				net_kind = "ethernet";
			} else if (f[0] == "wifi" && net_kind == "none") {
				net_kind = "wifi";
			}
		}
	}
	if (full) {
		wifi_enabled = trim(part(1)) != "disabled";
		ssid.clear();
		wifi_signal = 0;
	}
	for (const std::string &line : lines_of(full ? part(2) : std::string())) {
		const auto f = nm_fields(line);
		if (f.size() >= 3 && f[0] == "yes") {
			wifi_signal = std::atoi(f[1].c_str());
			ssid = f[2];
			break;
		}
	}

	// Sound
	int v = 0;
	bool m = false;
	has_audio = parse_wpctl(part(3), v, m);
	if (has_audio) {
		volume = v;
		muted = m;
	}
	if (parse_wpctl(part(4), v, m)) {
		mic_muted = m;
		if (want_mic_ < 0 && !mic_busy_) mic_volume = v;
	}

	// Brightness: "intel_backlight,backlight,12345,45%,30000"
	const std::string b = part(5);
	const auto pct = b.find('%');
	has_brightness = pct != std::string::npos;
	if (has_brightness) {
		const auto comma = b.rfind(',', pct);
		brightness = std::atoi(b.c_str() + (comma == std::string::npos ? 0 : comma + 1));
	}

	// Bluetooth (and the power mode): only on full checks.
	if (full) {
	{
		const std::string bt = part(6);
		const bool adapter = bt.find("ADAPTER") != std::string::npos;
		const bool listed = bt.find("Controller ") != std::string::npos;
		bt_available = adapter || listed;
		bt_service = listed || bt.find("SERVICE") != std::string::npos;
		bt_tool = bt.find("NOCTL") == std::string::npos;
		bt_powered = listed && bt.find("Powered: yes") != std::string::npos;
	}
	bt_device.clear();
	for (const std::string &line : lines_of(part(7))) {
		// "Device AA:BB:CC:DD:EE:FF Name with spaces"
		if (line.rfind("Device ", 0) == 0 && line.size() > 25) {
			bt_device = line.substr(25);
			break;
		}
	}

	if (!profile_busy_) {
		power_profile = trim(part(8));
	}
	audio_output = trim(part(9));
	}
	if (net_kind == "none") {
		ssid.clear();
	}
	if (airplane && wifi_enabled) {
		airplane = false;   // something else switched Wi-Fi back on
	}
	// Redraw the panels only when something they show changed.
	const std::string now = state_of(*this);
	if (now != last_state_) {
		last_state_ = now;
		notify();
	}
}

namespace {
const char *kMediaFormat = "'{{status}}\t{{title}}\t{{artist}}\t{{mpris:artUrl}}\t{{position}}\t{{mpris:length}}"
	"\t{{playerName}}\t{{album}}'";

// "file:///home/me/My%20Music/a.jpg" -> "/home/me/My Music/a.jpg"
std::string uri_path(const std::string &uri) {
	std::string out;
	for (size_t i = 7; i < uri.size(); i++) {
		if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
				std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
			out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else {
			out += uri[i];
		}
	}
	return out;
}

std::string art_cache_dir() {
	const char *xdg = std::getenv("XDG_CACHE_HOME");
	const char *home = std::getenv("HOME");
	return (xdg && *xdg ? std::string(xdg) : std::string(home ? home : "/tmp") + "/.cache") + "/dusk/art";
}
} // namespace

void SystemStatus::start_media_watch() {
	if (media_pid_ > 0) return;
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) return;
	const pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		signal(SIGPIPE, SIG_DFL);
		dup2(fds[1], STDOUT_FILENO);
		const std::string cmd = std::string("exec playerctl -a --follow metadata --format ") + kMediaFormat + " 2>/dev/null";
		execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	media_pid_ = pid;
	media_fd_ = fds[0];
	media_buf_.clear();
	media_source_ = wl_event_loop_add_fd(server_->loop, media_fd_, WL_EVENT_READABLE, &SystemStatus::handle_media_output, this);
}

void SystemStatus::start_event_watch() {
	if (event_pid_ > 0) return;
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) return;
	const pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		signal(SIGPIPE, SIG_DFL);
		dup2(fds[1], STDOUT_FILENO);
		const int null = open("/dev/null", O_WRONLY);
		if (null >= 0) dup2(null, STDERR_FILENO);
		// Each one prints a word and a line when its kind of thing changes.
		const char *cmd =
			"command -v pactl >/dev/null && { pactl subscribe | grep --line-buffered -E \"on (sink|source|server|card)\" "
			"| sed -u 's/^/snd /'; } & "
			"command -v nmcli >/dev/null && { nmcli monitor | sed -u 's/^/net /'; } & "
			"command -v udevadm >/dev/null && { udevadm monitor --udev -s backlight -s power_supply "
			"| grep --line-buffered ' change ' | sed -u 's/^/dev /'; } & "
			"command -v dbus-monitor >/dev/null && { dbus-monitor --system \"type='signal',sender='org.bluez'\" "
			"| grep --line-buffered -E 'PropertiesChanged|Interfaces(Added|Removed)' | sed -u 's/^/bt /'; } & "
			"wait";
		execl("/bin/sh", "/bin/sh", "-c", cmd, static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	event_pid_ = pid;
	event_fd_ = fds[0];
	event_buf_.clear();
	event_source_ = wl_event_loop_add_fd(server_->loop, event_fd_, WL_EVENT_READABLE, &SystemStatus::handle_event_output, this);
	if (!event_settle_) event_settle_ = wl_event_loop_add_timer(server_->loop, &SystemStatus::handle_event_settle, this);
}

void SystemStatus::stop_event_watch() {
	if (event_source_) wl_event_source_remove(event_source_);
	event_source_ = nullptr;
	if (event_fd_ >= 0) close(event_fd_);
	event_fd_ = -1;
	if (event_pid_ > 0) {
		kill(-event_pid_, SIGTERM);   // the whole group: every watcher in it
		waitpid(event_pid_, nullptr, 0);
	}
	event_pid_ = -1;
}

int SystemStatus::handle_event_output(int fd, uint32_t mask, void *data) {
	auto *self = static_cast<SystemStatus *>(data);
	char buf[4096];
	for (;;) {
		const ssize_t n = read(fd, buf, sizeof buf);
		if (n > 0) {
			self->event_buf_.append(buf, static_cast<size_t>(n));
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR) && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) break;
		self->stop_event_watch();   // (the timer keeps asking, as before)
		return 0;
	}
	size_t nl;
	bool any = false;
	while ((nl = self->event_buf_.find('\n')) != std::string::npos) {
		const std::string line = self->event_buf_.substr(0, nl);
		self->event_buf_.erase(0, nl + 1);
		if (line.rfind("net ", 0) == 0 || line.rfind("bt ", 0) == 0) self->event_full_ = true;
		if (line.rfind("dev ", 0) == 0) self->event_battery_ = true;
		any = true;
	}
	if (self->event_buf_.size() > 65536) self->event_buf_.clear();
	// A burst of changes (a device connecting prints many lines): one look, shortly after.
	if (any) wl_event_source_timer_update(self->event_settle_, 250);
	return 0;
}

int SystemStatus::handle_event_settle(void *data) {
	auto *self = static_cast<SystemStatus *>(data);
	if (self->server_->screens_off) return 0;
	if (self->event_battery_) self->read_battery();
	self->refresh(self->event_full_);
	self->event_full_ = self->event_battery_ = false;
	return 0;
}

void SystemStatus::stop_media_watch() {
	if (media_source_) wl_event_source_remove(media_source_);
	media_source_ = nullptr;
	if (media_fd_ >= 0) close(media_fd_);
	media_fd_ = -1;
	if (media_pid_ > 0) {
		kill(media_pid_, SIGTERM);
		waitpid(media_pid_, nullptr, 0);
	}
	media_pid_ = -1;
}

int SystemStatus::handle_media_output(int fd, uint32_t mask, void *data) {
	auto *self = static_cast<SystemStatus *>(data);
	char buf[4096];
	for (;;) {
		const ssize_t n = read(fd, buf, sizeof buf);
		if (n > 0) {
			self->media_buf_.append(buf, static_cast<size_t>(n));
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR) && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) break;
		// playerctl ended (not installed, or it quit): try again in a while.
		self->stop_media_watch();
		wl_event_source_timer_update(self->media_retry_, 15000);
		return 0;
	}
	size_t nl;
	while ((nl = self->media_buf_.find('\n')) != std::string::npos) {
		const std::string line = self->media_buf_.substr(0, nl);
		self->media_buf_.erase(0, nl + 1);
		self->apply_media_line(line);
	}
	return 0;
}

int SystemStatus::handle_media_retry(void *data) {
	static_cast<SystemStatus *>(data)->start_media_watch();
	return 0;
}

namespace {
std::vector<std::string> media_fields(const std::string &line) {
	std::vector<std::string> f;
	std::string field;
	for (char c : line) {
		if (c == '\t') {
			f.push_back(field);
			field.clear();
		} else if (c != '\n' && c != '\r') {
			field += c;
		}
	}
	f.push_back(field);
	return f;
}
} // namespace

// Every player (playerctl -a): which one the cards follow is chosen from all of them.
void SystemStatus::refresh_media() {
	server_->run_capture(std::string("playerctl -a metadata --format ") + kMediaFormat + " 2>/dev/null",
		[this](const std::string &out) {
			std::map<std::string, Player> now;
			std::istringstream in(out);
			std::string line;
			while (std::getline(in, line)) {
				const auto f = media_fields(line);
				if (f.size() < 7 || f[6].empty()) continue;
				Player p;
				p.fields = f;
				auto old = players_.find(f[6]);
				p.since = old != players_.end() ? old->second.since : 0;
				if (f[0] == "Playing" && (old == players_.end() || old->second.fields[0] != "Playing")) p.since = now_ms();
				now[f[6]] = p;
			}
			players_ = std::move(now);
			choose_player();
		});
}

// A line from "playerctl -a --follow": one player changed.
void SystemStatus::apply_media_line(const std::string &out) {
	const auto f = media_fields(out);
	if (f.size() < 7 || f[6].empty()) {
		refresh_media();   // a player went away (or said nothing useful): ask them all
		return;
	}
	Player &p = players_[f[6]];
	const bool was_playing = !p.fields.empty() && p.fields[0] == "Playing";
	p.fields = f;
	if (f[0] == "Playing" && !was_playing) p.since = now_ms();
	if (f[0] != "Playing" && f[0] != "Paused") players_.erase(f[6]);   // stopped
	choose_player();
}

// The one to show: the one shown already while it plays; else the one that
// started playing last; else the one shown (paused); else any.
void SystemStatus::choose_player() {
	const Player *pick = nullptr;
	auto playing = [](const Player &p) { return !p.fields.empty() && p.fields[0] == "Playing"; };
	// Picked by hand (the card's media source menu): that one while it's there.
	if (!pinned_player_.empty()) {
		auto pin = players_.find(pinned_player_);
		if (pin != players_.end()) {
			show_player(pin->second.fields);
			return;
		}
		pinned_player_.clear();
	}
	auto cur = players_.find(media_player);
	if (cur != players_.end() && playing(cur->second)) {
		pick = &cur->second;
	} else {
		for (const auto &[name, p] : players_) {
			if (playing(p) && (!pick || p.since > pick->since)) pick = &p;
		}
		if (!pick && cur != players_.end()) pick = &cur->second;
		if (!pick && !players_.empty()) pick = &players_.begin()->second;
	}
	show_player(pick ? pick->fields : std::vector<std::string>{});
}

void SystemStatus::show_player(const std::vector<std::string> &f) {
	const bool was = media_available;
	const std::string old_title = media_title;
	const bool old_playing = media_playing;
	media_available = f.size() >= 2 && (f[0] == "Playing" || f[0] == "Paused");
	media_playing = media_available && f[0] == "Playing";
	media_title = media_available ? f[1] : "";
	media_artist = media_available && f.size() > 2 ? f[2] : "";
	media_art = media_available && f.size() > 3 ? f[3] : "";
	const bool had_position = media_length > 0;
	media_position = media_available && f.size() > 4 ? std::atof(f[4].c_str()) / 1e6 : 0;
	media_length = media_available && f.size() > 5 ? std::atof(f[5].c_str()) / 1e6 : 0;
	media_read_at = now_ms();
	const std::string old_player = media_player;
	media_player = media_available && f.size() > 6 ? f[6] : "";
	media_album = media_available && f.size() > 7 ? f[7] : "";
	const std::string old_art = media_art;
	if (media_art.rfind("file://", 0) == 0) {
		media_art = uri_path(media_art);
	} else if (media_art.rfind("https://", 0) == 0 || media_art.rfind("http://", 0) == 0) {
		// Streaming apps and browsers send a web address: fetched once into
		// the cache, shown when it's there.
		const std::string url = media_art;
		const std::string file = art_cache_dir() + "/" + std::to_string(std::hash<std::string>{}(url));
		media_art.clear();
		if (access(file.c_str(), R_OK) == 0) {
			media_art = file;
		} else if (art_tried_.insert(url).second) {
			server_->run_capture("mkdir -p " + shell_quote(art_cache_dir()) + " && curl -fsSL --max-time 10 "
				"--max-filesize 8000000 -o " + shell_quote(file + ".part") + " " + shell_quote(url) +
				" && mv " + shell_quote(file + ".part") + " " + shell_quote(file) + "; rm -f " +
				shell_quote(file + ".part"),
				[this, url, file](const std::string &) {
					if (access(file.c_str(), R_OK) == 0 && media_art_url_ == url) {
						media_art = file;
						notify();
					}
				});
		}
		media_art_url_ = url;
	} else {
		media_art.clear();
	}
	if (was != media_available || old_title != media_title || old_playing != media_playing ||
			had_position != (media_length > 0) || old_art != media_art || old_player != media_player) {
		notify();
	}
}

void SystemStatus::act(const std::string &command) {
	server_->run_capture(command + " >/dev/null 2>&1", [this](const std::string &) {
		refresh();
	});
}

void SystemStatus::set_volume(int percent, bool quiet) {
	want_volume_ = std::clamp(percent, 0, 100);
	volume = want_volume_;
	if (muted && percent > 0) {
		muted = false;
	}
	if (!quiet) notify();
	flush_volume();
}

void SystemStatus::flush_volume() {
	if (volume_busy_ || want_volume_ < 0) {
		return;
	}
	volume_busy_ = true;
	const int v = want_volume_;
	want_volume_ = -1;
	server_->run_capture("wpctl set-mute @DEFAULT_AUDIO_SINK@ 0; wpctl set-volume @DEFAULT_AUDIO_SINK@ " +
		std::to_string(v) + "% >/dev/null 2>&1", [this](const std::string &) {
			volume_busy_ = false;
			flush_volume();
		});
}

void SystemStatus::set_mic_volume(int percent, bool quiet) {
	want_mic_ = std::clamp(percent, 0, 100);
	mic_volume = want_mic_;
	if (mic_muted && percent > 0) {
		mic_muted = false;
	}
	if (!quiet) notify();
	flush_mic();
}

void SystemStatus::flush_mic() {
	if (mic_busy_ || want_mic_ < 0) {
		return;
	}
	mic_busy_ = true;
	const int v = want_mic_;
	want_mic_ = -1;
	server_->run_capture("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0; wpctl set-volume @DEFAULT_AUDIO_SOURCE@ " +
		std::to_string(v) + "% >/dev/null 2>&1", [this](const std::string &) {
			mic_busy_ = false;
			flush_mic();
		});
}

void SystemStatus::set_brightness(int percent, bool quiet) {
	want_brightness_ = std::clamp(percent, 1, 100);
	brightness = want_brightness_;
	if (!quiet) notify();
	flush_brightness();
}

void SystemStatus::flush_brightness() {
	if (brightness_busy_ || want_brightness_ < 0) {
		return;
	}
	brightness_busy_ = true;
	const int v = want_brightness_;
	want_brightness_ = -1;
	server_->run_capture("brightnessctl -q set " + std::to_string(v) + "% >/dev/null 2>&1",
		[this](const std::string &) {
			brightness_busy_ = false;
			flush_brightness();
		});
}

void SystemStatus::toggle_mute() {
	muted = !muted;
	notify();
	act("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
}

void SystemStatus::toggle_mic() {
	mic_muted = !mic_muted;
	notify();
	act("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle");
}

void SystemStatus::toggle_wifi() {
	wifi_enabled = !wifi_enabled;
	notify();
	act(std::string("nmcli radio wifi ") + (wifi_enabled ? "on" : "off"));
}

void SystemStatus::toggle_bluetooth() {
	if (bt_available && (!bt_service || !bt_tool)) {
		fix_bluetooth();
		return;
	}
	bt_powered = !bt_powered;
	notify();
	// rfkill first: a blocked adapter can't be powered on.
	act(bt_powered ? "rfkill unblock bluetooth; sleep 0.3; bluetoothctl power on"
		: "bluetoothctl power off");
}

// The adapter is there but Bluetooth can't be used: its service isn't
// running (many systems install it switched off), or the tools are missing.
void SystemStatus::fix_bluetooth() {
	if (!bt_tool) {
		bt_message = "Bluetooth needs bluez (Fedora: sudo dnf install bluez; Arch: bluez bluez-utils; "
			"Debian/Ubuntu: bluez)";
		notify();
		return;
	}
	bt_message = "Starting the Bluetooth service…";
	notify();
	// Asks for your password (polkit), then starts it now and at every start-up.
	server_->run_capture("pkexec systemctl enable --now bluetooth.service >/dev/null 2>&1 && echo OK; "
		"rfkill unblock bluetooth 2>/dev/null; sleep 1; bluetoothctl power on >/dev/null 2>&1; true",
		[this](const std::string &out) {
			bt_message = out.find("OK") != std::string::npos ? ""
				: "Couldn't start the Bluetooth service. In a terminal: sudo systemctl enable --now bluetooth";
			refresh();
			notify();
		});
}

void SystemStatus::toggle_airplane() {
	airplane = !airplane;
	if (airplane) {
		wifi_enabled = false;
		bt_powered = false;
		notify();
		act("nmcli radio all off; bluetoothctl power off; rfkill block bluetooth");
	} else {
		wifi_enabled = true;
		notify();
		act("rfkill unblock all; nmcli radio all on");
	}
}

void SystemStatus::toggle_caffeine() {
	if (caffeine_pid_ > 0) {
		kill(-caffeine_pid_, SIGTERM);   // the whole group: systemd-inhibit and its sleep
		waitpid(caffeine_pid_, nullptr, 0);
		caffeine_pid_ = -1;
		caffeine = false;
		notify();
		return;
	}
	// Holds a systemd "inhibitor lock" until we stop it: no automatic
	// suspend or screen blanking while it runs.
	const pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		signal(SIGPIPE, SIG_DFL);   // Dusk ignores it; the apps it starts must not
		execlp("systemd-inhibit", "systemd-inhibit", "--what=idle:sleep", "--who=Dusk",
			"--why=Caffeine is on", "--mode=block", "sleep", "infinity",
			static_cast<char *>(nullptr));
		_exit(127);
	}
	if (pid > 0) {
		caffeine_pid_ = pid;
		caffeine = true;
	}
	notify();
}

void SystemStatus::cycle_power_profile() {
	if (power_profile.empty()) {
		return;
	}
	const std::string next = power_profile == "balanced" ? "performance"
		: power_profile == "performance" ? "power-saver" : "balanced";
	set_power_profile(next);
}

void SystemStatus::set_power_profile(const std::string &profile) {
	if (power_profile.empty() || (profile != "power-saver" && profile != "balanced" &&
			profile != "performance")) {
		return;
	}
	power_profile = profile;
	profile_busy_ = true;
	notify();
	// Straight over D-Bus: the new name first (power-profiles-daemon 0.20+,
	// tuned-ppd), then the old one, then the command line tool. Each needs
	// only an active session, no password.
	const std::string p = shell_quote(profile);
	server_->run_capture(
		"busctl --system set-property org.freedesktop.UPower.PowerProfiles "
		"/org/freedesktop/UPower/PowerProfiles org.freedesktop.UPower.PowerProfiles ActiveProfile s " + p +
		" 2>&1 || busctl --system set-property net.hadess.PowerProfiles /net/hadess/PowerProfiles "
		"net.hadess.PowerProfiles ActiveProfile s " + p + " 2>&1 || powerprofilesctl set " + p +
		" 2>&1; echo \"EXIT:$?\"",
		[this, profile](const std::string &out) {
			profile_busy_ = false;
			if (out.find("EXIT:0") == std::string::npos) {
				std::fprintf(stderr, "dusk: couldn't switch the power mode to %s: %s\n",
					profile.c_str(), out.c_str());
			}
			refresh();
		});
}

void SystemStatus::media(const char *command) {
	if (std::string(command) == "play-pause") {
		media_playing = !media_playing;
		notify();
	}
	server_->run_capture(std::string("playerctl ") + player_arg() + command + " >/dev/null 2>&1",
		[this](const std::string &) { refresh_media(); });
}

std::vector<std::pair<std::string, std::string>> SystemStatus::player_list() const {
	std::vector<std::pair<std::string, std::string>> out;
	for (const auto &[name, p] : players_) {
		const std::string title = p.fields.size() > 1 ? p.fields[1] : "";
		const std::string state = p.fields.empty() ? "" : p.fields[0];
		out.push_back({name, title + (state == "Playing" ? "" : " (" + state + ")")});
	}
	return out;
}

void SystemStatus::pick_player(const std::string &name) {
	pinned_player_ = name;
	choose_player();
}

// The player shown: buttons act on it, not on whichever playerctl picks.
std::string SystemStatus::player_arg() const {
	return media_player.empty() ? std::string() : "-p " + shell_quote(media_player) + " ";
}

void SystemStatus::seek(double seconds) {
	media_position = std::max(0.0, seconds);
	media_read_at = now_ms();
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.2f", media_position);
	server_->run_capture(std::string("playerctl ") + player_arg() + "position " + buf + " >/dev/null 2>&1",
		[this](const std::string &) { refresh_media(); });
}

void SystemStatus::scan_wifi() {
	scanning = true;
	notify();
	server_->run_capture(
		"nmcli -t -f IN-USE,SIGNAL,SECURITY,SSID dev wifi list --rescan auto 2>/dev/null; echo " +
		std::string(kSep) + "; nmcli -t -f NAME,TYPE con show 2>/dev/null",
		[this](const std::string &out) {
			scanning = false;
			const auto s = sections(out);
			std::vector<std::string> saved;
			if (s.size() > 1) {
				for (const std::string &line : lines_of(s[1])) {
					const auto f = nm_fields(line);
					if (f.size() >= 2 && f[1].find("wireless") != std::string::npos) {
						saved.push_back(f[0]);
					}
				}
			}
			networks.clear();
			for (const std::string &line : lines_of(s.empty() ? "" : s[0])) {
				const auto f = nm_fields(line);
				if (f.size() < 4 || f[3].empty()) {
					continue;
				}
				WifiNetwork n;
				n.active = f[0] == "*";
				n.signal = std::atoi(f[1].c_str());
				n.secure = !f[2].empty() && f[2] != "--";
				n.ssid = f[3];
				n.saved = std::find(saved.begin(), saved.end(), n.ssid) != saved.end();
				// The same network seen on several access points: keep the strongest.
				auto dup = std::find_if(networks.begin(), networks.end(),
					[&](const WifiNetwork &o) { return o.ssid == n.ssid; });
				if (dup != networks.end()) {
					dup->active = dup->active || n.active;
					dup->signal = std::max(dup->signal, n.signal);
					continue;
				}
				networks.push_back(n);
			}
			std::stable_sort(networks.begin(), networks.end(),
				[](const WifiNetwork &a, const WifiNetwork &b) {
					if (a.active != b.active) return a.active;
					return a.signal > b.signal;
				});
			notify();
		});
}

void SystemStatus::connect_wifi(const WifiNetwork &net, const std::string &password,
		std::function<void(bool, std::string)> done) {
	std::string command;
	if (net.saved && password.empty()) {
		command = "nmcli con up id " + shell_quote(net.ssid);
	} else if (!password.empty()) {
		command = "nmcli dev wifi connect " + shell_quote(net.ssid) + " password " +
			shell_quote(password);
	} else {
		command = "nmcli dev wifi connect " + shell_quote(net.ssid);
	}
	server_->run_capture(command + " 2>&1; echo \"EXIT:$?\"", [this, done](const std::string &out) {
		const bool ok = out.find("EXIT:0") != std::string::npos;
		std::string reason;
		if (!ok) {
			const bool needs_password = out.find("Secrets were required") != std::string::npos ||
				out.find("password") != std::string::npos ||
				out.find("802-11-wireless-security") != std::string::npos;
			reason = needs_password ? "password" : "failed";
		}
		refresh();
		if (done) {
			done(ok, reason);
		}
	});
}

namespace {
// "Device AA:BB:CC:DD:EE:FF Name" lines -> devices.
std::vector<BtDevice> parse_devices(const std::string &text) {
	std::vector<BtDevice> out;
	std::string line;
	std::stringstream ss(text);
	while (std::getline(ss, line)) {
		if (line.rfind("Device ", 0) != 0 || line.size() < 25) continue;
		BtDevice d;
		d.address = line.substr(7, 17);
		d.name = line.substr(25);
		out.push_back(d);
	}
	return out;
}
} // namespace

void SystemStatus::scan_bluetooth() {
	if (bt_available && (!bt_service || !bt_tool)) {
		bt_message = !bt_tool ? "Bluetooth needs bluez installed (Fedora: sudo dnf install bluez)."
			: "The Bluetooth service isn't running. Switch Bluetooth on above to start it (it asks for your password).";
		scanning = false;
		notify();
		return;
	}
	scanning = true;
	notify();
	// First what's paired (instant), then a 10 second search for devices
	// nearby that are in pairing mode.
	const std::string known = "timeout 3 bluetoothctl devices Paired 2>/dev/null; echo " +
		std::string(kSep) + "; timeout 3 bluetoothctl devices Connected 2>/dev/null";
	server_->run_capture(known, [this, known](const std::string &out) {
		const auto s = sections(out);
		bt_devices.clear();
		for (BtDevice d : parse_devices(s.empty() ? "" : s[0])) {
			d.connected = s.size() > 1 && s[1].find(d.address) != std::string::npos;
			bt_devices.push_back(d);
		}
		std::stable_sort(bt_devices.begin(), bt_devices.end(),
			[](const BtDevice &a, const BtDevice &b) { return a.connected > b.connected; });
		notify();
		server_->run_capture("timeout 14 bluetoothctl --timeout 10 scan on >/dev/null 2>&1; " + known +
			"; echo " + std::string(kSep) + "; timeout 3 bluetoothctl devices 2>/dev/null",
			[this](const std::string &out2) {
				scanning = false;
				const auto s2 = sections(out2);
				const std::string paired = s2.size() > 0 ? s2[0] : "";
				const std::string connected = s2.size() > 1 ? s2[1] : "";
				std::vector<BtDevice> list;
				for (BtDevice d : parse_devices(paired)) {
					d.connected = connected.find(d.address) != std::string::npos;
					list.push_back(d);
				}
				std::stable_sort(list.begin(), list.end(),
					[](const BtDevice &a, const BtDevice &b) { return a.connected > b.connected; });
				for (BtDevice d : parse_devices(s2.size() > 2 ? s2[2] : "")) {
					if (paired.find(d.address) != std::string::npos) continue;
					// Nameless devices show their address with dashes: skip those.
					std::string dashed = d.address;
					std::replace(dashed.begin(), dashed.end(), ':', '-');
					if (d.name == dashed || d.name.empty()) continue;
					d.paired = false;
					list.push_back(d);
				}
				bt_devices = list;
				notify();
			});
	});
}

void SystemStatus::connect_bluetooth(const BtDevice &device) {
	if (!bt_busy.empty()) {
		return;   // one at a time
	}
	const std::string mac = shell_quote(device.address);
	std::string cmd;
	if (!device.paired) {
		// One bluetoothctl session for everything: its agent answers the
		// pairing, and it keeps searching meanwhile (BlueZ forgets devices
		// it found as soon as a search stops, so a pair started from a
		// separate command finds nothing). Waits for each step instead of
		// guessing how long it takes.
		cmd = "rfkill unblock bluetooth 2>/dev/null; ( "
			"echo 'power on'; echo 'agent KeyboardDisplay'; echo 'default-agent'; echo 'scan on'; sleep 2; "
			"echo 'pair " + device.address + "'; "
			"for i in $(seq 30); do sleep 1; echo yes; "
			"bluetoothctl info " + mac + " 2>/dev/null | grep -q 'Paired: yes' && break; done; "
			"echo 'trust " + device.address + "'; sleep 1; echo 'scan off'; "
			"echo 'connect " + device.address + "'; "
			"for i in $(seq 15); do sleep 1; "
			"bluetoothctl info " + mac + " 2>/dev/null | grep -q 'Connected: yes' && break; done; "
			"echo quit ) | timeout 60 bluetoothctl >/dev/null 2>&1; ";
	} else {
		cmd = "timeout 20 bluetoothctl --timeout 15 " + std::string(device.connected ? "disconnect " :
			"connect ") + mac + " >/dev/null 2>&1; ";
	}
	cmd += "timeout 3 bluetoothctl info " + mac + " 2>/dev/null | grep -E 'Paired:|Connected:'";
	bt_busy = device.address;
	bt_message.clear();
	notify();
	const BtDevice d = device;
	server_->run_capture(cmd, [this, d](const std::string &out) {
		bt_busy.clear();
		const bool paired = out.find("Paired: yes") != std::string::npos;
		const bool connected = out.find("Connected: yes") != std::string::npos;
		if (!d.paired && !paired) {
			bt_message = "Couldn't pair " + d.name + ". Put it in pairing mode and try again.";
		} else if (!d.connected && !connected) {
			bt_message = "Couldn't connect to " + d.name + ".";
		}
		scan_bluetooth();
		refresh();
	});
}

void SystemStatus::forget_bluetooth(const BtDevice &device) {
	server_->run_capture("timeout 5 bluetoothctl remove " + shell_quote(device.address) + " >/dev/null 2>&1",
		[this](const std::string &) { scan_bluetooth(); });
}

} // namespace dusk
