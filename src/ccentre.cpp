#include "ccentre.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>
#include <pwd.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#include "apps.hpp"
#include "draw.hpp"
#include "glyphs.hpp"
#include "launcher.hpp"
#include "lyrics.hpp"
#include "notify.hpp"
#include "panel.hpp"
#include "popover.hpp"
#include "system.hpp"

namespace dusk {

namespace {

constexpr int kWidth = 392;
constexpr int kPad = 16;
constexpr int kMargin = 10;      // from the screen edge / panel
constexpr int kCols = 6;         // the tile grid
constexpr int kGap = 8;
constexpr int kMaxRows = 14;

bool inside(const wlr_box &b, double x, double y) {
	return x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height;
}

std::string ago(std::time_t when) {
	const long d = static_cast<long>(std::time(nullptr) - when);
	if (d < 60) return "now";
	if (d < 3600) return std::to_string(d / 60) + " min ago";
	if (d < 86400) return std::to_string(d / 3600) + " h ago";
	return std::to_string(d / 86400) + " d ago";
}

// An on/off switch; `t` goes 0 -> 1 as it turns on. Half way the knob
// stretches into a pill, so it looks like it's being pulled across.
void draw_switch(cairo_t *cr, double x, double y, double t) {
	t = std::clamp(t, 0.0, 1.0);
	fill_rounded(cr, x, y, 44, 24, 12, mix(theme::tile_off, theme::accent, t));
	const double stretch = 9 * std::sin(M_PI * t);
	const double kx = x + 3 + t * 20 - stretch * t;
	fill_rounded(cr, kx, y + 3, 18 + stretch, 18, 9, mix(rgb(0xffffff), theme::on_accent, t));
}

// ---- what can be a tile ------------------------------------------------------

struct TileDef {
	const char *id, *name, *kind;   // kind: toggle, slider, info, action
	int w, h;                        // size when added
};
const TileDef kTiles[] = {
	{"wifi", "Wi-Fi", "toggle", 3, 1},
	{"bluetooth", "Bluetooth", "toggle", 3, 1},
	{"dnd", "Do not disturb", "toggle", 1, 1},
	{"dark", "Dark mode", "toggle", 1, 1},
	{"nightlight", "Night light", "toggle", 1, 1},
	{"screenshot", "Screenshot", "action", 1, 1},
	{"airplane", "Airplane mode", "toggle", 1, 1},
	{"caffeine", "Caffeine", "toggle", 2, 1},
	{"power", "Power mode", "toggle", 1, 1},
	{"brightness", "Brightness", "slider", 1, 3},
	{"volume", "Volume", "slider", 1, 3},
	{"mic", "Microphone", "slider", 1, 3},
	{"battery", "Battery", "info", 3, 1},
	{"media", "Now playing", "info", 6, 3},
};

const TileDef *tile_def(const std::string &id) {
	for (const TileDef &d : kTiles) {
		if (id == d.id) return &d;
	}
	return nullptr;
}

// Tiles that make no sense on this computer are left out.
bool tile_available(Server *server, const std::string &id) {
	const SystemStatus &st = *server->status;
	if (id == "brightness") return st.has_brightness;
	// Without a battery (a desktop, or a dead one) the battery tile is just
	// the power mode selector, the same design.
	if (id == "battery") return st.has_battery || !st.power_profile.empty();
	if (id == "mic") return st.has_audio;
	// With a battery, its tile is the power mode tile too (one tile, not two).
	if (id == "power") return !st.power_profile.empty() && !st.has_battery;
	return tile_def(id) != nullptr;
}

// Like Samsung's quick panel: a row of round toggles (and a second for the
// rest), brightness and volume as wide bars, the battery, and the media card
// along the bottom.
std::vector<CcTile> default_layout(Server *server) {
	const bool brightness = tile_available(server, "brightness");
	const std::string &look = server->config.cc_style;
	auto fit = [server](std::vector<CcTile> l) {
		// Only what this computer has; the rest moves up into the gaps left.
		std::vector<CcTile> out;
		int drop = 0, last_row = -1;
		for (CcTile t : l) {
			if (!tile_available(server, t.id)) {
				if (t.r != last_row) drop++;
				last_row = t.r;
				continue;
			}
			out.push_back(t);
		}
		(void)drop;
		return out;
	};
	// Every look has the same tiles (the ones this computer has), only
	// arranged its own way: the connections, Do not disturb, dark mode,
	// night light, Caffeine, screenshot, the battery with the power modes,
	// brightness, volume, the microphone and what's playing.
	if (look == "windows") {
		// Windows 11: what's playing, buttons three to a row, the bars.
		std::vector<CcTile> l = {{"media", 0, 0, 6, 3}};
		int n = 0;
		for (const char *id : {"wifi", "bluetooth", "airplane", "dnd", "nightlight", "dark", "caffeine",
				"screenshot"}) {
			if (!tile_available(server, id)) continue;
			l.push_back({id, (n % 3) * 2, 3 + (n / 3) * 2, 2, 2});
			n++;
		}
		int row = 3 + ((n + 2) / 3) * 2;
		// The battery with the power modes, full width (square-cornered here).
		if (tile_available(server, "battery")) l.push_back({"battery", 0, row++, 6, 1});
		if (brightness) l.push_back({"brightness", 0, row++, 6, 1});
		l.push_back({"volume", 0, row++, 6, 1});
		if (tile_available(server, "mic")) l.push_back({"mic", 0, row++, 6, 1});
		return l;
	}
	if (look == "gnome") {
		// GNOME: brightness, pill toggles two by two, the battery, then
		// sound and what's playing.
		return fit({{"brightness", 0, 0, 6, 1},
			{"wifi", 0, 1, 3, 1}, {"bluetooth", 3, 1, 3, 1}, {"nightlight", 0, 2, 3, 1},
			{"dark", 3, 2, 3, 1}, {"airplane", 0, 3, 3, 1}, {"dnd", 3, 3, 3, 1}, {"caffeine", 0, 4, 3, 1},
			{"screenshot", 3, 4, 3, 1}, {"battery", 0, 5, 6, 1},
			{"volume", 0, 6, 6, 1}, {"mic", 0, 7, 6, 1}, {"media", 0, 8, 6, 1}});
	}
	if (look == "macos") {
		// macOS: the connections down the left, what's playing on the right,
		// three square modules, two wide ones (room for their names), the
		// battery, then Display and Sound.
		return fit({{"wifi", 0, 0, 3, 1}, {"bluetooth", 0, 1, 3, 1}, {"airplane", 0, 2, 3, 1}, {"media", 3, 0, 3, 3},
			{"dark", 0, 3, 2, 2}, {"screenshot", 2, 3, 2, 2}, {"dnd", 4, 3, 2, 2},
			{"nightlight", 0, 5, 3, 1}, {"caffeine", 3, 5, 3, 1}, {"battery", 0, 6, 6, 1},
			{"brightness", 0, 7, 6, 1}, {"volume", 0, 8, 6, 1}, {"mic", 0, 9, 6, 1}});
	}
	// Dusk: Wi-Fi and Bluetooth as pills with round toggles under them on
	// the left, brightness, volume and the microphone as tall bars on the
	// right, then the battery with the power modes and what's playing.
	std::vector<CcTile> l = {{"wifi", 0, 0, 3, 1}, {"bluetooth", 0, 1, 3, 1}};
	int n = 0;
	for (const char *id : {"airplane", "dark", "nightlight", "dnd", "caffeine", "screenshot"}) {
		if (!tile_available(server, id)) continue;
		l.push_back({id, n % 3, 2 + n / 3, 1, 1});
		n++;
	}
	int col = 3;
	for (const char *id : {"brightness", "volume", "mic"}) {
		if (id == std::string("brightness") ? !brightness : !tile_available(server, id)) continue;
		l.push_back({id, col++, 0, 1, 4});
	}
	int row = 4;
	if (tile_available(server, "battery")) l.push_back({"battery", 0, row++, 6, 1});
	l.push_back({"media", 0, row, 6, 4});
	return l;
}

void lyrics_note(Server *srv);

bool overlaps(const CcTile &a, const CcTile &b) {
	return a.c < b.c + b.w && b.c < a.c + a.w && a.r < b.r + b.h && b.r < a.r + a.h;
}

// Does `t` fit in `l` (not counting the tiles named)?
bool fits(const std::vector<CcTile> &l, const CcTile &t, const std::string &skip1,
		const std::string &skip2 = "") {
	if (t.c < 0 || t.r < 0 || t.w < 1 || t.h < 1 || t.c + t.w > kCols || t.r + t.h > kMaxRows) {
		return false;
	}
	for (const CcTile &o : l) {
		if (o.id != skip1 && o.id != skip2 && overlaps(o, t)) return false;
	}
	return true;
}

struct TileState {
	std::string icon, title, subtitle;
	bool on = false;
	bool usable = true;   // e.g. no Wi-Fi adapter
	int value = 0;        // sliders, 0..100
	bool muted = false;   // sliders
};

TileState tile_state(Server *server, const std::string &id) {
	const SystemStatus &st = *server->status;
	const Config &c = server->config;
	TileState i;
	if (id == "wifi") {
		i.title = "Wi-Fi";
		i.on = st.wifi_enabled && !st.airplane;
		i.usable = st.wifi_available;
		i.icon = !i.on ? "offline" : st.ssid.empty() ? "wifi-0" : wifi_icon(st.wifi_signal);
		i.subtitle = !i.usable ? "No adapter" : !i.on ? "Off"
			: st.net_kind == "ethernet" && st.ssid.empty() ? "Wired" : st.ssid.empty() ? "Not connected" : st.ssid;
	} else if (id == "bluetooth") {
		i.title = "Bluetooth";
		i.on = st.bt_powered;
		i.usable = st.bt_available;
		i.icon = i.on ? "bluetooth" : "bluetooth-off";
		i.subtitle = !i.usable ? "No adapter" : !st.bt_tool ? "Needs bluez" : !st.bt_service ? "Service off"
			: !i.on ? "Off" : st.bt_device.empty() ? "On" : st.bt_device;
	} else if (id == "sound") {
		i.title = "Sound";
		i.on = !st.muted;
		i.usable = st.has_audio;
		i.icon = speaker_icon(st.volume, st.muted);
		i.subtitle = st.muted ? "Muted" : std::to_string(st.volume) + "%";
	} else if (id == "dark") {
		i.title = "Dark mode";
		i.on = c.dark;
		i.icon = "moon";
		i.subtitle = c.dark ? "Dark" : "Light";
	} else if (id == "nightlight") {
		i.title = "Night light";
		i.on = c.night_light;
		i.icon = "nightlight";
		i.subtitle = c.night_light ? std::to_string(c.night_light_temp) + " K" : "Off";
	} else if (id == "dnd") {
		i.title = "Do not disturb";
		i.on = c.dnd;
		i.icon = c.dnd ? "bell-off" : "bell";
		i.subtitle = c.dnd ? "On" : "Off";
	} else if (id == "airplane") {
		i.title = "Airplane mode";
		i.on = st.airplane;
		i.icon = "airplane";
		i.subtitle = st.airplane ? "On" : "Off";
	} else if (id == "caffeine") {
		i.title = "Caffeine";
		i.on = st.caffeine;
		i.icon = "coffee";
		i.subtitle = st.caffeine ? "Staying awake" : "Off";
	} else if (id == "power") {
		i.title = "Power mode";
		i.usable = !st.power_profile.empty();
		i.on = i.usable && st.power_profile != "balanced";
		i.icon = "gauge-" + st.power_profile;
		i.subtitle = st.power_profile == "power-saver" ? "Power saver"
			: st.power_profile == "performance" ? "Performance" : i.usable ? "Balanced" : "Unavailable";
	} else if (id == "screenshot") {
		i.title = "Screenshot";
		i.icon = "screenshot";
		i.subtitle = "Area, window, screen";
	} else if (id == "brightness") {
		i.title = "Brightness";
		i.icon = "sun";
		i.value = st.brightness;
		i.usable = st.has_brightness;
	} else if (id == "volume") {
		i.title = "Volume";
		i.icon = speaker_icon(st.volume, st.muted);
		i.value = st.volume;
		i.muted = st.muted;
		i.usable = st.has_audio;
	} else if (id == "mic") {
		i.title = "Microphone";
		i.icon = st.mic_muted ? "mic-off" : "mic";
		i.value = st.mic_volume;
		i.muted = st.mic_muted;
		i.usable = st.has_audio;
	} else if (id == "battery") {
		// The battery and the power mode in one: click to change the mode.
		i.title = !st.has_battery ? std::string("Power mode")
			: std::to_string(st.battery) + "%" + (st.battery_state == "Charging" ? " · charging" : "");
		i.icon = st.has_battery ? battery_icon(st.battery, st.charging) : "gauge-" + st.power_profile;
		i.on = !st.power_profile.empty() && st.power_profile != "balanced";
		i.subtitle = st.power_profile.empty() ? "Battery"
			: std::string("Power mode: ") + (st.power_profile == "power-saver" ? "Power saver"
				: st.power_profile == "performance" ? "Performance" : "Balanced");
	} else if (id == "media") {
		i.title = st.media_available ? (st.media_title.empty() ? "Playing" : st.media_title) : "Nothing playing";
		i.subtitle = st.media_available ? st.media_artist : "";
		i.on = st.media_playing;
		i.icon = st.media_playing ? "pause" : "play";
	}
	return i;
}

std::string serialise(const std::vector<CcTile> &l) {
	std::string out;
	for (const CcTile &t : l) {
		out += (out.empty() ? "" : ",") + t.id + ":" + std::to_string(t.c) + ":" + std::to_string(t.r) + ":" +
			std::to_string(t.w) + ":" + std::to_string(t.h);
	}
	return out;
}

std::string uptime_text() {
	std::ifstream f("/proc/uptime");
	double s = 0;
	f >> s;
	const long sec = static_cast<long>(s);
	const long d = sec / 86400, h = sec % 86400 / 3600, m = sec % 3600 / 60;
	if (d > 0) return std::to_string(d) + "d " + std::to_string(h) + "h";
	if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m";
	return std::to_string(m) + "m";
}

} // namespace

ControlCentre::ControlCentre(Server *server)
		: server_(server), layer_(server->layer_menu, &tag),
		fx_(server->anim.get(), [this] { if (open_ || alpha_ > 0) redraw(); }),
		hl_(layer_, server->anim.get()) {
	layer_.set_enabled(false);
	timer_ = wl_event_loop_add_timer(server->loop, &ControlCentre::handle_timer, this);
	redraw_timer_ = wl_event_loop_add_timer(server->loop, &ControlCentre::handle_redraw_timer, this);
	media_ = std::make_unique<MediaView>(server);
	live_ = wlr_scene_buffer_create(layer_.tree, nullptr);
	wlr_scene_node_place_above(&live_->node, &layer_.buffer->node);
	wlr_scene_node_set_enabled(&live_->node, false);
	// Your name: the full name from your account, else the login name.
	if (const passwd *pw = getpwuid(getuid())) {
		std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
		gecos = gecos.substr(0, gecos.find(','));
		user_name_ = gecos.empty() ? pw->pw_name : gecos;
		if (!user_name_.empty()) user_name_[0] = static_cast<char>(std::toupper(user_name_[0]));
	}
}

ControlCentre::~ControlCentre() {
	server_->anim->cancel(anim_);
	if (media_ticker_) server_->anim->remove_ticker(media_ticker_);
	wl_event_source_remove(timer_);
	wl_event_source_remove(redraw_timer_);
	media_.reset();
	if (avatar_) cairo_surface_destroy(avatar_);
}

int ControlCentre::handle_timer(void *data) {
	auto *self = static_cast<ControlCentre *>(data);
	if (!self->open_) {
		return 0;
	}
	self->server_->status->refresh_media();
	wl_event_source_timer_update(self->timer_, 3000);
	return 0;
}

// ---- opening and closing ----------------------------------------------------

void ControlCentre::open_notes(Output *output, wlr_box anchor, const std::string &edge) {
	open(output, anchor, edge);
	notes_anchor_ = anchor;
	notes_edge_ = edge;
	notes_only_ = true;
	if (server_->notifications && server_->notifications->unseen) {
		server_->notifications->unseen = 0;
		server_->redraw_panels();
	}
	redraw();
	place();
}

void ControlCentre::toggle_notes(Output *output, wlr_box anchor, const std::string &edge) {
	if (notes_open_on(output)) close();
	else open_notes(output, anchor, edge);
}

void ControlCentre::open(Output *output, wlr_box anchor, const std::string &edge) {
	notes_only_ = false;
	// Seen now: the panel's bell goes (unless they have a centre of their own).
	if (server_->notifications && server_->notifications->unseen && !server_->config.separate_notes()) {
		server_->notifications->unseen = 0;
		server_->redraw_panels();
	}
	if (!output) {
		return;
	}
	output_ = output;
	{
		// The compact looks open from the panel's end of the screen.
		const wlr_box o = output->box();
		bottom_ = edge == "bottom" || (edge != "top" && anchor.width > 0 && anchor.y + anchor.height / 2 > o.y + o.height / 2);
	}
	const std::string &side = server_->config.cc_side;
	if (side == "left") {
		left_side_ = true;
	} else if (side == "right") {
		left_side_ = false;
	} else if (edge == "left" || edge == "right") {
		left_side_ = edge == "left";
	} else {
		// Follow the button: whichever half of the screen it is on.
		const wlr_box o = output->box();
		left_side_ = anchor.width > 0 && anchor.x + anchor.width / 2 < o.x + o.width / 2;
	}
	open_ = true;
	page_ = Page::Tiles;
	editing_ = false;
	typing_ = false;
	notes_scroll_ = 0;
	open_groups_.clear();
	server_->status->refresh();
	server_->status->refresh_media();
	fx_.snap("notes", 0);
	redraw();
	place();
	layer_.set_enabled(true);
	layer_.raise();
	wl_event_source_timer_update(timer_, 3000);

	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(alpha_, 1.0, c.animate(c.anim_menu) ? c.ms(280) : 0,
		[this] { place(); }, [this] { anim_ = 0; });
	server_->redraw_panels();
}

void ControlCentre::close() {
	if (!open_) {
		return;
	}
	open_ = false;
	typing_ = false;
	editing_ = false;
	edit_tile_.clear();
	dragging_.clear();
	server_->cc_grab = false;
	if (media_->dragging()) media_->end_drag();
	media_->set_shown(false);
	wl_event_source_timer_update(timer_, 0);
	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(alpha_, 0.0, c.animate(c.anim_menu) ? c.ms(200) : 0,
		[this] { place(); },
		[this] {
			anim_ = 0;
			if (!open_) {
				layer_.set_enabled(false);
			}
		});
	server_->redraw_panels();
}

void ControlCentre::toggle(Output *output, wlr_box anchor, const std::string &edge) {
	if (open_ && output_ == output) {
		close();
	} else {
		open(output, anchor, edge);
	}
}

void ControlCentre::toggle(Output *output) {
	if (open_ || !output) {
		close();
		return;
	}
	for (const auto &panel : output->panels) {
		const auto &w = panel->config().widgets;
		if (std::find(w.begin(), w.end(), "status") != w.end()) {
			open(output, panel->anchor("status"), panel->config().edge);
			return;
		}
	}
	open(output, wlr_box{}, "right");
}

void ControlCentre::output_gone(Output *output) {
	if (output_ == output) {
		open_ = false;
		server_->anim->cancel(anim_);
		anim_ = 0;
		alpha_ = 0;
		layer_.set_enabled(false);
		media_->set_shown(false);
		output_ = nullptr;
	}
}

void ControlCentre::refresh() {
	if (open_ || alpha_ > 0) {
		queue_redraw();   // (at most once a frame: status changes come in bursts)
	}
}

// A tall card along the side of the screen; slides in from that side, with
// a small spring at the end (like Arqen's).
void ControlCentre::place() {
	if (!output_) {
		return;
	}
	const wlr_box u = output_->usable();
	const double t = std::clamp(alpha_, 0.0, 1.1);
	if (notes_only_) {
		// The notification centre keeps the place redraw_notes() gave it
		// (by its button); a short drop or rise as it fades in.
		const int lift = static_cast<int>(std::lround((1.0 - std::min(1.0, t)) * 14 * (bottom_ ? 1 : -1)));
		layer_.show(box_.x, box_.y + lift, std::min(1.0, t * 1.4), 0.97 + 0.03 * std::min(1.0, t), width_ / 2.0,
			bottom_ ? box_.height : 0);
		place_live();
		return;
	}
	const int x = left_side_ ? u.x + kMargin : u.x + u.width - width_ - kMargin;
	if (compact()) {
		// Windows, GNOME and macOS: a short drop (or rise, from a bottom panel) as it fades in.
		box_.x = x;
		const int lift = static_cast<int>(std::lround((1.0 - std::min(1.0, t)) * 18 * (bottom_ ? 1 : -1)));
		layer_.show(x, box_.y + lift, std::min(1.0, t * 1.4), 0.97 + 0.03 * std::min(1.0, t), width_ / 2.0,
			bottom_ ? box_.height : 0);
		place_live();
		return;
	}
	// From off the screen's edge, unless another screen is on that side (it
	// would show there): then a short slide while it fades.
	const wlr_box ob = output_->box();
	const double edge_x = left_side_ ? ob.x - 1.0 : ob.x + ob.width + 1.0;
	const bool neighbour = wlr_output_layout_output_at(server_->layout, edge_x, ob.y + ob.height / 2.0) != nullptr;
	const int reach = neighbour ? std::min(48, std::max(0, left_side_ ? x - ob.x : ob.x + ob.width - x - width_))
		: width_ + kMargin;
	const int slide = static_cast<int>(std::lround((1.0 - t) * reach));
	box_.x = x;
	// Sliding off the screen it can stay a little visible (it's gone past the
	// edge by the end); with the short slide it fades out completely, or
	// it would be left standing faintly and then vanish.
	const double alpha = neighbour ? std::min(1.0, t * t * (3 - 2 * std::min(1.0, t))) : std::min(1.0, 0.25 + t);
	layer_.show(left_side_ ? x - slide : x + slide, box_.y, alpha, 1.0, 0, 0);
	place_live();
}

// Hover is drawn by the GPU highlight (no redraws while the mouse moves).
double ControlCentre::hv(const std::string &) {
	return 0.0;
}

// Cards inside follow the sidebar's corners: outer radius = inner + padding.
int ControlCentre::card_radius() const {
	return std::max(10, theme::ui_radius - 4);
}

bool ControlCentre::contains(double lx, double ly) const {
	return open_ && inside(box_, lx, ly);
}

// ---- drawing ------------------------------------------------------------------

void ControlCentre::add_hot(const std::string &id, double x, double y, double w, double h,
		double radius, wlr_box vis) {
	const wlr_box box{static_cast<int>(x), static_cast<int>(y), static_cast<int>(w),
		static_cast<int>(h)};
	hots_.push_back(Hot{id, box, vis.width > 0 ? vis : box,
		radius == -1 ? static_cast<double>(card_radius()) : radius});
}

void ControlCentre::update_highlight() {
	for (const Hot &h : hots_) {
		if (h.id == hover_ && h.radius >= 0) {
			hl_.show(h.vis, h.radius, with_alpha(theme::text, theme::dark ? 0.09 : 0.07));
			return;
		}
	}
	hl_.hide();
}

std::string ControlCentre::hot_at(double px, double py) const {
	for (auto it = hots_.rbegin(); it != hots_.rend(); ++it) {
		if (inside(it->box, px, py)) {
			return it->id;
		}
	}
	return "";
}

wlr_box ControlCentre::tile_box(const CcTile &t) const {
	const double step = cell_ + kGap, vstep = row_h_ + kGap;
	return wlr_box{static_cast<int>(std::lround(kPad + t.c * step)),
		static_cast<int>(std::lround(grid_top_ + t.r * vstep)),
		static_cast<int>(std::lround(t.w * cell_ + (t.w - 1) * kGap)),
		static_cast<int>(std::lround(t.h * row_h_ + (t.h - 1) * kGap))};
}

void ControlCentre::redraw() {
	if (!output_ || drawing_) {
		return;
	}
	drawing_ = true;
	const wlr_box u = output_->usable();
	const std::string &look = server_->config.cc_style;
	if (look != last_style_) {
		// Another look: start clean (no half-finished edit, drag, list or
		// moving media card from the old one).
		last_style_ = look;
		editing_ = false;
		edit_tile_.clear();
		edit_resize_ = false;
		if (dragging_ == "mp:seek") media_->end_drag();
		dragging_.clear();
		server_->cc_grab = false;
		typing_ = false;
		page_ = Page::Tiles;
		fx_.snap("page", 0);
		notes_scroll_ = 0;
		hover_.clear();
		hl_.hide();
		media_box_ = wlr_box{};
		media_live_ = false;
		wlr_scene_node_set_enabled(&live_->node, false);
	}
	// A tile can only be held while the pointer holds it.
	if (!edit_tile_.empty() && !server_->cc_grab) {
		edit_tile_.clear();
		edit_resize_ = false;
	}
	if (notes_only_) {
		redraw_notes();
		drawing_ = false;
		place();
		update_highlight();
		return;
	}
	width_ = look == "macos" ? 340 : look == "windows" || look == "gnome" ? 364 : kWidth;
	box_.width = width_;
	box_.x = left_side_ ? u.x + kMargin : u.x + u.width - width_ - kMargin;
	media_box_ = wlr_box{};

	// The layout: saved, or the default; only tiles that work here.
	if (edit_tile_.empty()) {
		const Config &cfg = server_->config;
		const auto &saved = look == "windows" ? cfg.cc_layout_windows : look == "gnome" ? cfg.cc_layout_gnome
			: look == "macos" ? cfg.cc_layout_macos : cfg.cc_layout;
		layout_.clear();
		for (const CcTile &t : saved.empty() ? default_layout(server_) : saved) {
			if (tile_available(server_, t.id) && fits(layout_, t, "")) {
				layout_.push_back(t);
			}
		}
	}
	// Nothing playing: the media card shrinks to one row (an empty card
	// four rows tall was dead space), when nothing sits under it.
	if (look == "dusk" && !editing_ && !server_->status->media_available) {
		for (CcTile &t : layout_) {
			if (t.id != "media" || t.h <= 1) continue;
			const bool below = std::any_of(layout_.begin(), layout_.end(),
				[&t](const CcTile &o) { return o.r >= t.r + t.h; });
			if (!below) t.h = 1;
		}
	}

	const float scale = output_->output->scale;
	const double outer = card_radius() + kPad;
	const double slide = fx_.get("page");

	if (compact()) {
		// Sized to what's in it: measure first (drawing nowhere), then draw.
		const int max_h = u.height - 2 * kMargin;
		int content = 0;
		{
			Canvas probe(width_, 1, 1.0f);
			hots_.clear();
			int y = kPad;
			if (page_ == Page::Tiles) {
				if (look == "windows") draw_windows(probe.cr(), y, scale);
				else if (look == "gnome") draw_gnome(probe.cr(), y, scale);
				else draw_macos(probe.cr(), y, scale);
			} else {
				y += 380;   // the Wi-Fi / Bluetooth list
			}
			content = y;
		}
		const Notifications *notes = server_->notifications.get();
		size_t count = 0;
		if (notes) {
			for (const Notification &n : notes->list()) count += !n.transient;
		}
		const bool with_notes = page_ == Page::Tiles && server_->config.cc_notifications && count > 0 &&
			!server_->config.separate_notes();
		box_.height = std::min(max_h, content + (with_notes ? 340 : 0) + kPad);
		box_.y = bottom_ ? u.y + u.height - kMargin - box_.height : u.y + kMargin;
		media_box_ = wlr_box{};
		Canvas canvas(width_, box_.height, scale);
		cairo_t *cr = canvas.cr();
		hots_.clear();
		ui_card(cr, 0, 0, width_, box_.height, outer, with_alpha(theme::bg_alt, theme::ui_opacity));
		int y = kPad;
		const int bottom = box_.height - kPad;
		cairo_save(cr);
		cairo_rectangle(cr, 0, 0, width_, box_.height);
		cairo_clip(cr);
		if (page_ == Page::Tiles) {
			if (look == "windows") draw_windows(cr, y, scale);
			else if (look == "gnome") draw_gnome(cr, y, scale);
			else draw_macos(cr, y, scale);
			if (with_notes) draw_notifications(cr, y + 4, bottom, scale);
		} else {
			draw_detail(cr, y, bottom, scale);
		}
		cairo_restore(cr);
		media_live_ = page_ == Page::Tiles && media_box_.width > 0 && std::fabs(slide) < 0.01;
		layer_.present(canvas, wlr_box{0, 0, width_, box_.height}, outer);
		drawing_ = false;
		place();
		update_highlight();
		run_media();
		return;
	}

	box_.height = std::max(300, u.height - 2 * kMargin);
	if (server_->config.separate_notes() && !editing_ && page_ == Page::Tiles) {
		// No notifications under the tiles: only as tall as the tiles.
		Canvas probe(width_, 1, 1.0f);
		hots_.clear();
		int py = kPad;
		draw_header(probe.cr(), py, scale);
		draw_grid(probe.cr(), py, scale);
		box_.height = std::min(box_.height, py - 16 + kPad);
		media_box_ = wlr_box{};
	}
	box_.y = bottom_ ? u.y + u.height - kMargin - box_.height : u.y + kMargin;
	Canvas canvas(width_, box_.height, scale);
	cairo_t *cr = canvas.cr();
	hots_.clear();
	ui_card(cr, 0, 0, width_, box_.height, outer, with_alpha(theme::bg_alt, theme::ui_opacity));

	int y = kPad;
	draw_header(cr, y, scale);
	const int bottom = box_.height - kPad;

	// Changing page: the new one slides in and fades up.
	cairo_save(cr);
	cairo_rectangle(cr, 0, y - 4, width_, bottom - y + 4 + kPad);
	cairo_clip(cr);
	const double dx = slide * 60 * (page_ == Page::Tiles ? -1 : 1);
	cairo_translate(cr, dx, 0);
	cairo_push_group(cr);
	const size_t hots_before = hots_.size();
	if (page_ == Page::Tiles) {
		draw_grid(cr, y, scale);
		if (editing_) {
			draw_edit_tray(cr, y, scale);
		} else if (server_->config.cc_notifications && !server_->config.separate_notes()) {
			draw_notifications(cr, y, bottom, scale);
		}
	} else {
		draw_detail(cr, y, bottom, scale);
	}
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, 1.0 - 0.8 * std::fabs(slide));
	cairo_restore(cr);
	for (size_t i = hots_before; i < hots_.size(); i++) {
		hots_[i].box.x += static_cast<int>(std::lround(dx));
		hots_[i].vis.x += static_cast<int>(std::lround(dx));
	}
	media_live_ = page_ == Page::Tiles && !editing_ && media_box_.width > 0 && std::fabs(slide) < 0.01 &&
		media_box_.y + media_box_.height <= bottom;
	layer_.present(canvas, wlr_box{0, 0, width_, box_.height}, outer);
	drawing_ = false;
	place();
	update_highlight();
	run_media();
}

// The notification centre: the day and date, Do not disturb and the
// calendar, then the notifications; as tall as they need.
void ControlCentre::redraw_notes() {
	const wlr_box u = output_->usable();
	const float scale = output_->output->scale;
	width_ = 430;
	box_.width = width_;
	// Under (or over) the button that opened it, kept on the screen: the
	// bell at the right end, the clock in the middle of a GNOME-like bar.
	if (notes_anchor_.width > 0) {
		const int want = notes_anchor_.x + notes_anchor_.width / 2 - width_ / 2;
		box_.x = std::clamp(want, u.x + kMargin, u.x + u.width - width_ - kMargin);
	} else {
		box_.x = left_side_ ? u.x + kMargin : u.x + u.width - width_ - kMargin;
	}
	media_box_ = wlr_box{};
	media_live_ = false;
	wlr_scene_node_set_enabled(&live_->node, false);
	size_t count = 0;
	if (const Notifications *n = server_->notifications.get()) {
		for (const Notification &x : n->list()) count += !x.transient;
	}
	constexpr int kHead = 52, kFoot = 60;
	// As tall as the notifications really are (measured), up to most of the
	// screen; the list scrolls past that.
	int list_h = 150;
	if (count > 0) {
		const int keep = notes_scroll_;
		Canvas probe(width_, 1, 1.0f);
		hots_.clear();
		draw_notifications(probe.cr(), 0, 100000, scale);
		notes_scroll_ = keep;
		list_h = std::max(150, notes_total_ + 12);
	}
	list_h = std::min(list_h, std::max(200, u.height - 2 * kMargin - kPad - kHead - kFoot - 40));
	const int max_h = u.height - 2 * kMargin;
	box_.height = std::min(max_h, kPad + kHead + list_h + kFoot);
	box_.y = bottom_ ? u.y + u.height - kMargin - box_.height : u.y + kMargin;
	const double outer = card_radius() + kPad;
	Canvas canvas(width_, box_.height, scale);
	cairo_t *cr = canvas.cr();
	hots_.clear();
	ui_card(cr, 0, 0, width_, box_.height, outer, with_alpha(theme::bg_alt, theme::ui_opacity));

	// Header: the title; Clear all and Do not disturb on the right.
	const int hy = kPad;
	const double hc = hy + 20;
	draw_text(cr, "Notifications", kPad + 4, hy, width_ - 2 * kPad - 150, 40, theme::font_heading, theme::text);
	{
		const bool dnd = server_->config.dnd;
		const int bs = 36;
		int bx = width_ - kPad - bs;
		cairo_new_path(cr);
		cairo_arc(cr, bx + bs / 2.0, hc, bs / 2.0, 0, 2 * M_PI);
		set_color(cr, dnd ? theme::accent : theme::tile_off);
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, dnd ? "bell-off" : "bell", bx + bs / 2.0 - 9, hc - 9, 18,
			dnd ? theme::on_accent : theme::text, scale);
		add_hot("ti:dnd", bx, static_cast<int>(hc - bs / 2.0), bs, bs, bs / 2.0);
		if (count > 0) {
			const std::string label = "Clear all";
			const int cw = text_width(label, theme::font_small) + 26, ch = 30;
			bx -= cw + 8;
			fill_rounded(cr, bx, hc - ch / 2.0, cw, ch, ch / 2.0, theme::tile_off);
			draw_text(cr, label, bx, hc - ch / 2.0, cw, ch, theme::font_small, theme::text, Align::Center);
			add_hot("clear", bx, static_cast<int>(hc - ch / 2.0), cw, ch, ch / 2.0);
		}
	}

	// The notifications.
	const int list_top = kPad + kHead;
	const int foot_y = box_.height - kFoot;
	draw_notifications(cr, list_top, foot_y - 4, scale);

	// Footer: today's date (opens the calendar) and the settings.
	cairo_rectangle(cr, kPad, foot_y, width_ - 2 * kPad, 1);
	set_color(cr, theme::divider);
	cairo_fill(cr);
	{
		const time_t now = time(nullptr);
		tm lt{};
		localtime_r(&now, &lt);
		char date[64];
		strftime(date, sizeof date, "%A, %-d %B", &lt);
		const double fc = foot_y + kFoot / 2.0;
		const int dw = std::min(width_ - 2 * kPad - 60, text_width(date, theme::font_title) + 44);
		draw_named_icon(cr, server_->icons, "calendar-clock", kPad + 10, fc - 9, 18, theme::text_dim, scale);
		draw_text(cr, date, kPad + 36, fc - 18, dw - 36, 36, theme::font_title, theme::text);
		add_hot("note-calendar", kPad, static_cast<int>(fc - 18), dw, 36, 18);
		const int bs = 36, bx = width_ - kPad - bs;
		cairo_new_path(cr);
		cairo_arc(cr, bx + bs / 2.0, fc, bs / 2.0, 0, 2 * M_PI);
		set_color(cr, theme::tile_off);
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, "gear", bx + bs / 2.0 - 9, fc - 9, 18, theme::text, scale);
		add_hot("note-settings", bx, static_cast<int>(fc - bs / 2.0), bs, bs, bs / 2.0);
	}
	layer_.present(canvas, wlr_box{0, 0, width_, box_.height}, outer);
}

int ControlCentre::look_index() const {
	const std::string &look = server_->config.cc_style;
	return look == "windows" ? 1 : look == "gnome" ? 2 : look == "macos" ? 3 : 0;
}

bool ControlCentre::compact() const {
	const std::string &look = server_->config.cc_style;
	return look == "windows" || look == "gnome" || look == "macos";
}

// ---- What's playing -----------------------------------------------------------------

// The media card at `b`; its buttons become the control centre's.
void ControlCentre::draw_media(cairo_t *cr, const wlr_box &b, double radius, float scale, MediaView::Look look,
		bool hot) {
	std::vector<MediaHot> hots;
	media_->draw(cr, b.x, b.y, b.width, b.height, radius, scale, look, hot ? &hots : nullptr);
	media_box_ = b;
	media_look_ = look;
	media_radius_ = radius;
	for (const MediaHot &h : hots) {
		add_hot(h.id, h.box.x, h.box.y, h.box.width, h.box.height, h.radius);
	}
}

// Keeps the media card moving while it's on screen and something plays.
void ControlCentre::run_media() {
	const bool shown = open_ && media_box_.width > 0 && page_ == Page::Tiles;
	media_->set_shown(shown);
	if (!media_live_ || !shown) {
		wlr_scene_node_set_enabled(&live_->node, false);
		return;
	}
	redraw_live();
	if (!media_ticker_ && media_->moving()) {
		live_wait_ = 0;
		media_ticker_ = server_->anim->add_ticker([this](double dt) {
			const bool more = open_ && media_live_ && media_->step(dt);
			live_wait_ += dt;
			if (live_wait_ >= 33 || !more) {   // ~30 times a second is smooth for this
				live_wait_ = 0;
				redraw_live();
			}
			if (!more) media_ticker_ = 0;
			return more;
		});
	}
}

void ControlCentre::redraw_live() {
	if (!output_ || !media_live_ || media_box_.width <= 0) {
		wlr_scene_node_set_enabled(&live_->node, false);
		return;
	}
	const float scale = output_->output->scale;
	Canvas canvas(media_box_.width, media_box_.height, scale);
	media_->draw(canvas.cr(), 0, 0, media_box_.width, media_box_.height, media_radius_, scale, media_look_, nullptr);
	canvas.present(live_);
	wlr_scene_node_set_enabled(&live_->node, true);
	place_live();
}

void ControlCentre::place_live() {
	if (!live_ || media_box_.width <= 0) return;
	const double k = layer_.scale();
	wlr_scene_node_set_position(&live_->node, static_cast<int>(std::lround(layer_.map_x(media_box_.x))),
		static_cast<int>(std::lround(layer_.map_y(media_box_.y))));
	wlr_scene_buffer_set_dest_size(live_, std::max(1, static_cast<int>(std::lround(media_box_.width * k))),
		std::max(1, static_cast<int>(std::lround(media_box_.height * k))));
	wlr_scene_buffer_set_opacity(live_, static_cast<float>(layer_.alpha()));
}

// You (picture, name, how long the computer has been on) and round buttons:
// edit the tiles, Settings, power.
void ControlCentre::draw_header(cairo_t *cr, int &y, float scale) {
	// The day and date on the left; edit, Settings and power on the right.
	const int h = 48;
	const int buttons = 3, bsize = 40, bgap = 8;
	const int me_w = width_ - 2 * kPad - buttons * (bsize + bgap);
	{
		const time_t now = time(nullptr);
		tm lt{};
		localtime_r(&now, &lt);
		char day[32], date[48];
		strftime(day, sizeof day, "%A", &lt);
		strftime(date, sizeof date, "%-d %B", &lt);
		draw_text(cr, day, kPad + 4, y + 3, me_w - 6, 24, theme::font_heading, theme::text);
		draw_text(cr, date, kPad + 4, y + 26, me_w - 6, 18, theme::font_small, theme::text_dim);
	}
	(void)avatar_tried_;
	const struct {
		const char *id, *icon;
	} kButtons[] = {{"edit", "edit"}, {"settings", "gear"}, {"power", "power"}};
	int bx = kPad + me_w + bgap;
	for (const auto &b : kButtons) {
		const bool lit = std::string(b.id) == "edit" && editing_;
		cairo_new_path(cr);
		cairo_arc(cr, bx + bsize / 2.0, y + h / 2.0, bsize / 2.0, 0, 2 * M_PI);
		set_color(cr, lit ? theme::accent : theme::tile_off);
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, b.icon, bx + bsize / 2.0 - 9, y + h / 2.0 - 9, 18,
			lit ? theme::on_accent : theme::text, scale);
		add_hot(b.id, bx, y + (h - bsize) / 2, bsize, bsize, bsize / 2.0);
		bx += bsize + bgap;
	}
	y += h + 12;
}

namespace {

// What the battery is doing, in words.
std::string battery_words(const SystemStatus &st) {
	if (st.battery_state == "Charging") return "Charging";
	if (st.battery_state == "Full") return "Full";
	if (st.battery_state == "Not charging") return "Plugged in, not charging";
	return "On battery";
}

// Each power mode has its colour, made from the accent: warmer for
// Performance, greener for Power saver, the accent itself for Balanced.
Color power_mode_color(const std::string &mode) {
	if (mode == "performance") return mix(theme::accent, theme::warning, 0.6);
	if (mode == "power-saver") return mix(theme::accent, theme::success, 0.6);
	return theme::accent;
}

// Dark or white, whichever reads on `bg`.
Color readable_on(const Color &bg) {
	const double lum = 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b;
	return lum > 0.62 ? rgb(0x1c1c1c) : rgb(0xffffff);
}

} // namespace

void ControlCentre::draw_tile(cairo_t *cr, const CcTile &t, const wlr_box &b, float scale,
		bool floating) {
	const TileDef *def = tile_def(t.id);
	if (!def) return;
	const TileState s = tile_state(server_, t.id);
	const std::string kind = def->kind;
	const double r_tile = std::min<double>(card_radius(), std::min(b.width, b.height) / 2.0);
	const double r = r_tile;
	const std::string id = t.id;
	const bool edit = editing_;
	const int lk = look_index();
	const bool hot = !edit && !floating;

	// The other looks draw their tiles their way (the grid is the same).
	if (lk != 0 && id == "media") {
		const MediaView::Look ml = lk == 1 ? MediaView::Look::Windows : lk == 2 ? MediaView::Look::Gnome
			: MediaView::Look::Mac;
		draw_media(cr, b, lk == 1 ? 8 : lk == 2 ? 16 : 16, scale, ml, hot);
		if (!hot) media_box_ = wlr_box{};
		return;
	}
	if (lk != 0 && kind == "slider" && b.width > b.height) {
		if (lk == 3) {
			// macOS: a module with a thick bar in it.
			fill_rounded(cr, b.x, b.y, b.width, b.height, 14, theme::card_bg);
			const double bh = std::min(24.0, b.height - 16.0);
			draw_hslider(cr, id, b.x + 10, b.y + (b.height - bh) / 2, b.width - 20, bh, scale, 2, hot);
		} else {
			draw_hslider(cr, id, b.x, b.y, b.width, b.height, scale, lk == 1 ? 0 : 1, hot);
		}
		return;
	}
	if (lk != 0 && kind != "slider" && (id != "battery" || (lk == 1 && b.height > b.width * 0.45))) {
		const bool on = s.on && s.usable;
		const Color fill = on ? theme::accent : theme::tile_off;
		const Color fg = !s.usable ? theme::text_dim : on ? theme::on_accent : theme::text;
		if (lk == 1) {
			// Windows 11: square-cornered buttons.
			if (t.w == 1 && t.h == 1) {
				fill_rounded(cr, b.x, b.y, b.width, b.height, theme::small_radius(), fill);
				draw_named_icon(cr, server_->icons, s.icon, b.x + b.width / 2.0 - 9, b.y + b.height / 2.0 - 9, 18, fg, scale);
				if (hot) add_hot("ti:" + id, b.x, b.y, b.width, b.height, theme::small_radius());
			} else if (t.w == 1 || t.h >= 2) {
				// The button with its name under it (as in Windows).
				draw_pill_toggle(cr, id, b.x, b.y, b.width, b.height - 22, scale, 0, hot);
			} else {
				draw_pill_toggle(cr, id, b.x, b.y, b.width, b.height, scale, 2, hot);
			}
			return;
		}
		if (lk == 2) {
			// GNOME: pills (a round button when it's one cell).
			if (t.w == 1) {
				const double d = std::min(b.width, b.height) - 2;
				const double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
				fill_rounded(cr, b.x, b.y, b.width, b.height, b.width / 2.0, fill);
				draw_named_icon(cr, server_->icons, s.icon, cx - 9, cy - 9, 18, fg, scale);
				if (hot) add_hot("ti:" + id, b.x, b.y, b.width, b.height, std::min(d, b.width * 1.0) / 2);
			} else {
				draw_pill_toggle(cr, id, b.x, b.y, b.width, b.height, scale, 1, hot);
			}
			return;
		}
		// macOS: a module with a round icon; its name beside it (wide) or under it (big).
		const double mr = 14;
		fill_rounded(cr, b.x, b.y, b.width, b.height, mr, theme::card_bg);
		const bool wide = b.width >= b.height * 1.6;
		const bool big = !wide && b.width >= 2 * cell_ && b.height >= 2 * cell_;
		const double ir = std::min(16.0, std::min(b.width, b.height) / 2.0 - 6);
		const double cx = wide ? b.x + 10 + ir : b.x + b.width / 2.0;
		const double cy = wide ? b.y + b.height / 2.0 : big ? b.y + b.height / 2.0 - 10 : b.y + b.height / 2.0;
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, ir, 0, 2 * M_PI);
		set_color(cr, on ? theme::accent : theme::tile_off);
		cairo_fill(cr);
		const double is = std::min(18.0, ir * 1.1);
		draw_named_icon(cr, server_->icons, id == "dnd" ? "suspend" : s.icon, cx - is / 2, cy - is / 2, is,
			on ? theme::on_accent : theme::text, scale);
		if (wide) {
			const double tx = cx + ir + 8, tw = b.x + b.width - tx - 6;
			// "Airplane mode" → "Airplane" when it doesn't fit (as macOS does).
			std::string title = s.title;
			const std::string tail = " mode";
			if (text_width(title, theme::font_bold) > tw && title.size() > tail.size() &&
					title.compare(title.size() - tail.size(), tail.size(), tail) == 0) {
				title.resize(title.size() - tail.size());
			}
			if (b.height >= 40) {
				draw_text(cr, title, tx, cy - 17, tw, 17, theme::font_bold, theme::text);
				draw_text(cr, s.subtitle, tx, cy + 1, tw, 15, theme::font_small, theme::text_dim);
			} else {
				draw_text(cr, title, tx, b.y, tw, b.height, theme::font_small, theme::text);
			}
		} else if (big) {
			draw_text(cr, s.title, b.x + 6, cy + ir + 4, b.width - 12, 17, theme::font_small, theme::text, Align::Center);
		}
		if (hot) {
			add_hot("tm:" + id, b.x, b.y, b.width, b.height, mr);
			add_hot("ti:" + id, cx - ir, cy - ir, 2 * ir, 2 * ir, ir);
		}
		return;
	}

	if (kind == "slider") {
		// A bar that fills from the bottom (upright) or from the left.
		const bool vertical = b.height > b.width;
		const std::string vkey = "val:" + id;
		if (dragging_ == "sl:" + id) fx_.snap(vkey, s.value);
		else fx_.set(vkey, s.value, s.value);
		const double v = std::clamp(fx_.get(vkey), 0.0, 100.0) / 100.0;
		// Samsung's: a pill, filled from the start by a pill of the accent colour.
		const double pr = std::min(b.width, b.height) / 2.0;
		fill_rounded(cr, b.x, b.y, b.width, b.height, pr, theme::tile_off);
		cairo_save(cr);
		rounded_rect(cr, b.x, b.y, b.width, b.height, pr);
		cairo_clip(cr);
		const Color fill = s.muted ? theme::text_dim : theme::accent;
		if (vertical) {
			const double fh = std::max<double>(b.width, b.height * v);
			fill_rounded(cr, b.x, b.y + b.height - fh, b.width, fh, pr, fill);
		} else {
			const double fw = std::max<double>(b.height, b.width * v);
			fill_rounded(cr, b.x, b.y, fw, b.height, pr, fill);
		}
		cairo_restore(cr);
		const double isz = 20;
		double ix, iy;
		if (vertical) {
			ix = b.x + b.width / 2.0 - isz / 2;
			iy = b.y + b.height - isz - 12;
		} else {
			ix = b.x + b.height / 2.0 - isz / 2;
			iy = b.y + b.height / 2.0 - isz / 2;
		}
		const bool icon_on_fill = true;   // the fill always covers the icon's end
		draw_named_icon(cr, server_->icons, s.icon, ix, iy, isz,
			icon_on_fill ? theme::on_accent : theme::text, scale);
		if (vertical) {
			// The number at the top, small and quiet (brighter while dragged).
			draw_text(cr, std::to_string(s.value), b.x, b.y + 10, b.width, 18,
				dragging_ == "sl:" + id ? theme::font_bold : theme::font_small,
				v > 0.88 ? theme::on_accent : dragging_ == "sl:" + id ? theme::text : theme::text_dim, Align::Center);
		} else if (!vertical && dragging_ == "sl:" + id && b.width >= 3 * cell_) {
			draw_text(cr, std::to_string(s.value) + "%", b.x + b.width - 60, b.y, 44, b.height,
				theme::font_bold, v > 0.86 ? theme::on_accent : theme::text, Align::Right);
		}
		if (!edit && !floating) {
			add_hot("sl:" + id, b.x, b.y, b.width, b.height, -2);
			if (dragging_ == "sl:" + id) {
				drag_track_ = b;
				drag_vertical_ = vertical;
			}
		}
		return;
	}

	if (id == "media") {
		// Samsung's media card (see mediaview.cpp).
		draw_media(cr, b, r, scale, MediaView::Look::Samsung, !edit && !floating);
		if (floating || edit) media_box_ = wlr_box{};
		return;
	}

	if (id == "battery") {
		// The charge fills the tile like a gauge, in the power mode's colour
		// (red when it's running low); the three modes sit at the right end.
		const SystemStatus &st = *server_->status;
		const std::string &mode = st.power_profile;
		const bool has_bat = st.has_battery;
		const bool low = has_bat && st.battery <= 15 && st.battery_state == "Discharging";
		const Color mc = low ? theme::danger : power_mode_color(mode);
		// Smooth changes: the level, and the colour from one mode to the next.
		fx_.set("bat:level", has_bat ? st.battery : 0, has_bat ? st.battery : 0);
		const double level = std::clamp(fx_.get("bat:level"), 0.0, 100.0) / 100.0;
		// When the mode (or low battery) changes the colour, it eases across.
		const std::string ck = "bat:col";
		static Color from = mc, target = mc;
		if (mc.r != target.r || mc.g != target.g || mc.b != target.b) {
			from = mix(from, target, 1.0 - std::clamp(fx_.get(ck), 0.0, 1.0));
			target = mc;
			fx_.snap(ck, 1);
			fx_.set(ck, 0);
		}
		const Color col = mix(from, target, 1.0 - std::clamp(fx_.get(ck), 0.0, 1.0));
		// Its shape decides the layout: a bar lying down, or a gauge standing up.
		const bool upright = b.height > b.width * 1.2;
		// (macOS: a module like the others, not a pill.)
		const double r = lk == 3 ? std::min(14.0, b.height / 2.0) : lk == 1 ? theme::small_radius()
			: t.h == 1 || upright ? std::min(b.width, b.height) / 2.0 : r_tile;
		fill_rounded(cr, b.x, b.y, b.width, b.height, r, with_alpha(col, theme::dark ? 0.20 : 0.16));
		cairo_save(cr);
		rounded_rect(cr, b.x, b.y, b.width, b.height, r);
		cairo_clip(cr);
		if (upright) cairo_rectangle(cr, b.x, b.y + b.height * (1 - level), b.width, b.height * level);
		else cairo_rectangle(cr, b.x, b.y, b.width * level, b.height);
		set_color(cr, with_alpha(col, theme::dark ? 0.55 : 0.45));
		cairo_fill(cr);
		cairo_restore(cr);
		if (!edit && !floating) add_hot("tm:battery", b.x, b.y, b.width, b.height, r);

		const bool modes = !mode.empty();
		const std::string pct = has_bat ? std::to_string(st.battery) + "%" : std::string("Power mode");
		static const char *kModes[] = {"power-saver", "balanced", "performance"};
		// The three modes as round buttons on a soft pill, in a row or a column.
		auto mode_buttons = [&](double gx, double gy, int bs, bool column) {
			const double gw = column ? bs + 8 : 3 * bs + 2 * 3 + 8, gh = column ? 3 * bs + 2 * 3 + 8 : bs + 8;
			// (Windows 11: square-cornered, like its buttons.)
			const bool square = lk == 1;
			const double sr = square ? theme::small_radius() : 0;
			fill_rounded(cr, gx, gy, gw, gh, square ? sr : std::min(gw, gh) / 2.0, with_alpha(theme::card_bg, 0.75));
			for (int m = 0; m < 3; m++) {
				const std::string pm = kModes[m];
				const double cx = column ? gx + 4 + bs / 2.0 : gx + 4 + bs / 2.0 + m * (bs + 3);
				const double cy = column ? gy + 4 + bs / 2.0 + m * (bs + 3) : gy + 4 + bs / 2.0;
				const bool chosen = pm == mode;
				const double is = std::min(16, bs - 8);
				if (chosen) {
					const Color pc = power_mode_color(pm);
					cairo_new_path(cr);
					if (square) rounded_rect(cr, cx - bs / 2.0, cy - bs / 2.0, bs, bs, std::max(1.5, sr - 2));
					else cairo_arc(cr, cx, cy, bs / 2.0, 0, 2 * M_PI);
					set_color(cr, pc);
					cairo_fill(cr);
					draw_named_icon(cr, server_->icons, "gauge-" + pm, cx - is / 2, cy - is / 2, is, readable_on(pc), scale);
				} else {
					draw_named_icon(cr, server_->icons, "gauge-" + pm, cx - is / 2, cy - is / 2, is, theme::text_dim, scale);
				}
				if (!edit && !floating) {
					add_hot("pp:" + pm, static_cast<int>(cx - bs / 2.0), static_cast<int>(cy - bs / 2.0), bs, bs,
						square ? std::max(1.5, sr - 2) : bs / 2.0);
				}
			}
		};
		if (upright) {
			// Standing: the number on top, the modes in a column underneath (if they fit).
			draw_text(cr, pct, b.x, b.y + 10, b.width, 22, theme::font_bold, theme::text, Align::Center);
			if (has_bat && st.battery_state == "Charging") {
				draw_named_icon(cr, server_->icons, "zap", b.x + b.width / 2.0 - 7, b.y + 32, 14, theme::text, scale);
			}
			const int bs = std::min(26, b.width - 16);
			if (modes && bs >= 18 && b.height >= 3 * bs + 70) {
				mode_buttons(b.x + (b.width - bs - 8) / 2.0, b.y + b.height - (3 * bs + 2 * 3 + 8) - 8, bs, true);
			}
			return;
		}
		// (Windows' rows are short: a tighter two lines, and the buttons nearly its height.)
		const bool tight = lk == 1;
		const int bs = std::min(26, b.height - (tight ? 12 : 18));
		const bool row = modes && b.width >= 3 * bs + 2 * 3 + 8 + 80;
		const int group_w = row ? 3 * bs + 2 * 3 + 8 : 0;
		const double tx = b.x + std::max(10.0, r * 0.6) + (tight ? 4 : 0), tw = b.x + b.width - tx - group_w - 10;
		const bool two_lines = b.height >= (tight ? 34 : 44);
		const double l1 = tight ? 18 : 22, top = tight ? b.y + b.height / 2.0 - 17 : b.y + b.height / 2.0 - 20;
		draw_text(cr, pct, tx, two_lines ? top : b.y, tw, two_lines ? l1 : b.height,
			two_lines && !tight ? theme::font_large : theme::font_bold, theme::text);
		if (has_bat && st.battery_state == "Charging") {
			const double gx = tx + text_width(pct, two_lines && !tight ? theme::font_large : theme::font_bold) + 6;
			if (gx + 14 < tx + tw) {
				draw_named_icon(cr, server_->icons, "zap", gx,
					two_lines ? top + (l1 - 14) / 2 + (tight ? 0 : 3) : b.y + b.height / 2.0 - 7, 14, theme::text, scale);
			}
		}
		if (two_lines) {
			const std::string sub = !modes ? battery_words(st)
				: mode == "performance" ? "Performance" : mode == "power-saver" ? "Power saver" : "Balanced";
			draw_text(cr, sub, tx, tight ? top + l1 : b.y + b.height / 2.0 + 2, tw, tight ? 16 : 17, theme::font_small,
				with_alpha(theme::text, 0.75));
		}
		if (row) mode_buttons(b.x + b.width - group_w - 6, b.y + (b.height - bs - 8) / 2.0, bs, false);
		return;
	}

	// Toggles, screenshot.
	const std::string on_key = "on:" + id;
	const bool on = s.on && s.usable;
	fx_.set(on_key, on ? 1.0 : 0.0, on ? 1.0 : 0.0);
	const double onv = std::clamp(fx_.get(on_key), 0.0, 1.0);
	const double press = fx_.get("press:" + id);
	const Color off_fill = !s.usable ? with_alpha(theme::tile_off, theme::tile_off.a * 0.5) : theme::tile_off;
	const Color fill = mix(off_fill, theme::accent, onv);
	const Color fg = !s.usable ? theme::text_dim : mix(theme::text, theme::on_accent, onv);
	cairo_save(cr);
	{
		const double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0, k = 1.0 - 0.06 * press;
		cairo_translate(cr, cx, cy);
		cairo_scale(cr, k, k);
		cairo_translate(cr, -cx, -cy);
	}
	if (t.w == 1 && t.h == 1) {
		// Samsung's quick toggles: round buttons.
		const double d = std::min(b.width, b.height) - 2;
		const double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, d / 2, 0, 2 * M_PI);
		set_color(cr, fill);
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, s.icon, cx - 11, cy - 11, 22, fg, scale);
		if (!edit && !floating) {
			add_hot("ti:" + id, cx - d / 2, cy - d / 2, d, d, d / 2);
		}
		cairo_restore(cr);
		return;
	}
	if (t.w == 1) {
		// One cell wide, taller: a standing pill, the round icon at the top and
		// the name at the bottom (when it fits).
		const double pr = b.width / 2.0;
		fill_rounded(cr, b.x, b.y, b.width, b.height, pr, fill);
		const double d = b.width - 10, cx = b.x + b.width / 2.0, cy = b.y + 5 + d / 2;
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, d / 2, 0, 2 * M_PI);
		set_color(cr, onv > 0.5 ? with_alpha(theme::on_accent, 0.22) : with_alpha(theme::text, 0.08));
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, s.icon, cx - 10, cy - 10, 20, fg, scale);
		if (b.height >= d + 40) {
			std::string label = s.title;
			for (const std::string &cand : {s.title, s.title.substr(0, s.title.find(' '))}) {
				if (text_width(cand, theme::font_small) <= b.width - 8) {
					label = cand;
					break;
				}
				label.clear();
			}
			if (!label.empty()) {
				draw_text(cr, label, b.x + 2, b.y + b.height - 26, b.width - 4, 18, theme::font_small, fg, Align::Center);
			}
		}
		if (!edit && !floating) {
			add_hot("tm:" + id, b.x, b.y, b.width, b.height, pr);
			add_hot("ti:" + id, cx - d / 2, cy - d / 2, d, d, d / 2);
		}
		cairo_restore(cr);
		return;
	}
	const double rr = t.h == 1 ? b.height / 2.0 : r;   // wide tiles: pills
	// Calm: a wide tile that's on is a soft tint of the accent, its round
	// button the solid accent; the words stay in the text colour.
	const bool calm_pill = lk == 0 && t.w >= 3 && t.h == 1;
	if (calm_pill) {
		fill_rounded(cr, b.x, b.y, b.width, b.height, rr,
			mix(off_fill, with_alpha(theme::accent, theme::dark ? 0.24 : 0.16), onv));
		const double d = b.height - 12, cx = b.x + 6 + d / 2, cy = b.y + b.height / 2.0;
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, d / 2, 0, 2 * M_PI);
		set_color(cr, mix(with_alpha(theme::text, 0.08), theme::accent, onv));
		cairo_fill(cr);
		const Color ifg = !s.usable ? theme::text_dim : mix(theme::text, theme::on_accent, onv);
		draw_named_icon(cr, server_->icons, s.icon, cx - 10, cy - 10, 20, ifg, scale);
		const double tx = b.x + 14 + d, tw = b.x + b.width - tx - 30;
		draw_text(cr, s.title, tx, cy - 19, tw, 20, theme::font_bold, theme::text);
		draw_text(cr, s.subtitle, tx, cy + 1, tw, 17, theme::font_small, theme::text_dim);
		draw_named_icon(cr, server_->icons, "chevron-right", b.x + b.width - 28, cy - 8, 16, theme::text_dim, scale);
		if (!edit && !floating) {
			const int ir = static_cast<int>(d / 2);
			add_hot("tm:" + id, b.x, b.y, b.width, b.height, rr);
			add_hot("ti:" + id, cx - ir - 2, cy - ir - 2, 2 * ir + 4, 2 * ir + 4, ir);
		}
		cairo_restore(cr);
		return;
	}
	fill_rounded(cr, b.x, b.y, b.width, b.height, rr, fill);
	if (false) {
	} else if (t.w <= 2 && t.h == 1) {
		// Two cells wide: no room for the round button, so a plain icon and
		// the title beside it (the icon's end still switches it).
		const double isz = 18, ix = b.x + 12, iy = b.y + b.height / 2.0 - isz / 2;
		draw_named_icon(cr, server_->icons, s.icon, ix, iy, isz, fg, scale);
		const double tx = ix + isz + 9, tw = b.x + b.width - tx - 8;
		draw_text(cr, s.title, tx, b.y + b.height / 2.0 - 18, tw, 19, theme::font_bold, fg);
		draw_text(cr, s.subtitle, tx, b.y + b.height / 2.0 + 1, tw, 17, theme::font_small, with_alpha(fg, 0.8));
		if (!edit && !floating) {
			add_hot("tm:" + id, b.x, b.y, b.width, b.height, rr);
			add_hot("ti:" + id, b.x, b.y, static_cast<int>(isz + 20), b.height, rr,
				wlr_box{b.x, b.y, b.width, b.height});
		}
	} else {
		// A round icon you click to switch it; the rest opens its list or options.
		const double d = std::min<double>(b.height, cell_) - 12;
		const double cx = b.x + 6 + d / 2, cy = t.h == 1 ? b.y + b.height / 2.0 : b.y + 6 + d / 2;
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, d / 2, 0, 2 * M_PI);
		set_color(cr, onv > 0.5 ? with_alpha(theme::on_accent, 0.22) : with_alpha(theme::text, 0.08));
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, s.icon, cx - 10, cy - 10, 20, fg, scale);
		double tx, ty, tw;
		if (t.h == 1) {
			tx = b.x + 12 + d;
			ty = b.y + b.height / 2.0 - 19;
			tw = b.x + b.width - tx - 10;
		} else {
			tx = b.x + 12;
			ty = b.y + b.height - 46;
			tw = b.width - 24;
		}
		draw_text(cr, s.title, tx, ty, tw, 20, theme::font_bold, fg);
		draw_text(cr, s.subtitle, tx, ty + 20, tw, 18, theme::font_small, with_alpha(fg, 0.8));
		if (!edit && !floating) {
			const int ir = static_cast<int>(d / 2);
			add_hot("tm:" + id, b.x, b.y, b.width, b.height, rr);
			add_hot("ti:" + id, cx - ir - 2, cy - ir - 2, 2 * ir + 4, 2 * ir + 4, ir);
		}
	}
	cairo_restore(cr);
}

void ControlCentre::draw_grid(cairo_t *cr, int &y, float scale) {
	if (editing_) {
		// The edit bar: a hint, Reset and Done.
		draw_text(cr, "Drag tiles, pull a corner to resize", kPad + 4, y, width_ - 2 * kPad - 150, 30,
			theme::font_small, theme::text_dim);
		const struct {
			const char *id, *label;
			int w;
			bool primary;
		} kButtons[] = {{"edit-reset", "Reset", 64, false}, {"edit-done", "Done", 64, true}};
		int bx = width_ - kPad - 64 - 6 - 64;
		for (const auto &b : kButtons) {
			fill_rounded(cr, bx, y + 2, b.w, 26, 13, b.primary ? theme::accent : theme::tile_off);
			draw_text(cr, b.label, bx, y + 2, b.w, 26, b.primary ? theme::font_bold : theme::font_small,
				b.primary ? theme::on_accent : theme::text, Align::Center);
			add_hot(b.id, bx, y + 2, b.w, 26, 13);
			bx += b.w + 6;
		}
		y += 38;
	}
	cell_ = (width_ - 2 * kPad - (kCols - 1) * kGap) / static_cast<double>(kCols);
	// Windows 11's rows are short: a 2x2 tile is a wide button with its name under it.
	row_h_ = look_index() == 1 ? std::round(cell_ * 0.72) : cell_;
	grid_top_ = y;
	int rows = 0;
	for (const CcTile &t : layout_) rows = std::max(rows, t.r + t.h);
	if (editing_) rows = std::min(kMaxRows, rows + 1);   // room to drop tiles below

	if (editing_) {
		// The free cells: faint filled squares (no outlines) where a tile can go.
		for (int r = 0; r < rows; r++) {
			for (int c = 0; c < kCols; c++) {
				const bool taken = std::any_of(layout_.begin(), layout_.end(), [&](const CcTile &t) {
					return t.id != edit_tile_ && c >= t.c && c < t.c + t.w && r >= t.r && r < t.r + t.h;
				});
				if (taken) continue;
				const wlr_box cb = tile_box(CcTile{"", c, r, 1, 1});
				fill_rounded(cr, cb.x + 3, cb.y + 3, cb.width - 6, cb.height - 6,
					std::min<double>(card_radius(), cb.width / 2.0) - 3, with_alpha(theme::text, 0.05));
			}
		}
	}
	for (const CcTile &t : layout_) {
		if (t.id == edit_tile_ && !edit_resize_) continue;   // drawn under the pointer below
		const wlr_box b = tile_box(t);
		draw_tile(cr, t, b, scale, false);
		if (editing_) {
			add_hot("mv:" + t.id, b.x, b.y, b.width, b.height, -2);
			// Hide: a small minus badge inside the top corner (OneUI-like).
			const double hx = b.x + b.width - 13, hy = b.y + 13;
			cairo_new_path(cr);
			cairo_arc(cr, hx, hy, 9, 0, 2 * M_PI);
			set_color(cr, theme::close_hover);
			cairo_fill(cr);
			set_color(cr, rgb(0xffffff));
			cairo_set_line_width(cr, 1.8);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_move_to(cr, hx - 3.5, hy);
			cairo_line_to(cr, hx + 3.5, hy);
			cairo_stroke(cr);
			add_hot("rm:" + t.id, hx - 12, hy - 12, 24, 24, 12);
			// Resize: a short rounded arc following the bottom corner.
			const double cr_r = std::min<double>(card_radius(), std::min(b.width, b.height) / 2.0);
			const double inset = 5, ar = std::max(6.0, cr_r - inset);
			const double rx = b.x + b.width - inset - ar, ry = b.y + b.height - inset - ar;
			set_color(cr, with_alpha(theme::text, 0.5));
			cairo_set_line_width(cr, 3);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_new_path(cr);
			cairo_arc(cr, rx, ry, ar, M_PI * 0.08, M_PI * 0.42);
			cairo_stroke(cr);
			const double rx2 = b.x + b.width - 5, ry2 = b.y + b.height - 5;
			add_hot("rs:" + t.id, rx2 - 22, ry2 - 22, 26, 26, -2);
		}
	}
	// A tile being moved: where it would land, and the tile under the pointer.
	if (editing_ && !edit_tile_.empty()) {
		const wlr_box tb = tile_box(edit_target_);
		rounded_rect(cr, tb.x + 1, tb.y + 1, tb.width - 2, tb.height - 2, card_radius());
		set_color(cr, with_alpha(edit_target_ok_ ? theme::accent : theme::danger, 0.25));
		cairo_fill_preserve(cr);
		set_color(cr, edit_target_ok_ ? theme::accent : theme::danger);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
		if (!edit_resize_) {
			for (const CcTile &t : layout_) {
				if (t.id != edit_tile_) continue;
				wlr_box b = tile_box(t);
				b.x = static_cast<int>(std::lround(edit_px_ - grab_dx_));
				b.y = static_cast<int>(std::lround(edit_py_ - grab_dy_));
				cairo_push_group(cr);
				draw_tile(cr, t, b, scale, true);
				cairo_pop_group_to_source(cr);
				cairo_paint_with_alpha(cr, 0.85);
			}
		}
	}
	y = grid_top_ + (rows > 0 ? static_cast<int>(std::lround(rows * row_h_ + (rows - 1) * kGap)) : 0) + 16;
}

// In edit mode, under the grid: the tiles you hid, to add back.
void ControlCentre::draw_edit_tray(cairo_t *cr, int &y, float) {
	std::vector<const TileDef *> hidden;
	for (const TileDef &d : kTiles) {
		const bool shown = std::any_of(layout_.begin(), layout_.end(),
			[&d](const CcTile &t) { return t.id == d.id; });
		if (!shown && tile_available(server_, d.id)) hidden.push_back(&d);
	}
	draw_text(cr, hidden.empty() ? "Every tile is on the grid" : "Add a tile", kPad + 4, y, 300, 22,
		theme::font_small, theme::text_dim);
	y += 26;
	int x = kPad;
	for (const TileDef *d : hidden) {
		const std::string label = std::string("+  ") + d->name;
		const int w = text_width(label, theme::font_small) + 24;
		if (x + w > width_ - kPad) {
			x = kPad;
			y += 36;
		}
		fill_rounded(cr, x, y, w, 30, 15, theme::tile_off);
		draw_text(cr, label, x, y, w, 30, theme::font_small, theme::text, Align::Center);
		add_hot(std::string("add:") + d->id, x, y, w, 30, 15);
		x += w + 8;
	}
	y += 40;
}

void ControlCentre::draw_detail(cairo_t *cr, int &y, int bottom, float scale) {
	SystemStatus &st = *server_->status;
	const bool wifi = page_ == Page::Wifi;

	// Header: back, title, rescan, on/off switch.
	if (const double h = hv("back"); h > 0.01) {
		cairo_new_path(cr);
		cairo_arc(cr, kPad + 18, y + 18, 18, 0, 2 * M_PI);
		set_color(cr, with_alpha(theme::text, 0.12 * h));
		cairo_fill(cr);
	}
	draw_named_icon(cr, server_->icons, "chevron-left", kPad + 9, y + 9, 18, theme::text, scale);
	add_hot("back", kPad, y, 36, 36, 18);
	draw_text(cr, wifi ? "Wi-Fi" : "Bluetooth", kPad + 44, y, 200, 36, theme::font_large, theme::text);
	const bool on = wifi ? st.wifi_enabled && !st.airplane : st.bt_powered;
	fx_.set("switch", on ? 1.0 : 0.0, on ? 1.0 : 0.0);
	draw_switch(cr, width_ - kPad - 48, y + 6, fx_.get("switch"));
	add_hot("detail-switch", width_ - kPad - 52, y, 52, 36, -2);
	const int rx = width_ - kPad - 48 - 44;
	if (const double h = hv("rescan"); h > 0.01) {
		cairo_new_path(cr);
		cairo_arc(cr, rx + 16, y + 18, 16, 0, 2 * M_PI);
		set_color(cr, with_alpha(theme::text, 0.12 * h));
		cairo_fill(cr);
	}
	draw_named_icon(cr, server_->icons, "refresh", rx + 8, y + 10, 16,
		st.scanning ? theme::text_dim : theme::text, scale);
	add_hot("rescan", rx, y + 2, 32, 32, 16);
	y += 48;

	// Password entry for a new secured network.
	if (wifi && !password_for_.empty()) {
		const int w = width_ - 2 * kPad;
		fill_rounded(cr, kPad, y, w, 104, 20, theme::card_bg);
		draw_text(cr, "Password for " + password_for_, kPad + 14, y + 8, w - 28, 22, theme::font_small,
			theme::text_dim);
		fill_rounded(cr, kPad + 12, y + 32, w - 24, 36, 18, theme::field_bg);
		std::string dots;
		for (size_t i = 0; i < password_.size(); i++) {
			dots += "•";
		}
		draw_text(cr, dots.empty() ? "Type the password" : dots, kPad + 26, y + 32, w - 120, 36,
			theme::font, dots.empty() ? theme::text_dim : theme::text);
		if (typing_) {
			const int cx = kPad + 26 + (dots.empty() ? 0 : text_width(dots, theme::font)) + 1;
			cairo_rectangle(cr, cx, y + 41, 2, 18);
			set_color(cr, theme::accent);
			cairo_fill(cr);
		}
		add_hot("password", kPad + 12, y + 32, w - 24, 36, -2);
		const int bw = 90;
		fill_rounded(cr, kPad + w - 12 - bw, y + 72, bw, 26, 13,
			connecting_ ? theme::tile_off : theme::accent);
		draw_text(cr, connecting_ ? "Connecting…" : "Connect", kPad + w - 12 - bw, y + 72, bw, 26,
			theme::font_small, connecting_ ? theme::text_dim : theme::on_accent, Align::Center);
		add_hot("connect", kPad + w - 12 - bw, y + 72, bw, 26, 13);
		if (!wifi_message_.empty()) {
			draw_text(cr, wifi_message_, kPad + 14, y + 72, w - bw - 36, 26, theme::font_small,
				theme::danger);
		}
		y += 116;
	} else if (!wifi_message_.empty()) {
		draw_text(cr, wifi_message_, kPad + 4, y, width_ - 2 * kPad, 22, theme::font_small,
			theme::text_dim);
		y += 26;
	} else if (!wifi && !st.bt_message.empty()) {
		y += draw_paragraph(cr, st.bt_message, kPad + 4, y, width_ - 2 * kPad, 2, theme::font_small,
			theme::danger) + 8;
	}

	// The list.
	list_area_ = wlr_box{kPad, y, width_ - 2 * kPad, bottom - y};
	const int row_h = 52;
	const int count = wifi ? static_cast<int>(st.networks.size()) : static_cast<int>(st.bt_devices.size());
	if (!on) {
		draw_text(cr, wifi ? "Wi-Fi is off" : "Bluetooth is off", kPad, y + 20, width_ - 2 * kPad, 24,
			theme::font, theme::text_dim, Align::Center);
		return;
	}
	if (count == 0) {
		draw_text(cr, st.scanning ? "Searching…" : wifi ? "No networks found"
			: "No devices found. Put yours in pairing mode, then press refresh.",
			kPad, y + 20, width_ - 2 * kPad, 40, theme::font_small, theme::text_dim, Align::Center);
		return;
	}
	cairo_save(cr);
	cairo_rectangle(cr, list_area_.x, list_area_.y, list_area_.width, list_area_.height);
	cairo_clip(cr);
	fx_.set("list", list_scroll_, 0);
	int ry = y - static_cast<int>(std::lround(fx_.get("list")));
	for (int i = 0; i < count; i++, ry += row_h + 6) {
		if (ry + row_h < list_area_.y || ry > list_area_.y + list_area_.height) {
			continue;
		}
		const std::string id = (wifi ? "net:" : "bt:") + std::to_string(i);
		std::string name, sub, icon;
		bool active = false;
		if (wifi) {
			const WifiNetwork &n = st.networks[static_cast<size_t>(i)];
			name = n.ssid;
			active = n.active;
			sub = n.active ? "Connected" : n.saved ? "Saved" : n.secure ? "Secured" : "Open";
			icon = wifi_icon(n.signal);
		} else {
			const BtDevice &d = st.bt_devices[static_cast<size_t>(i)];
			name = d.name;
			active = d.connected;
			const bool busy = st.bt_busy == d.address;
			sub = busy ? (d.paired ? (d.connected ? "Disconnecting…" : "Connecting…") : "Pairing…")
				: d.connected ? "Connected" : d.paired ? "Paired" : "Nearby · click to pair";
			icon = "bluetooth";
			// A line above the first nearby device.
			if (!d.paired && (i == 0 || st.bt_devices[static_cast<size_t>(i - 1)].paired)) {
				draw_text(cr, "NEARBY", kPad + 4, ry - 2, 200, 16, theme::font_small, theme::text_dim);
				ry += 18;
			}
		}
		fill_rounded(cr, kPad, ry, width_ - 2 * kPad, row_h, card_radius(),
			active ? with_alpha(theme::accent, 0.22) : theme::card_bg);
		if (const double h = hv(id); h > 0.01) {
			fill_rounded(cr, kPad, ry, width_ - 2 * kPad, row_h, card_radius(),
				with_alpha(theme::text, 0.06 * h));
		}
		draw_named_icon(cr, server_->icons, icon, kPad + 16, ry + row_h / 2.0 - 9, 18, theme::text, scale);
		draw_text(cr, name, kPad + 48, ry + 7, width_ - 2 * kPad - 90, 20, theme::font, theme::text);
		draw_text(cr, sub, kPad + 48, ry + 27, width_ - 2 * kPad - 90, 18, theme::font_small,
			active ? theme::accent : theme::text_dim);
		if (wifi && st.networks[static_cast<size_t>(i)].secure) {
			draw_named_icon(cr, server_->icons, "lock", width_ - kPad - 34, ry + row_h / 2.0 - 7, 14,
				theme::text_dim, scale);
		}
		const int top = std::max(ry, list_area_.y);
		const int bot = std::min(ry + row_h, list_area_.y + list_area_.height);
		add_hot(id, kPad, top, width_ - 2 * kPad, bot - top);
	}
	cairo_restore(cr);
	const bool nearby = !wifi && std::any_of(st.bt_devices.begin(), st.bt_devices.end(),
		[](const BtDevice &d) { return !d.paired; });
	list_max_scroll_ = std::max(0, count * (row_h + 6) - 6 + (nearby ? 18 : 0) - list_area_.height);
	list_scroll_ = std::clamp(list_scroll_, 0, list_max_scroll_);
}

// Notifications under the tiles, stacked by app like a phone (or one card
// each). Click a stack's count to open it up; hover a card for its ✕.
void ControlCentre::draw_notifications(cairo_t *cr, int y, int bottom, float scale) {
	if (bottom - y < 60) {
		return;
	}
	const Notifications *notes = server_->notifications.get();
	std::vector<const Notification *> items;
	if (notes) {
		for (const Notification &n : notes->list()) {
			if (!n.transient) items.push_back(&n);
		}
	}
	// Header: "Notifications · 3", then Settings and Clear (the notification
	// centre has its own header).
	if (!notes_only_) {
	const std::string title = std::string("Notifications") +
		(items.empty() ? std::string() : "  ·  " + std::to_string(items.size()));
	draw_text(cr, title, kPad + 6, y, 220, 28, theme::font_bold, theme::text_dim);
	{
		int bx = width_ - kPad;
		auto button = [&](const char *id, const char *label) {
			const int w = text_width(label, theme::font_small) + 22;
			bx -= w;
			fill_rounded(cr, bx, y + 1, w, 26, 13, theme::card_bg);
			draw_text(cr, label, bx, y + 1, w, 26, theme::font_small, theme::text, Align::Center);
			add_hot(id, bx, y + 1, w, 26, 13);
			bx -= 6;
		};
		if (!items.empty()) button("clear", "Clear");
		button("note-settings", "Settings");
	}
	y += 36;
	}
	notes_area_ = wlr_box{kPad, y, width_ - 2 * kPad, bottom - y};

	if (!notes || !notes->running()) {
		draw_text(cr, "Another app is showing notifications", kPad, y, width_ - 2 * kPad, 40,
			theme::font_small, theme::text_dim, Align::Center);
		return;
	}
	if (items.empty()) {
		draw_named_icon(cr, server_->icons, "bell", width_ / 2.0 - 14, y + 30, 28,
			with_alpha(theme::text_dim, 0.6), scale);
		draw_text(cr, "No notifications", kPad, y + 64, width_ - 2 * kPad, 20, theme::font,
			theme::text_dim, Align::Center);
		return;
	}

	// Stacks: one per app, newest first.
	struct Group {
		std::string app;
		std::vector<const Notification *> notes;
	};
	std::vector<Group> groups;
	for (const Notification *n : items) {
		const std::string app = n->app.empty() ? "Notification" : n->app;
		auto it = std::find_if(groups.begin(), groups.end(), [&](const Group &g) { return g.app == app; });
		if (!server_->config.notif_group || it == groups.end()) {
			groups.push_back(Group{app, {n}});
		} else {
			it->notes.push_back(n);
		}
	}

	cairo_save(cr);
	cairo_rectangle(cr, notes_area_.x, notes_area_.y - 2, notes_area_.width, notes_area_.height + 2);
	cairo_clip(cr);
	const int w = width_ - 2 * kPad;
	const int text_w = w - 24 - 40;
	fx_.set("notes", notes_scroll_, 0);
	int cy = y - static_cast<int>(std::lround(fx_.get("notes")));
	int total = 0;
	auto visible = [&](int top, int h) { return top + h > notes_area_.y && top < notes_area_.y + notes_area_.height; };
	auto hit = [&](const std::string &id, int top, int h, double radius) {
		const int t = std::max(top, notes_area_.y);
		const int b = std::min(top + h, notes_area_.y + notes_area_.height);
		if (b > t) add_hot(id, kPad, t, w, b - t, radius);
	};

	for (const Group &g : groups) {
		const bool stacked = g.notes.size() > 1;
		const bool opened = stacked && open_groups_.count(g.app);
		const size_t shown = opened ? g.notes.size() : 1;
		if (opened) {
			// The open stack's header: app, count, "Show less", ✕ all.
			if (visible(cy, 26)) {
				draw_text(cr, g.app + "  ·  " + std::to_string(g.notes.size()), kPad + 6, cy, w - 140, 26,
					theme::font_small, theme::text_dim);
				const int lw = 82;
				fill_rounded(cr, kPad + w - lw - 32, cy + 1, lw, 24, 12, theme::card_bg);
				draw_text(cr, "Show less", kPad + w - lw - 32, cy + 1, lw, 24, theme::font_small, theme::text,
					Align::Center);
				add_hot("grp:" + g.app, kPad + w - lw - 32, cy + 1, lw, 24, 12);
				cairo_new_path(cr);
				cairo_arc(cr, kPad + w - 12, cy + 13, 12, 0, 2 * M_PI);
				set_color(cr, theme::card_bg);
				cairo_fill(cr);
				draw_named_icon(cr, server_->icons, "close", kPad + w - 19, cy + 6, 14, theme::text, scale);
				add_hot("grpclose:" + g.app, kPad + w - 24, cy + 1, 24, 24, 12);
			}
			cy += 32;
			total += 32;
		}
		for (size_t i = 0; i < shown; i++) {
			const Notification *n = g.notes[i];
			const int sh = std::max(18, draw_paragraph(cr, n->summary, 0, 0, text_w, 2, theme::font_bold,
				theme::text, false));
			const int bh = draw_paragraph(cr, n->body, 0, 0, text_w, 3, theme::font, theme::text, false);
			const int h = 12 + 18 + 4 + sh + (bh ? 2 + bh : 0) + 12;
			const std::string id = "note:" + std::to_string(n->id);
			const std::string xid = "noteclose:" + std::to_string(n->id);
			const bool closed_stack = stacked && !opened;
			const int under = closed_stack ? std::min<int>(2, static_cast<int>(g.notes.size()) - 1) : 0;
			if (visible(cy, h + under * 6)) {
				// A closed stack shows the cards behind as edges peeking out.
				for (int k = under; k >= 1; k--) {
					fill_rounded(cr, kPad + 8 * k, cy + h - 20 + 6 * k, w - 16 * k, 20, card_radius() - 2,
						mix(theme::card_bg, theme::bg_alt, 0.35 * k));
				}
				fill_rounded(cr, kPad, cy, w, h, card_radius(), theme::card_bg);
				draw_notification_icon(cr, server_->icons, *n, kPad + 12, cy + 34, 30, scale);
				draw_text(cr, g.app + " · " + ago(n->time), kPad + 12, cy + 10, w - 100, 18, theme::font_small,
					theme::text_dim);
				draw_paragraph(cr, n->summary, kPad + 52, cy + 34, text_w, 2, theme::font_bold, theme::text);
				if (bh) {
					draw_paragraph(cr, n->body, kPad + 52, cy + 34 + sh + 2, text_w, 3, theme::font,
						theme::text_dim);
				}
				hit(id, cy, h, card_radius());
				int right = kPad + w - 10;
				if (closed_stack) {
					// The count: click to open the stack.
					const std::string count = "+" + std::to_string(g.notes.size() - 1);
					const int cw = text_width(count, theme::font_small) + 16;
					right -= cw;
					fill_rounded(cr, right, cy + 8, cw, 22, 11, with_alpha(theme::accent, 0.22));
					draw_text(cr, count, right, cy + 8, cw, 22, theme::font_small, theme::text, Align::Center);
					if (cy + 8 >= notes_area_.y) add_hot("grp:" + g.app, right, cy + 8, cw, 22, 11);
					right -= 6;
				}
				if (hover_ == id || hover_ == xid || hover_ == "grp:" + g.app) {
					const int bx = right - 24, by = cy + 7;
					cairo_new_path(cr);
					cairo_arc(cr, bx + 12, by + 12, 12, 0, 2 * M_PI);
					set_color(cr, theme::tile_off);
					cairo_fill(cr);
					draw_named_icon(cr, server_->icons, "close", bx + 5, by + 5, 14, theme::text, scale);
					// A closed stack's ✕ clears the whole stack.
					if (by >= notes_area_.y) {
						add_hot(closed_stack ? "grpclose:" + g.app : xid, bx, by, 24, 24, 12);
					}
				}
			}
			const int step = h + under * 6 + 8;
			cy += step;
			total += step;
		}
	}
	cairo_restore(cr);
	notes_total_ = total;
	notes_max_scroll_ = std::max(0, total - 8 - notes_area_.height);
	notes_scroll_ = std::clamp(notes_scroll_, 0, notes_max_scroll_);
}

// ---- input ----------------------------------------------------------------------

void ControlCentre::motion(double lx, double ly) {
	if (!edit_tile_.empty()) {
		drag_edit(lx - box_.x, ly - box_.y);
		return;
	}
	if (dragging_ == "mp:seek") {
		media_->drag(lx - box_.x - media_box_.x);
		if (media_live_) redraw_live();
		else redraw();
		return;
	}
	if (!dragging_.empty()) {
		drag_slider(lx, ly);
		return;
	}
	const std::string now = hot_at(lx - box_.x, ly - box_.y);
	if (now != hover_) {
		// A notification shows its close button while pointed at; that needs
		// a redraw. Everything else is just the GPU highlight moving.
		auto noteish = [](const std::string &s) { return s.rfind("note", 0) == 0 || s.rfind("grp", 0) == 0; };
		const bool notes = noteish(now) || noteish(hover_);
		hover_ = now;
		if (notes) {
			redraw();
		} else {
			update_highlight();
		}
	}
}

void ControlCentre::leave() {
	if (!hover_.empty() && dragging_.empty() && edit_tile_.empty()) {
		const bool notes = hover_.rfind("note", 0) == 0 || hover_.rfind("grp", 0) == 0;
		hover_.clear();
		if (notes) {
			redraw();
		} else {
			update_highlight();
		}
	}
}

void ControlCentre::drag_slider(double lx, double ly) {
	const wlr_box &t = drag_track_;
	if (t.width <= 0 || t.height <= 0) {
		return;
	}
	const double px = lx - box_.x, py = ly - box_.y;
	const double f = drag_vertical_ ? 1.0 - (py - t.y) / t.height : (px - t.x) / t.width;
	const int value = static_cast<int>(std::lround(std::clamp(f, 0.0, 1.0) * 100));
	// Only when it changes (a mouse reports far more moves than there are
	// steps), and quietly: just this card is redrawn, at most once a frame;
	// the panels and the rest catch up when it's let go.
	if (value == drag_value_) return;
	drag_value_ = value;
	SystemStatus &st = *server_->status;
	if (dragging_ == "sl:volume") st.set_volume(value, true);
	else if (dragging_ == "sl:brightness") st.set_brightness(std::max(1, value), true);
	else if (dragging_ == "sl:mic") st.set_mic_volume(value, true);
	queue_redraw();
}

// ---- edit mode: moving and resizing tiles ----

void ControlCentre::drag_edit(double px, double py) {
	edit_px_ = px;
	edit_py_ = py;
	auto self = std::find_if(layout_.begin(), layout_.end(), [this](const CcTile &t) { return t.id == edit_tile_; });
	if (self == layout_.end()) {
		return;
	}
	const double step = cell_ + kGap, vstep = row_h_ + kGap;
	CcTile target = *self;
	if (edit_resize_) {
		// The corner follows the pointer, as far as the free cells allow.
		const wlr_box b = tile_box(*self);
		target.w = std::clamp(static_cast<int>(std::lround((px - b.x + kGap) / step)), 1, kCols - self->c);
		target.h = std::clamp(static_cast<int>(std::lround((py - b.y + kGap) / vstep)), 1, 6);
		if (fits(layout_, target, self->id)) {
			self->w = target.w;
			self->h = target.h;
		}
		edit_target_ = *self;
		edit_target_ok_ = true;
	} else {
		target.c = std::clamp(static_cast<int>(std::lround((px - grab_dx_ - kPad) / step)), 0, kCols - self->w);
		target.r = std::clamp(static_cast<int>(std::lround((py - grab_dy_ - grid_top_) / vstep)), 0,
			kMaxRows - self->h);
		edit_target_ = target;
		edit_target_ok_ = fits(layout_, target, self->id);
		if (!edit_target_ok_) {
			// Onto one tile that can take this one's old place: they swap.
			const CcTile *other = nullptr;
			int count = 0;
			for (const CcTile &o : layout_) {
				if (o.id != self->id && overlaps(o, target)) {
					other = &o;
					count++;
				}
			}
			if (count == 1) {
				CcTile moved = *other;
				moved.c = self->c;
				moved.r = self->r;
				edit_target_ok_ = fits(layout_, moved, self->id, other->id) &&
					fits(layout_, target, self->id, other->id) && !overlaps(moved, target);
			}
		}
	}
	queue_redraw();
}

// While a tile is dragged the pointer moves far more often than the screen
// refreshes: draw at most once a frame (~60 times a second).
void ControlCentre::queue_redraw() {
	if (!redraw_queued_) {
		redraw_queued_ = true;
		wl_event_source_timer_update(redraw_timer_, 14);
	}
}

int ControlCentre::handle_redraw_timer(void *data) {
	auto *self = static_cast<ControlCentre *>(data);
	if (self->redraw_queued_) {
		self->redraw_queued_ = false;
		self->redraw();
	}
	return 0;
}

void ControlCentre::end_edit_drag() {
	auto self = std::find_if(layout_.begin(), layout_.end(), [this](const CcTile &t) { return t.id == edit_tile_; });
	if (self != layout_.end() && !edit_resize_ && edit_target_ok_) {
		const CcTile target = edit_target_;
		for (CcTile &o : layout_) {
			if (o.id != self->id && overlaps(o, target)) {
				o.c = self->c;   // the swap
				o.r = self->r;
			}
		}
		self->c = target.c;
		self->r = target.r;
	}
	edit_tile_.clear();
	edit_resize_ = false;
	server_->cc_grab = false;
	redraw_queued_ = false;
	save_layout();
	redraw();
}

void ControlCentre::save_layout() {
	server_->write_setting(layout_key(), serialise(layout_));
}

// Each look keeps its own arrangement.
std::string ControlCentre::layout_key() const {
	const std::string &look = server_->config.cc_style;
	// (Windows' rows got shorter in 0.27: its own key, so an old arrangement starts over.)
	return look == "dusk" ? "cc_layout" : look == "windows" ? "cc_layout_win11" : "cc_layout_" + look;
}

void ControlCentre::press(double lx, double ly, uint32_t button) {
	const double px = lx - box_.x, py = ly - box_.y;
	const std::string id = hot_at(px, py);
	auto starts = [&](const char *p) { return id.rfind(p, 0) == 0; };
	if (button == BTN_RIGHT) {
		if (starts("mp:")) {
			media_menu(lx, ly);
			return;
		}
		if (starts("ti:") || starts("tm:") || starts("sl:")) {
			tile_more(id.substr(3), lx, ly);
		} else if (starts("note:")) {
			note_menu(static_cast<uint32_t>(std::strtoul(id.c_str() + 5, nullptr, 10)), lx, ly);
		} else if (starts("bt:")) {
			// A paired device: connect or disconnect, or forget it.
			SystemStatus &st = *server_->status;
			const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 3));
			if (i < st.bt_devices.size() && st.bt_devices[i].paired) {
				const BtDevice d = st.bt_devices[i];
				Server *srv = server_;
				std::vector<MenuItem> items;
				items.push_back(MenuItem::header(d.name));
				items.push_back(MenuItem::action(d.connected ? "Disconnect" : "Connect",
					[srv, d] { srv->status->connect_bluetooth(d); }));
				MenuItem forget = MenuItem::action("Forget this device",
					[srv, d] { srv->status->forget_bluetooth(d); });
				forget.danger = true;
				items.push_back(forget);
				srv->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1},
					"point", std::move(items));
			}
		} else if (page_ == Page::Tiles) {
			show_menu(lx, ly);
		}
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}
	if (starts("sl:")) {
		dragging_ = id;
		drag_value_ = -1;
		server_->cc_grab = true;
		for (const Hot &h : hots_) {
			if (h.id == id) {
				drag_track_ = h.box;
				drag_vertical_ = h.box.height > h.box.width;
			}
		}
		drag_slider(lx, ly);
		return;
	}
	if (id == "mp:seek") {
		dragging_ = id;
		server_->cc_grab = true;
		media_->drag(px - media_box_.x);
		if (media_live_) redraw_live();
		else redraw();
		return;
	}
	if (starts("mv:") || starts("rs:")) {
		edit_tile_ = id.substr(3);
		edit_resize_ = starts("rs:");
		for (const CcTile &t : layout_) {
			if (t.id == edit_tile_) {
				const wlr_box b = tile_box(t);
				grab_dx_ = px - b.x;
				grab_dy_ = py - b.y;
				edit_target_ = t;
				edit_target_ok_ = true;
			}
		}
		server_->cc_grab = true;
		drag_edit(px, py);
		return;
	}
	if (id != "password" && typing_) {
		typing_ = false;
	}
	if (!id.empty()) {
		hl_.flash();
	}
	if (starts("note:")) {
		// A notification: its options, instead of it just going away.
		note_menu(static_cast<uint32_t>(std::strtoul(id.c_str() + 5, nullptr, 10)), lx, ly);
		return;
	}
	activate(id);
}

void ControlCentre::note_menu(uint32_t nid, double lx, double ly) {
	Server *srv = server_;
	if (!srv->notifications) return;
	const Notification *found = srv->notifications->get(nid);
	if (!found) return;
	const Notification n = *found;   // (it may be gone by the time an item is picked)
	// Laid out like One UI's: who it's from on top, then what you can do
	// with it (opening first, the app's own buttons after), then copying and
	// settings, and Dismiss on its own at the bottom; every line with its icon.
	std::vector<MenuItem> items;
	const std::string from = n.app.empty() ? std::string("Notification") : n.app;
	items.push_back(MenuItem::header(n.summary.empty() || n.summary == from ? from : n.summary + "  ·  " + from));
	bool has_default = false;
	for (const auto &a : n.actions) has_default = has_default || a.first == "default";
	const AppInfo *app = nullptr;
	auto lower = [](std::string t) {
		for (char &c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return t;
	};
	for (const AppInfo &a : srv->apps.apps()) {
		std::string id = a.id;
		if (id.size() > 8 && id.compare(id.size() - 8, 8, ".desktop") == 0) id.resize(id.size() - 8);
		if ((!n.desktop_entry.empty() && lower(id) == lower(n.desktop_entry)) ||
				(!n.app.empty() && lower(a.name) == lower(n.app))) {
			app = &a;
			break;
		}
	}
	if (has_default) {
		items.push_back(MenuItem::action("Open", [srv, nid] { srv->notifications->run_action(nid, "default"); },
			"external-link"));
	} else if (app) {
		items.push_back(MenuItem::action("Open " + app->name, [srv, exec = app->exec, nid] {
			srv->spawn(exec);
			srv->notifications->dismiss(nid);
		}, "external-link"));
	}
	// The app's own buttons, each with an icon that says what it does.
	auto icon_for = [&lower](const std::string &label) -> std::string {
		const std::string l = lower(label);
		auto has = [&l](const char *w) { return l.find(w) != std::string::npos; };
		if (has("reply") || has("answer") || has("respond")) return "message-square";
		if (has("read") || has("done") || has("ok") || has("accept") || has("confirm")) return "check";
		if (has("mute") || has("silence") || has("snooze")) return "bell-off";
		if (has("delete") || has("remove") || has("trash")) return "trash";
		if (has("call")) return "smartphone";
		if (has("download") || has("save")) return "download";
		if (has("show") || has("view") || has("see")) return "eye";
		if (has("copy")) return "copy";
		if (has("open")) return "external-link";
		if (has("cancel") || has("decline") || has("reject") || has("stop")) return "circle-x";
		return "circle-dot";
	};
	for (const auto &[key, label] : n.actions) {
		if (key == "default" || label.empty()) continue;
		items.push_back(MenuItem::action(label, [srv, nid, k = key] { srv->notifications->run_action(nid, k); },
			icon_for(label)));
	}
	items.push_back(MenuItem::separator());
	const std::string text = n.body.empty() ? n.summary : n.summary + "\n" + n.body;
	if (!text.empty()) {
		items.push_back(MenuItem::action("Copy the text", [srv, text] {
			srv->spawn("printf %s " + shell_quote(text) + " | wl-copy");
		}, "copy"));
	}
	items.push_back(MenuItem::action("Notification settings", [srv] { srv->open_settings("notifications"); }, "gear"));
	items.push_back(MenuItem::separator());
	MenuItem gone = MenuItem::action("Dismiss", [srv, nid] { srv->notifications->dismiss(nid); }, "close");
	gone.danger = true;
	items.push_back(gone);
	srv->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point", std::move(items));
}

void ControlCentre::release() {
	if (!edit_tile_.empty()) {
		end_edit_drag();
		return;
	}
	if (dragging_ == "mp:seek") media_->end_drag();
	const bool slider = dragging_.rfind("sl:", 0) == 0;
	dragging_.clear();
	drag_value_ = -1;
	server_->cc_grab = false;
	if (slider && server_->status->changed) server_->status->changed();   // the panels catch up
	redraw();
}

void ControlCentre::scroll(double lx, double ly, double delta) {
	const double px = lx - box_.x, py = ly - box_.y;
	const int step = static_cast<int>(std::lround(delta * 3));
	if (page_ == Page::Tiles && inside(notes_area_, px, py)) {
		notes_scroll_ = std::clamp(notes_scroll_ + step, 0, notes_max_scroll_);
		redraw();
	} else if (page_ != Page::Tiles && inside(list_area_, px, py)) {
		list_scroll_ = std::clamp(list_scroll_ + step, 0, list_max_scroll_);
		redraw();
	} else {
		// Over a slider tile: the wheel changes it.
		std::string id = hot_at(px, py);
		if (id.rfind("si:", 0) == 0) id = "sl:" + id.substr(3);   // (its icon)
		const int change = delta < 0 ? 5 : -5;
		SystemStatus &st = *server_->status;
		if (id == "sl:volume") st.set_volume(st.volume + change);
		else if (id == "sl:brightness") st.set_brightness(st.brightness + change);
		else if (id == "sl:mic") st.set_mic_volume(st.mic_volume + change);
	}
}

void ControlCentre::open_page(Page page) {
	SystemStatus &st = *server_->status;
	page_ = page;
	list_scroll_ = 0;
	fx_.snap("list", 0);
	fx_.snap("page", 1);
	fx_.set("page", 0);
	password_for_.clear();
	wifi_message_.clear();
	if (page_ == Page::Wifi) {
		st.scan_wifi();
	} else if (page_ == Page::Bluetooth) {
		st.scan_bluetooth();
	}
	redraw();
}

// A tile's main click: switch it, or do it.
void ControlCentre::tile_action(const std::string &t) {
	SystemStatus &st = *server_->status;
	const Config &c = server_->config;
	fx_.snap("press:" + t, 1);
	fx_.set("press:" + t, 0);
	if (t == "wifi") st.toggle_wifi();
	else if (t == "bluetooth") st.toggle_bluetooth();
	else if (t == "sound") st.toggle_mute();
	else if (t == "airplane") st.toggle_airplane();
	else if (t == "caffeine") st.toggle_caffeine();
	else if (t == "power") st.cycle_power_profile();
	else if (t == "battery" && look_index() == 1) {
		// Windows 11's "Energy saver": power saver on and off.
		st.set_power_profile(st.power_profile == "power-saver" ? "balanced" : "power-saver");
	} else if (t == "battery") st.cycle_power_profile();
	else if (t == "dark") server_->write_setting("theme", c.dark ? "light" : "dark");
	else if (t == "nightlight") server_->write_setting("night_light", c.night_light ? "false" : "true");
	else if (t == "dnd") server_->write_setting("dnd", c.dnd ? "false" : "true");
	else if (t == "screenshot") {
		// After the control centre has slid away, so it isn't in the picture.
		close();
		Server *srv = server_;
		// The screenshot tool, once the control centre has gone.
		srv->run_then("sleep 0.3", [srv](bool) { srv->screenshot("tool"); });
		return;
	}
	redraw();
}

// The rest of a tile (or a right-click): Wi-Fi and Bluetooth open their
// lists; the others their options.
void ControlCentre::tile_more(const std::string &t, double lx, double ly) {
	if (t == "wifi" && server_->status->wifi_available) {
		open_page(Page::Wifi);
	} else if (t == "bluetooth" && server_->status->bt_available) {
		open_page(Page::Bluetooth);
	} else if (t == "volume" || t == "sound") {
		sound_output_menu(server_, output_, lx, ly);   // where sound goes (and Mute)
	} else {
		show_tile_menu(t, lx, ly);
	}
}

void ControlCentre::show_tile_menu(const std::string &tile, double lx, double ly) {
	Server *srv = server_;
	const Config &c = srv->config;
	SystemStatus &st = *srv->status;
	std::vector<MenuItem> items;
	auto set = [srv](std::string key, std::string value) {
		return [srv, key, value] { srv->write_setting(key, value); };
	};
	if (tile == "dark") {
		items.push_back(MenuItem::radio("Dark", c.dark, set("theme", "dark")));
		items.push_back(MenuItem::radio("Light", !c.dark, set("theme", "light")));
	} else if (tile == "nightlight") {
		items.push_back(MenuItem::check("Night light", c.night_light,
			set("night_light", c.night_light ? "false" : "true")));
		items.push_back(MenuItem::header("Warmth"));
		for (const auto &[label, k] : std::vector<std::pair<std::string, int>>{
				{"A little", 5500}, {"Medium", 4300}, {"Warm", 3400}, {"Very warm", 2700}}) {
			items.push_back(MenuItem::radio(label, std::abs(c.night_light_temp - k) < 300, [srv, k = k] {
				Config::write_key("night_light", "true");
				srv->write_setting("night_light_temp", std::to_string(k));
			}));
		}
	} else if (tile == "dnd") {
		items.push_back(MenuItem::check("Do not disturb", c.dnd, set("dnd", c.dnd ? "false" : "true")));
		items.push_back(MenuItem::action("Clear notifications", [srv] {
			if (srv->notifications) srv->notifications->clear_all();
		}));
		items.push_back(MenuItem::action("Notification settings…", [srv] { srv->open_settings("notifications"); }));
	} else if (tile == "power" || tile == "battery") {
		if (tile == "battery" && st.has_battery) {
			items.push_back(MenuItem::header(std::to_string(st.battery) + "%, " + battery_words(st)));
		}
		for (const auto &[label, p] : std::vector<std::pair<std::string, std::string>>{
				{"Power saver", "power-saver"}, {"Balanced", "balanced"}, {"Performance", "performance"}}) {
			if (!st.power_profile.empty()) {
				items.push_back(MenuItem::radio(label, st.power_profile == p,
					[srv, p = p] { srv->status->set_power_profile(p); }));
			}
		}
		items.push_back(MenuItem::action("Power settings…", [srv] { srv->open_settings("power"); }));
	} else if (tile == "caffeine") {
		items.push_back(MenuItem::check("Keep the screen awake", st.caffeine, [srv] { srv->status->toggle_caffeine(); }));
	} else if (tile == "airplane") {
		items.push_back(MenuItem::check("Airplane mode", st.airplane, [srv] { srv->status->toggle_airplane(); }));
	} else if (tile == "sound" || tile == "volume") {
		items.push_back(MenuItem::check("Mute", st.muted, [srv] { srv->status->toggle_mute(); }));
	} else if (tile == "mic") {
		items.push_back(MenuItem::check("Mute microphone", st.mic_muted, [srv] { srv->status->toggle_mic(); }));
	} else if (tile == "screenshot") {
		items.push_back(MenuItem::action("Screenshot tool…", [this] { tile_action("screenshot"); }, "area"));
		items.push_back(MenuItem::action("This screen", [srv] {
			srv->control_centre->close();
			srv->run_then("sleep 0.35", [srv](bool) { srv->screenshot("screen"); });
		}, "display"));
		items.push_back(MenuItem::action("All screens", [srv] {
			srv->control_centre->close();
			srv->run_then("sleep 0.35", [srv](bool) { srv->screenshot("all"); });
		}));
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::action("Screenshot settings…", [srv] { srv->open_settings("screenshots"); }, "gear"));
	} else if (tile == "media") {
		items.push_back(MenuItem::action("Play / pause", [srv] { srv->status->media("play-pause"); }));
		items.push_back(MenuItem::action("Next", [srv] { srv->status->media("next"); }));
		items.push_back(MenuItem::action("Previous", [srv] { srv->status->media("previous"); }));
	}
	if ((tile == "sound" || tile == "volume" || tile == "mic")) {
		for (const char *mixer : {"pwvucontrol", "pavucontrol"}) {
			if (access((std::string("/usr/bin/") + mixer).c_str(), X_OK) == 0) {
				items.push_back(MenuItem::action("Sound settings…", [srv, m = std::string(mixer)] { srv->spawn(m); }));
				break;
			}
		}
	}
	if (compact()) {
		if (!items.empty()) {
			srv->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point",
				std::move(items));
		}
		return;
	}
	if (!items.empty()) items.push_back(MenuItem::separator());
	items.push_back(MenuItem::action("Edit tiles", [this] {
		editing_ = true;
		redraw();
	}, "edit"));
	MenuItem hide = MenuItem::action("Hide this tile", [this, tile] {
		layout_.erase(std::remove_if(layout_.begin(), layout_.end(),
			[&tile](const CcTile &t) { return t.id == tile; }), layout_.end());
		save_layout();
	});
	hide.danger = true;
	items.push_back(hide);
	srv->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point",
		std::move(items));
}

void ControlCentre::show_menu(double lx, double ly) {
	Server *srv = server_;
	const Config &c = srv->config;
	std::vector<MenuItem> items;
	items.push_back(MenuItem::action("Edit tiles", [this] {
		editing_ = true;
		redraw();
	}, "edit"));
	items.push_back(MenuItem::header("Look"));
	for (const auto &[label, look] : std::vector<std::pair<std::string, std::string>>{
			{"Dusk", "dusk"}, {"Windows", "windows"}, {"GNOME", "gnome"}, {"macOS", "macos"}}) {
		items.push_back(MenuItem::radio(label, c.cc_style == look, [srv, l = look] { srv->write_setting("cc_style", l); }));
	}
	items.push_back(MenuItem::separator());
	items.push_back(MenuItem::check("Notifications", c.cc_notifications,
		[srv, on = c.cc_notifications] { srv->write_setting("cc_notifications", on ? "false" : "true"); }));
	items.push_back(MenuItem::check("Stack notifications by app", c.notif_group,
		[srv, on = c.notif_group] { srv->write_setting("notif_group", on ? "false" : "true"); }));
	items.push_back(MenuItem::header("Opens on the"));
	for (const auto &[label, side] : std::vector<std::pair<std::string, std::string>>{
			{"Side of its button", "auto"}, {"Left", "left"}, {"Right", "right"}}) {
		items.push_back(MenuItem::radio(label, c.cc_side == side, [srv, s = side] { srv->write_setting("cc_side", s); }));
	}
	items.push_back(MenuItem::separator());
	items.push_back(MenuItem::action("Control centre settings…", [srv] { srv->open_settings("controlcentre"); }, "gear"));
	srv->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point",
		std::move(items));
}

// Power: the same menu as the app menu's power button (power_actions).
void ControlCentre::show_power_menu(double lx, double ly) {
	server_->show_power_menu(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point");
}

void ControlCentre::activate(const std::string &id) {
	SystemStatus &st = *server_->status;
	auto starts = [&](const char *p) { return id.rfind(p, 0) == 0; };
	if (id.empty()) {
		return;
	}
	if (id == "settings") {
		close();
		server_->open_settings();
		return;
	}
	if (id == "settings-cc") {
		close();
		server_->open_settings("controlcentre");
		return;
	}
	if (id == "lock-now") {
		close();
		server_->power_action("lock");
		return;
	}
	if (id == "me") {
		close();
		server_->open_settings("about");
		return;
	}
	if (id == "power") {
		const wlr_box b = [&] {
			for (const Hot &h : hots_) if (h.id == "power") return h.box;
			return wlr_box{};
		}();
		show_power_menu(box_.x + b.x, box_.y + b.y + b.height);
		return;
	}
	if (id == "edit" || id == "edit-done") {
		editing_ = id == "edit" ? !editing_ : false;
	} else if (id == "edit-reset") {
		layout_ = default_layout(server_);
		server_->write_setting(layout_key(), "");
	} else if (starts("rm:")) {
		const std::string t = id.substr(3);
		layout_.erase(std::remove_if(layout_.begin(), layout_.end(),
			[&t](const CcTile &x) { return x.id == t; }), layout_.end());
		save_layout();
	} else if (starts("add:")) {
		// Into the first free place, row by row.
		const TileDef *d = tile_def(id.substr(4));
		if (d) {
			CcTile t{d->id, 0, 0, d->w, d->h};
			bool placed = false;
			for (int r = 0; r < kMaxRows && !placed; r++) {
				for (int c = 0; c + t.w <= kCols && !placed; c++) {
					t.c = c;
					t.r = r;
					placed = fits(layout_, t, "");
				}
			}
			if (!placed) {
				t.w = t.h = 1;
				for (int r = 0; r < kMaxRows && !placed; r++) {
					for (int c = 0; c < kCols && !placed; c++) {
						t.c = c;
						t.r = r;
						placed = fits(layout_, t, "");
					}
				}
			}
			if (placed) {
				layout_.push_back(t);
				save_layout();
			}
		}
	} else if (starts("ti:")) {
		tile_action(id.substr(3));
		return;
	} else if (starts("tm:")) {
		const Hot *h = nullptr;
		for (const Hot &x : hots_) if (x.id == id) h = &x;
		tile_more(id.substr(3), box_.x + (h ? h->box.x + h->box.width / 2 : 0),
			box_.y + (h ? h->box.y + h->box.height : 0));
		return;
	} else if (starts("pp:")) {
		// A power mode button on the battery tile.
		st.set_power_profile(id.substr(3));
	} else if (id == "mp:output") {
		const Hot *h = nullptr;
		for (const Hot &x : hots_) if (x.id == id) h = &x;
		output_menu(box_.x + (h ? h->box.x : 0), box_.y + (h ? h->box.y + h->box.height : 0));
		return;
	} else if (id == "mp:lyrics") {
		const bool was = server_->config.lyrics_show;
		server_->write_setting("lyrics_show", was ? "false" : "true");
		if (!was) lyrics_note(server_);
	} else if (id == "mp:source") {
		const Hot *h = nullptr;
		for (const Hot &x : hots_) if (x.id == id) h = &x;
		media_source_menu(server_, output_, box_.x + (h ? h->box.x : 0), box_.y + (h ? h->box.y + h->box.height : 0));
		return;
	} else if (id == "mp:card" || id == "mp:seek") {
		return;
	} else if (starts("mp:")) {
		st.media(id.substr(3).c_str());
	} else if (id == "back") {
		page_ = Page::Tiles;
		fx_.snap("page", 1);
		fx_.set("page", 0);
		typing_ = false;
		password_for_.clear();
	} else if (id == "detail-switch") {
		if (page_ == Page::Wifi) {
			if (st.airplane) st.toggle_airplane();
			else st.toggle_wifi();
		} else {
			st.toggle_bluetooth();
		}
	} else if (id == "rescan") {
		if (page_ == Page::Wifi) st.scan_wifi();
		else st.scan_bluetooth();
	} else if (id == "password") {
		typing_ = true;
	} else if (id == "connect") {
		handle_key(XKB_KEY_Return, "");
		return;
	} else if (starts("net:")) {
		const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 4));
		if (i < st.networks.size() && !connecting_) {
			const WifiNetwork n = st.networks[i];
			if (n.active) {
				return;
			}
			if (n.secure && !n.saved) {
				password_for_ = n.ssid;
				password_.clear();
				typing_ = true;
				wifi_message_.clear();
			} else {
				connecting_ = true;
				wifi_message_ = "Connecting to " + n.ssid + "…";
				st.connect_wifi(n, "", [this, n](bool ok, std::string reason) {
					connecting_ = false;
					if (ok) {
						wifi_message_.clear();
					} else if (reason == "password") {
						password_for_ = n.ssid;
						password_.clear();
						typing_ = true;
						wifi_message_ = "Wrong or missing password";
					} else {
						wifi_message_ = "Couldn't connect to " + n.ssid;
					}
					server_->status->scan_wifi();
					redraw();
				});
			}
		}
	} else if (starts("bt:")) {
		const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 3));
		if (i < st.bt_devices.size()) {
			st.connect_bluetooth(st.bt_devices[i]);
		}
	} else if (id == "clear") {
		if (server_->notifications) server_->notifications->clear_all();
		notes_scroll_ = 0;
		open_groups_.clear();
	} else if (id == "note-calendar") {
		Output *out = output_;
		const wlr_box a = notes_anchor_;
		const std::string edge = notes_edge_.empty() ? "bottom" : notes_edge_;
		close();
		if (out && server_->calendar) server_->calendar->open(out, a, edge);
		return;
	} else if (id == "note-settings") {
		close();
		server_->open_settings("notifications");
		return;
	} else if (starts("grpclose:")) {
		const std::string app = id.substr(9);
		if (Notifications *n = server_->notifications.get()) {
			std::vector<uint32_t> ids;
			for (const Notification &x : n->list()) {
				if ((x.app.empty() ? "Notification" : x.app) == app) ids.push_back(x.id);
			}
			for (uint32_t nid : ids) n->dismiss(nid);
		}
		open_groups_.erase(app);
	} else if (starts("grp:")) {
		const std::string app = id.substr(4);
		if (!open_groups_.erase(app)) open_groups_.insert(app);
	} else if (starts("si:")) {
		const std::string t = id.substr(3);
		if (t == "volume") st.toggle_mute();
		else if (t == "mic") st.toggle_mic();
	} else if (starts("noteclose:")) {
		if (server_->notifications) {
			server_->notifications->dismiss(static_cast<uint32_t>(std::strtoul(id.c_str() + 10, nullptr, 10)));
		}
	} else if (starts("note:")) {
		if (server_->notifications) {
			server_->notifications->activate(static_cast<uint32_t>(std::strtoul(id.c_str() + 5, nullptr, 10)));
		}
	}
	redraw();
}

// ---- Media menus ------------------------------------------------------------------------

namespace {
// Lyrics turned on, or searched again: say what's happening, so it's not
// just "nothing shows" (none found, still looking, SongRec missing…).
void lyrics_note(Server *srv) {
	if (!srv->notifications || !srv->lyrics) return;
	static uint32_t note = 0;
	const Lyrics &ly = *srv->lyrics;
	if (ly.available()) {
		// Found: they're on the screen, nothing to say, unless they're
		// behind the windows (where a maximised one hides them).
		// (Said once per session, not every time.)
		static bool told = false;
		if (srv->config.lyrics_on_top || told) {
			if (note) srv->notifications->dismiss(note);
			note = 0;
			return;
		}
		told = true;
		note = srv->notifications->post("Lyrics are on the desktop",
			"Behind your windows. To have them over the windows, turn on \"Lyrics over the windows\" "
			"in this menu.", "audio-x-generic-symbolic", 0, note);
		return;
	}
	const std::string why = ly.status().empty() ? std::string("Nothing playing to look up") : ly.status();
	note = srv->notifications->post(ly.searching() ? "Looking for lyrics" : "No lyrics for this song", why +
		(ly.searching() ? "\nThey show as soon as they're found." : ""), "audio-x-generic-symbolic", 0, note);
}
} // namespace

// The media card's own menu items (lyrics, their style, the visualizer,
// settings): in the control centre's media card and the panel's media
// widget alike. The style menus open at `at` (next to a panel at `edge`,
// or "point").
void media_menu_items(Server *srv, Output *out, wlr_box at, const std::string &edge, std::vector<MenuItem> &items) {
	const Config &c = srv->config;
	const SystemStatus &st = *srv->status;
	items.push_back(MenuItem::check("Lyrics on the screen", c.lyrics_show, [srv, on = c.lyrics_show] {
		if (!srv->config.lyrics) Config::write_key("lyrics", "true");
		srv->write_setting("lyrics_show", on ? "false" : "true");
		if (!on) lyrics_note(srv);
	}));
	if (c.lyrics_show) {
		items.push_back(MenuItem::check("Lyrics over the windows", c.lyrics_on_top, [srv, on = c.lyrics_on_top] {
			srv->write_setting("lyrics_on_top", on ? "false" : "true");
		}));
	}
	if (st.media_available && srv->lyrics) {
		// Also when lyrics were found: they may be another song's, or badly timed.
		items.push_back(MenuItem::action(srv->lyrics->searching() ? "Looking for lyrics…" : "Search for lyrics again",
			[srv] { srv->lyrics->retry(false); lyrics_note(srv); }, "refresh"));
		if (c.lyrics_recognize) {
			items.push_back(MenuItem::action("Listen for the song (SongRec)",
				[srv] { srv->lyrics->retry(true); lyrics_note(srv); }, "mic-vocal"));
		}
	}
	// The styles in menus of their own (all in one was taller than a laptop's screen).
	items.push_back(MenuItem::action("Lyrics style…", [srv, out, at, edge] {
		static const std::pair<const char *, const char *> kStyles[] = {
			{"Classic", "classic"}, {"Karaoke", "karaoke"}, {"Subtitles", "subtitle"}, {"Glow", "glow"},
			{"Scrolling", "scroll"}, {"Bounce", "bounce"}, {"Typewriter", "typewriter"}, {"Comic", "comic"},
		};
		std::vector<MenuItem> sub;
		sub.push_back(MenuItem::header("Lyrics style"));
		for (const auto &[label, key] : kStyles) {
			sub.push_back(MenuItem::radio(label, srv->config.lyrics_style == key, [srv, k = std::string(key)] {
				Config::write_key("lyrics_show", "true");
				srv->write_setting("lyrics_style", k);
				lyrics_note(srv);
			}));
		}
		srv->menu->show(out, at, edge, std::move(sub));
	}, "mic-vocal"));
	items.push_back(MenuItem::separator());
	items.push_back(MenuItem::action("Beat visualizer…", [srv, out, at, edge] {
		static const std::pair<const char *, const char *> kViz[] = {
			{"Bars", "bars"}, {"Mirror", "mirror"}, {"Dots", "dots"}, {"Wave", "wave"}, {"Hills", "hills"}, {"Off", "off"},
		};
		static const std::pair<const char *, const char *> kColor[] = {
			{"From the album art", "art"}, {"Accent", "accent"}, {"White", "text"}, {"Rainbow", "rainbow"},
		};
		std::vector<MenuItem> sub;
		sub.push_back(MenuItem::header("Beat visualizer"));
		for (const auto &[label, key] : kViz) {
			sub.push_back(MenuItem::radio(label, srv->config.media_viz == key,
				[srv, k = std::string(key)] { srv->write_setting("media_viz", k); }));
		}
		sub.push_back(MenuItem::header("Colour"));
		for (const auto &[label, key] : kColor) {
			sub.push_back(MenuItem::radio(label, srv->config.media_viz_color == key,
				[srv, k = std::string(key)] { srv->write_setting("media_viz_color", k); }));
		}
		sub.push_back(MenuItem::separator());
		sub.push_back(MenuItem::check("Follow the music's beat", srv->config.media_wave_beat,
			[srv, on = srv->config.media_wave_beat] { srv->write_setting("media_wave_beat", on ? "false" : "true"); }));
		srv->menu->show(out, at, edge, std::move(sub));
	}, "audio-lines"));
	items.push_back(MenuItem::action("Media & lyrics settings…", [srv] { srv->open_settings("media"); }, "gear"));
}

void ControlCentre::media_menu(double lx, double ly) {
	Server *srv = server_;
	const SystemStatus &st = *srv->status;
	std::vector<MenuItem> items;
	if (st.media_available) {
		items.push_back(MenuItem::action(st.media_playing ? "Pause" : "Play", [srv] { srv->status->media("play-pause"); },
			st.media_playing ? "pause" : "play"));
		items.push_back(MenuItem::action("Next", [srv] { srv->status->media("next"); }, "next"));
		items.push_back(MenuItem::action("Previous", [srv] { srv->status->media("previous"); }, "previous"));
		items.push_back(MenuItem::separator());
	}
	media_menu_items(srv, output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point", items);
	srv->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point", std::move(items));
}

void ControlCentre::output_menu(double lx, double ly) {
	sound_output_menu(server_, output_, lx, ly);
}

// ---- The other looks ------------------------------------------------------------------------
// They share the tiles' state, clicks and lists with Dusk's own; only the
// drawing and the layout are theirs. Looks: 0 Windows, 1 GNOME, 2 macOS.

namespace {
bool has_list(const std::string &tile) {
	return tile == "wifi" || tile == "bluetooth" || tile == "power";
}
} // namespace

void ControlCentre::draw_hslider(cairo_t *cr, const std::string &tile, double x, double y, double w, double h,
		float scale, int look, bool hot) {
	const TileState s = tile_state(server_, tile);
	const std::string vkey = "val:" + tile;
	if (dragging_ == "sl:" + tile) fx_.snap(vkey, s.value);
	else fx_.set(vkey, s.value, s.value);
	const double v = std::clamp(fx_.get(vkey), 0.0, 100.0) / 100.0;
	const Color fill = s.muted ? theme::text_dim : theme::accent;
	if (look == 2) {
		// macOS: a thick white bar with the icon at its start.
		const double r = h / 2;
		fill_rounded(cr, x, y, w, h, r, theme::tile_off);
		const Color bar = theme::dark ? rgb(0xf2f2f7) : rgb(0xffffff);
		cairo_save(cr);
		rounded_rect(cr, x, y, w, h, r);
		cairo_clip(cr);
		fill_rounded(cr, x, y, std::max(h, w * v), h, r, bar);
		cairo_restore(cr);
		draw_named_icon(cr, server_->icons, s.icon, x + h / 2 - 7, y + h / 2 - 7, 14, rgb(0x5a5a60), scale);
		if (hot) add_hot("sl:" + tile, x, y, w, h, r);
		return;
	}
	// Windows and GNOME: the icon, then a thin track with a round knob.
	const double is = 16;
	draw_named_icon(cr, server_->icons, s.icon, x + 4, y + h / 2 - is / 2, is, theme::text, scale);
	const double tx = x + 34, tw = w - 34 - 10, cy = y + h / 2;
	const double th = look == 0 ? 4 : 6;
	fill_rounded(cr, tx, cy - th / 2, tw, th, th / 2, theme::tile_off);
	fill_rounded(cr, tx, cy - th / 2, std::max(th, tw * v), th, th / 2, fill);
	const double kx = tx + tw * v;
	cairo_new_path(cr);
	if (look == 0) {
		cairo_arc(cr, kx, cy, 10, 0, 2 * M_PI);
		set_color(cr, theme::dark ? rgb(0x454545) : rgb(0xffffff));
		cairo_fill(cr);
		cairo_arc(cr, kx, cy, dragging_ == "sl:" + tile ? 7 : 5.5, 0, 2 * M_PI);
		set_color(cr, fill);
		cairo_fill(cr);
	} else {
		cairo_arc(cr, kx, cy + 1, 11, 0, 2 * M_PI);
		set_color(cr, rgb(0x000000, 0.18));
		cairo_fill(cr);
		cairo_arc(cr, kx, cy, 10.5, 0, 2 * M_PI);
		set_color(cr, rgb(0xffffff));
		cairo_fill(cr);
	}
	if (hot) add_hot("sl:" + tile, tx - 6, y, tw + 12, h, -2);
	// The icon: the wheel over it changes the level too; a click mutes.
	if (hot) add_hot("si:" + tile, x, y, 28, h, -2);
}

void ControlCentre::draw_pill_toggle(cairo_t *cr, const std::string &tile, double x, double y, double w, double h,
		float scale, int look, bool hot) {
	TileState s = tile_state(server_, tile);
	if (look == 0 && tile == "battery") {
		// Windows 11: the battery button is Energy saver, lit while it's on.
		s.on = server_->status->power_profile == "power-saver";
		s.usable = !server_->status->power_profile.empty();
	}
	const bool on = s.on && s.usable;
	const Color fill = on ? theme::accent : !s.usable ? with_alpha(theme::tile_off, theme::tile_off.a * 0.5)
		: theme::tile_off;
	const Color fg = !s.usable ? theme::text_dim : on ? theme::on_accent : theme::text;
	const bool more = has_list(tile) && s.usable;
	if (look == 1) {
		// GNOME: a pill; the ones with a list have a chevron end of their own.
		const double r = std::min({h / 2, w / 2, 26.0});
		fill_rounded(cr, x, y, w, h, r, fill);
		const double main_w = more ? w - 40 : w;
		draw_named_icon(cr, server_->icons, s.icon, x + 16, y + h / 2 - 8, 16, fg, scale);
		const bool sub = more && !s.subtitle.empty();
		draw_text(cr, s.title, x + 42, sub ? y + h / 2 - 17 : y, main_w - 48, sub ? 18 : h, theme::font_bold, fg);
		if (sub) draw_text(cr, s.subtitle, x + 42, y + h / 2 + 1, main_w - 48, 16, theme::font_small, with_alpha(fg, 0.8));
		if (more) {
			fill_rounded(cr, x + main_w, y, 40, h, r, on ? mix(theme::accent, rgb(0x000000), 0.15) : theme::tile_off);
			draw_named_icon(cr, server_->icons, "chevron-right", x + main_w + 12, y + h / 2 - 8, 16, fg, scale);
			if (hot) add_hot("tm:" + tile, x + main_w, y, 40, h, r);
		}
		if (hot) add_hot("ti:" + tile, x, y, main_w, h, r);
		return;
	}
	// Windows 11: a square-ish button (split in two for the ones with a list),
	// the name underneath; made wide, the name is inside.
	const double r = theme::small_radius();
	if (look == 2) {
		fill_rounded(cr, x, y, w, h, r, fill);
		const double main_w = more ? w - 30 : w;
		draw_named_icon(cr, server_->icons, s.icon, x + 14, y + h / 2 - 9, 18, fg, scale);
		draw_text(cr, s.title, x + 42, y + h / 2 - 17, main_w - 48, 18, theme::font_bold, fg);
		draw_text(cr, s.subtitle, x + 42, y + h / 2 + 1, main_w - 48, 16, theme::font_small, with_alpha(fg, 0.8));
		if (more) {
			cairo_rectangle(cr, x + main_w, y + 8, 1, h - 16);
			set_color(cr, with_alpha(fg, 0.25));
			cairo_fill(cr);
			draw_named_icon(cr, server_->icons, "chevron-right", x + main_w + 8, y + h / 2 - 7, 14, fg, scale);
			if (hot) add_hot("tm:" + tile, x + main_w, y, w - main_w, h, r);
		}
		if (hot) add_hot("ti:" + tile, x, y, main_w, h, r);
		return;
	}
	if (more) {
		const double split = w - 30;
		fill_rounded(cr, x, y, w, h, r, fill);
		cairo_rectangle(cr, x + split, y + 8, 1, h - 16);
		set_color(cr, with_alpha(fg, 0.25));
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, s.icon, x + split / 2 - 9, y + h / 2 - 9, 18, fg, scale);
		draw_named_icon(cr, server_->icons, "chevron-right", x + split + 8, y + h / 2 - 7, 14, fg, scale);
		if (hot) add_hot("ti:" + tile, x, y, split, h, r);
		if (hot) add_hot("tm:" + tile, x + split, y, w - split, h, r);
	} else {
		fill_rounded(cr, x, y, w, h, r, fill);
		draw_named_icon(cr, server_->icons, s.icon, x + w / 2 - 9, y + h / 2 - 9, 18, fg, scale);
		if (hot) add_hot("ti:" + tile, x, y, w, h, r);
	}
	const SystemStatus &sst = *server_->status;
	const std::string label = tile == "battery" ? std::string(sst.power_profile.empty() ? "Battery" : "Energy saver")
		: tile == "wifi" && on && !server_->status->ssid.empty() ? server_->status->ssid
		: tile == "bluetooth" && on && !server_->status->bt_device.empty() ? server_->status->bt_device : s.title;
	draw_text(cr, label, x - 4, y + h + 4, w + 8, 18, theme::font_small, s.usable ? theme::text : theme::text_dim,
		Align::Center);
}

// The other looks share Dusk's grid (drag tiles anywhere, pull a corner to
// resize, hide and add them back); only how each tile is drawn, their own
// starting layout, and what's around the grid are theirs.

// Windows 11's quick settings: the grid of buttons, bars and what's playing,
// then a strip along the bottom with the battery, edit and Settings.
void ControlCentre::draw_windows(cairo_t *cr, int &y, float scale) {
	draw_grid(cr, y, scale);
	if (editing_) {
		draw_edit_tray(cr, y, scale);
		return;
	}
	cairo_rectangle(cr, 0, y, width_, 1);
	set_color(cr, theme::divider);
	cairo_fill(cr);
	const SystemStatus &st = *server_->status;
	const int pad = kPad, fy = y + 8;
	if (st.has_battery) {
		draw_named_icon(cr, server_->icons, battery_icon(st.battery, st.charging), pad, fy + 6, 18, theme::text, scale);
		draw_text(cr, std::to_string(st.battery) + "%", pad + 24, fy, 80, 30, theme::font, theme::text);
		add_hot("tm:battery", pad - 4, fy, 90, 30, 6);
	}
	int bx = width_ - pad - 30;
	for (const auto &[id, icon] : std::vector<std::pair<const char *, const char *>>{
			{"power", "power"}, {"settings", "gear"}, {"edit", "edit"}}) {
		draw_named_icon(cr, server_->icons, icon, bx + 6, fy + 6, 18, theme::text, scale);
		add_hot(id, bx, fy, 30, 30, 6);
		bx -= 36;
	}
	y = fy + 38;
}

// GNOME's: the battery and round buttons on top, then the grid.
void ControlCentre::draw_gnome(cairo_t *cr, int &y, float scale) {
	const int pad = kPad;
	const SystemStatus &st = *server_->status;
	const int bs = 36;
	if (st.has_battery) {
		const std::string pct = std::to_string(st.battery) + " %";
		const int pw = text_width(pct, theme::font) + 46;
		fill_rounded(cr, pad, y, pw, bs, bs / 2.0, theme::tile_off);
		draw_named_icon(cr, server_->icons, battery_icon(st.battery, st.charging), pad + 12, y + bs / 2 - 8, 16,
			theme::text, scale);
		draw_text(cr, pct, pad + 34, y, pw - 38, bs, theme::font, theme::text);
		add_hot("tm:battery", pad, y, pw, bs, bs / 2.0);
	}
	int bx = width_ - pad - bs;
	for (const auto &[id, icon] : std::vector<std::pair<const char *, const char *>>{
			{"power", "power"}, {"lock-now", "lock"}, {"settings", "gear"}, {"edit", "edit"}}) {
		cairo_new_path(cr);
		cairo_arc(cr, bx + bs / 2.0, y + bs / 2.0, bs / 2.0, 0, 2 * M_PI);
		set_color(cr, id == std::string("edit") && editing_ ? theme::accent : theme::tile_off);
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, icon, bx + bs / 2.0 - 8, y + bs / 2.0 - 8, 16,
			id == std::string("edit") && editing_ ? theme::on_accent : theme::text, scale);
		add_hot(id, bx, y, bs, bs, bs / 2.0);
		bx -= bs + 8;
	}
	y += bs + 14;
	draw_grid(cr, y, scale);
	if (editing_) draw_edit_tray(cr, y, scale);
}

// macOS's Control Centre: modules on the grid, and "Edit Controls…" under it.
void ControlCentre::draw_macos(cairo_t *cr, int &y, float scale) {
	draw_grid(cr, y, scale);
	if (editing_) {
		draw_edit_tray(cr, y, scale);
		return;
	}
	const std::string edit = "Edit Controls…";
	const int ew = text_width(edit, theme::font_small) + 28;
	fill_rounded(cr, (width_ - ew) / 2, y, ew, 28, 14, theme::card_bg);
	draw_text(cr, edit, (width_ - ew) / 2, y, ew, 28, theme::font_small, theme::text, Align::Center);
	add_hot("edit", (width_ - ew) / 2, y, ew, 28, 14);
	y += 28 + 6;
}

bool ControlCentre::handle_key(xkb_keysym_t sym, const std::string &text) {
	if (sym == XKB_KEY_Escape) {
		if (typing_) {
			typing_ = false;
			password_for_.clear();
			redraw();
		} else {
			close();
		}
		return true;
	}
	if (!typing_) {
		return false;
	}
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if (password_.empty() || connecting_) {
			return true;
		}
		WifiNetwork n;
		for (const WifiNetwork &w : server_->status->networks) {
			if (w.ssid == password_for_) {
				n = w;
			}
		}
		n.ssid = password_for_;
		connecting_ = true;
		wifi_message_.clear();
		const std::string password = password_;
		server_->status->connect_wifi(n, password, [this](bool ok, std::string) {
			connecting_ = false;
			if (ok) {
				password_for_.clear();
				password_.clear();
				typing_ = false;
				wifi_message_.clear();
			} else {
				wifi_message_ = "Couldn't connect";
			}
			server_->status->scan_wifi();
			redraw();
		});
		redraw();
		return true;
	}
	if (sym == XKB_KEY_BackSpace) {
		if (!password_.empty()) {
			size_t cut = password_.size() - 1;
			while (cut > 0 && (static_cast<unsigned char>(password_[cut]) & 0xc0) == 0x80) {
				cut--;
			}
			password_.erase(cut);
		}
	} else if (!text.empty() && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f) {
		password_ += text;
	}
	redraw();
	return true;
}

} // namespace dusk
