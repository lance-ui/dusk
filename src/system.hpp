// What the panel's status icons and the control centre show: battery,
// network, volume, brightness, Bluetooth, power mode and media. Read with
// the usual Fedora tools (nmcli, wpctl, brightnessctl, bluetoothctl,
// powerprofilesctl, playerctl) without ever blocking the desktop.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "wlr.hpp"

namespace dusk {

class Server;

struct WifiNetwork {
	std::string ssid;
	int signal = 0;        // 0..100
	bool secure = false;
	bool active = false;
	bool saved = false;    // NetworkManager has a profile for it
};

struct BtDevice {
	std::string address, name;
	bool connected = false;
	bool paired = true;     // false: found nearby, not set up yet
};

class SystemStatus {
public:
	explicit SystemStatus(Server *server);
	~SystemStatus();
	SystemStatus(const SystemStatus &) = delete;
	SystemStatus &operator=(const SystemStatus &) = delete;

	// ---- state ----
	bool has_battery = false;
	bool battery_dead = false;   // there's a battery, but it's worn out (0% on the charger): treated as none
	int battery = 100;              // percent
	bool charging = false;            // on mains power (charging, full, or held)
	int on_mains = -1;                // the charger, as the computer sees it: 1 in, 0 out, -1 can't tell
	std::string battery_state;        // "Charging", "Full", "Not charging", "Discharging"

	std::string net_kind = "none";  // none, wifi, ethernet
	std::string ssid;
	int wifi_signal = 0;
	bool wifi_enabled = true;
	bool wifi_available = false;    // there is a Wi-Fi adapter

	int volume = 50;
	bool muted = false;
	bool mic_muted = false;
	int mic_volume = 100;
	bool has_audio = false;

	int brightness = 100;
	bool has_brightness = false;

	bool bt_available = false;
	bool bt_powered = false;
	bool bt_service = true;         // the Bluetooth service (bluetoothd) is running
	bool bt_tool = true;            // bluetoothctl is installed
	std::string bt_device;          // a connected device's name

	std::string power_profile;      // power-saver, balanced, performance ("" = unavailable)
	bool airplane = false;
	bool caffeine = false;
	// Privacy: an app is recording from the microphone, or using the camera.
	bool mic_in_use = false;
	bool camera_in_use = false;

	bool media_available = false;
	bool media_playing = false;
	std::string media_title, media_artist, media_art;
	std::string media_album;
	std::string media_player;       // the app playing it ("spotify", "firefox"...)
	std::string audio_output;       // the speakers / headphones sound goes to (their name)
	double media_position = 0;      // seconds, when last read
	double media_length = 0;        // seconds (0 = unknown)
	double media_read_at = 0;       // now_ms() of that reading

	std::vector<WifiNetwork> networks;   // after scan_wifi()
	std::vector<BtDevice> bt_devices;    // after scan_bluetooth(): paired first, then nearby
	std::string bt_busy;                 // address being paired/connected
	std::string bt_message;              // the last pairing problem
	bool scanning = false;

	// Called whenever something above changes.
	std::function<void()> changed;

	// ---- actions ----
	void refresh(bool full = true);   // now; `full` includes the slower checks
	void refresh_media();
	// Follows the player (playerctl --follow): play/pause, track changes and
	// seeking show at once instead of at the next poll.
	void start_media_watch();
	void stop_media_watch();
	void apply_media_line(const std::string &line);
	// `quiet`: while a slider is dragged: no "changed" for every step (that
	// redrew every panel and card for each pointer move); one at the end.
	void set_volume(int percent, bool quiet = false);
	void set_mic_volume(int percent, bool quiet = false);
	void set_brightness(int percent, bool quiet = false);
	void toggle_mute();
	void toggle_mic();
	void toggle_wifi();
	void toggle_bluetooth();
	void fix_bluetooth();   // starts the Bluetooth service (asks for your password)
	void toggle_airplane();
	void toggle_caffeine();
	void cycle_power_profile();
	void set_power_profile(const std::string &profile);
	void media(const char *command);    // play-pause, next, previous
	// Everything playing or paused: (player, its title); and picking which one the cards show.
	std::vector<std::pair<std::string, std::string>> player_list() const;
	void pick_player(const std::string &name);
	void seek(double seconds);

	void scan_wifi();
	// Connects to a network; `done` gets false and a reason if it failed
	// (e.g. a password is needed).
	void connect_wifi(const WifiNetwork &net, const std::string &password,
		std::function<void(bool, std::string)> done);
	void scan_bluetooth();
	void connect_bluetooth(const BtDevice &device);   // pairs it first if needed
	void forget_bluetooth(const BtDevice &device);

private:
	// Told when things change, instead of only asking every few seconds: one
	// long-running watcher (sound: pactl subscribe; network: nmcli monitor;
	// brightness and power: udevadm monitor; Bluetooth: dbus-monitor), each
	// line it prints is a nudge to look again (gathered for a moment first).
	void start_event_watch();
	void stop_event_watch();
	static int handle_event_output(int fd, uint32_t mask, void *data);
	static int handle_event_settle(void *data);
	pid_t event_pid_ = -1;
	int event_fd_ = -1;
	wl_event_source *event_source_ = nullptr;
	wl_event_source *event_settle_ = nullptr;
	std::string event_buf_;
	bool event_full_ = false, event_battery_ = false;
	int dead_reads_ = 0;
	pid_t media_pid_ = -1;
	int media_fd_ = -1;
	wl_event_source *media_source_ = nullptr, *media_retry_ = nullptr;
	std::string media_buf_;
	std::string media_art_url_;              // the web address of the art now
	std::set<std::string> art_tried_;        // art addresses already fetched (or tried)
	// Every player playerctl knows: their last report, and when they started playing.
	struct Player {
		std::vector<std::string> fields;
		double since = 0;
	};
	std::map<std::string, Player> players_;
	std::string pinned_player_;   // picked by hand
	void choose_player();
	void show_player(const std::vector<std::string> &fields);
	std::string player_arg() const;
	static int handle_media_output(int fd, uint32_t mask, void *data);
	static int handle_media_retry(void *data);
	static int handle_timer(void *data);
	void read_battery();
	void parse_poll(const std::string &out, bool full);
	void notify();
	// Runs commands that change something, then refreshes.
	void act(const std::string &command);
	void flush_volume();
	void flush_brightness();

	Server *server_;
	wl_event_source *timer_ = nullptr;
	wl_event_source *privacy_timer_ = nullptr;
	bool privacy_busy_ = false;
	void poll_privacy();
	static int handle_privacy_timer(void *data);
	pid_t caffeine_pid_ = -1;
	bool polling_ = false;
	int poll_again_ = 0;            // 1 = quick, 2 = full: asked for while a check ran
	bool profile_busy_ = false;     // a power mode switch is under way
	// Slider drags send many values; only one command runs at a time.
	int want_volume_ = -1, want_brightness_ = -1, want_mic_ = -1;
	bool mic_busy_ = false;
	void flush_mic();
	bool volume_busy_ = false, brightness_busy_ = false;
	int ticks_ = 0;
	std::string last_state_;   // to only tell others when something changed
};

} // namespace dusk
