#include "panel.hpp"

#include "mediaview.hpp"
#include "notify.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <linux/input-event-codes.h>

#include "ccentre.hpp"
#include "clipboard.hpp"
#include "draw.hpp"
#include "glyphs.hpp"
#include "launcher.hpp"
#include "osk.hpp"
#include "overview.hpp"
#include "trash.hpp"
#include "popover.hpp"
#include "recorder.hpp"
#include "system.hpp"
#include "tray.hpp"

namespace dusk {

namespace {

// The distribution's logo icon name (LOGO= in os-release), read once.
const std::string &distro_logo() {
	static const std::string name = [] {
		for (const char *file : {"/etc/os-release", "/usr/lib/os-release"}) {
			std::ifstream in(file);
			std::string line;
			while (std::getline(in, line)) {
				if (line.rfind("LOGO=", 0) == 0) {
					std::string v = line.substr(5);
					if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
						v = v.substr(1, v.size() - 2);
					}
					return v;
				}
			}
		}
		return std::string("distributor-logo");
	}();
	return name;
}

constexpr int kGap = 6;           // between widgets
constexpr int kTaskWidth = 200;   // a window button with its title
constexpr int kStatusIcon = 22;
constexpr int kBellSlot = 12;   // the small "notifications waiting" bell

// The recording pill: a red dot and the time ("● 1:07"), while the screen is recorded.
bool recording(Server *server) {
	return server->recorder && (server->recorder->active() || server->recorder->counting_down());
}
std::string rec_text(Server *server) {
	return server->recorder->starting() ? "REC" : server->recorder->elapsed_text();
}
int rec_width(Server *server) {
	return 8 + 8 + 6 + std::max(text_width("0:00", theme::font_bold), text_width(rec_text(server), theme::font_bold)) + 10;
}

// The colour across the colour wheel from c, bright and saturated enough to
// read on the panel (a grey accent has no opposite: then a warm coral).
Color opposite_colour(const Color &c, bool dark) {
	const double mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
	const double d = mx - mn;
	double h = 0;
	if (d > 1e-6) {
		if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.0);
		else if (mx == c.g) h = (c.b - c.r) / d + 2;
		else h = (c.r - c.g) / d + 4;
		h *= 60;
		if (h < 0) h += 360;
	}
	const double l0 = (mx + mn) / 2, s0 = d < 1e-6 ? 0 : d / (1 - std::fabs(2 * l0 - 1));
	if (s0 < 0.12) return dark ? rgb(0xff8a65) : rgb(0xe8590c);
	h = std::fmod(h + 180, 360);
	const double s = std::max(0.75, s0), l = dark ? 0.64 : 0.46;
	const double ch = (1 - std::fabs(2 * l - 1)) * s, x = ch * (1 - std::fabs(std::fmod(h / 60, 2) - 1)), m = l - ch / 2;
	double r = 0, g = 0, b = 0;
	if (h < 60) { r = ch; g = x; }
	else if (h < 120) { r = x; g = ch; }
	else if (h < 180) { g = ch; b = x; }
	else if (h < 240) { g = x; b = ch; }
	else if (h < 300) { r = x; b = ch; }
	else { r = ch; b = x; }
	return Color{r + m, g + m, b + m, 1.0};
}
constexpr int kMediaMax = 170;    // the now-playing chip, at most

std::string clock_text(bool h24) {
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char buf[16];
	std::strftime(buf, sizeof buf, h24 ? "%H:%M" : "%l:%M %p", &local);
	std::string s = buf;
	if (!s.empty() && s[0] == ' ') {
		s.erase(0, 1);   // "%l" pads single-digit hours
	}
	return s;
}

// GNOME and macOS: the day, date and time on one line ("Sun 1 Nov  09:19").
std::string one_line_clock(bool h24) {
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char buf[32];
	std::strftime(buf, sizeof buf, "%a %e %b", &local);
	std::string d = buf;
	for (size_t i = d.find("  "); i != std::string::npos; i = d.find("  ")) d.erase(i, 1);
	return d + "  " + clock_text(h24);
}

// Windows 11: the date in numbers under the time.
std::string numeric_date() {
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char buf[32];
	std::strftime(buf, sizeof buf, "%d/%m/%Y", &local);
	return buf;
}

std::string date_text() {
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char buf[32];
	std::strftime(buf, sizeof buf, "%a %d %b", &local);
	return buf;
}

} // namespace

Panel::Panel(Server *server, Output *output, const PanelConfig &config)
		: server_(server), output_(output), config_(config),
		layer_(server->layer_panel, &tag), fx_(server->anim.get(), [this] { redraw(); }),
		hl_(layer_, server->anim.get()) {
	// The shadow goes under the windows, so tiled windows sit on top of it.
	layer_.put_shadow_in(server->layer_panel_shadow);
	layer_.buffer->point_accepts_input = &Panel::accepts_input;
	hide_timer_ = wl_event_loop_add_timer(server->loop, &Panel::handle_hide_timer, this);
	if (config_.autohide) {
		shown_ = 0.0;
		want_shown_ = false;
	}
}

Panel::~Panel() {
	server_->anim->cancel(anim_);
	server_->anim->cancel(mag_anim_);
	if (mag_idle_) wl_event_source_remove(mag_idle_);
	wl_event_source_remove(hide_timer_);
}

void Panel::set_config(const PanelConfig &config) {
	const bool was_autohide = config_.autohide;
	config_ = config;
	if (config_.autohide != was_autohide) {
		server_->anim->cancel(anim_);
		anim_ = 0;
		want_shown_ = !config_.autohide;
		shown_ = want_shown_ ? 1.0 : 0.0;
	}
}

int Panel::thickness() const {
	const bool margin = config_.style != "full";
	return config_.size + (margin ? 14 : 0);
}

int Panel::reserved() const {
	return config_.autohide ? 0 : thickness();
}

void Panel::place(int offset, int from, int to) {
	const wlr_box b = output_->box();
	const int t = thickness();
	const std::string &e = config_.edge;
	if (e == "bottom") {
		strip_ = wlr_box{b.x, b.y + b.height - offset - t, b.width, t};
	} else if (e == "top") {
		strip_ = wlr_box{b.x, b.y + offset, b.width, t};
	} else if (e == "left") {
		strip_ = wlr_box{b.x + offset, from, t, std::max(1, to - from)};
	} else {
		strip_ = wlr_box{b.x + b.width - offset - t, from, t, std::max(1, to - from)};
	}
	redraw();
}

bool Panel::accepts_input(wlr_scene_buffer *buffer, double *sx, double *sy) {
	wlr_scene_tree *tree = buffer->node.parent;
	auto *tag = tree ? static_cast<NodeTag *>(tree->node.data) : nullptr;
	if (!tag || tag->kind != NodeTag::Kind::Panel) {
		return true;
	}
	const Panel *p = static_cast<const Panel *>(tag->object);
	const wlr_box &b = p->bar_;
	return p->shown_ > 0.01 && *sx >= b.x && *sy >= b.y && *sx < b.x + b.width &&
		*sy < b.y + b.height;
}

wlr_box Panel::bar_box() const {
	const int slide = static_cast<int>(std::lround((1.0 - shown_) * thickness()));
	wlr_box r{strip_.x + bar_.x, strip_.y + bar_.y, bar_.width, bar_.height};
	const std::string &e = config_.edge;
	if (e == "bottom") r.y += slide;
	else if (e == "top") r.y -= slide;
	else if (e == "left") r.x -= slide;
	else r.x += slide;
	return r;
}

bool Panel::contains(double lx, double ly) const {
	const wlr_box r = bar_box();
	return shown_ > 0.01 && lx >= r.x && ly >= r.y && lx < r.x + r.width && ly < r.y + r.height;
}

bool Panel::task_box(const View *view, wlr_box *out) const {
	const wlr_box bar = bar_box();
	for (const Slot &s : slots_) {
		if (s.kind == "tasks" && s.view == view) {
			*out = wlr_box{bar.x - bar_.x + s.box.x, bar.y - bar_.y + s.box.y, s.box.width,
				s.box.height};
			return shown_ > 0.5;
		}
	}
	return false;
}

std::string Panel::media_label() const {
	const SystemStatus *st = server_->status.get();
	if (!st || !st->media_available) {
		return "";
	}
	return st->media_title.empty() ? "Playing" : st->media_title;
}

wlr_box Panel::anchor(const std::string &widget) const {
	const wlr_box bar = bar_box();
	for (const Slot &s : slots_) {
		if (s.kind == widget) {
			return wlr_box{bar.x - bar_.x + s.box.x, bar.y - bar_.y + s.box.y, s.box.width,
				s.box.height};
		}
	}
	return bar;
}

std::vector<std::string> Panel::status_icons() const {
	std::vector<std::string> icons;
	const SystemStatus *st = server_->status.get();
	if (!st) {
		return icons;
	}
	// Quiet now: Do not disturb, or a recording under way (Dusk quiets
	// itself so nothing pops into the picture).
	const bool quiet_now = server_->config.dnd || (server_->recorder && server_->recorder->active());
	if (recording(server_)) {
		icons.push_back("rec");
	}
	// Notifications waiting: a small bell just before the network icon.
	// (Not when this panel has the bell of a separate notification centre: it has the count.)
	const bool bell_here = server_->config.separate_notes() &&
		std::find(config_.widgets.begin(), config_.widgets.end(), "notifications") != config_.widgets.end();
	if (server_->notifications && server_->notifications->unseen > 0 && !quiet_now && !bell_here) {
		icons.push_back("unseen:bell");
	}
	if (st->airplane) {
		icons.push_back("airplane");
	} else if (st->net_kind == "ethernet") {
		icons.push_back("wired");
	} else if (st->net_kind == "wifi") {
		icons.push_back(wifi_icon(st->wifi_signal));
	} else if (st->wifi_available) {
		icons.push_back("offline");
	}
	if (st->bt_powered && !st->bt_device.empty()) {
		icons.push_back("bluetooth");
	}
	// Privacy first: an app using the camera or recording from the microphone
	// (shown in amber, like on a phone).
	if (st->camera_in_use) {
		icons.push_back("privacy:camera");
	}
	if (st->mic_in_use) {
		icons.push_back("privacy:mic");
	}
	if (quiet_now) {
		icons.push_back("bell-off");
	}
	if (st->has_audio) {
		icons.push_back(speaker_icon(st->volume, st->muted));
	}
	if (st->has_battery) {
		icons.push_back(battery_icon(st->battery, st->charging));
	}
	if (icons.empty()) {
		icons.push_back("gear");   // still something to click for the control centre
	}
	return icons;
}

const AppInfo *Panel::pin_app(int index) const {
	if (index < 0 || index >= static_cast<int>(config_.pins.size())) {
		return nullptr;
	}
	const std::string &id = config_.pins[static_cast<size_t>(index)];
	for (const AppInfo &a : server_->apps.apps()) {
		if (a.id == id || a.id == id + ".desktop") {
			return &a;
		}
	}
	return nullptr;
}

View *Panel::pin_window(int index) const {
	const AppInfo *app = pin_app(index);
	if (!app) {
		return nullptr;
	}
	for (View *v : server_->views) {
		if (v->mapped && !app->icon.empty() && server_->apps.icon_for(v->app_id(), v->title()) == app->icon) {
			return v;
		}
	}
	return nullptr;
}

// Works out the bar and where each widget goes. Everything is computed
// along the panel ("main" axis) and across it, then turned into boxes.
//
// Flexible spaces split the widgets into groups: the first group sits at
// the start of the bar and the last at the end. With two flexible spaces the
// group between them is centred on the bar (so "spacer, clock, spacer" puts
// the clock in the middle of the screen, whatever is on either side), as far
// as the groups beside it allow. With more, the spaces share the room
// equally.
void Panel::layout() {
	const bool vertical = config_.vertical();
	const int t = thickness();
	const int size = config_.size;
	const bool margin = config_.style != "full";
	const bool dock = config_.style == "dock";
	const int m = margin ? 8 : 0;
	const int strip_len = vertical ? strip_.height : strip_.width;
	const int avail = std::max(0, strip_len - 2 * m);
	const int pad = 5;
	const int c = size - 2 * pad;   // square items

	// Windows to show (oldest first, like a taskbar).
	std::vector<View *> windows;
	const Config &cfg = server_->config;
	for (View *v : server_->views) {
		// This workspace's windows (or every workspace's: Settings > Workspaces).
		if (v->mapped && (cfg.tasks_all_workspaces || v->sticky || v->workspace == server_->workspace)) {
			windows.push_back(v);
		}
	}
	std::sort(windows.begin(), windows.end(), [](View *a, View *b) { return a->id < b->id; });

	// The apps on the panel, like a dock: the pinned ones first, in their
	// order (lit with a dot while open, each with its windows), then the
	// other open windows. A pinned app that isn't open is a launcher.
	struct Entry {
		int pin = -1;
		View *view = nullptr;
	};
	std::vector<Entry> entries;
	{
		std::vector<bool> used(windows.size(), false);
		for (int i = 0; i < static_cast<int>(config_.pins.size()); i++) {
			const AppInfo *app = pin_app(i);
			bool any = false;
			for (size_t k = 0; k < windows.size(); k++) {
				if (used[k] || !app || app->icon.empty() || server_->apps.icon_for(windows[k]->app_id(), windows[k]->title()) != app->icon) continue;
				used[k] = true;
				entries.push_back(Entry{any ? -1 : i, windows[k]});
				any = true;
			}
			if (!any && app) entries.push_back(Entry{i, nullptr});
		}
		for (size_t k = 0; k < windows.size(); k++) {
			if (!used[k]) entries.push_back(Entry{-1, windows[k]});
		}
	}

	const bool labels = config_.labels && !vertical;
	const int n_icons = static_cast<int>(status_icons().size());

	// Length of each widget along the bar; tasks stretch (worked out below).
	struct Part {
		std::string kind;
		int length;
		int widget;
	};
	std::vector<Part> parts;
	bool has_tasks = false;
	for (size_t wi = 0; wi < config_.widgets.size(); wi++) {
		const std::string &w = config_.widgets[wi];
		int len = 0;
		if (w == "menu" || w == "clipboard" || w == "trash" || w == "keyboard") {
			len = c;
		} else if (w == "overview") {
			const std::string &look = cfg.overview_icon;
			const int n = std::clamp(server_->workspace_count, 1, 12);
			if (look == "dots") {
				len = 26 + 20 + (n - 1) * 13;
			} else if (look == "numbers" || look == "roman" || look == "letters") {
				// A chip per workspace; the current one is a wider pill.
				const int chip = look == "roman" && !vertical ? 22 : 18;
				len = 22 + (chip + 12) + (n - 1) * (chip + 4);
			} else if (look == "lines") {
				len = 26 + 22 + (n - 1) * 15;
			} else if (look == "led") {
				// LED segments, one per workspace (the OSD's cockpit gauge).
				len = 26 + n * 10 + (n - 1) * 5;
			} else if (look == "rings" || look == "boxes") {
				len = 26 + n * 9 + (n - 1) * 6 + 6;
			} else if (look == "count") {
				len = vertical ? c + 10 : text_width(std::to_string(server_->workspace + 1) + " / " + std::to_string(n),
					theme::font_bold) + 26;
			} else {
				len = c;
			}
		} else if (w == "pins") {
			const int n = static_cast<int>(config_.pins.size());
			len = n > 0 ? n * c + (n - 1) * 4 : 0;
		} else if (w == "tray") {
			const int n = server_->tray ? static_cast<int>(server_->tray->items().size()) : 0;
			const int tc = std::min(c, 30);
			len = n > 0 ? n * tc + (n - 1) * 2 : 0;
		} else if (w == "tasks") {
			has_tasks = true;
		} else if (w == "spacer") {
			len = 0;
		} else if (w == "separator") {
			len = 13;
		} else if (w == "clock") {
			if (vertical) {
				len = cfg.show_date ? 70 : 40;   // the time, then the day and date (like a horizontal panel)
			} else if ((cfg.panel_look == "gnome" || cfg.panel_look == "macos") && cfg.show_date) {
				len = text_width(one_line_clock(cfg.clock_24h), theme::font_bold) + 24;
			} else {
				int tw = text_width(clock_text(cfg.clock_24h), theme::font_title);
				if (cfg.show_date && size >= 38) {
					tw = std::max(tw, text_width(cfg.panel_look == "windows" ? numeric_date() : date_text(),
						theme::font_small));
				}
				len = tw + 24;
			}
		} else if (w == "status") {
			len = n_icons * kStatusIcon + 16;
			if (recording(server_) && !vertical) {
				len += rec_width(server_) - kStatusIcon;
			}
			const std::vector<std::string> shown = status_icons();
			if (std::find(shown.begin(), shown.end(), "unseen:bell") != shown.end()) {
				len -= kStatusIcon - kBellSlot;   // the bell takes less room
			}
			if (vertical && server_->status && server_->status->has_battery && cfg.battery_style != "icon") {
				len += 14;   // the percentage under the battery
			}
			if (!vertical && server_->status && server_->status->has_battery) {
				if (cfg.battery_style == "percent") {
					len += text_width(std::to_string(server_->status->battery) + "%", theme::font_bold) + 4;
				} else if (cfg.battery_style == "ring") {
					len += 40 - kStatusIcon;   // the ring is wider than an icon
				}
			}
		} else if (w == "media") {
			// Grows in when something starts playing, shrinks away after.
			const std::string label = media_label();
			int want = 0;
			if (!label.empty()) {
				want = vertical ? c : std::min(kMediaMax, text_width(label, theme::font_small) + 46);
			}
			if (!drawing_) {
				fx_.set("len:media", want, want);
			}
			len = static_cast<int>(std::lround(fx_.get("len:media", want)));
		} else if (w == "showdesktop") {
			len = c;   // a button like the others (it was a thin line)
		} else if (w == "notifications") {
			// The bell: only when notifications have their own centre.
			len = cfg.separate_notes() ? c : 0;
		}
		parts.push_back(Part{w, len, static_cast<int>(wi)});
	}

	// Groups between flexible spaces (a dock has no flexible spaces).
	std::vector<std::vector<size_t>> groups(1);
	for (size_t i = 0; i < parts.size(); i++) {
		if (parts[i].kind == "spacer") {
			if (!dock) groups.emplace_back();
		} else {
			groups.back().push_back(i);
		}
	}
	const int spacers = static_cast<int>(groups.size()) - 1;
	auto group_len = [&](const std::vector<size_t> &g) {
		int len = 0, shown = 0;
		for (size_t i : g) {
			len += parts[i].length;
			shown++;
		}
		return len + std::max(0, shown - 1) * kGap;
	};
	const int ends = vertical ? pad + 2 : (margin ? size / 4 : 6);
	int used = 2 * ends + spacers * kGap;   // a flexible space is at least one gap wide
	for (const auto &g : groups) {
		used += group_len(g);
	}

	// Window buttons take what they need (up to the room left); with no
	// flexible space they fill the bar.
	int n_windows = 0, n_launchers = 0;
	for (const Entry &en : entries) (en.view ? n_windows : n_launchers)++;
	const int task_ideal = n_windows * (labels ? kTaskWidth : c + 4) + n_launchers * (c + 4);
	const int rest = std::max(0, avail - used);
	if (has_tasks) {
		const int task_len = (spacers == 0 && !dock) ? rest : std::min(task_ideal, rest);
		for (Part &p : parts) {
			if (p.kind == "tasks") p.length = task_len;
		}
	}
	int content = 2 * ends + spacers * kGap;
	for (const auto &g : groups) {
		content += group_len(g);
	}

	// The bar.
	const int bar_len = dock ? std::min(avail, std::max(content, size)) : (margin ? avail : strip_len);
	const int bar_start = dock ? (strip_len - bar_len) / 2 : m;
	const std::string &e = config_.edge;
	const int cross = (e == "bottom" || e == "right") ? t - m - size : m;
	bar_ = vertical ? wlr_box{cross, bar_start, size, bar_len} : wlr_box{bar_start, cross, bar_len, size};

	// Where each group starts along the bar.
	std::vector<int> starts(groups.size(), ends);
	if (spacers >= 1) {
		const int last = static_cast<int>(groups.size()) - 1;
		starts[last] = bar_len - ends - group_len(groups[last]);
		if (spacers == 2) {
			// The middle group, centred on the bar if the others leave room.
			const int len = group_len(groups[1]);
			const int lo = starts[0] + group_len(groups[0]) + kGap;
			const int hi = starts[last] - kGap - len;
			starts[1] = std::max(lo, std::min((bar_len - len) / 2, hi));
		} else if (spacers > 2) {
			int free = bar_len - 2 * ends;
			for (const auto &g : groups) free -= group_len(g);
			const double share = std::max(0, free) / static_cast<double>(spacers);
			double pos = ends;
			for (size_t gi = 0; gi + 1 < groups.size(); gi++) {
				starts[gi] = static_cast<int>(std::lround(pos));
				pos += group_len(groups[gi]) + share;
			}
		}
	}

	// The widgets.
	slots_.clear();
	auto make_box = [&](int main_pos, int main_len, int cross_pos, int cross_len) {
		return vertical ? wlr_box{bar_.x + cross_pos, bar_.y + main_pos, cross_len, main_len}
			: wlr_box{bar_.x + main_pos, bar_.y + cross_pos, main_len, cross_len};
	};
	size_t spacer_index = 0;
	int prev_end = 0;   // where the last group ended (for the flexible space after it)
	for (size_t gi = 0; gi < groups.size(); gi++) {
		int pos = starts[gi];
		if (gi > 0) {
			// The flexible space before this group: empty bar, but it knows
			// which widget it is (for its right-click menu).
			while (spacer_index < parts.size() && parts[spacer_index].kind != "spacer") spacer_index++;
			if (spacer_index < parts.size() && pos - prev_end > 0) {
				Slot s;
				s.kind = "spacer";
				s.widget = parts[spacer_index].widget;
				s.key = "spacer" + std::to_string(s.widget);
				s.box = make_box(prev_end, pos - prev_end, 0, size);
				slots_.push_back(s);
			}
			spacer_index++;
		}
		for (size_t pi : groups[gi]) {
			const Part &p = parts[pi];
			if (p.kind == "tasks") {
				const int win_room = std::max(0, p.length - n_launchers * (c + 4));
				int item = labels ? std::min(kTaskWidth, n_windows > 0 ? win_room / n_windows : kTaskWidth) : c + 4;
				bool icon_only = !labels || item < c + 40;
				if (icon_only) {
					item = c + 4;
				}
				int at_pos = pos;
				for (const Entry &en : entries) {
					const int len = en.view ? item : c + 4;
					if (at_pos + len > pos + p.length) break;   // no more room
					Slot s;
					s.widget = p.widget;
					s.pin = en.pin;
					if (en.view) {
						s.kind = "tasks";
						s.view = en.view;
						s.icon_only = icon_only;
						s.key = "task" + std::to_string(s.view->id);
					} else {
						s.kind = "pins";
						s.key = "pin" + std::to_string(en.pin);
					}
					// Buttons glide to their new place when others come or go.
					const double target = at_pos;
					fx_.set("pos:" + s.key, target, target);
					const int at = static_cast<int>(std::lround(fx_.get("pos:" + s.key, target)));
					s.box = make_box(at, (en.view ? item : c + 4) - 4, pad, c);
					slots_.push_back(s);
					at_pos += len;
				}
			} else if (p.kind == "tray" && server_->tray) {
				const int tc = std::min(c, 30);
				int i = 0;
				for (const TrayItem *item : server_->tray->items()) {
					Slot s;
					s.kind = "tray";
					s.widget = p.widget;
					s.tray = item->key;
					s.key = "tray:" + item->key;
					s.box = make_box(pos + i * (tc + 2), tc, (size - tc) / 2, tc);
					slots_.push_back(s);
					i++;
				}
			} else if (p.kind == "pins") {
				for (int i = 0; i < static_cast<int>(config_.pins.size()); i++) {
					Slot s;
					s.kind = "pins";
					s.widget = p.widget;
					s.pin = i;
					s.key = "pin" + std::to_string(i);
					s.box = make_box(pos + i * (c + 4), c, pad, c);
					slots_.push_back(s);
				}
			} else if (p.length > 0) {
				Slot s;
				s.kind = p.kind;
				s.widget = p.widget;
				s.key = p.kind + std::to_string(p.widget);
				const int inset = p.kind == "separator" ? 0 : pad;
				s.box = make_box(pos, p.length, inset, size - 2 * inset);
				slots_.push_back(s);
			}
			pos += p.length + kGap;
		}
		prev_end = groups[gi].empty() ? pos : pos - kGap;
	}
}

void Panel::redraw() {
	if (strip_.width <= 0 || strip_.height <= 0 || drawing_) {
		return;
	}
	layout();
	drawing_ = true;
	const float scale = output_->output->scale;
	Canvas canvas(strip_.width, strip_.height, scale);
	cairo_t *cr = canvas.cr();

	// GNOME's top bar is black with white writing, whatever the theme.
	const std::string &look = server_->config.panel_look;
	const bool black = look == "gnome" && config_.edge == "top";
	struct Saved {
		Color text, dim, hover, active, divider;
	} saved{theme::text, theme::text_dim, theme::panel_hover, theme::panel_active, theme::divider};
	if (black) {
		theme::text = rgb(0xffffff);
		theme::text_dim = rgb(0xffffff, 0.72);
		theme::panel_hover = rgb(0xffffff, 0.16);
		theme::panel_active = rgb(0xffffff, 0.24);
		theme::divider = rgb(0xffffff, 0.2);
	}
	const Color bg = with_alpha(black ? rgb(0x000000) : theme::panel_bg, config_.opacity);
	layer_.set_body_opacity(config_.opacity);
	layer_.set_blur(server_->config.panel_blur);
	const wlr_box &b = bar_;
	double radius = 0;
	if (config_.style == "full") {
		set_color(cr, bg);
		cairo_paint(cr);
		// A line on the side facing the windows (none with Lines: None, or
		// when the panel has no background at all).
		const std::string &e = config_.edge;
		if (theme::ui_line > 0 && config_.opacity > 0.02 && look != "gnome" && look != "macos") {
		const double lw = theme::ui_line == 2 ? 2 : 1;
		if (e == "bottom") cairo_rectangle(cr, 0, 0, strip_.width, lw);
		else if (e == "top") cairo_rectangle(cr, 0, strip_.height - lw, strip_.width, lw);
		else if (e == "left") cairo_rectangle(cr, strip_.width - lw, 0, lw, strip_.height);
		else cairo_rectangle(cr, 0, 0, lw, strip_.height);
		set_color(cr, theme::ui_line == 2 ? theme::ui_line_color : theme::divider);
		cairo_fill(cr);
		}
	} else {
		radius = config_.style == "dock" ? std::min<double>(theme::ui_radius, b.height / 2.0)
			: std::min(b.width, b.height) / 2.0;
		ui_card(cr, b.x, b.y, b.width, b.height, radius, bg);
	}

	// (Hover is a GPU highlight on top: see update_highlight.)
	for (const Slot &slot : slots_) {
		draw_slot(cr, slot, 0.0, scale);
	}
	if (black) {
		theme::text = saved.text;
		theme::text_dim = saved.dim;
		theme::panel_hover = saved.hover;
		theme::panel_active = saved.active;
		theme::divider = saved.divider;
	}
	layer_.present(canvas, b, radius);
	drawing_ = false;
	update_highlight();   // things may have moved

	// Slide in/out for auto-hide.
	const wlr_box bar = bar_box();
	layer_.show(bar.x - bar_.x, bar.y - bar_.y, std::clamp(0.3 + 0.7 * shown_, 0.0, 1.0));
	layer_.set_enabled(shown_ > 0.001);
}

void Panel::draw_battery_ring(cairo_t *cr, double x, double y, double w, double h) {
	// A pill whose outline fills up like a progress ring, with the number
	// inside.
	const SystemStatus &st = *server_->status;
	const double r = h / 2.0, lw = 2.2;
	const Color low = theme::danger;
	const Color fillc = st.battery <= 15 && !st.charging ? low
		: st.charging ? theme::success : theme::text;
	rounded_rect(cr, x + lw / 2, y + lw / 2, w - lw, h - lw, r - lw / 2);
	set_color(cr, with_alpha(theme::text, 0.18));
	cairo_set_line_width(cr, lw);
	cairo_stroke(cr);
	// Progress along the outline, starting at the top middle, clockwise.
	cairo_save(cr);
	rounded_rect(cr, x + lw / 2, y + lw / 2, w - lw, h - lw, r - lw / 2);
	cairo_path_t *path = cairo_copy_path_flat(cr);
	cairo_new_path(cr);
	double total = 0;
	for (int pass = 0; pass < 2; pass++) {
		double walked = 0, px = 0, py = 0, sx = 0, sy = 0;
		bool have = false;
		const double limit = total * std::clamp(st.battery, 0, 100) / 100.0;
		for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
			const cairo_path_data_t *d = &path->data[i];
			if (d->header.type == CAIRO_PATH_MOVE_TO) {
				px = sx = d[1].point.x;
				py = sy = d[1].point.y;
				if (pass == 1) cairo_move_to(cr, px, py);
				have = true;
				continue;
			}
			double nx, ny;
			if (d->header.type == CAIRO_PATH_LINE_TO) {
				nx = d[1].point.x;
				ny = d[1].point.y;
			} else if (d->header.type == CAIRO_PATH_CLOSE_PATH) {
				nx = sx;
				ny = sy;
			} else {
				continue;
			}
			const double seg = std::hypot(nx - px, ny - py);
			if (pass == 1 && have) {
				if (walked + seg >= limit) {
					const double t = seg > 0 ? (limit - walked) / seg : 0;
					cairo_line_to(cr, px + (nx - px) * t, py + (ny - py) * t);
					break;
				}
				cairo_line_to(cr, nx, ny);
			}
			walked += seg;
			px = nx;
			py = ny;
		}
		total = walked;
	}
	cairo_path_destroy(path);
	set_color(cr, fillc);
	cairo_set_line_width(cr, lw);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_stroke(cr);
	cairo_restore(cr);
	draw_text(cr, std::to_string(st.battery), x, y, w, h, theme::font_small, theme::text,
		Align::Center);
}

void Panel::draw_slot(cairo_t *cr, const Slot &s, double hover, float scale) {
	const wlr_box &b = s.box;
	const bool vertical = config_.vertical();
	const Config &cfg = server_->config;
	const std::string &e = config_.edge;
	// Inside the bar, corners follow the bar's own curve (or the look's).
	const double r = corner(b);
	const std::string &plook = cfg.panel_look;
	auto hover_bg = [&](double amount) {
		if (amount > 0.01) {
			fill_rounded(cr, b.x, b.y, b.width, b.height, r, with_alpha(theme::panel_hover,
				theme::panel_hover.a * amount));
		}
	};

	if (s.kind == "menu") {
		const bool open = server_->launcher && server_->launcher->is_open_on(output_);
		const double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
		const double rad = std::min(b.width, b.height) / 2.0;
		fx_.set("open:menu", open ? 1.0 : 0.0);
		const double dim = std::max(hover, fx_.get("open:menu"));
		const std::string &look = server_->config.menu_icon;
		// A picture of its own (the distribution's logo, or one you chose):
		// shown as it is, on the usual hover background.
		if (look == "distro" || look == "custom") {
			const std::string name = look == "distro" ? distro_logo() : server_->config.menu_icon_custom;
			hover_bg(dim);
			const double size = std::round(rad * 1.45);
			draw_icon(cr, server_->icons, name.empty() ? "distributor-logo" : name, "Apps",
				cx - size / 2, cy - size / 2, size, output_->output->scale);
			return;
		}
		// Windows 11 and macOS: the sign on its own (in the accent, or the
		// text colour), on the usual hover background. Dusk and GNOME: a disc.
		if (plook == "windows" || plook == "macos") {
			hover_bg(dim);
			set_color(cr, plook == "windows" ? theme::accent : theme::text);
		} else {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, rad, 0, 2 * M_PI);
			const double k = 1.0 - 0.15 * dim;
			set_color(cr, Color{theme::accent.r * k, theme::accent.g * k, theme::accent.b * k, 1.0});
			cairo_fill_preserve(cr);
			if (theme::ui_line == 2) {
				set_color(cr, theme::ui_line_color);
				cairo_set_line_width(cr, 1.5);
				cairo_stroke(cr);
			} else {
				cairo_new_path(cr);
			}
			set_color(cr, theme::on_accent);
		}
		if (look == "squares") {
			// Four rounded squares, like an app grid.
			const double q = rad * 0.34, g = rad * 0.12;
			for (int row = 0; row < 2; row++) {
				for (int col = 0; col < 2; col++) {
					rounded_rect(cr, cx - q - g / 2 + col * (q + g), cy - q - g / 2 + row * (q + g), q, q,
						q * 0.3);
					cairo_fill(cr);
				}
			}
		} else if (look == "dusk") {
			// Dusk's logo: the sun half set on the horizon, two bands of
			// evening through it, and the first star out.
			cairo_push_group(cr);
			const double sr = rad * 0.56, hy = cy + rad * 0.30;
			cairo_new_path(cr);
			cairo_arc(cr, cx, hy, sr, M_PI, 2 * M_PI);
			cairo_close_path(cr);
			cairo_fill(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
			const double band = std::max(1.2, sr * 0.13);
			cairo_rectangle(cr, cx - sr - 1, hy - sr * 0.42, 2 * sr + 2, band);
			cairo_rectangle(cr, cx - sr - 1, hy - sr * 0.17, 2 * sr + 2, band * 0.85);
			cairo_fill(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
			// The star: four points.
			const double stx = cx + rad * 0.40, sty = cy - rad * 0.36, so = rad * 0.20, si = so * 0.32;
			cairo_new_path(cr);
			cairo_move_to(cr, stx, sty - so);
			cairo_line_to(cr, stx + si, sty - si);
			cairo_line_to(cr, stx + so, sty);
			cairo_line_to(cr, stx + si, sty + si);
			cairo_line_to(cr, stx, sty + so);
			cairo_line_to(cr, stx - si, sty + si);
			cairo_line_to(cr, stx - so, sty);
			cairo_line_to(cr, stx - si, sty - si);
			cairo_close_path(cr);
			cairo_fill(cr);
			cairo_pop_group_to_source(cr);
			cairo_paint(cr);
		} else if (look == "search") {
			cairo_set_line_width(cr, std::max(1.6, rad * 0.13));
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_new_path(cr);
			cairo_arc(cr, cx - rad * 0.08, cy - rad * 0.08, rad * 0.32, 0, 2 * M_PI);
			cairo_stroke(cr);
			cairo_move_to(cr, cx + rad * 0.17, cy + rad * 0.17);
			cairo_line_to(cr, cx + rad * 0.42, cy + rad * 0.42);
			cairo_stroke(cr);
		} else {
			const double step = rad * 0.35;
			for (int row = 0; row < 3; row++) {
				for (int col = 0; col < 3; col++) {
					cairo_new_sub_path(cr);
					cairo_arc(cr, cx + (col - 1) * step, cy + (row - 1) * step, rad * 0.11, 0, 2 * M_PI);
				}
			}
			cairo_fill(cr);
		}
		return;
	}

	if (s.kind == "spacer") {
		return;   // empty bar
	}

	if (s.kind == "tray") {
		hover_bg(hover);
		if (server_->tray) {
			for (const TrayItem *item : server_->tray->items()) {
				if (item->key != s.tray) continue;
				const double icon = std::min(18.0, std::min(b.width, b.height) - 8.0);
				server_->tray->draw_icon(cr, *item, b.x + (b.width - icon) / 2.0, b.y + (b.height - icon) / 2.0, icon,
					scale);
			}
		}
		return;
	}

	if (s.kind == "overview") {
		draw_overview_button(cr, b, hover, scale);
		return;
	}

	if (s.kind == "trash") {
		hover_bg(hover);
		const double icon = std::min(18.0, std::min(b.width, b.height) - 8.0);
		draw_named_icon(cr, server_->icons, "trash", b.x + (b.width - icon) / 2.0, b.y + (b.height - icon) / 2.0,
			icon, theme::text, scale);
		if (server_->trash && !server_->trash->is_empty()) {
			// Something in it: a small dot in the accent colour.
			cairo_new_path(cr);
			cairo_arc(cr, b.x + b.width / 2.0 + icon / 2.0, b.y + b.height / 2.0 - icon / 2.0 + 1, 3.2, 0, 2 * M_PI);
			set_color(cr, theme::accent);
			cairo_fill(cr);
		}
		return;
	}

	if (s.kind == "clipboard") {
		const bool open = server_->clipboard_card && server_->clipboard_card->is_open_on(output_);
		fx_.set("open:" + s.key, open ? 1.0 : 0.0);
		const double o = fx_.get("open:" + s.key);
		if (o > 0.01) {
			fill_rounded(cr, b.x, b.y, b.width, b.height, r,
				with_alpha(theme::panel_active, theme::panel_active.a * o));
		}
		hover_bg(hover * (1.0 - o));
		const double icon = std::min(18.0, std::min(b.width, b.height) - 8.0);
		draw_named_icon(cr, server_->icons, "clipboard", b.x + (b.width - icon) / 2.0,
			b.y + (b.height - icon) / 2.0, icon, theme::text, scale);
		return;
	}

	// Running indicator on the side of the item facing the screen edge.
	auto indicator = [&](bool active) {
		if (plook == "macos") {
			// A small dot under every open app, the same for the one in front.
			const double d = 4;
			const double dx = vertical ? (e == "left" ? b.x + 1 : b.x + b.width - d - 1) : b.x + b.width / 2.0 - d / 2;
			const double dy = vertical ? b.y + b.height / 2.0 - d / 2 : (e == "top" ? b.y + 1 : b.y + b.height - d - 1);
			fill_rounded(cr, dx, dy, d, d, d / 2, theme::text_dim);
			return;
		}
		const double len = active ? 16 : plook == "windows" ? 6 : 4;
		double ix, iy, iw, ih;
		if (vertical) {
			iw = 3;
			ih = len;
			ix = e == "left" ? b.x + 1 : b.x + b.width - 4;
			iy = b.y + b.height / 2.0 - len / 2;
		} else {
			iw = len;
			ih = 3;
			ix = b.x + b.width / 2.0 - len / 2;
			iy = e == "top" ? b.y + 1 : b.y + b.height - 4;
		}
		fill_rounded(cr, ix, iy, iw, ih, 1.5, active ? theme::accent : theme::text_dim);
	};

	if (s.kind == "tasks" && s.view) {
		View *view = s.view;
		const bool active = view->active() && !view->minimized;
		fx_.set("active:" + s.key, active ? 1.0 : 0.0, active ? 1.0 : 0.0);
		const double act = fx_.get("active:" + s.key);
		// The window in front: lit (Windows 11 a little, macOS's dock not at all).
		const double lit = plook == "macos" ? 0.0 : plook == "windows" ? 0.6 : 1.0;
		if (act * lit > 0.01) {
			fill_rounded(cr, b.x, b.y, b.width, b.height, r,
				with_alpha(theme::panel_active, theme::panel_active.a * act * lit));
		}
		hover_bg(hover * (1.0 - act * lit));
		if (view->urgent && act < 0.5) {
			// It asked for attention.
			fill_rounded(cr, b.x, b.y, b.width, b.height, r, with_alpha(theme::accent, 0.35));
		}
		indicator(act > 0.5);
		const std::string title = view->title();
		double icon = plook == "macos" && config_.style == "dock" ? std::min(b.width, b.height) - 6.0
			: std::min(24.0, std::min(b.width, b.height) - 10.0);
		if (magnifies()) {
			// Smaller at rest, so there's room to grow into.
			const double full = std::min(b.width, b.height) - 1.0;
			icon = full * 0.74 + full * 0.26 * magnify_at(b);
		}
		const double ix = s.icon_only ? b.x + (b.width - icon) / 2.0 : b.x + 10;
		cairo_push_group(cr);
		draw_icon(cr, server_->icons, server_->apps.icon_for(view->app_id(), view->title()), title, ix,
			b.y + (b.height - icon) / 2.0, icon, scale);
		cairo_pop_group_to_source(cr);
		// Minimised, or on another workspace: a little faded.
		const bool elsewhere = !view->sticky && view->workspace != server_->workspace;
		cairo_paint_with_alpha(cr, view->minimized ? 0.45 : elsewhere ? 0.7 : 1.0);
		if (!s.icon_only) {
			draw_text(cr, title, ix + icon + 8, b.y, b.width - icon - 26, b.height,
				active ? theme::font_bold : theme::font,
				view->minimized ? theme::text_dim : theme::text);
		}
		return;
	}

	if (s.kind == "pins") {
		const AppInfo *app = pin_app(s.pin);
		hover_bg(hover);
		double icon = plook == "macos" && config_.style == "dock" ? std::min(b.width, b.height) - 6.0
			: std::min(26.0, b.width - 8.0);
		if (magnifies()) {
			const double full = std::min(b.width, b.height) - 1.0;
			icon = full * 0.74 + full * 0.26 * magnify_at(b);
		}
		draw_icon(cr, server_->icons, app ? app->icon : "", app ? app->name : "?",
			b.x + (b.width - icon) / 2.0, b.y + (b.height - icon) / 2.0, icon, scale);
		if (View *v = pin_window(s.pin)) {
			indicator(v->active());
		}
		return;
	}

	if (s.kind == "separator") {
		if (vertical) {
			cairo_rectangle(cr, b.x + 10, b.y + b.height / 2.0, b.width - 20, 1);
		} else {
			cairo_rectangle(cr, b.x + b.width / 2.0, b.y + 10, 1, b.height - 20);
		}
		set_color(cr, theme::divider);
		cairo_fill(cr);
		return;
	}

	if (s.kind == "keyboard") {
		// The on-screen keyboard: lit while it's up.
		if (server_->osk && server_->osk->is_open_on(output_)) {
			fill_rounded(cr, b.x, b.y, b.width, b.height, r, theme::panel_active);
		}
		hover_bg(hover);
		const double icon = std::min(18.0, std::min(b.width, b.height) - 8.0);
		draw_named_icon(cr, server_->icons, "keyboard", b.x + (b.width - icon) / 2.0, b.y + (b.height - icon) / 2.0,
			icon, theme::text, scale);
		return;
	}

	if (s.kind == "notifications") {
		if (b.width <= 0) return;
		// The bell: lit while the notification centre is open, a count
		// badge while there are notifications, crossed out in Do not disturb.
		const bool open = server_->control_centre && server_->control_centre->notes_open_on(output_);
		if (open) fill_rounded(cr, b.x, b.y, b.width, b.height, r, theme::panel_active);
		hover_bg(hover);
		const double icon = std::min(18.0, std::min(b.width, b.height) - 8.0);
		const double ix = b.x + (b.width - icon) / 2.0, iy = b.y + (b.height - icon) / 2.0;
		draw_named_icon(cr, server_->icons, server_->config.dnd ? "bell-off" : "bell", ix, iy, icon, theme::text, scale);
		int count = 0;
		if (server_->notifications) {
			for (const Notification &n : server_->notifications->list()) count += !n.transient;
		}
		if (count > 0 && !server_->config.dnd) {
			const std::string label = count > 9 ? "9+" : std::to_string(count);
			const double bh = 15, bw = std::max(bh, text_width(label, theme::font_small) + 7.0);
			const double bx = ix + icon - bw / 2.0 + 2, by = iy - 5;
			const bool unseen = server_->notifications->unseen > 0;
			fill_rounded(cr, bx, by, bw, bh, bh / 2, unseen ? theme::accent : with_alpha(theme::text, 0.55));
			draw_text(cr, label, bx, by, bw, bh, theme::font_small, unseen ? theme::on_accent : theme::bg_alt, Align::Center);
		}
		return;
	}

	if (s.kind == "showdesktop") {
		// The desktop: lit while the windows are out of the way.
		const bool shown = !server_->shown_desktop.empty();
		if (shown) {
			fill_rounded(cr, b.x, b.y, b.width, b.height, r, theme::panel_active);
		}
		hover_bg(hover);
		const double icon = std::min(18.0, std::min(b.width, b.height) - 8.0);
		draw_named_icon(cr, server_->icons, "desktop", b.x + (b.width - icon) / 2.0, b.y + (b.height - icon) / 2.0,
			icon, theme::text, scale);
		return;
	}

	if (s.kind == "clock") {
		const bool open = server_->calendar && server_->calendar->is_open_on(output_);
		fx_.set("open:clock", open ? 1.0 : 0.0);
		const double o = fx_.get("open:clock");
		if (o > 0.01) {
			fill_rounded(cr, b.x, b.y, b.width, b.height, r,
				with_alpha(theme::panel_active, theme::panel_active.a * o));
		}
		hover_bg(hover * (1.0 - o));
		const std::string time = clock_text(cfg.clock_24h);
		if (vertical) {
			// "21" over "34"
			const auto colon = time.find(':');
			const std::string hours = time.substr(0, colon);
			std::string minutes = colon == std::string::npos ? "" : time.substr(colon + 1, 2);
			const double mid = b.y + (cfg.show_date ? b.height / 2.0 - 13 : b.height / 2.0);
			draw_text(cr, hours, b.x, mid - 17, b.width, 18, theme::font_title, theme::text,
				Align::Center);
			draw_text(cr, minutes, b.x, mid - 1, b.width, 18, theme::font_title, theme::text,
				Align::Center);
			if (cfg.show_date) {
				// "Fri" over "30 Oct", like "Fri 30 Oct" on a horizontal panel.
				std::time_t now = std::time(nullptr);
				std::tm local{};
				localtime_r(&now, &local);
				char day[16], date[16];
				std::strftime(day, sizeof day, "%a", &local);
				std::strftime(date, sizeof date, "%d %b", &local);
				draw_text(cr, day, b.x, mid + 17, b.width, 13, theme::font_small, theme::text_dim, Align::Center);
				draw_text(cr, date, b.x, mid + 30, b.width, 13, theme::font_small, theme::text_dim, Align::Center);
			}
		} else if ((plook == "gnome" || plook == "macos") && cfg.show_date) {
			draw_text(cr, one_line_clock(cfg.clock_24h), b.x, b.y, b.width, b.height, theme::font_bold, theme::text,
				Align::Center);
		} else if (plook == "windows" && cfg.show_date && b.height >= 28) {
			// Windows 11: the time over the date, to the right.
			draw_text(cr, time, b.x + 4, b.y + b.height / 2.0 - 18, b.width - 12, 18, theme::font_small,
				theme::text, Align::Right);
			draw_text(cr, numeric_date(), b.x + 4, b.y + b.height / 2.0 + 1, b.width - 12, 18,
				theme::font_small, theme::text, Align::Right);
		} else if (cfg.show_date && b.height >= 28) {
			draw_text(cr, time, b.x, b.y + b.height / 2.0 - 17, b.width, 20, theme::font_title,
				theme::text, Align::Center);
			draw_text(cr, date_text(), b.x, b.y + b.height / 2.0 + 1, b.width, 16,
				theme::font_small, theme::text_dim, Align::Center);
		} else {
			draw_text(cr, time, b.x, b.y, b.width, b.height, theme::font_title, theme::text,
				Align::Center);
		}
		return;
	}

	if (s.kind == "status") {
		const bool open = server_->control_centre && server_->control_centre->tiles_open_on(output_);
		fx_.set("open:status", open ? 1.0 : 0.0);
		const double o = fx_.get("open:status");
		if (o > 0.01) {
			fill_rounded(cr, b.x, b.y, b.width, b.height, r,
				with_alpha(theme::panel_active, theme::panel_active.a * o));
		}
		hover_bg(hover * (1.0 - o));
		const std::vector<std::string> icons = status_icons();
		const double icon = 16;
		double p = (vertical ? b.y : b.x) + 8;
		for (const std::string &name : icons) {
			const bool battery = name.rfind("battery", 0) == 0;
			if (battery && vertical && cfg.battery_style != "icon" && server_->status) {
				// Standing panel: the icon, and the number under it (any style).
				const double ix = b.x + (b.width - icon) / 2.0, iy = p + (kStatusIcon - icon) / 2;
				draw_named_icon(cr, server_->icons, name, ix, iy, icon, theme::text, scale);
				const int level = server_->status->battery;
				const Color col = server_->status->charging || level >= 50 ? rgb(theme::dark ? 0x57d36b : 0x1f9d3a)
					: level >= 20 ? theme::warning : theme::danger;
				draw_text(cr, std::to_string(level) + "%", b.x, p + kStatusIcon - 3, b.width, 14, theme::font_small, col,
					Align::Center);
				p += kStatusIcon + 14;
				continue;
			}
			if (battery && !vertical && cfg.battery_style == "ring" && server_->status) {
				draw_battery_ring(cr, p + 2, b.y + (b.height - 18) / 2.0, 34, 18);
				p += 40;
				continue;
			}
			const double ix = vertical ? b.x + (b.width - icon) / 2.0 : p + (kStatusIcon - icon) / 2;
			const double iy = vertical ? p + (kStatusIcon - icon) / 2 : b.y + (b.height - icon) / 2.0;
			if (name == "rec") {
				// Recording: a red pill with the time (click it to stop).
				const Color red = theme::danger;
				if (vertical) {
					fill_rounded(cr, ix - 3, iy - 3, icon + 6, icon + 6, (icon + 6) / 2.0, red);
					fill_rounded(cr, ix + 4, iy + 4, icon - 8, icon - 8, 1.5, rgb(0xffffff));
					p += kStatusIcon;
					continue;
				}
				const int rw = rec_width(server_);
				const double ph = std::min<double>(b.height - 6, 22);
				const double py = b.y + (b.height - ph) / 2;
				fill_rounded(cr, p, py, rw - 4, ph, ph / 2, red);
				const double blink = server_->recorder->starting() ? 0.5 : 1.0;
				cairo_new_path(cr);
				cairo_arc(cr, p + 12, py + ph / 2, 4, 0, 2 * M_PI);
				set_color(cr, rgb(0xffffff, blink));
				cairo_fill(cr);
				draw_text(cr, rec_text(server_), p + 20, py, rw - 28, ph, theme::font_bold, rgb(0xffffff));
				p += rw;
				continue;
			}
			if (name == "unseen:bell") {
				// Small, sitting high, in the accent's opposite colour (the other
				// side of the colour wheel) so it stands out from everything
				// accent-coloured round it.
				const Color vivid = opposite_colour(theme::accent, theme::dark);
				const double bs = 9;
				const double bx = vertical ? b.x + (b.width - bs) / 2.0 : p + (kBellSlot - bs) / 2.0;
				const double by = vertical ? p + (kBellSlot - bs) / 2.0 : b.y + b.height / 2.0 - bs - 1;
				draw_named_icon(cr, server_->icons, "bell", bx, by, bs, vivid, scale);
				p += kBellSlot;
				continue;
			}
			if (name.rfind("privacy:", 0) == 0) {
				// In use: an amber pill with the icon on it.
				const Color amber = rgb(theme::dark ? 0xf5a524 : 0xd97706);
				fill_rounded(cr, ix - 3, iy - 3, icon + 6, icon + 6, (icon + 6) / 2.0, amber);
				draw_named_icon(cr, server_->icons, name.substr(8), ix + 1.5, iy + 1.5, icon - 3, rgb(0x1a1206), scale);
				p += kStatusIcon;
				continue;
			}
			draw_named_icon(cr, server_->icons, name, ix, iy, icon, theme::text, scale);
			p += kStatusIcon;
			if (!vertical && battery && cfg.battery_style == "percent" && server_->status) {
				// The number in a colour for how full it is: green, amber, red
				// (green while charging).
				const int level = server_->status->battery;
				const std::string pct = std::to_string(level) + "%";
				const int tw = text_width(pct, theme::font_bold);
				const Color col = server_->status->charging || level >= 50 ? rgb(theme::dark ? 0x57d36b : 0x1f9d3a)
					: level >= 20 ? theme::warning : theme::danger;
				draw_text(cr, pct, p, b.y, tw + 4, b.height, theme::font_bold, col);
				p += tw + 4;
			}
		}
		return;
	}

	if (s.kind == "media") {
		// The now-playing chip: an accent pill with play/pause and the title.
		const SystemStatus *st = server_->status.get();
		if (!st || b.width < 8) {
			return;
		}
		const bool open = server_->media_card && server_->media_card->is_open_on(output_);
		const double ch = std::min<double>(b.height, 28);
		const double cy = b.y + (b.height - ch) / 2.0;
		cairo_save(cr);
		rounded_rect(cr, b.x, cy, b.width, ch, ch / 2);
		cairo_clip(cr);
		const double k = 1.0 - 0.12 * std::max(hover, open ? 1.0 : 0.0);
		fill_rounded(cr, b.x, cy, b.width, ch, ch / 2,
			Color{theme::accent.r * k, theme::accent.g * k, theme::accent.b * k, 1.0});
		draw_named_icon(cr, server_->icons, st->media_playing ? "pause" : "play",
			b.x + 9, cy + ch / 2 - 7, 14, theme::on_accent, scale);
		if (!vertical) {
			draw_text(cr, media_label(), b.x + 30, cy, b.width - 40, ch, theme::font_small,
				theme::on_accent);
		}
		cairo_restore(cr);
		return;
	}
}

// The corners of a button on the bar: following the bar's own curve
// (Dusk), nearly square (Windows 11), pills (GNOME), small (macOS's menu bar).
double Panel::corner(const wlr_box &b) const {
	const std::string &look = server_->config.panel_look;
	const double half = std::min(b.width, b.height) / 2.0;
	if (look == "windows") return std::min(5.0, half);
	if (look == "gnome") return half;
	if (look == "macos" && config_.style != "dock") return std::min(6.0, half);
	const double bar_r = config_.style == "full" ? 14.0 : std::min(bar_.width, bar_.height) / 2.0;
	return std::min(inner_radius(bar_r, 5), half);
}

double Panel::slot_radius(const Slot &s) const {
	const wlr_box &b = s.box;
	const std::string &look = server_->config.panel_look;
	if (s.kind == "menu" && look != "windows" && look != "macos") {
		return std::min(b.width, b.height) / 2.0;
	}
	if (s.kind == "media") {
		return std::min<double>(b.height, 28) / 2.0;
	}
	return corner(b);
}

void Panel::update_highlight() {
	if (hover_ < 0 || hover_ >= static_cast<int>(slots_.size())) {
		hl_.hide();
		return;
	}
	const Slot &s = slots_[static_cast<size_t>(hover_)];
	if (s.kind == "separator" || s.kind == "spacer") {
		hl_.hide();
		return;
	}
	wlr_box b = s.box;
	if (s.kind == "media") {
		const int ch = std::min(b.height, 28);
		b = wlr_box{b.x, b.y + (b.height - ch) / 2, b.width, ch};
	}
	const bool black = server_->config.panel_look == "gnome" && config_.edge == "top";
	hl_.show(b, slot_radius(s), black ? rgb(0xffffff, 0.14) : with_alpha(theme::text, theme::dark ? 0.09 : 0.07));
}

int Panel::slot_at(double px, double py) const {
	for (size_t i = 0; i < slots_.size(); i++) {
		const wlr_box &b = slots_[i].box;
		if (px >= b.x && px < b.x + b.width && py >= b.y && py < b.y + b.height) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

bool Panel::magnifies() const {
	return server_->config.dock_magnify && config_.style == "dock" && server_->config.panel_look == "macos";
}

// Icons near the pointer grow (a bell curve about two icons wide).
double Panel::magnify_at(const wlr_box &b) const {
	if (mag_ <= 0.001) return 0;
	const bool vert = config_.vertical();
	const double c = vert ? b.y + b.height / 2.0 : b.x + b.width / 2.0;
	const double size = vert ? b.height : b.width;
	const double d = ((vert ? mag_y_ : mag_x_) - c) / std::max(1.0, size * 1.5);
	return mag_ * std::exp(-d * d);
}

void Panel::queue_magnify() {
	if (mag_idle_) return;
	mag_idle_ = wl_event_loop_add_idle(server_->loop, [](void *data) {
		auto *p = static_cast<Panel *>(data);
		p->mag_idle_ = nullptr;
		p->redraw();
	}, this);
}

void Panel::motion(double lx, double ly) {
	const wlr_box bar = bar_box();
	if (magnifies()) {
		mag_x_ = lx - (bar.x - bar_.x);
		mag_y_ = ly - (bar.y - bar_.y);
		if (mag_ < 1.0 && !server_->anim->running(mag_anim_)) {
			mag_anim_ = server_->anim->tween(mag_, 1.0, server_->config.ms(140), [this] { queue_magnify(); },
				[this] { mag_anim_ = 0; });
		}
		queue_magnify();
	}
	const int now = slot_at(lx - (bar.x - bar_.x), ly - (bar.y - bar_.y));
	if (now != hover_) {
		hover_ = now;
		update_highlight();   // no redraw: the highlight moves by itself
	}
}

void Panel::leave() {
	if (mag_ > 0 || mag_anim_) {
		server_->anim->cancel(mag_anim_);
		mag_anim_ = server_->anim->tween(mag_, 0.0, server_->config.ms(200), [this] { queue_magnify(); },
			[this] { mag_anim_ = 0; });
	}
	if (hover_ != -1) {
		hover_ = -1;
		hl_.hide();
	}
}

void Panel::click(double lx, double ly, uint32_t button) {
	const wlr_box bar = bar_box();
	const int index = slot_at(lx - (bar.x - bar_.x), ly - (bar.y - bar_.y));
	const Slot *s = index >= 0 ? &slots_[static_cast<size_t>(index)] : nullptr;
	if (s && s->kind == "tray" && server_->tray) {
		// An app's status icon: its window (left), its menu (right).
		const wlr_box anchor{bar.x - bar_.x + s->box.x, bar.y - bar_.y + s->box.y, s->box.width, s->box.height};
		const std::string key = s->tray;
		server_->close_popups();
		hl_.flash();
		if (button == BTN_RIGHT) {
			server_->tray->context_menu(key, output_, anchor, config_.edge);
		} else if (button == BTN_MIDDLE) {
			server_->tray->secondary(key, static_cast<int>(lx), static_cast<int>(ly));
		} else {
			server_->tray->activate(key, static_cast<int>(lx), static_cast<int>(ly));
		}
		return;
	}
	if (button == BTN_RIGHT) {
		// Right-click: a menu for what's under the pointer and the panel.
		show_menu(s, lx, ly);
		return;
	}
	const std::string kind = s ? s->kind : "";
	if (s && s->kind == "spacer") {
		s = nullptr;   // empty bar
	}
	if (s) {
		hl_.flash();
	}
	const wlr_box anchor_box = s ? wlr_box{bar.x - bar_.x + s->box.x, bar.y - bar_.y + s->box.y,
		s->box.width, s->box.height} : bar;

	if (kind == "overview") {
		server_->close_popups();
		if (server_->overview) server_->overview->toggle(output_);
		return;
	}
	if (kind == "trash") {
		server_->close_popups();
		if (server_->trash) server_->trash->open();
		return;
	}
	if (kind == "menu") {
		const bool was = server_->launcher->is_open_on(output_);
		server_->close_popups();
		if (!was) {
			server_->launcher->open(output_, anchor_box, config_.edge);
		}
		return;
	}
	if (kind == "notifications" || (kind == "clock" && server_->config.clock_click == "notifications")) {
		const bool was = server_->control_centre->notes_open_on(output_);
		server_->close_popups();
		if (!was) server_->control_centre->open_notes(output_, anchor_box, config_.edge);
		return;
	}
	if (kind == "clock" && server_->config.clock_click == "calendar") {
		const bool was = server_->calendar->is_open_on(output_);
		server_->close_popups();
		if (!was) {
			server_->calendar->open(output_, anchor_box, config_.edge);
		}
		return;
	}
	if (kind == "clipboard") {
		const bool was = server_->clipboard_card->is_open_on(output_);
		server_->close_popups();
		if (!was) {
			server_->clipboard_card->open(output_, anchor_box, config_.edge);
		}
		return;
	}
	if (kind == "media") {
		const bool was = server_->media_card->is_open_on(output_);
		server_->close_popups();
		if (!was) {
			server_->media_card->open(output_, anchor_box, config_.edge);
		}
		return;
	}
	if (kind == "status" && recording(server_) && !config_.vertical()) {
		// The recording pill comes first in the status area: clicking it stops.
		const double sx = bar.x - bar_.x + s->box.x + 8;
		if (lx >= sx && lx < sx + rec_width(server_)) {
			server_->recorder->stop();
			return;
		}
	}
	if (kind == "status" || kind == "clock") {
		const bool was = server_->control_centre->is_open_on(output_);
		server_->close_popups();
		if (!was) {
			server_->control_centre->open(output_, anchor_box, config_.edge);
		}
		return;
	}
	server_->close_popups();
	if (kind == "showdesktop") {
		server_->toggle_show_desktop();
		return;
	}
	if (kind == "keyboard") {
		if (server_->osk) server_->osk->toggle(output_);
		redraw();
		return;
	}
	if (kind == "pins") {
		if (button != BTN_LEFT) {
			return;
		}
		View *running = pin_window(s->pin);
		if (running) {
			if (running->minimized) {
				running->restore_from_minimize();
			} else {
				server_->focus(running);
			}
		} else if (const AppInfo *app = pin_app(s->pin)) {
			server_->spawn(app->exec);
		}
		return;
	}
	if (kind == "tasks" && s->view) {
		View *view = s->view;
		if (button == BTN_MIDDLE) {
			view->close();
			return;
		}
		if (button != BTN_LEFT) {
			return;
		}
		if (server_->overview && server_->overview->is_open()) {
			server_->overview->close(view);   // out of the overview, to that window
			return;
		}
		if (view->minimized) {
			view->restore_from_minimize();
		} else if (view->active()) {
			view->minimize();
		} else {
			server_->focus(view);
		}
	}
}

// The overview button: GNOME's workspace dots (the one shown is a pill),
// or an icon of your choice.
void Panel::draw_overview_button(cairo_t *cr, const wlr_box &b, double hover, float scale) {
	const Config &cfg = server_->config;
	const bool vertical = config_.vertical();
	const bool open = server_->overview && server_->overview->is_open();
	fx_.set("open:overview", open ? 1.0 : 0.0);
	const double o = fx_.get("open:overview");
	const double r = corner(b);
	if (o > 0.01) {
		fill_rounded(cr, b.x, b.y, b.width, b.height, r, with_alpha(theme::panel_active, theme::panel_active.a * o));
	}
	if (hover * (1.0 - o) > 0.01) {
		fill_rounded(cr, b.x, b.y, b.width, b.height, r,
			with_alpha(theme::panel_hover, theme::panel_hover.a * hover * (1.0 - o)));
	}
	const double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
	const std::string &look = cfg.overview_icon;
	if (look == "dots") {
		const int n = std::clamp(server_->workspace_count, 1, 12);
		const int cur = std::clamp(server_->workspace, 0, n - 1);
		// The pill glides to the workspace shown.
		fx_.set("ws:pill", cur, cur);
		const double at = fx_.get("ws:pill", cur);
		const double dot = 7, pill = 20, gap = 6;
		const double total = pill + (n - 1) * (dot + gap);
		double p = (vertical ? cy : cx) - total / 2;
		for (int i = 0; i < n; i++) {
			const double grow = std::max(0.0, 1.0 - std::fabs(at - i));   // 1 at the current one
			const double len = dot + (pill - dot) * grow;
			// The one you're on in the accent colour, like the rest of Dusk's controls.
			const Color col = mix(with_alpha(theme::text, 0.32), theme::accent, grow);
			if (vertical) {
				fill_rounded(cr, cx - dot / 2, p, dot, len, dot / 2, col);
			} else {
				fill_rounded(cr, p, cy - dot / 2, len, dot, dot / 2, col);
			}
			p += len + gap;
		}
		return;
	}
	if (look == "led") {
		// The cockpit gauge, the OSD's segments: one per workspace, the one
		// you are on lit in the accent, the ones with windows a little
		// brighter than the empty ones. Scroll here to move between
		// workspaces, click for the overview.
		const int n = std::clamp(server_->workspace_count, 1, 12);
		const int cur = std::clamp(server_->workspace, 0, n - 1);
		bool occupied[12] = {};
		for (const View *v : server_->views) {
			if (!v->mapped) continue;
			if (v->sticky) {
				std::fill(occupied, occupied + 12, true);
			} else if (v->workspace >= 0 && v->workspace < 12) {
				occupied[v->workspace] = true;
			}
		}
		auto colour = [&](int i) {
			if (i == cur) {
				return theme::accent;
			}
			return occupied[i] ? mix(with_alpha(theme::text, 0.55), theme::accent, 0.25)
				: with_alpha(theme::text, theme::dark ? 0.15 : 0.11);
		};
		Color seg[12];
		for (int i = 0; i < n; i++) {
			// Upright panels stack the segments bottom to top (the OSD's way).
			seg[vertical ? n - 1 - i : i] = colour(i);
		}
		const double total = n * 10.0 + (n - 1) * 5;
		led_segments(cr, vertical ? cx - 5.0 : cx - total / 2, vertical ? cy - total / 2 : cy - 5.0,
			total, 10, vertical, n, cur + 1, theme::accent, false, seg);
		return;
	}
	// More ways to show the workspaces: which one you're on, and how many
	// there are.
	if (look == "numbers" || look == "roman" || look == "letters" || look == "lines" || look == "rings" ||
			look == "boxes" || look == "count") {
		const int n = std::clamp(server_->workspace_count, 1, 12);
		const int cur = std::clamp(server_->workspace, 0, n - 1);
		fx_.set("ws:pill", cur, cur);
		const double at = fx_.get("ws:pill", cur);
		auto label = [&](int i) -> std::string {
			static const char *kRoman[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII"};
			if (look == "roman") return kRoman[i];
			if (look == "letters") return std::string(1, static_cast<char>('A' + i));
			return std::to_string(i + 1);
		};
		if (look == "count") {
			// "2 / 4": the one you're on, of how many.
			if (vertical) {
				draw_text(cr, std::to_string(cur + 1), b.x, cy - 17, b.width, 18, theme::font_bold, theme::accent,
					Align::Center);
				cairo_set_source_rgba(cr, theme::text.r, theme::text.g, theme::text.b, 0.35);
				cairo_rectangle(cr, cx - 6, cy, 12, 1.5);
				cairo_fill(cr);
				draw_text(cr, std::to_string(n), b.x, cy + 2, b.width, 16, theme::font_small, theme::text_dim,
					Align::Center);
			} else {
				// The current one in the accent colour, the count quieter.
				const std::string a = std::to_string(cur + 1), rest = " / " + std::to_string(n);
				const int aw = text_width(a, theme::font_bold), rw = text_width(rest, theme::font);
				const double x0 = cx - (aw + rw) / 2.0;
				draw_text(cr, a, x0, b.y, aw + 2, b.height, theme::font_bold, theme::accent);
				draw_text(cr, rest, x0 + aw, b.y, rw + 4, b.height, theme::font, theme::text_dim);
			}
			return;
		}
		const bool labelled = look == "numbers" || look == "roman" || look == "letters";
		// Along the panel: the size of each mark, the current one's, and the gap.
		double mark, wide, gap, across;
		if (labelled) {
			mark = look == "roman" && !vertical ? 22 : 18;
			wide = mark + 12;
			gap = 4;
			across = vertical && look == "roman" ? 28 : 18;   // "VIII" across an upright panel
		} else if (look == "lines") {
			mark = 10; wide = 22; gap = 5; across = 4;
		} else {
			mark = 9; wide = 9; gap = 6; across = 9;
		}
		double total = 0;
		for (int i = 0; i < n; i++) {
			const double grow = std::max(0.0, 1.0 - std::fabs(at - i));
			total += mark + (wide - mark) * grow + (i ? gap : 0);
		}
		double p = (vertical ? cy : cx) - total / 2;
		for (int i = 0; i < n; i++) {
			const double grow = std::max(0.0, 1.0 - std::fabs(at - i));   // 1 at the current one
			const double len = mark + (wide - mark) * grow;
			const double x = vertical ? cx - across / 2 : p, y = vertical ? p : cy - across / 2;
			const double w = vertical ? across : len, h = vertical ? len : across;
			if (labelled) {
				// The current one filled, the others a faint chip; the label on top.
				const Color fill = mix(with_alpha(theme::text, 0.08), theme::accent, grow);
				fill_rounded(cr, x, y, w, h, std::min(w, h) / 2, fill);
				const Color fg = mix(theme::text_dim, theme::on_accent, grow);
				draw_text(cr, label(i), x, y, w, h, look == "roman" ? theme::font_small : theme::font_bold, fg,
					Align::Center);
			} else if (look == "lines") {
				fill_rounded(cr, x, y, w, h, across / 2, mix(with_alpha(theme::text, 0.32), theme::accent, grow));
			} else if (look == "rings") {
				cairo_new_path(cr);
				cairo_arc(cr, x + w / 2, y + h / 2, mark / 2 - 0.75, 0, 2 * M_PI);
				set_color(cr, with_alpha(theme::accent, grow));
				cairo_fill_preserve(cr);
				set_color(cr, mix(with_alpha(theme::text, 0.45), theme::accent, grow));
				cairo_set_line_width(cr, 1.5);
				cairo_stroke(cr);
			} else {   // boxes
				rounded_rect(cr, x + 0.75, y + 0.75, w - 1.5, h - 1.5, 2.5);
				set_color(cr, with_alpha(theme::accent, grow));
				cairo_fill_preserve(cr);
				set_color(cr, mix(with_alpha(theme::text, 0.45), theme::accent, grow));
				cairo_set_line_width(cr, 1.5);
				cairo_stroke(cr);
			}
			p += len + gap;
		}
		return;
	}
	if (look == "distro" || look == "custom") {
		const std::string name = look == "custom" ? cfg.overview_icon_custom : "";
		const double size = std::round(std::min(b.width, b.height) * 0.62);
		draw_icon(cr, server_->icons, name.empty() ? distro_logo() : name, "Overview", cx - size / 2, cy - size / 2,
			size, scale);
		return;
	}
}

bool Panel::scroll(double lx, double ly, double delta) {
	const wlr_box bar = bar_box();
	const int index = slot_at(lx - (bar.x - bar_.x), ly - (bar.y - bar_.y));
	if (index < 0) return false;
	const std::string &kind = slots_[static_cast<size_t>(index)].kind;
	if (kind == "status") {
		// The status icons (the speaker among them): the wheel sets the
		// volume, a notch a step, with the on-screen display.
		scroll_accum_ += delta;
		while (std::fabs(scroll_accum_) >= 12) {
			server_->volume_key(scroll_accum_ > 0 ? "down" : "up", false);
			scroll_accum_ += scroll_accum_ > 0 ? -15 : 15;
			if (std::fabs(scroll_accum_) < 3) scroll_accum_ = 0;
		}
		return true;
	}
	if (kind != "overview") {
		return false;
	}
	scroll_accum_ += delta;
	if (std::fabs(scroll_accum_) >= 12) {
		server_->switch_workspace(server_->workspace + (scroll_accum_ > 0 ? 1 : -1));
		scroll_accum_ = 0;
	}
	return true;
}

// ---- right-click menus --------------------------------------------------------

std::function<Panel *()> Panel::finder() const {
	Server *srv = server_;
	Output *out = output_;
	const std::string id = config_.id;
	return [srv, out, id]() -> Panel * {
		for (Output *o : srv->outputs) {
			if (o != out) continue;
			for (auto &p : o->panels) {
				if (p->config().id == id) return p.get();
			}
		}
		return nullptr;
	};
}

namespace {

std::string join_list(const std::vector<std::string> &v) {
	std::string out;
	for (const std::string &x : v) {
		out += (out.empty() ? "" : ",") + x;
	}
	return out;
}

std::string long_date() {
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char buf[64];
	std::strftime(buf, sizeof buf, "%A %-d %B %Y", &local);
	return buf;
}

} // namespace

// A widget's own options (and a way to the panel's).
void Panel::show_menu(const Slot *slot, double lx, double ly) {
	if (!slot || slot->kind == "spacer" || slot->kind == "separator") {
		show_panel_menu(slot, lx, ly);
		return;
	}
	std::vector<MenuItem> items;
	add_widget_items(*slot, items);
	items.push_back(MenuItem::separator());
	auto find = finder();
	items.push_back(MenuItem::action("Panel options", [find, lx, ly] {
		if (Panel *p = find()) p->show_panel_menu(nullptr, lx, ly);
	}, "gear"));
	server_->close_popups();
	server_->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1},
		config_.edge, std::move(items));
}

void Panel::add_widget_items(const Slot &slot, std::vector<MenuItem> &items) {
	Server *srv = server_;
	const Config &cfg = srv->config;
	const std::string pre = "panel." + config_.id + ".";
	const std::vector<std::string> widgets = config_.widgets;
	auto find = finder();
	auto set = [srv](std::string key, std::string value) {
		return [srv, key, value] { srv->write_setting(key, value); };
	};
	// Takes this widget off the panel.
	auto remove_item = [&] {
		const int wi = slot.widget;
		MenuItem m = MenuItem::action("Remove from panel", [srv, pre, widgets, wi] {
			std::vector<std::string> w = widgets;
			if (wi >= 0 && wi < static_cast<int>(w.size())) {
				w.erase(w.begin() + wi);
			}
			srv->write_setting(pre + "widgets", join_list(w));
		}, "minus");
		return m;
	};
	const std::string &kind = slot.kind;

	if (kind == "tasks" && slot.view) {
		View *v = slot.view;
		auto alive = [srv, v] {
			return std::find(srv->views.begin(), srv->views.end(), v) != srv->views.end();
		};
		items.push_back(MenuItem::header(v->title().empty() ? "Window" : v->title()));
		if (v->minimized) {
			items.push_back(MenuItem::action("Restore", [v, alive] { if (alive()) v->restore_from_minimize(); }));
		} else {
			items.push_back(MenuItem::action("Minimise", [v, alive] { if (alive()) v->minimize(); }));
		}
		items.push_back(MenuItem::action(v->maximized ? "Restore size" : "Maximise",
			[v, alive] { if (alive()) v->toggle_maximize(); }));
		int screens = 0;
		for (Output *o : srv->outputs) screens += o->enabled();
		if (screens > 1) {
			items.push_back(MenuItem::action("Move to the next screen",
				[srv, v, alive] { if (alive()) srv->move_to_next_screen(v, 1); }, "display"));
		}
		// Pin its app to this panel (adding pinned apps to it if needed).
		const std::string icon = srv->apps.icon_for(v->app_id(), v->title());
		const AppInfo *app = nullptr;
		for (const AppInfo &a : srv->apps.apps()) {
			if (!icon.empty() && a.icon == icon) {
				app = &a;
				break;
			}
		}
		if (app) {
			const std::string app_id = app->id;
			const bool pinned = std::find(config_.pins.begin(), config_.pins.end(), app_id) != config_.pins.end();
			std::vector<std::string> pins = config_.pins;
			if (pinned) {
				pins.erase(std::remove(pins.begin(), pins.end(), app_id), pins.end());
			} else {
				pins.push_back(app_id);
			}
			std::vector<std::string> w = widgets;   // (pinned apps show with the open ones)
			items.push_back(MenuItem::action(pinned ? "Unpin from panel" : "Pin to panel",
				[srv, pre, pins, w, widgets] {
					if (w != widgets) Config::write_key(pre + "widgets", join_list(w));
					srv->write_setting(pre + "pins", join_list(pins));
				}, "plus"));
		}
		MenuItem close = MenuItem::action("Close", [v, alive] { if (alive()) v->close(); }, "close");
		close.danger = true;
		items.push_back(MenuItem::separator());
		items.push_back(close);
		return;
	}

	if (kind == "pins" && slot.pin >= 0) {
		const AppInfo *app = pin_app(slot.pin);
		const std::string exec = app ? app->exec : "";
		std::vector<std::string> pins = config_.pins;
		const size_t i = static_cast<size_t>(slot.pin);
		if (app) {
			items.push_back(MenuItem::header(app->name));
			items.push_back(MenuItem::action(pin_window(slot.pin) ? "New window" : "Open",
				[srv, exec] { srv->spawn(exec); }, "open"));
		}
		if (i > 0 && i < pins.size()) {
			std::vector<std::string> moved = pins;
			std::swap(moved[i], moved[i - 1]);
			items.push_back(MenuItem::action(config_.vertical() ? "Move up" : "Move left",
				[srv, pre, moved] { srv->write_setting(pre + "pins", join_list(moved)); }));
		}
		if (i + 1 < pins.size()) {
			std::vector<std::string> moved = pins;
			std::swap(moved[i], moved[i + 1]);
			items.push_back(MenuItem::action(config_.vertical() ? "Move down" : "Move right",
				[srv, pre, moved] { srv->write_setting(pre + "pins", join_list(moved)); }));
		}
		if (i < pins.size()) {
			pins.erase(pins.begin() + slot.pin);
		}
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::action("Unpin from panel",
			[srv, pre, pins] { srv->write_setting(pre + "pins", join_list(pins)); }, "minus"));
		return;
	}

	if (kind == "menu") {
		items.push_back(MenuItem::action("Open the app menu", [srv, find] {
			if (Panel *p = find()) srv->launcher->open(p->output(), p->anchor("menu"), p->config().edge);
		}, "apps"));
		items.push_back(MenuItem::action("Terminal", [srv] { srv->spawn_terminal(); }, "terminal"));
		items.push_back(MenuItem::action("Files", [srv] { srv->spawn("xdg-open \"$HOME\""); }, "folder"));
		items.push_back(MenuItem::action("Settings", [srv] { srv->open_settings(); }, "gear"));
		items.push_back(MenuItem::separator());
		items.push_back(remove_item());
		return;
	}

	if (kind == "clock") {
		items.push_back(MenuItem::header(long_date()));
		items.push_back(MenuItem::action("Calendar", [srv, find] {
			if (Panel *p = find()) srv->calendar->open(p->output(), p->anchor("clock"), p->config().edge);
		}));
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::check("24-hour clock", cfg.clock_24h,
			set("clock_24h", cfg.clock_24h ? "false" : "true")));
		items.push_back(MenuItem::check("Show the date", cfg.show_date,
			set("show_date", cfg.show_date ? "false" : "true")));
		items.push_back(MenuItem::header("Clicking the clock opens"));
		items.push_back(MenuItem::radio("The calendar", cfg.clock_click == "calendar",
			set("clock_click", "calendar")));
		items.push_back(MenuItem::radio("The notifications", cfg.clock_click == "notifications",
			set("clock_click", "notifications")));
		items.push_back(MenuItem::radio("The control centre", cfg.clock_click == "controlcentre",
			set("clock_click", "controlcentre")));
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::action("Date & time settings…", [srv] { srv->open_settings("datetime"); }));
		items.push_back(remove_item());
		return;
	}

	if (kind == "status") {
		const SystemStatus *st = srv->status.get();
		items.push_back(MenuItem::action("Control centre", [srv, find] {
			if (Panel *p = find()) srv->control_centre->open(p->output(), p->anchor("status"), p->config().edge);
		}, "gear"));
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::check("Do not disturb", cfg.dnd, set("dnd", cfg.dnd ? "false" : "true")));
		if (st && st->has_audio) {
			items.push_back(MenuItem::check("Mute sound", st->muted, [srv] { srv->status->toggle_mute(); }));
			items.push_back(MenuItem::action("Play sound on…", [srv, find] {
				if (Panel *p = find()) {
					const wlr_box a = p->anchor("status");
					sound_output_menu(srv, p->output(), a.x + a.width / 2.0, a.y + a.height / 2.0);
				}
			}, "speaker"));
		}
		items.push_back(MenuItem::check("Night light", cfg.night_light,
			set("night_light", cfg.night_light ? "false" : "true")));
		items.push_back(MenuItem::check("Dark mode", cfg.dark, set("theme", cfg.dark ? "light" : "dark")));
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::action("Control centre settings…",
			[srv] { srv->open_settings("controlcentre"); }));
		items.push_back(remove_item());
		return;
	}

	if (kind == "media") {
		const SystemStatus *st = srv->status.get();
		if (st && !st->media_title.empty()) {
			items.push_back(MenuItem::header(st->media_title));
		}
		items.push_back(MenuItem::action(st && st->media_playing ? "Pause" : "Play",
			[srv] { srv->status->media("play-pause"); }, st && st->media_playing ? "pause" : "play"));
		items.push_back(MenuItem::action("Next", [srv] { srv->status->media("next"); }, "next"));
		items.push_back(MenuItem::action("Previous", [srv] { srv->status->media("previous"); }, "previous"));
		items.push_back(MenuItem::action("Show the player", [srv, find] {
			if (Panel *p = find()) srv->media_card->open(p->output(), p->anchor("media"), p->config().edge);
		}));
		items.push_back(MenuItem::separator());
		// The same as the media card's own menu: lyrics, the visualizer, settings.
		media_menu_items(srv, output_, anchor("media"), config_.edge, items);
		items.push_back(MenuItem::separator());
		items.push_back(remove_item());
		return;
	}

	if (kind == "clipboard") {
		const bool any = srv->clipboard && !srv->clipboard->history().empty();
		items.push_back(MenuItem::action("Clipboard history", [srv, find] {
			srv->clipboard_card->show_tab(0);
			if (Panel *p = find()) srv->clipboard_card->open(p->output(), p->anchor("clipboard"), p->config().edge);
		}, "clipboard"));
		MenuItem clear = MenuItem::action("Clear history", [srv] {
			if (srv->clipboard) srv->clipboard->clear();
		}, "trash");
		clear.enabled = any;
		items.push_back(clear);
		items.push_back(MenuItem::check("Keep a history", cfg.clipboard_history,
			set("clipboard_history", cfg.clipboard_history ? "false" : "true")));
		items.push_back(MenuItem::check("Middle-click pastes", cfg.middle_click_paste,
			set("middle_click_paste", cfg.middle_click_paste ? "false" : "true")));
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::action("Clipboard settings…", [srv] { srv->open_settings("desktop"); }));
		items.push_back(remove_item());
		return;
	}

	if (kind == "overview") {
		Output *out = output_;
		items.push_back(MenuItem::action("Overview", [srv, out] { if (srv->overview) srv->overview->toggle(out); }, "layout-grid"));
		items.push_back(MenuItem::header("Workspaces"));
		for (int i = 0; i < srv->workspace_count; i++) {
			const bool empty = !srv->workspace_used(i);
			items.push_back(MenuItem::radio("Workspace " + std::to_string(i + 1) + (empty ? "  (empty)" : ""),
				i == srv->workspace, [srv, i] { srv->switch_workspace(i); }));
		}
		if (srv->workspace_count < 12) {
			items.push_back(MenuItem::action("New workspace", [srv] { srv->add_workspace(); }, "plus"));
		}
		if (srv->workspace_count > 1) {
			const int cur = srv->workspace;
			items.push_back(MenuItem::action("Remove this workspace", [srv, cur] { srv->remove_workspace(cur); }, "minus"));
		}
		items.push_back(MenuItem::header("Button"));
		for (const auto &[label, value] : std::vector<std::pair<std::string, std::string>>{
				{"Dots", "dots"}, {"Numbers", "numbers"}, {"Roman numerals", "roman"}, {"Letters", "letters"},
				{"Short lines", "lines"}, {"Rings", "rings"}, {"Small squares", "boxes"}, {"2 / 4", "count"},
				{"Distribution logo", "distro"}}) {
			items.push_back(MenuItem::radio(label, cfg.overview_icon == value, set("overview_icon", value)));
		}
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::action("Workspace settings…", [srv] { srv->open_settings("workspaces"); }, "gear"));
		items.push_back(remove_item());
		return;
	}

	if (kind == "trash") {
		if (srv->trash) {
			const wlr_box bar = bar_box();
			const wlr_box anchor{bar.x - bar_.x + slot.box.x, bar.y - bar_.y + slot.box.y, slot.box.width, slot.box.height};
			srv->trash->add_menu_items(items, output_, anchor, config_.edge);
		}
		items.push_back(MenuItem::separator());
		items.push_back(remove_item());
		return;
	}

	if (kind == "notifications") {
		items.push_back(MenuItem::check("Do not disturb", cfg.dnd, set("dnd", cfg.dnd ? "false" : "true")));
		items.push_back(MenuItem::action("Clear all", [srv] {
			if (srv->notifications) srv->notifications->clear_all();
			srv->redraw_panels();
		}, "trash"));
		items.push_back(MenuItem::action("Notification settings", [srv] { srv->open_settings("notifications"); }, "gear"));
		items.push_back(MenuItem::separator());
		items.push_back(remove_item());
		return;
	}

	if (kind == "showdesktop") {
		items.push_back(MenuItem::action(srv->shown_desktop.empty() ? "Show the desktop" : "Bring the windows back",
			[srv] { srv->toggle_show_desktop(); }, "desktop"));
		items.push_back(MenuItem::separator());
		items.push_back(remove_item());
		return;
	}
}

// The panel itself: auto-hide, position, style, panels.
void Panel::show_panel_menu(const Slot *slot, double lx, double ly) {
	Server *srv = server_;
	const std::string id = config_.id;
	const std::string pre = "panel." + id + ".";
	std::vector<MenuItem> items;

	// A flexible space or separator: it can go.
	if (slot && (slot->kind == "spacer" || slot->kind == "separator") && slot->widget >= 0) {
		const std::vector<std::string> widgets = config_.widgets;
		const int wi = slot->widget;
		items.push_back(MenuItem::action(slot->kind == "spacer" ? "Remove this flexible space"
			: "Remove this separator", [srv, pre, widgets, wi] {
				std::vector<std::string> w = widgets;
				if (wi < static_cast<int>(w.size())) w.erase(w.begin() + wi);
				srv->write_setting(pre + "widgets", join_list(w));
			}, "minus"));
		items.push_back(MenuItem::separator());
	}

	items.push_back(MenuItem::header("Panel"));
	items.push_back(MenuItem::check("Auto-hide", config_.autohide,
		[srv, pre, on = config_.autohide] { srv->write_setting(pre + "autohide", on ? "false" : "true"); }));
	if (!config_.vertical() && std::find(config_.widgets.begin(), config_.widgets.end(), "tasks") !=
			config_.widgets.end()) {
		items.push_back(MenuItem::check("Window titles", config_.labels,
			[srv, pre, on = config_.labels] { srv->write_setting(pre + "labels", on ? "false" : "true"); }));
	}
	items.push_back(MenuItem::header("Position"));
	for (const auto &[label, edge] : std::vector<std::pair<std::string, std::string>>{
			{"Bottom", "bottom"}, {"Top", "top"}, {"Left", "left"}, {"Right", "right"}}) {
		items.push_back(MenuItem::radio(label, config_.edge == edge,
			[srv, pre, e = edge] { srv->write_setting(pre + "edge", e); }));
	}
	items.push_back(MenuItem::header("Style"));
	for (const auto &[label, style] : std::vector<std::pair<std::string, std::string>>{
			{"Floating", "floating"}, {"Full width", "full"}, {"Dock", "dock"}}) {
		items.push_back(MenuItem::radio(label, config_.style == style,
			[srv, pre, st = style] { srv->write_setting(pre + "style", st); }));
	}
	items.push_back(MenuItem::separator());

	std::vector<std::string> ids;
	for (const PanelConfig &pc : srv->config.panels) {
		ids.push_back(pc.id);
	}
	items.push_back(MenuItem::action("Add a panel", [srv, ids] {
		std::vector<std::string> next = ids;
		std::string fresh;
		for (int n = 2; fresh.empty(); n++) {
			const std::string candidate = "panel" + std::to_string(n);
			if (std::find(next.begin(), next.end(), candidate) == next.end()) {
				fresh = candidate;
			}
		}
		// On an edge nobody uses yet, if there is one.
		std::string edge = "top";
		for (const char *e : {"top", "left", "right", "bottom"}) {
			bool used = false;
			for (const PanelConfig &pc : srv->config.panels) {
				used = used || pc.edge == e;
			}
			if (!used) {
				edge = e;
				break;
			}
		}
		Config::write_key("panel." + fresh + ".edge", edge);
		Config::write_key("panel." + fresh + ".widgets", "menu,tasks,spacer,clock");
		next.push_back(fresh);
		srv->write_setting("panels", join_list(next));
	}, "plus"));
	if (ids.size() > 1) {
		std::vector<std::string> rest = ids;
		rest.erase(std::remove(rest.begin(), rest.end(), id), rest.end());
		MenuItem remove = MenuItem::action("Remove this panel",
			[srv, rest] { srv->write_setting("panels", join_list(rest)); }, "trash");
		remove.danger = true;
		items.push_back(remove);
	}
	items.push_back(MenuItem::action("Panel settings…", [srv] { srv->open_settings("panels"); }, "gear"));

	srv->close_popups();
	srv->menu->show(output_, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1},
		config_.edge, std::move(items));
}

// ---- auto-hide --------------------------------------------------------------

bool Panel::something_open() const {
	return (server_->launcher && server_->launcher->is_open_on(output_)) ||
		(server_->control_centre && server_->control_centre->is_open_on(output_)) ||
		(server_->calendar && server_->calendar->is_open_on(output_)) ||
		(server_->media_card && server_->media_card->is_open_on(output_)) ||
		(server_->clipboard_card && server_->clipboard_card->is_open_on(output_)) ||
		(server_->menu && server_->menu->is_open_on(output_));
}

void Panel::show(bool on) {
	if (on == want_shown_) {
		return;
	}
	want_shown_ = on;
	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(shown_, on ? 1.0 : 0.0,
		c.animate(c.anim_panel) ? c.ms(on ? 160 : 220) : 0, [this] { redraw(); },
		[this] { anim_ = 0; });
}

int Panel::handle_hide_timer(void *data) {
	auto *self = static_cast<Panel *>(data);
	self->hide_armed_ = false;
	if (self->something_open()) {
		self->hide_armed_ = true;
		wl_event_source_timer_update(self->hide_timer_, 500);   // check again later
		return 0;
	}
	const double cx = self->server_->cursor->x, cy = self->server_->cursor->y;
	if (!self->contains(cx, cy)) {
		self->show(false);
	}
	return 0;
}

void Panel::pointer_moved(double lx, double ly) {
	if (!config_.autohide) {
		return;
	}
	const wlr_box o = output_->box();
	const std::string &e = config_.edge;
	const bool inside_range = config_.vertical()
		? (ly >= strip_.y && ly < strip_.y + strip_.height)
		: (lx >= strip_.x && lx < strip_.x + strip_.width);
	bool at_edge = false;
	if (inside_range) {
		if (e == "bottom") at_edge = ly >= o.y + o.height - 2;
		else if (e == "top") at_edge = ly <= o.y + 1;
		else if (e == "left") at_edge = lx <= o.x + 1;
		else at_edge = lx >= o.x + o.width - 2;
	}
	if (at_edge || (want_shown_ && contains(lx, ly))) {
		show(true);
		wl_event_source_timer_update(hide_timer_, 0);
		hide_armed_ = false;
	} else if (want_shown_ && !hide_armed_) {
		wl_event_source_timer_update(hide_timer_, 600);
		hide_armed_ = true;
	}
}

// ---- Dragging pinned apps into order ---------------------------------------------

bool Panel::pin_press(double lx, double ly) {
	const wlr_box bar = bar_box();
	const int index = slot_at(lx - (bar.x - bar_.x), ly - (bar.y - bar_.y));
	if (index < 0) return false;
	const Slot &s = slots_[static_cast<size_t>(index)];
	if (s.kind != "pins" || s.pin < 0 || config_.pins.size() < 2) return false;
	drag_pin_ = s.pin;
	drag_x_ = lx;
	drag_y_ = ly;
	pin_dragging_ = false;
	pins_before_ = config_.pins;
	return true;
}

void Panel::pin_drag(double lx, double ly) {
	if (drag_pin_ < 0) return;
	if (!pin_dragging_) {
		if (std::hypot(lx - drag_x_, ly - drag_y_) < 8) return;   // still a click
		pin_dragging_ = true;
		server_->close_popups();
	}
	// The pinned app under the pointer: ours moves into its place.
	const wlr_box bar = bar_box();
	const double px = lx - (bar.x - bar_.x), py = ly - (bar.y - bar_.y);
	const int index = slot_at(px, py);
	if (index < 0) return;
	const Slot &s = slots_[static_cast<size_t>(index)];
	if (s.kind != "pins" || s.pin < 0 || s.pin == drag_pin_) return;
	const int to = s.pin;
	if (to >= static_cast<int>(config_.pins.size()) || drag_pin_ >= static_cast<int>(config_.pins.size())) return;
	// Only once past the middle of the other one (no flicking back and forth).
	const double mid = config_.vertical() ? s.box.y + s.box.height / 2.0 : s.box.x + s.box.width / 2.0;
	const double at = config_.vertical() ? py : px;
	if ((to > drag_pin_ && at < mid) || (to < drag_pin_ && at > mid)) return;
	const std::string moving = config_.pins[static_cast<size_t>(drag_pin_)];
	config_.pins.erase(config_.pins.begin() + drag_pin_);
	config_.pins.insert(config_.pins.begin() + to, moving);
	drag_pin_ = to;
	redraw();
}

void Panel::pin_release(double lx, double ly) {
	const bool dragged = pin_dragging_;
	drag_pin_ = -1;
	pin_dragging_ = false;
	if (!dragged) {
		click(lx, ly, BTN_LEFT);
		return;
	}
	if (config_.pins != pins_before_) {
		server_->write_setting("panel." + config_.id + ".pins", join_list(config_.pins));
	}
}

} // namespace dusk
