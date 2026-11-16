#include "notify.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <poll.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <linux/input-event-codes.h>
#include <systemd/sd-bus.h>

#include "draw.hpp"
#include "glyphs.hpp"

namespace dusk {

namespace {

constexpr const char *kPath = "/org/freedesktop/Notifications";
constexpr const char *kInterface = "org.freedesktop.Notifications";
constexpr int kToastWidth = 360;
constexpr int kPad = 14;
constexpr int kMaxToasts = 4;
constexpr size_t kMaxKept = 50;
constexpr int kTimerW = 40;        // the countdown line at the bottom right
constexpr int kTimerH = 3;
constexpr int kTimerBottom = 8;    // from the card's bottom edge to the line

std::string string_or(const char *s) {
	return s ? s : "";
}

// Apps may send a little HTML ("body-markup"); we said we don't support
// it, but some send it anyway. Strip tags and decode the basic entities.
std::string plain_text(const std::string &in) {
	std::string out;
	bool tag = false;
	for (size_t i = 0; i < in.size(); i++) {
		const char c = in[i];
		if (c == '<') {
			tag = true;
		} else if (c == '>' && tag) {
			tag = false;
		} else if (!tag) {
			out += c;
		}
	}
	const std::pair<const char *, const char *> entities[] = {
		{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
	for (const auto &[from, to] : entities) {
		size_t pos = 0;
		while ((pos = out.find(from, pos)) != std::string::npos) {
			out.replace(pos, std::strlen(from), to);
			pos += std::strlen(to);
		}
	}
	return out;
}

// Pixels sent with a notification (image-data: width, height, rowstride,
// has alpha, bits, channels, RGB(A) bytes) as a cairo picture.
cairo_surface_t *picture_from_pixels(int w, int h, int stride, bool alpha, int channels, const uint8_t *data,
		size_t size) {
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || channels < 3 || stride < w * channels ||
			size < static_cast<size_t>(stride) * (h - 1) + static_cast<size_t>(w) * channels) {
		return nullptr;
	}
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	unsigned char *out = cairo_image_surface_get_data(s);
	const int ostride = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++) {
		const uint8_t *in = data + static_cast<size_t>(y) * stride;
		auto *row = reinterpret_cast<uint32_t *>(out + static_cast<size_t>(y) * ostride);
		for (int x = 0; x < w; x++) {
			const uint8_t *p = in + x * channels;
			const uint32_t a = alpha && channels >= 4 ? p[3] : 255;
			const uint32_t r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;   // premultiplied
			row[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	cairo_surface_mark_dirty(s);
	return s;
}

std::shared_ptr<cairo_surface_t> share(cairo_surface_t *s) {
	return s ? std::shared_ptr<cairo_surface_t>(s, cairo_surface_destroy) : nullptr;
}

std::string state_dir() {
	const char *xdg = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	return xdg && *xdg ? std::string(xdg) + "/dusk" : std::string(home ? home : "/tmp") + "/.local/state/dusk";
}

// One field per line in the history file: newlines and backslashes escaped.
std::string escape_line(const std::string &s) {
	std::string out;
	for (char c : s) {
		if (c == '\\') out += "\\\\";
		else if (c == '\n') out += "\\n";
		else if (c == '\t') out += "\\t";
		else out += c;
	}
	return out;
}
std::string unescape_line(const std::string &s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '\\' && i + 1 < s.size()) {
			const char n = s[++i];
			out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
		} else {
			out += s[i];
		}
	}
	return out;
}

} // namespace

std::string app_icon_for(const std::vector<AppInfo> &apps, const std::string &desktop_entry,
		const std::string &app_name) {
	auto lower = [](std::string x) {
		for (char &c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return x;
	};
	if (!desktop_entry.empty()) {
		const std::string want = lower(desktop_entry) + ".desktop";
		for (const AppInfo &a : apps) {
			if (lower(a.id) == want) return a.icon;
		}
	}
	if (app_name.empty()) return "";
	const std::string name = lower(app_name);
	for (const AppInfo &a : apps) {
		if (lower(a.name) == name) return a.icon;
	}
	// "firefox" -> org.mozilla.firefox.desktop, "discord" -> com.discordapp.Discord.desktop...
	for (const AppInfo &a : apps) {
		std::string id = lower(a.id);
		if (id.size() > 8) id = id.substr(0, id.size() - 8);   // .desktop
		const size_t dot = id.rfind('.');
		const std::string last = dot == std::string::npos ? id : id.substr(dot + 1);
		if (last == name || lower(a.wm_class) == name) return a.icon;
	}
	return "";
}

void draw_notification_icon(cairo_t *cr, IconLoader &icons, const Notification &n, double x, double y,
		double size, float scale) {
	if (n.picture) {
		cairo_surface_t *p = n.picture.get();
		const double iw = cairo_image_surface_get_width(p), ih = cairo_image_surface_get_height(p);
		const double s = std::max(size / iw, size / ih);
		cairo_save(cr);
		rounded_rect(cr, x, y, size, size, size * 0.22);
		cairo_clip(cr);
		cairo_translate(cr, x + (size - iw * s) / 2, y + (size - ih * s) / 2);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, p, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		return;
	}
	const std::string name = !n.image.empty() ? n.image : !n.icon.empty() ? n.icon : n.app_icon;
	const std::string sfx = "-symbolic";
	if (name.size() > sfx.size() && name.compare(name.size() - sfx.size(), sfx.size(), sfx) == 0) {
		// A one-colour icon (they come in dark grey, lost on a dark card):
		// in the text colour on a soft tile, like the other badges.
		fill_rounded(cr, x, y, size, size, size * 0.28, theme::field_bg);
		const double g = size * 0.58;
		draw_named_icon(cr, icons, name, x + (size - g) / 2, y + (size - g) / 2, g, theme::text, scale);
		return;
	}
	draw_icon(cr, icons, name, n.app.empty() ? n.summary : n.app, x, y, size, scale);
}

void Toast::update_timer_bar() {
	if (!timer_bar) {
		return;
	}
	const bool timed = total > 0 && deadline > 0 && !closing;
	if (!timed || alpha <= 0.01 || width <= 0) {
		wlr_scene_node_set_enabled(&timer_bar->node, false);
		return;
	}
	const double left = paused_left >= 0 ? paused_left : std::max(0.0, deadline - now_ms());
	const double frac = std::clamp(left / total, 0.0, 1.0);
	const double s = layer->scale();
	// Shrinks towards its left end as the time runs out.
	const double bx = width - kPad - kTimerW, by = height - kTimerBottom - kTimerH;   // on its track
	const int pw = static_cast<int>(std::lround(kTimerW * frac * s));
	const int ph = std::max(1, static_cast<int>(std::lround(kTimerH * s)));
	if (pw < 1) {
		wlr_scene_node_set_enabled(&timer_bar->node, false);
		return;
	}
	Color c = theme::accent;
	c.a = std::clamp(layer->alpha(), 0.0, 1.0) * (paused_left >= 0 ? 0.55 : 1.0);
	float color[4];
	to_float(c, color);
	wlr_scene_rect_set_size(timer_bar, pw, ph);
	wlr_scene_rect_set_color(timer_bar, color);
	wlr_scene_rect_set_corner_radius(timer_bar, std::min(pw, ph) / 2);
	wlr_scene_node_set_position(&timer_bar->node, static_cast<int>(std::lround(layer->map_x(bx))),
		static_cast<int>(std::lround(layer->map_y(by))));
	wlr_scene_node_set_enabled(&timer_bar->node, true);
}

Notifications::Notifications(Server *server) : server_(server) {
	if (!server->config.notifications) {
		return;
	}
	load_list();
	if (sd_bus_open_user(&bus_) < 0) {
		wlr_log(WLR_INFO, "No session D-Bus; notifications are off");
		bus_ = nullptr;
		return;
	}
	if (sd_bus_add_filter(bus_, &filter_slot_, &Notifications::handle_message, this) < 0 ||
			sd_bus_request_name(bus_, "org.freedesktop.Notifications", 0) < 0) {
		wlr_log(WLR_INFO, "Another notification service is running; Dusk won't show notifications");
		sd_bus_slot_unref(filter_slot_);
		filter_slot_ = nullptr;
		sd_bus_unref(bus_);
		bus_ = nullptr;
		return;
	}
	fd_source_ = wl_event_loop_add_fd(server->loop, sd_bus_get_fd(bus_), WL_EVENT_READABLE,
		&Notifications::handle_bus_fd, this);
	timeout_source_ = wl_event_loop_add_timer(server->loop, &Notifications::handle_bus_timeout,
		this);
	process_bus();
}

Notifications::~Notifications() {
	for (auto &[path, img] : previews_) {
		if (img) cairo_surface_destroy(img);
	}
	if (ticker_) {
		server_->anim->remove_ticker(ticker_);
		ticker_ = 0;
	}
	while (!toasts_.empty()) {
		destroy_toast(toasts_.back().get());
	}
	if (fd_source_) {
		wl_event_source_remove(fd_source_);
	}
	if (timeout_source_) {
		wl_event_source_remove(timeout_source_);
	}
	if (filter_slot_) {
		sd_bus_slot_unref(filter_slot_);
	}
	if (bus_) {
		sd_bus_flush_close_unref(bus_);
	}
}

// ---- D-Bus plumbing ---------------------------------------------------------

int Notifications::handle_bus_fd(int, uint32_t, void *data) {
	static_cast<Notifications *>(data)->process_bus();
	return 0;
}

int Notifications::handle_bus_timeout(void *data) {
	static_cast<Notifications *>(data)->process_bus();
	return 0;
}

void Notifications::process_bus() {
	if (!bus_) {
		return;
	}
	while (sd_bus_process(bus_, nullptr) > 0) {
	}
	update_bus_events();
}

void Notifications::update_bus_events() {
	if (!bus_) {
		return;
	}
	const int events = sd_bus_get_events(bus_);
	uint32_t mask = 0;
	if (events & POLLIN) mask |= WL_EVENT_READABLE;
	if (events & POLLOUT) mask |= WL_EVENT_WRITABLE;
	wl_event_source_fd_update(fd_source_, mask);
	uint64_t usec = 0;
	if (sd_bus_get_timeout(bus_, &usec) >= 0 && usec != UINT64_MAX) {
		timespec ts{};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
		// At least 5 ms: never a tight loop if D-Bus keeps saying "now".
		const int ms = usec > now ? std::max(5, static_cast<int>((usec - now) / 1000) + 1) : 5;
		wl_event_source_timer_update(timeout_source_, ms);
	} else {
		wl_event_source_timer_update(timeout_source_, 0);
	}
}

int Notifications::handle_message(sd_bus_message *m, void *data, sd_bus_error *) {
	auto *self = static_cast<Notifications *>(data);
	const char *path = sd_bus_message_get_path(m);
	if (!path || std::strcmp(path, kPath) != 0) {
		return 0;
	}
	if (sd_bus_message_is_method_call(m, kInterface, "Notify")) {
		return self->on_notify(m);
	}
	if (sd_bus_message_is_method_call(m, kInterface, "CloseNotification")) {
		uint32_t id = 0;
		if (sd_bus_message_read(m, "u", &id) >= 0) {
			if (Toast *t = self->find_toast(id)) {
				self->close_toast(t);
			}
			self->list_.erase(std::remove_if(self->list_.begin(), self->list_.end(),
				[id](const Notification &n) { return n.id == id; }), self->list_.end());
			self->emit_closed(id, 3);
			self->list_changed();
		}
		sd_bus_reply_method_return(m, nullptr);
		return 1;
	}
	if (sd_bus_message_is_method_call(m, kInterface, "GetCapabilities")) {
		sd_bus_message *reply = nullptr;
		if (sd_bus_message_new_method_return(m, &reply) >= 0) {
			const char *caps[] = {"body", "actions", "icon-static", "persistence", nullptr};
			sd_bus_message_append_strv(reply, const_cast<char **>(caps));
			sd_bus_send(nullptr, reply, nullptr);
			sd_bus_message_unref(reply);
		}
		return 1;
	}
	if (sd_bus_message_is_method_call(m, kInterface, "GetServerInformation")) {
		sd_bus_reply_method_return(m, "ssss", "Dusk", "Dusk", "0.18", "1.2");
		return 1;
	}
	return 0;
}

int Notifications::on_notify(sd_bus_message *m) {
	const char *app = nullptr, *icon = nullptr, *summary = nullptr, *body = nullptr;
	uint32_t replaces = 0;
	if (sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body) < 0) {
		return 0;
	}
	Notification n;
	n.app = string_or(app);
	n.icon = string_or(icon);
	n.summary = plain_text(string_or(summary));
	n.body = plain_text(string_or(body));
	n.time = std::time(nullptr);

	char **actions = nullptr;
	if (sd_bus_message_read_strv(m, &actions) >= 0 && actions) {
		for (char **a = actions; a[0] && a[1]; a += 2) {
			n.actions.push_back({a[0], a[1]});
		}
		for (char **a = actions; *a; a++) {
			free(*a);
		}
		free(actions);
	}

	// Hints: we only care about a few; the rest are skipped.
	if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			const char *key = nullptr;
			sd_bus_message_read(m, "s", &key);
			char type = 0;
			const char *contents = nullptr;
			sd_bus_message_peek_type(m, &type, &contents);
			const std::string k = string_or(key);
			const std::string c = string_or(contents);
			if (k == "urgency" && c == "y") {
				uint8_t u = 1;
				sd_bus_message_read(m, "v", "y", &u);
				n.urgency = u;
			} else if ((k == "image-path" || k == "image_path" || k == "desktop-entry") && c == "s") {
				const char *v = nullptr;
				sd_bus_message_read(m, "v", "s", &v);
				(k == "desktop-entry" ? n.desktop_entry : n.image) = string_or(v);
			} else if ((k == "image-data" || k == "image_data" || k == "icon_data") && c == "(iiibiiay)") {
				int32_t w = 0, h = 0, stride = 0, bits = 0, channels = 0;
				int alpha = 0;
				const void *data = nullptr;
				size_t size = 0;
				if (sd_bus_message_enter_container(m, 'v', "(iiibiiay)") > 0) {
					if (sd_bus_message_enter_container(m, 'r', "iiibiiay") > 0) {
						sd_bus_message_read(m, "iiibii", &w, &h, &stride, &alpha, &bits, &channels);
						sd_bus_message_read_array(m, 'y', &data, &size);
						sd_bus_message_exit_container(m);
					}
					sd_bus_message_exit_container(m);
				}
				if (data && bits == 8 && (k != "icon_data" || !n.picture)) {
					n.picture = share(picture_from_pixels(w, h, stride, alpha != 0, channels,
						static_cast<const uint8_t *>(data), size));
				}
			} else if (k == "transient" && c == "b") {
				int b = 0;
				sd_bus_message_read(m, "v", "b", &b);
				n.transient = b != 0;
			} else {
				sd_bus_message_skip(m, "v");
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}
	int32_t timeout = -1;
	sd_bus_message_read(m, "i", &timeout);
	n.timeout = timeout;

	// Replacing an existing notification keeps its id.
	if (replaces != 0 && find(replaces)) {
		n.id = replaces;
	} else if (replaces != 0 && find_toast(replaces)) {
		n.id = replaces;
	} else {
		n.id = next_id_++;
		if (next_id_ == 0) next_id_ = 1;
	}
	if (!n.desktop_entry.empty() && n.app.empty()) {
		for (const AppInfo &a : server_->apps.apps()) {
			if (a.id == n.desktop_entry + ".desktop") n.app = a.name;
		}
	}
	// The app's own icon, for the header (and when the notification has none).
	n.app_icon = app_icon_for(server_->apps.apps(), n.desktop_entry, n.app);
	if (n.app_icon.empty() && !n.icon.empty() && n.icon.find('/') == std::string::npos &&
			n.icon.find("dialog-") != 0) {
		n.app_icon = n.icon;   // most apps send their own icon name as app_icon
	}
	sd_bus_reply_method_return(m, "u", n.id);

	list_.erase(std::remove_if(list_.begin(), list_.end(),
		[&](const Notification &o) { return o.id == n.id; }), list_.end());
	if (!n.transient) {
		list_.push_front(n);
		while (list_.size() > kMaxKept) {
			list_.pop_back();
		}
		remember(n);
	}
	// Do not disturb, or while the screen is being recorded (so nothing
	// pops into the picture); criticals get through either way.
	const bool quiet = (server_->config.dnd || (server_->recorder && server_->recorder->active()))
		&& n.urgency < 2;
	if (!quiet) {
		show_toast(n);
	}
	list_changed();
	return 1;
}

void Notifications::emit_closed(uint32_t id, uint32_t reason) {
	if (bus_) {
		sd_bus_emit_signal(bus_, kPath, kInterface, "NotificationClosed", "uu", id, reason);
		update_bus_events();
	}
}

void Notifications::emit_action(uint32_t id, const std::string &key) {
	if (bus_) {
		sd_bus_emit_signal(bus_, kPath, kInterface, "ActionInvoked", "us", id, key.c_str());
		update_bus_events();
	}
}

const Notification *Notifications::find(uint32_t id) const {
	for (const Notification &n : list_) {
		if (n.id == id) {
			return &n;
		}
	}
	return nullptr;
}

Toast *Notifications::find_toast(uint32_t id) {
	for (auto &t : toasts_) {
		if (t->id == id && !t->closing) {
			return t.get();
		}
	}
	return nullptr;
}

uint32_t Notifications::post(const std::string &summary, const std::string &body,
		const std::string &icon, int urgency, uint32_t replaces,
		std::vector<std::pair<std::string, std::string>> actions,
		std::function<void(const std::string &)> on_action) {
	Notification n;
	n.app = "Dusk";
	n.summary = summary;
	n.body = body;
	n.icon = icon;
	n.urgency = urgency;
	n.actions = std::move(actions);
	n.time = std::time(nullptr);
	if (replaces != 0 && (find(replaces) || find_toast(replaces))) {
		n.id = replaces;
	} else {
		n.id = next_id_++;
		if (next_id_ == 0) next_id_ = 1;
	}
	if (on_action) {
		own_actions_[n.id] = std::move(on_action);
	}
	list_.erase(std::remove_if(list_.begin(), list_.end(),
		[&](const Notification &o) { return o.id == n.id; }), list_.end());
	n.app_icon = "org.dusk.Settings";
	list_.push_front(n);
	while (list_.size() > kMaxKept) {
		list_.pop_back();
	}
	remember(n);
	// Same rule as the app notifications: quiet in Do not disturb and while
	// the screen is being recorded, unless it is critical.
	if (!((server_->config.dnd || (server_->recorder && server_->recorder->active()))
			&& urgency < 2)) {
		show_toast(n);
	}
	list_changed();
	return n.id;
}

void Notifications::invoke(uint32_t id, const std::string &key) {
	if (auto it = own_actions_.find(id); it != own_actions_.end()) {
		auto fn = it->second;
		own_actions_.erase(it);
		fn(key);
	}
	emit_action(id, key);
	if (Toast *t = find_toast(id)) {
		close_toast(t);
	}
	list_.erase(std::remove_if(list_.begin(), list_.end(),
		[id](const Notification &n) { return n.id == id; }), list_.end());
	emit_closed(id, 2);
	server_->close_popups();
	list_changed();
}

void Notifications::activate(uint32_t id) {
	const Notification *n = find(id);
	if (!n) {
		return;
	}
	for (const auto &[key, label] : n->actions) {
		if (key == "default") {
			invoke(id, key);
			return;
		}
	}
	dismiss(id);
}

void Notifications::dismiss(uint32_t id) {
	if (Toast *t = find_toast(id)) {
		close_toast(t);
	}
	list_.erase(std::remove_if(list_.begin(), list_.end(),
		[id](const Notification &n) { return n.id == id; }), list_.end());
	emit_closed(id, 2);
	list_changed();
}

void Notifications::clear_all() {
	for (const Notification &n : list_) {
		emit_closed(n.id, 2);
	}
	list_.clear();
	for (auto &t : toasts_) {
		if (!t->closing) {
			close_toast(t.get());
		}
	}
	list_changed();
}

// ---- popups -------------------------------------------------------------------

void Notifications::show_toast(const Notification &n) {
	// A game or video is full screen: no pop-up (unless that's wanted);
	// it still goes into the control centre's list.
	if (!server_->config.toast_over_fullscreen && n.urgency < 2 && server_->focused &&
			server_->focused->fullscreen) {
		return;
	}
	Toast *toast = find_toast(n.id);
	if (!toast) {
		// Too many on screen: the oldest goes.
		int open = 0;
		for (auto &t : toasts_) {
			if (!t->closing) open++;
		}
		if (open >= kMaxToasts) {
			for (auto &t : toasts_) {
				if (!t->closing) {
					close_toast(t.get());
					break;
				}
			}
		}
		auto owned = std::make_unique<Toast>();
		toast = owned.get();
		toast->owner = this;
		toast->id = n.id;
		toast->layer = std::make_unique<UiLayer>(server_->layer_notify, &toast->tag);
		toast->timer = wl_event_loop_add_timer(server_->loop, &Notifications::handle_toast_timeout,
			toast);
		float none[4] = {0, 0, 0, 0};
		toast->timer_bar = wlr_scene_rect_create(toast->layer->tree, 1, 1, none);
		toast->timer_bar->accepts_input = false;
		wlr_scene_node_set_enabled(&toast->timer_bar->node, false);
		toasts_.push_back(std::move(owned));
	}
	// Keep a copy for drawing even if the list doesn't keep it (transient).
	if (!find(n.id)) {
		list_.push_front(n);
		list_.front().transient = true;
	}
	arm_timer(toast);   // before drawing: the card shows the countdown's track
	draw_toast(toast);
	relayout();

	const Config &c = server_->config;
	if (toast->alpha < 1.0 && !toast->fade_anim) {
		toast->fade_anim = server_->anim->tween(toast->alpha, 1.0,
			c.animate(c.anim_osd) ? c.ms(180) : 0, [this, toast] {
				toast->show();
				relayout();
			}, [toast] { toast->fade_anim = 0; });
	}
}

void Notifications::arm_timer(Toast *toast) {
	const Notification *n = find(toast->id);
	int ms = server_->config.notification_timeout;
	if (n) {
		if (n->urgency >= 2 || n->timeout == 0) {
			ms = 0;   // stays until dismissed
		} else if (n->timeout > 0) {
			ms = std::clamp(n->timeout, 1000, 60000);
		}
	}
	toast->total = ms;
	toast->deadline = ms > 0 ? now_ms() + ms : 0;
	toast->paused_left = -1;
	wl_event_source_timer_update(toast->timer, ms);
	if (toast->hover >= 0) {
		pause_timer(toast);   // updated while pointed at: it keeps waiting
	}
	run_timer_bars();
}

// Pointed at: the countdown stops where it is.
void Notifications::pause_timer(Toast *toast) {
	if (toast->deadline <= 0 || toast->paused_left >= 0) {
		return;
	}
	toast->paused_left = std::max(0.0, toast->deadline - now_ms());
	wl_event_source_timer_update(toast->timer, 0);
	toast->update_timer_bar();
}

// Pointer gone: it carries on (with at least a moment to spare).
void Notifications::resume_timer(Toast *toast) {
	if (toast->paused_left < 0 || toast->closing) {
		return;
	}
	const double left = std::max(toast->paused_left, 1500.0);
	toast->total = std::max(toast->total, left);
	toast->deadline = now_ms() + left;
	toast->paused_left = -1;
	wl_event_source_timer_update(toast->timer, static_cast<int>(left));
	run_timer_bars();
}

void Notifications::run_timer_bars() {
	if (ticker_) {
		return;
	}
	ticker_ = server_->anim->add_ticker([this](double) {
		bool running = false;
		for (auto &t : toasts_) {
			t->update_timer_bar();
			running = running || (t->deadline > 0 && t->paused_left < 0 && !t->closing);
		}
		if (!running) {
			ticker_ = 0;
		}
		return running;
	});
}

int Notifications::handle_toast_timeout(void *data) {
	auto *toast = static_cast<Toast *>(data);
	if (toast->hover >= 0) {
		toast->owner->pause_timer(toast);   // waits while pointed at
		return 0;
	}
	toast->owner->close_toast(toast);
	return 0;
}

void Notifications::close_toast(Toast *toast) {
	if (toast->closing) {
		return;
	}
	toast->closing = true;
	wl_event_source_timer_update(toast->timer, 0);
	toast->update_timer_bar();
	if (hovered_ == toast) {
		hovered_ = nullptr;
	}
	// Transient ones were only kept for drawing.
	const uint32_t id = toast->id;
	list_.erase(std::remove_if(list_.begin(), list_.end(),
		[id](const Notification &n) { return n.id == id && n.transient; }), list_.end());
	const Config &c = server_->config;
	server_->anim->cancel(toast->fade_anim);
	toast->fade_anim = server_->anim->tween(toast->alpha, 0.0,
		c.animate(c.anim_osd) ? c.ms(200) : 0, [toast] {
			toast->show();
		}, [this, toast] {
			toast->fade_anim = 0;
			destroy_toast(toast);
			relayout();
		});
}

void Notifications::destroy_toast(Toast *toast) {
	server_->anim->cancel(toast->fade_anim);
	server_->anim->cancel(toast->move_anim);
	if (hovered_ == toast) {
		hovered_ = nullptr;
	}
	wl_event_source_remove(toast->timer);
	toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
		[toast](const std::unique_ptr<Toast> &t) { return t.get() == toast; }), toasts_.end());
}

// Stacks the popups in the chosen corner of the focused screen; they glide
// to their new places when one goes away.
// Pop-ups stack in one of nine places on the screen (Settings >
// Notifications): along the top they stack downwards, along the bottom
// upwards, in the middle around the centre.
void Notifications::relayout() {
	Output *out = server_->focused_output();
	if (!out) {
		return;
	}
	const wlr_box u = out->usable();
	const std::string &pos = server_->config.notification_position;
	auto ends = [&pos](const std::string &tail) {
		return pos.size() > tail.size() && pos.compare(pos.size() - tail.size(), tail.size(), tail) == 0;
	};
	const bool left = pos == "left" || ends("-left");
	const bool right = pos == "right" || ends("-right");
	const bool top = pos.rfind("top", 0) == 0;
	const bool bottom = pos.rfind("bottom", 0) == 0;
	constexpr int kGap = 10;
	int total = 0;
	for (auto &owned : toasts_) {
		if (!owned->closing) total += owned->height + kGap;
	}
	total = std::max(0, total - kGap);
	int y = top ? u.y + kGap : bottom ? u.y + u.height - kGap : u.y + (u.height - total) / 2;
	for (auto &owned : toasts_) {
		Toast *t = owned.get();
		t->x = left ? u.x + kGap : right ? u.x + u.width - t->width - kGap : u.x + (u.width - t->width) / 2;
		if (t->closing) {
			t->show();
			continue;
		}
		double target;
		if (bottom) {
			y -= t->height;
			target = y;
			y -= kGap;
		} else {
			target = y;
			y += t->height + kGap;
		}
		if (!t->placed) {
			t->placed = true;
			// New ones slide in from the edge they sit at.
			t->y = target + (bottom ? 12 : -12);
			t->target_y = target - 1;   // forces the glide below
		}
		if (t->target_y != target) {
			t->target_y = target;
			t->move_from = t->y;
			t->move_t = 0;
			server_->anim->cancel(t->move_anim);
			const Config &c = server_->config;
			t->move_anim = server_->anim->tween(t->move_t, 1.0,
				c.animate(c.anim_osd) ? c.ms(200) : 0, [t] {
					t->y = t->move_from + (t->target_y - t->move_from) * t->move_t;
					t->show();
				}, [t] { t->move_anim = 0; });
		}
		t->show();
	}
}

void Notifications::draw_toast(Toast *toast) {
	const Notification *n = find(toast->id);
	if (!n) {
		return;
	}
	Output *out = server_->focused_output();
	const float scale = out ? out->output->scale : 1.0f;
	const int w = kToastWidth;
	const int text_w = w - 2 * kPad - 44;

	// Measure first.
	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *pc = cairo_create(probe);
	const int summary_h = std::max(18, draw_paragraph(pc, n->summary, 0, 0, text_w, 2,
		theme::font_bold, theme::text, false));
	const int body_h = draw_paragraph(pc, n->body, 0, 0, text_w, 4, theme::font, theme::text, false);
	cairo_destroy(pc);
	cairo_surface_destroy(probe);

	std::vector<std::pair<std::string, std::string>> buttons;
	for (const auto &a : n->actions) {
		if (a.first != "default" && buttons.size() < 3) {
			buttons.push_back(a);
		}
	}
	const int head = 20;
	int h = kPad + head + 6 + summary_h + (body_h ? 2 + body_h : 0) + kPad;
	if (!buttons.empty()) {
		h += 40;
	}
	// A picture (a screenshot, a photo someone sent): shown big under the text,
	// at the screen's full sharpness, instead of as a small icon.
	cairo_surface_t *preview = preview_for(*n, scale);
	int preview_h = 0;
	if (preview) {
		const double iw = cairo_image_surface_get_width(preview), ih = cairo_image_surface_get_height(preview);
		preview_h = static_cast<int>(std::lround(std::min(200.0, (w - 2 * kPad) * ih / std::max(1.0, iw))));
		h += preview_h + 10;
	}
	toast->width = w;
	toast->height = h;
	Canvas canvas(w, h, scale);
	cairo_t *cr = canvas.cr();
	const double radius = theme::ui_radius;
	ui_card(cr, 0, 0, w, h, radius, theme::menu_bg);

	// The app's icon and name on top, then the notification's own picture.
	const double icon = 36;
	int name_x = kPad;
	if (!n->app_icon.empty()) {
		draw_icon(cr, server_->icons, n->app_icon, n->app, kPad, kPad + 2, 16, scale);
		name_x += 22;
	}
	draw_text(cr, n->app.empty() ? "Notification" : n->app, name_x, kPad, w - name_x - kPad - 30, head,
		theme::font_small, theme::text_dim);
	if (preview) {
		// The picture is shown big below: the icon is the app's.
		Notification plain = *n;
		plain.picture.reset();
		plain.image.clear();
		if (!plain.icon.empty() && plain.icon[0] == '/') plain.icon.clear();
		if (plain.icon.empty() && plain.app_icon.empty()) plain.icon = "image-x-generic";
		draw_notification_icon(cr, server_->icons, plain, kPad, kPad + head + 4, icon, scale);
	} else {
		draw_notification_icon(cr, server_->icons, *n, kPad, kPad + head + 4, icon, scale);
	}
	const double tx = kPad + 44;
	double ty = kPad + head + 6;
	draw_paragraph(cr, n->summary, tx, ty, text_w, 2, theme::font_bold, theme::text);
	ty += summary_h + 2;
	if (body_h) {
		draw_paragraph(cr, n->body, tx, ty, text_w, 4, theme::font, theme::text_dim);
	}
	if (preview) {
		const double py = kPad + head + 6 + summary_h + (body_h ? 2 + body_h : 0) + 10;
		const double pw = w - 2 * kPad;
		const double iw = cairo_image_surface_get_width(preview), ih = cairo_image_surface_get_height(preview);
		const double k = std::min(pw / iw, preview_h / ih);
		const double dw = iw * k, dh = ih * k;
		const double px = kPad + (pw - dw) / 2;
		fill_rounded(cr, kPad, py, pw, preview_h, 10, theme::field_bg);
		cairo_save(cr);
		rounded_rect(cr, px, py + (preview_h - dh) / 2, dw, dh, 10);
		cairo_clip(cr);
		cairo_translate(cr, px, py + (preview_h - dh) / 2);
		cairo_scale(cr, k, k);
		cairo_set_source_surface(cr, preview, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
		cairo_paint(cr);
		cairo_restore(cr);
	}

	// Close button, top right (shown on hover).
	toast->close_box = wlr_box{w - kPad - 22, kPad - 2, 24, 24};
	if (toast->hover >= 0) {
		const wlr_box &cb = toast->close_box;
		cairo_new_path(cr);
		cairo_arc(cr, cb.x + 12, cb.y + 12, 12, 0, 2 * M_PI);
		set_color(cr, toast->hover == 1 ? with_alpha(theme::text, 0.18) : theme::button_bg);
		cairo_fill(cr);
		draw_named_icon(cr, server_->icons, "close", cb.x + 5, cb.y + 5, 14, theme::text, scale);
	}

	// Action buttons.
	toast->action_boxes.clear();
	if (!buttons.empty()) {
		const int by = h - kPad - 32;
		const int bw = (w - 2 * kPad - 8 * (static_cast<int>(buttons.size()) - 1)) /
			static_cast<int>(buttons.size());
		for (size_t i = 0; i < buttons.size(); i++) {
			const wlr_box b{kPad + static_cast<int>(i) * (bw + 8), by, bw, 32};
			toast->action_boxes.push_back(b);
			const bool lit = toast->hover == static_cast<int>(i) + 2;
			fill_rounded(cr, b.x, b.y, b.width, b.height, inner_radius(radius, kPad),
				lit ? with_alpha(theme::text, 0.16) : theme::field_bg);
			draw_text(cr, buttons[i].second, b.x + 8, b.y, b.width - 16, b.height, theme::font,
				theme::text, Align::Center);
		}
	}
	// The countdown's track: the line itself is drawn over it by the GPU.
	if (toast->total > 0) {
		fill_rounded(cr, w - kPad - kTimerW, h - kTimerBottom - kTimerH, kTimerW, kTimerH, kTimerH / 2.0,
			with_alpha(theme::text, theme::dark ? 0.14 : 0.10));
	}
	toast->layer->present(canvas, wlr_box{0, 0, w, h}, radius);
	toast->show();
}

// A notification's picture big enough for a preview (its own pixels, or a
// picture file), loaded once at the size it's shown.
cairo_surface_t *Notifications::preview_for(const Notification &n, float scale) {
	if (n.picture) {
		cairo_surface_t *p = n.picture.get();
		return cairo_image_surface_get_width(p) >= 160 && cairo_image_surface_get_height(p) >= 90 ? p : nullptr;
	}
	std::string path = !n.image.empty() && n.image[0] == '/' ? n.image : !n.icon.empty() && n.icon[0] == '/' ? n.icon : "";
	if (path.empty()) return nullptr;
	std::string low = path;
	std::transform(low.begin(), low.end(), low.begin(), ::tolower);
	auto ends = [&low](const char *e) {
		const size_t m = std::strlen(e);
		return low.size() > m && low.compare(low.size() - m, m, e) == 0;
	};
	if (!ends(".png") && !ends(".jpg") && !ends(".jpeg") && !ends(".webp")) return nullptr;
	const std::string key = path + "@" + std::to_string(scale);
	auto it = previews_.find(key);
	if (it != previews_.end()) return it->second;
	if (previews_.size() >= 6) {
		for (auto &[k, img] : previews_) {
			if (img) cairo_surface_destroy(img);
		}
		previews_.clear();
	}
	const int px = static_cast<int>(std::lround((kToastWidth - 2 * kPad) * scale));
	cairo_surface_t *img = load_image_sized(path, px, static_cast<int>(std::lround(200 * scale)));
	if (img && (cairo_image_surface_get_width(img) < 100 || cairo_image_surface_get_height(img) < 60)) {
		cairo_surface_destroy(img);   // an icon, not a picture
		img = nullptr;
	}
	previews_[key] = img;
	return img;
}

void Notifications::toast_motion(Toast *toast, double lx, double ly) {
	const double px = lx - toast->x, py = ly - std::lround(toast->y);
	int hover = 0;
	const wlr_box &cb = toast->close_box;
	if (px >= cb.x && px < cb.x + cb.width && py >= cb.y && py < cb.y + cb.height) {
		hover = 1;
	}
	for (size_t i = 0; i < toast->action_boxes.size(); i++) {
		const wlr_box &b = toast->action_boxes[i];
		if (px >= b.x && px < b.x + b.width && py >= b.y && py < b.y + b.height) {
			hover = static_cast<int>(i) + 2;
		}
	}
	if (hovered_ && hovered_ != toast) {
		hovered_->hover = -1;
		resume_timer(hovered_);
		draw_toast(hovered_);
	}
	hovered_ = toast;
	pause_timer(toast);
	if (toast->hover != hover) {
		toast->hover = hover;
		draw_toast(toast);
	}
}

void Notifications::toast_leave() {
	if (hovered_) {
		hovered_->hover = -1;
		resume_timer(hovered_);
		draw_toast(hovered_);
		hovered_ = nullptr;
	}
}

void Notifications::toast_click(Toast *toast, double lx, double ly, uint32_t button) {
	toast_motion(toast, lx, ly);
	const uint32_t id = toast->id;
	if (button != BTN_LEFT || toast->hover == 1) {
		// Any other button, or the X: just put it away (it stays in the list).
		close_toast(toast);
		return;
	}
	const Notification *n = find(id);
	if (!n) {
		close_toast(toast);
		return;
	}
	if (toast->hover >= 2) {
		int index = toast->hover - 2;
		for (const auto &a : n->actions) {
			if (a.first == "default") continue;
			if (index-- == 0) {
				invoke(id, a.first);
				return;
			}
		}
	}
	activate(id);
}

// ---- Kept until cleared ---------------------------------------------------
//
// Notifications stay in the control centre until you dismiss or clear them,
// also after a restart: the list is saved as it changes (Settings ›
// Notifications › "Keep them after a restart").

void Notifications::remember(const Notification &n) {
	// A new one (not a replacement, like a download's progress): the bell.
	auto same = std::find_if(seen_.begin(), seen_.end(), [&n](const std::pair<std::string, uint32_t> &o) {
		return o.second == n.id && o.first == n.app;
	});
	if (same == seen_.end()) {
		unseen++;
		seen_.push_back({n.app, n.id});
		if (seen_.size() > 200) seen_.erase(seen_.begin());
	}
	if (history_changed) history_changed();
}

void Notifications::list_changed() {
	save_list();
	if (changed) changed();
}

// ~/.local/state/dusk/notifications-open: one notification per line, fields
// separated by tabs (time, app, app icon, icon, summary, body, desktop entry, urgency).
void Notifications::load_list() {
	const std::string dir = state_dir();
	std::remove((dir + "/notifications").c_str());   // the old history (0.17-0.21)
	if (!server_->config.notification_history_keep) {
		return;
	}
	std::ifstream in(dir + "/notifications-open");
	std::string line;
	while (std::getline(in, line) && list_.size() < kMaxKept) {
		std::vector<std::string> f;
		size_t start = 0;
		while (true) {
			const size_t tab = line.find('\t', start);
			f.push_back(unescape_line(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
			if (tab == std::string::npos) break;
			start = tab + 1;
		}
		if (f.size() < 6) continue;
		Notification n;
		n.time = static_cast<std::time_t>(std::atoll(f[0].c_str()));
		n.app = f[1];
		n.app_icon = f[2];
		n.icon = f[3];
		if (!n.icon.empty() && n.icon[0] == '/') n.image = n.icon;
		n.summary = f[4];
		n.body = f[5];
		if (f.size() > 6) n.desktop_entry = f[6];
		if (f.size() > 7) n.urgency = std::clamp(std::atoi(f[7].c_str()), 0, 2);
		n.timeout = 0;
		n.id = next_id_++;
		n.hid = next_hid_++;
		list_.push_back(n);
	}
	unseen = static_cast<int>(list_.size());   // still waiting for you: the bell
}

void Notifications::save_list() {
	const std::string dir = state_dir(), file = dir + "/notifications-open";
	if (!server_->config.notification_history_keep) {
		std::remove(file.c_str());
		return;
	}
	for (size_t i = 1; i <= dir.size(); i++) {   // mkdir -p
		if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0700);
	}
	std::ofstream out(file + ".tmp", std::ios::trunc);
	for (const Notification &n : list_) {
		if (n.transient) continue;
		// A picture sent as pixels isn't kept; a file or icon name is.
		const std::string icon = !n.image.empty() && n.image[0] == '/' ? n.image : n.icon;
		out << n.time << '\t' << escape_line(n.app) << '\t' << escape_line(n.app_icon) << '\t' << escape_line(icon)
			<< '\t' << escape_line(n.summary) << '\t' << escape_line(n.body) << '\t' << escape_line(n.desktop_entry)
			<< '\t' << n.urgency << '\n';
	}
	out.close();
	// Notification bodies can be private: never wider than the owner, whatever
	// the umask was when the temporary file appeared.
	::chmod((file + ".tmp").c_str(), 0600);
	std::rename((file + ".tmp").c_str(), file.c_str());
}

} // namespace dusk
