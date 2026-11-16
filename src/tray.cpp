#include "tray.hpp"
#include <sstream>

#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>

#include "apps.hpp"
#include "draw.hpp"
#include "popover.hpp"

namespace dusk {

namespace {

constexpr const char *kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char *kWatcherPath = "/StatusNotifierWatcher";
constexpr const char *kWatcherIface = "org.kde.StatusNotifierWatcher";
constexpr const char *kItemIface = "org.kde.StatusNotifierItem";
constexpr const char *kMenuIface = "com.canonical.dbusmenu";

std::string str(const char *s) {
	return s ? s : "";
}

// An async call's context: freed with its slot.
struct CallData {
	Tray *tray;
	std::string key;
	Output *output = nullptr;
	wlr_box anchor{};
	std::string edge;
	int x = 0, y = 0;
};

sd_bus_slot *float_slot(sd_bus_slot *slot, CallData *data) {
	if (!slot) {
		delete data;
		return nullptr;
	}
	sd_bus_slot_set_floating(slot, 1);
	sd_bus_slot_set_destroy_callback(slot, [](void *d) { delete static_cast<CallData *>(d); });
	return slot;
}

// Icon pixels from an item: ARGB32 in network byte order, the size closest
// to what's drawn (largest if none is big enough).
std::shared_ptr<cairo_surface_t> read_pixmap(sd_bus_message *m) {
	std::shared_ptr<cairo_surface_t> best;
	int best_w = 0;
	if (sd_bus_message_enter_container(m, 'v', "a(iiay)") <= 0) {
		sd_bus_message_skip(m, "v");
		return nullptr;
	}
	if (sd_bus_message_enter_container(m, 'a', "(iiay)") > 0) {
		while (sd_bus_message_enter_container(m, 'r', "iiay") > 0) {
			int32_t w = 0, h = 0;
			const void *data = nullptr;
			size_t size = 0;
			sd_bus_message_read(m, "ii", &w, &h);
			sd_bus_message_read_array(m, 'y', &data, &size);
			sd_bus_message_exit_container(m);
			if (w <= 0 || h <= 0 || w > 512 || h > 512 || size < static_cast<size_t>(w) * h * 4) continue;
			// Prefer about 32 px (crisp at 1x and 2x).
			const bool better = !best || (best_w < 32 ? w > best_w : (w >= 32 && w < best_w));
			if (!better) continue;
			cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
			unsigned char *out = cairo_image_surface_get_data(s);
			const int stride = cairo_image_surface_get_stride(s);
			const auto *in = static_cast<const uint8_t *>(data);
			for (int y = 0; y < h; y++) {
				auto *row = reinterpret_cast<uint32_t *>(out + y * stride);
				for (int x = 0; x < w; x++) {
					const uint8_t *p = in + (static_cast<size_t>(y) * w + x) * 4;
					const uint32_t a = p[0], r = p[1] * a / 255, g = p[2] * a / 255, b = p[3] * a / 255;
					row[x] = (a << 24) | (r << 16) | (g << 8) | b;
				}
			}
			cairo_surface_mark_dirty(s);
			best = std::shared_ptr<cairo_surface_t>(s, cairo_surface_destroy);
			best_w = w;
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	return best;
}

// "_File" -> "File" ("__" is a real underscore).
std::string strip_mnemonic(const std::string &s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '_') {
			if (i + 1 < s.size() && s[i + 1] == '_') out += '_', i++;
			continue;
		}
		out += s[i];
	}
	return out;
}

struct MenuNode {
	int32_t id = 0;
	std::string label, type, toggle_type, children_display;
	bool enabled = true, visible = true;
	int32_t toggle_state = -1;
	std::vector<MenuNode> children;
};

bool read_node(sd_bus_message *m, MenuNode &node, int depth = 0) {
	if (sd_bus_message_enter_container(m, 'r', "ia{sv}av") <= 0) return false;
	sd_bus_message_read(m, "i", &node.id);
	if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			const char *key = nullptr;
			sd_bus_message_read(m, "s", &key);
			const std::string k = str(key);
			char type = 0;
			const char *contents = nullptr;
			sd_bus_message_peek_type(m, &type, &contents);
			const std::string c = str(contents);
			if (c == "s" && (k == "label" || k == "type" || k == "toggle-type" || k == "children-display")) {
				const char *v = nullptr;
				sd_bus_message_read(m, "v", "s", &v);
				(k == "label" ? node.label : k == "type" ? node.type : k == "toggle-type" ? node.toggle_type
					: node.children_display) = str(v);
			} else if (c == "b" && (k == "enabled" || k == "visible")) {
				int b = 1;
				sd_bus_message_read(m, "v", "b", &b);
				(k == "enabled" ? node.enabled : node.visible) = b != 0;
			} else if (c == "i" && k == "toggle-state") {
				sd_bus_message_read(m, "v", "i", &node.toggle_state);
			} else {
				sd_bus_message_skip(m, "v");
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}
	if (sd_bus_message_enter_container(m, 'a', "v") > 0) {
		while (sd_bus_message_enter_container(m, 'v', "(ia{sv}av)") > 0) {
			MenuNode child;
			// The layout comes from an untrusted tray app: bound the recursion,
			// or a deeply nested GetLayout reply exhausts the stack and brings
			// the whole session down. Past the limit the subtree is skipped
			// (consumed but unread), keeping the message in sync.
			if (depth < 10) {
				read_node(m, child, depth + 1);
			} else {
				sd_bus_message_skip(m, "(ia{sv}av)");
			}
			node.children.push_back(std::move(child));
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	return true;
}

} // namespace

// ============================================================================

Tray::Tray(Server *server) : server_(server) {
	if (sd_bus_open_user(&bus_) < 0) {
		bus_ = nullptr;
		return;
	}
	sd_bus_add_filter(bus_, &filter_slot_, &Tray::handle_message, this);
	sd_bus_match_signal(bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
		"NameOwnerChanged", &Tray::handle_name_owner, this);
	watcher_ = sd_bus_request_name(bus_, kWatcherName, 0) >= 0;
	const std::string host = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
	sd_bus_request_name(bus_, host.c_str(), 0);
	fd_source_ = wl_event_loop_add_fd(server->loop, sd_bus_get_fd(bus_), WL_EVENT_READABLE, &Tray::handle_bus_fd, this);
	timeout_source_ = wl_event_loop_add_timer(server->loop, &Tray::handle_bus_timeout, this);
	if (watcher_) {
		sd_bus_emit_signal(bus_, kWatcherPath, kWatcherIface, "StatusNotifierHostRegistered", "");
	} else {
		use_other_watcher();
	}
	find_existing_items();
	// Apps started with the session (Steam, Discord...) may have looked for a
	// tray a moment too early and given up: say it's here again a few times,
	// and pick up any that are already waiting.
	again_ = wl_event_loop_add_timer(server->loop, &Tray::handle_again, this);
	wl_event_source_timer_update(again_, 3000);
	process_bus();
}

int Tray::handle_again(void *data) {
	auto *self = static_cast<Tray *>(data);
	if (self->watcher_ && self->bus_) {
		sd_bus_emit_signal(self->bus_, kWatcherPath, kWatcherIface, "StatusNotifierHostRegistered", "");
	}
	self->find_existing_items();
	self->process_bus();
	if (++self->again_count_ < 4) wl_event_source_timer_update(self->again_, 4000 * self->again_count_);
	return 0;
}

// Items with the usual well-known name ("org.kde.StatusNotifierItem-<pid>-<n>")
// that are on the bus already.
void Tray::find_existing_items() {
	if (!bus_) return;
	char **names = nullptr;
	if (sd_bus_list_names(bus_, &names, nullptr) < 0 || !names) return;
	for (char **n = names; *n; n++) {
		const std::string name = *n;
		free(*n);
		if (name.rfind("org.kde.StatusNotifierItem-", 0) != 0) continue;
		if (std::any_of(items_.begin(), items_.end(), [&name](const auto &i) { return i->service == name; })) continue;
		// Only if it really answers there (some put their item elsewhere and
		// tell the watcher where: those come in through it).
		auto *cd = new CallData{this, name};
		sd_bus_slot *slot = nullptr;
		sd_bus_call_method_async(bus_, &slot, name.c_str(), "/StatusNotifierItem", "org.freedesktop.DBus.Properties",
			"Get", [](sd_bus_message *reply, void *d, sd_bus_error *) -> int {
				auto *c = static_cast<CallData *>(d);
				if (!sd_bus_message_is_method_error(reply, nullptr)) c->tray->add_item(c->key, "/StatusNotifierItem");
				return 0;
			}, cd, "ss", kItemIface, "Id");
		float_slot(slot, cd);
	}
	free(names);
}

Tray::~Tray() {
	for (auto &[key, slot] : item_slots_) {
		sd_bus_slot_unref(slot);
	}
	if (fd_source_) wl_event_source_remove(fd_source_);
	if (timeout_source_) wl_event_source_remove(timeout_source_);
	if (again_) wl_event_source_remove(again_);
	if (filter_slot_) sd_bus_slot_unref(filter_slot_);
	if (bus_) sd_bus_flush_close_unref(bus_);
}

int Tray::handle_bus_fd(int, uint32_t, void *data) {
	static_cast<Tray *>(data)->process_bus();
	return 0;
}

int Tray::handle_bus_timeout(void *data) {
	static_cast<Tray *>(data)->process_bus();
	return 0;
}

void Tray::process_bus() {
	if (!bus_) return;
	while (sd_bus_process(bus_, nullptr) > 0) {
	}
	update_bus_events();
}

void Tray::update_bus_events() {
	if (!bus_) return;
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
		const int ms = usec > now ? std::max(5, static_cast<int>((usec - now) / 1000) + 1) : 5;
		wl_event_source_timer_update(timeout_source_, ms);
	} else {
		wl_event_source_timer_update(timeout_source_, 0);
	}
}

void Tray::notify() {
	if (changed) changed();
}

std::vector<const TrayItem *> Tray::items() const {
	std::vector<const TrayItem *> out;
	for (const auto &item : items_) {
		if (item->loaded && item->status != "Passive") out.push_back(item.get());
	}
	return out;
}

// ---- The watcher ---------------------------------------------------------------

int Tray::handle_message(sd_bus_message *m, void *data, sd_bus_error *) {
	auto *self = static_cast<Tray *>(data);
	if (!sd_bus_message_is_method_call(m, nullptr, nullptr)) {
		return 0;
	}
	const char *path = sd_bus_message_get_path(m);
	if (!self->watcher_ || !path || std::strcmp(path, kWatcherPath) != 0) {
		return 0;
	}
	return self->on_watcher_call(m);
}

int Tray::on_watcher_call(sd_bus_message *m) {
	auto append_property = [this](sd_bus_message *reply, const std::string &name) {
		if (name == "RegisteredStatusNotifierItems") {
			std::vector<std::string> list;
			for (const auto &item : items_) list.push_back(item->service + item->path);
			sd_bus_message_open_container(reply, 'v', "as");
			sd_bus_message_open_container(reply, 'a', "s");
			for (const std::string &s : list) sd_bus_message_append(reply, "s", s.c_str());
			sd_bus_message_close_container(reply);
			sd_bus_message_close_container(reply);
		} else if (name == "IsStatusNotifierHostRegistered") {
			sd_bus_message_append(reply, "v", "b", 1);
		} else if (name == "ProtocolVersion") {
			sd_bus_message_append(reply, "v", "i", 0);
		}
	};
	if (sd_bus_message_is_method_call(m, kWatcherIface, "RegisterStatusNotifierItem")) {
		const char *arg = nullptr;
		sd_bus_message_read(m, "s", &arg);
		const std::string a = str(arg), sender = str(sd_bus_message_get_sender(m));
		// A bus name (".../StatusNotifierItem" is the path), or a path on the caller.
		if (!a.empty() && a[0] == '/') add_item(sender, a);
		else add_item(a.empty() ? sender : a, "/StatusNotifierItem");
		sd_bus_reply_method_return(m, "");
		return 1;
	}
	if (sd_bus_message_is_method_call(m, kWatcherIface, "RegisterStatusNotifierHost")) {
		sd_bus_reply_method_return(m, "");
		return 1;
	}
	if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "Get")) {
		const char *iface = nullptr, *name = nullptr;
		sd_bus_message_read(m, "ss", &iface, &name);
		sd_bus_message *reply = nullptr;
		sd_bus_message_new_method_return(m, &reply);
		append_property(reply, str(name));
		sd_bus_send(bus_, reply, nullptr);
		sd_bus_message_unref(reply);
		return 1;
	}
	if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "GetAll")) {
		sd_bus_message *reply = nullptr;
		sd_bus_message_new_method_return(m, &reply);
		sd_bus_message_open_container(reply, 'a', "{sv}");
		for (const char *name : {"RegisteredStatusNotifierItems", "IsStatusNotifierHostRegistered", "ProtocolVersion"}) {
			sd_bus_message_open_container(reply, 'e', "sv");
			sd_bus_message_append(reply, "s", name);
			append_property(reply, name);
			sd_bus_message_close_container(reply);
		}
		sd_bus_message_close_container(reply);
		sd_bus_send(bus_, reply, nullptr);
		sd_bus_message_unref(reply);
		return 1;
	}
	if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		static const char *kXml =
			"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
			"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
			"<node><interface name=\"org.kde.StatusNotifierWatcher\">"
			"<method name=\"RegisterStatusNotifierItem\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>"
			"<method name=\"RegisterStatusNotifierHost\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>"
			"<property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>"
			"<property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>"
			"<property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>"
			"<signal name=\"StatusNotifierItemRegistered\"><arg type=\"s\"/></signal>"
			"<signal name=\"StatusNotifierItemUnregistered\"><arg type=\"s\"/></signal>"
			"<signal name=\"StatusNotifierHostRegistered\"/>"
			"<signal name=\"StatusNotifierHostUnregistered\"/>"
			"</interface></node>";
		sd_bus_reply_method_return(m, "s", kXml);
		return 1;
	}
	return 0;
}

void Tray::emit_watcher(const char *member, const std::string &arg) {
	if (watcher_ && bus_) {
		sd_bus_emit_signal(bus_, kWatcherPath, kWatcherIface, member, "s", arg.c_str());
	}
}

// Someone else (snixembed, a KDE service...) is the watcher: register as a
// host there and follow its list.
void Tray::use_other_watcher() {
	const std::string host = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
	sd_bus_call_method_async(bus_, nullptr, kWatcherName, kWatcherPath, kWatcherIface, "RegisterStatusNotifierHost",
		nullptr, nullptr, "s", host.c_str());
	sd_bus_match_signal(bus_, nullptr, kWatcherName, kWatcherPath, kWatcherIface, nullptr,
		&Tray::handle_watcher_signal, this);
	sd_bus_call_method_async(bus_, nullptr, kWatcherName, kWatcherPath, "org.freedesktop.DBus.Properties", "Get",
		[](sd_bus_message *reply, void *data, sd_bus_error *) -> int {
			auto *self = static_cast<Tray *>(data);
			if (sd_bus_message_is_method_error(reply, nullptr)) return 0;
			if (sd_bus_message_enter_container(reply, 'v', "as") > 0) {
				char **list = nullptr;
				if (sd_bus_message_read_strv(reply, &list) >= 0 && list) {
					for (char **s = list; *s; s++) {
						const std::string full = *s;
						const size_t slash = full.find('/');
						if (slash == std::string::npos) self->add_item(full, "/StatusNotifierItem");
						else self->add_item(full.substr(0, slash), full.substr(slash));
						free(*s);
					}
					free(list);
				}
			}
			return 0;
		}, this, "ss", kWatcherIface, "RegisteredStatusNotifierItems");
}

int Tray::handle_watcher_signal(sd_bus_message *m, void *data, sd_bus_error *) {
	auto *self = static_cast<Tray *>(data);
	const char *arg = nullptr;
	if (sd_bus_message_read(m, "s", &arg) < 0) return 0;
	const std::string full = str(arg);
	const size_t slash = full.find('/');
	const std::string service = slash == std::string::npos ? full : full.substr(0, slash);
	const std::string path = slash == std::string::npos ? "/StatusNotifierItem" : full.substr(slash);
	if (sd_bus_message_is_signal(m, kWatcherIface, "StatusNotifierItemRegistered")) {
		self->add_item(service, path);
	} else if (sd_bus_message_is_signal(m, kWatcherIface, "StatusNotifierItemUnregistered")) {
		self->remove_item(service + path);
	}
	return 0;
}

int Tray::handle_name_owner(sd_bus_message *m, void *data, sd_bus_error *) {
	auto *self = static_cast<Tray *>(data);
	const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0) return 0;
	if (new_owner && *new_owner) return 0;
	// Gone: its items go too (known by unique or well-known name).
	std::vector<std::string> gone;
	for (const auto &item : self->items_) {
		if (item->service == str(name) || item->service == str(old_owner)) gone.push_back(item->key);
	}
	for (const std::string &key : gone) self->remove_item(key);
	return 0;
}

// ---- Items -------------------------------------------------------------------------

void Tray::add_item(const std::string &service, const std::string &path) {
	if (service.empty()) return;
	const std::string key = service + path;
	for (const auto &item : items_) {
		if (item->key == key) return;
	}
	auto item = std::make_unique<TrayItem>();
	item->key = key;
	item->service = service;
	item->path = path;
	items_.push_back(std::move(item));
	sd_bus_slot *slot = nullptr;
	if (sd_bus_match_signal(bus_, &slot, service.c_str(), path.c_str(), kItemIface, nullptr,
			&Tray::handle_item_signal, this) >= 0) {
		item_slots_[key] = slot;
	}
	refresh_item(key);
	find_owner_app(key);
	emit_watcher("StatusNotifierItemRegistered", key);
}

// Which app the item belongs to, from the program behind it (D-Bus knows its
// process): a Flatpak's app id, or the program's name, matched against the
// installed apps. That's how Electron apps (Heroic, Discord), which give the
// tray a made-up id, still get their own colour icon.
void Tray::find_owner_app(const std::string &key) {
	TrayItem *item = nullptr;
	for (const auto &i : items_) if (i->key == key) item = i.get();
	if (!item || !bus_) return;
	auto *cd = new CallData{this, key};
	sd_bus_slot *slot = nullptr;
	sd_bus_call_method_async(bus_, &slot, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
		"GetConnectionUnixProcessID", [](sd_bus_message *reply, void *d, sd_bus_error *) -> int {
			auto *c = static_cast<CallData *>(d);
			uint32_t pid = 0;
			if (sd_bus_message_is_method_error(reply, nullptr) || sd_bus_message_read(reply, "u", &pid) < 0 || !pid) return 0;
			auto read = [](const std::string &path) {
				std::ifstream f(path);
				std::stringstream ss;
				ss << f.rdbuf();
				return ss.str();
			};
			const std::string proc = "/proc/" + std::to_string(pid);
			// A Flatpak: "app-flatpak-com.heroicgameslauncher.hgl-1234.scope" in its cgroup.
			std::string flatpak;
			{
				const std::string cg = read(proc + "/cgroup");
				const size_t at = cg.find("app-flatpak-");
				if (at != std::string::npos) {
					std::string rest = cg.substr(at + 12);
					rest = rest.substr(0, rest.find_first_of(".\n") == std::string::npos ? rest.size() : rest.find(".scope"));
					const size_t dash = rest.rfind('-');
					flatpak = dash == std::string::npos ? rest : rest.substr(0, dash);
				}
			}
			std::string exe;
			{
				char buf[4096];
				const ssize_t n = readlink((proc + "/exe").c_str(), buf, sizeof buf - 1);
				if (n > 0) {
					buf[n] = 0;
					exe = buf;
					exe = exe.substr(exe.rfind('/') + 1);
				}
				if (exe.empty()) {
					exe = read(proc + "/comm");
					while (!exe.empty() && (exe.back() == '\n' || exe.back() == ' ')) exe.pop_back();
				}
				for (char &ch : exe) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			std::string icon;
			for (const AppInfo &a : c->tray->server_->apps.apps()) {
				if (a.icon.empty()) continue;
				std::string id = a.id;
				if (id.size() > 8 && id.compare(id.size() - 8, 8, ".desktop") == 0) id.erase(id.size() - 8);
				std::string lid = id;
				for (char &ch : lid) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				std::string first = a.exec.substr(0, a.exec.find(' '));
				first = first.substr(first.rfind('/') + 1);
				for (char &ch : first) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				const bool match = (!flatpak.empty() && (id == flatpak || a.exec.find(flatpak) != std::string::npos)) ||
					(flatpak.empty() && exe.size() >= 3 && exe != "electron" && exe != "python3" && exe != "java" &&
						(first == exe || lid == exe || lid.find(exe) != std::string::npos));
				if (match) {
					icon = a.icon;
					break;
				}
			}
			if (icon.empty()) return 0;
			for (const auto &i : c->tray->items_) {
				if (i->key == c->key) {
					i->app_icon = icon;
					c->tray->notify();
				}
			}
			return 0;
		}, cd, "s", item->service.c_str());
	float_slot(slot, cd);
}

void Tray::remove_item(const std::string &key) {
	auto it = std::find_if(items_.begin(), items_.end(), [&key](const auto &i) { return i->key == key; });
	if (it == items_.end()) return;
	items_.erase(it);
	if (auto s = item_slots_.find(key); s != item_slots_.end()) {
		sd_bus_slot_unref(s->second);
		item_slots_.erase(s);
	}
	emit_watcher("StatusNotifierItemUnregistered", key);
	notify();
}

int Tray::handle_item_signal(sd_bus_message *m, void *data, sd_bus_error *) {
	auto *self = static_cast<Tray *>(data);
	const std::string path = str(sd_bus_message_get_path(m)), sender = str(sd_bus_message_get_sender(m));
	std::vector<std::string> keys;
	for (const auto &item : self->items_) {
		if (item->path == path && (item->service == sender || item->service[0] != ':')) keys.push_back(item->key);
	}
	for (const std::string &k : keys) self->refresh_item(k);
	return 0;
}

void Tray::refresh_item(const std::string &key) {
	TrayItem *item = nullptr;
	for (const auto &i : items_) {
		if (i->key == key) item = i.get();
	}
	if (!item || !bus_) return;
	auto *data = new CallData{this, key};
	sd_bus_slot *slot = nullptr;
	sd_bus_call_method_async(bus_, &slot, item->service.c_str(), item->path.c_str(), "org.freedesktop.DBus.Properties",
		"GetAll", [](sd_bus_message *reply, void *d, sd_bus_error *) -> int {
			auto *cd = static_cast<CallData *>(d);
			if (sd_bus_message_is_method_error(reply, nullptr)) return 0;
			for (const auto &i : cd->tray->items_) {
				if (i->key == cd->key) {
					cd->tray->read_properties(*i, reply);
					cd->tray->notify();
					break;
				}
			}
			return 0;
		}, data, "s", kItemIface);
	float_slot(slot, data);
	update_bus_events();
}

void Tray::read_properties(TrayItem &item, sd_bus_message *m) {
	if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0) return;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = nullptr;
		sd_bus_message_read(m, "s", &key);
		const std::string k = str(key);
		char type = 0;
		const char *contents = nullptr;
		sd_bus_message_peek_type(m, &type, &contents);
		const std::string c = str(contents);
		std::string *target = k == "Id" ? &item.id : k == "Title" ? &item.title : k == "Status" ? &item.status
			: k == "IconName" ? &item.icon_name : k == "AttentionIconName" ? &item.attention_icon
			: k == "IconThemePath" ? &item.theme_path : k == "Menu" ? &item.menu : nullptr;
		if (target && (c == "s" || c == "o")) {
			const char *v = nullptr;
			sd_bus_message_read(m, "v", c.c_str(), &v);
			*target = str(v);
		} else if (k == "ItemIsMenu" && c == "b") {
			int b = 0;
			sd_bus_message_read(m, "v", "b", &b);
			item.item_is_menu = b != 0;
		} else if ((k == "IconPixmap" || k == "AttentionIconPixmap") && c == "a(iiay)") {
			(k == "IconPixmap" ? item.pixmap : item.attention_pixmap) = read_pixmap(m);
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	item.loaded = true;
}

namespace {

// How colourful (0 grey .. 1) and how light the visible part of a picture is.
struct Tone { double sat = 0, lum = 0, weight = 0, spread = 0; };   // spread: how much light and dark differ

Tone tone_of(cairo_surface_t *img) {
	Tone t;
	if (!img || cairo_surface_get_type(img) != CAIRO_SURFACE_TYPE_IMAGE) return t;
	cairo_surface_flush(img);
	const int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
	const auto *data = reinterpret_cast<const uint32_t *>(cairo_image_surface_get_data(img));
	if (!data) return t;
	const int stride = cairo_image_surface_get_stride(img) / 4;
	for (int yy = 0; yy < h; yy++) {
		for (int xx = 0; xx < w; xx++) {
			const uint32_t v = data[yy * stride + xx];
			const double a = ((v >> 24) & 0xff) / 255.0;
			if (a < 0.15) continue;
			const double r = ((v >> 16) & 0xff) / 255.0 / a, g = ((v >> 8) & 0xff) / 255.0 / a, b = (v & 0xff) / 255.0 / a;
			const double l = 0.299 * r + 0.587 * g + 0.114 * b;
			t.lum += a * l;
			t.spread += a * l * l;
			t.sat += a * (std::max({r, g, b}) - std::min({r, g, b}));
			t.weight += a;
		}
	}
	if (t.weight > 0) {
		t.sat /= t.weight;
		t.lum /= t.weight;
		t.spread = std::sqrt(std::max(0.0, t.spread / t.weight - t.lum * t.lum));
	}
	return t;
}

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

} // namespace

// App status icons in colour: many apps give the tray a grey "mono" icon
// (Steam's is steam_tray_mono). Then the app's own colour icon is used
// instead, when there is one; otherwise the grey one, in the panel's text
// colour if it would vanish against the panel.
void Tray::draw_icon(cairo_t *cr, const TrayItem &item, double x, double y, double size, float scale) {
	const int px = std::max(1, static_cast<int>(std::lround(size * scale)));
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
	cairo_surface_set_device_scale(tmp, scale, scale);
	cairo_t *tc = cairo_create(tmp);
	draw_icon_as_is(tc, item, 0, 0, size, scale);
	cairo_destroy(tc);
	const Tone t = tone_of(tmp);
	const bool grey = t.weight > 0 && t.sat < 0.16;
	if (grey) {
		// The colour icon: from the icon's own name without "-tray-mono" and
		// the like, the app's id, or its name.
		std::string base = lower(item.icon_name);
		if (const size_t slash = base.rfind('/'); slash != std::string::npos) base = base.substr(slash + 1);
		if (const size_t dot = base.rfind('.'); dot != std::string::npos) base = base.substr(0, dot);
		for (const char *end : {"_tray_mono", "-tray-mono", "-symbolic", "_symbolic", "_tray", "-tray", "_mono", "-mono",
				"-panel", "_panel", "-indicator", "_indicator", "-status", "-dark", "-light"}) {
			const std::string e = end;
			if (base.size() > e.size() && base.compare(base.size() - e.size(), e.size(), e) == 0) {
				base.erase(base.size() - e.size());
			}
		}
		// Electron apps (Heroic, Discord...) give a made-up id, but their title
		// is the app's name: the installed app with that name has the icon.
		std::vector<std::string> names;
		if (!item.app_icon.empty()) names.push_back(item.app_icon);   // the app that owns it
		const std::string title = lower(item.title);
		const std::string first_word = title.substr(0, title.find(' '));
		for (const AppInfo &a : server_->apps.apps()) {
			const std::string an = lower(a.name);
			if (a.icon.empty()) continue;
			if ((!title.empty() && (an == title || title.rfind(an, 0) == 0 || an.rfind(title, 0) == 0)) ||
					(first_word.size() >= 4 && (an.rfind(first_word, 0) == 0 || lower(a.id).find(first_word) != std::string::npos))) {
				names.push_back(a.icon);
			}
		}
		for (const std::string &want : {base, lower(item.id), title, first_word}) {
			if (want.empty()) continue;
			const std::string n = server_->apps.icon_for(want);
			names.push_back(n.empty() ? want : n);
		}
		for (const std::string &name : names) {
			cairo_surface_t *img = name[0] == '/' ? nullptr : server_->icons.get(name, px);
			const bool file = name[0] == '/' && access(name.c_str(), R_OK) == 0;
			if ((img && tone_of(img).sat >= 0.16) || file) {
				dusk::draw_icon(cr, server_->icons, name, item.title, x, y, size, scale);
				cairo_surface_destroy(tmp);
				return;
			}
		}
	}
	const Color &bg = theme::panel_bg;
	const double bg_lum = 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b;
	// Only a one-colour icon is recoloured: one with light and dark parts
	// (a white mark on a black shape) would lose its mark.
	const bool faint = t.weight > 0 && t.spread < 0.18 && std::fabs(t.lum - bg_lum) < 0.42;
	cairo_save(cr);
	if (grey && faint) {
		set_color(cr, theme::text);
		cairo_mask_surface(cr, tmp, x, y);
	} else {
		cairo_set_source_surface(cr, tmp, x, y);
		cairo_paint(cr);
	}
	cairo_restore(cr);
	cairo_surface_destroy(tmp);
}

void Tray::draw_icon_as_is(cairo_t *cr, const TrayItem &item, double x, double y, double size, float scale) {
	const bool attention = item.status == "NeedsAttention";
	const std::string &name = attention && !item.attention_icon.empty() ? item.attention_icon : item.icon_name;
	const auto &pix = attention && item.attention_pixmap ? item.attention_pixmap : item.pixmap;
	// A named icon: in the app's own folder first, then the icon theme.
	if (!name.empty()) {
		if (!item.theme_path.empty() && name[0] != '/') {
			for (const char *ext : {".png", ".svg", ".xpm"}) {
				for (const char *sub : {"", "/hicolor/scalable/apps", "/hicolor/48x48/apps", "/hicolor/32x32/apps",
						"/hicolor/24x24/apps", "/hicolor/22x22/apps"}) {
					const std::string file = item.theme_path + sub + "/" + name + ext;
					if (access(file.c_str(), R_OK) == 0) {
						dusk::draw_icon(cr, server_->icons, file, item.title, x, y, size, scale);
						return;
					}
				}
			}
		}
		const int px = static_cast<int>(size * scale + 0.5);
		if (name[0] == '/' || server_->icons.get(name, px)) {
			dusk::draw_icon(cr, server_->icons, name, item.title, x, y, size, scale);
			return;
		}
	}
	if (pix) {
		cairo_surface_t *p = pix.get();
		const double iw = cairo_image_surface_get_width(p), ih = cairo_image_surface_get_height(p);
		const double s = std::min(size / iw, size / ih);
		cairo_save(cr);
		cairo_translate(cr, x + (size - iw * s) / 2, y + (size - ih * s) / 2);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, p, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		return;
	}
	dusk::draw_icon(cr, server_->icons, name, item.title.empty() ? item.id : item.title, x, y, size, scale);
}

// ---- Clicks ------------------------------------------------------------------------

void Tray::activate(const std::string &key, int x, int y) {
	const TrayItem *item = nullptr;
	for (const auto &i : items_) {
		if (i->key == key) item = i.get();
	}
	if (!item || !bus_) return;
	auto *data = new CallData{this, key};
	data->x = x;
	data->y = y;
	sd_bus_slot *slot = nullptr;
	// Most apps show their window; some only have a menu (then they ask for that).
	sd_bus_call_method_async(bus_, &slot, item->service.c_str(), item->path.c_str(), kItemIface, "Activate",
		[](sd_bus_message *reply, void *d, sd_bus_error *) -> int {
			auto *cd = static_cast<CallData *>(d);
			if (sd_bus_message_is_method_error(reply, nullptr)) {
				for (const auto &i : cd->tray->items_) {
					if (i->key == cd->key) {
						sd_bus_call_method_async(cd->tray->bus_, nullptr, i->service.c_str(), i->path.c_str(), kItemIface,
							"ContextMenu", nullptr, nullptr, "ii", cd->x, cd->y);
					}
				}
			}
			return 0;
		}, data, "ii", x, y);
	float_slot(slot, data);
	update_bus_events();
}

void Tray::secondary(const std::string &key, int x, int y) {
	for (const auto &i : items_) {
		if (i->key == key && bus_) {
			sd_bus_call_method_async(bus_, nullptr, i->service.c_str(), i->path.c_str(), kItemIface, "SecondaryActivate",
				nullptr, nullptr, "ii", x, y);
		}
	}
	update_bus_events();
}

void Tray::scroll(const std::string &key, int delta) {
	for (const auto &i : items_) {
		if (i->key == key && bus_) {
			sd_bus_call_method_async(bus_, nullptr, i->service.c_str(), i->path.c_str(), kItemIface, "Scroll",
				nullptr, nullptr, "is", delta, "vertical");
		}
	}
	update_bus_events();
}

void Tray::context_menu(const std::string &key, Output *output, wlr_box anchor, const std::string &edge) {
	const TrayItem *item = nullptr;
	for (const auto &i : items_) {
		if (i->key == key) item = i.get();
	}
	if (!item || !bus_) return;
	if (item->menu.empty() || item->menu == "/") {
		// No menu of its own to read: the app draws one itself.
		sd_bus_call_method_async(bus_, nullptr, item->service.c_str(), item->path.c_str(), kItemIface, "ContextMenu",
			nullptr, nullptr, "ii", anchor.x, anchor.y);
		update_bus_events();
		return;
	}
	// Ask it to get the menu ready, then read the whole menu.
	sd_bus_call_method_async(bus_, nullptr, item->service.c_str(), item->menu.c_str(), kMenuIface, "AboutToShow",
		nullptr, nullptr, "i", 0);
	auto *data = new CallData{this, key, output, anchor, edge};
	sd_bus_message *call = nullptr;
	sd_bus_message_new_method_call(bus_, &call, item->service.c_str(), item->menu.c_str(), kMenuIface, "GetLayout");
	sd_bus_message_append(call, "ii", 0, -1);
	sd_bus_message_open_container(call, 'a', "s");
	for (const char *p : {"label", "enabled", "visible", "type", "toggle-type", "toggle-state", "children-display"}) {
		sd_bus_message_append(call, "s", p);
	}
	sd_bus_message_close_container(call);
	sd_bus_slot *slot = nullptr;
	sd_bus_call_async(bus_, &slot, call, [](sd_bus_message *reply, void *d, sd_bus_error *) -> int {
		auto *cd = static_cast<CallData *>(d);
		Tray *self = cd->tray;
		const TrayItem *it = nullptr;
		for (const auto &i : self->items_) {
			if (i->key == cd->key) it = i.get();
		}
		if (!it || sd_bus_message_is_method_error(reply, nullptr)) return 0;
		uint32_t revision = 0;
		sd_bus_message_read(reply, "u", &revision);
		MenuNode root;
		if (!read_node(reply, root)) return 0;
		std::vector<MenuItem> items;
		const std::string service = it->service, menu_path = it->menu;
		const std::string title = !it->title.empty() ? it->title : it->id;
		if (!title.empty()) items.push_back(MenuItem::header(title));
		std::function<void(const MenuNode &, int)> add = [&](const MenuNode &n, int depth) {
			for (const MenuNode &c : n.children) {
				if (!c.visible) continue;
				if (c.type == "separator") {
					if (!items.empty() && items.back().kind != MenuItem::Kind::Separator) {
						items.push_back(MenuItem::separator());
					}
					continue;
				}
				const std::string label = std::string(depth > 0 ? "   " : "") + strip_mnemonic(c.label);
				if (!c.children.empty() || c.children_display == "submenu") {
					items.push_back(MenuItem::header(strip_mnemonic(c.label)));
					add(c, depth + 1);
					continue;
				}
				const int32_t id = c.id;
				auto run = [self, service, menu_path, id] {
					if (!self->bus_) return;
					sd_bus_call_method_async(self->bus_, nullptr, service.c_str(), menu_path.c_str(), kMenuIface,
						"Event", nullptr, nullptr, "isvu", id, "clicked", "i", 0,
						static_cast<uint32_t>(std::time(nullptr)));
					self->update_bus_events();
				};
				MenuItem mi = c.toggle_type == "checkmark" ? MenuItem::check(label, c.toggle_state == 1, run)
					: c.toggle_type == "radio" ? MenuItem::radio(label, c.toggle_state == 1, run)
					: MenuItem::action(label, run);
				mi.enabled = c.enabled;
				items.push_back(std::move(mi));
			}
		};
		add(root, 0);
		while (!items.empty() && items.back().kind == MenuItem::Kind::Separator) items.pop_back();
		if (items.size() <= 1) return 0;
		self->server_->menu->show(cd->output, cd->anchor, cd->edge, std::move(items));
		return 0;
	}, data, 0);
	sd_bus_message_unref(call);
	float_slot(slot, data);
	update_bus_events();
}

} // namespace dusk
