// dusk-xembed: old-style (XEmbed) tray icons as StatusNotifierItems.
//
// Wine and Proton games, and some older X11 apps, put their tray icon in an
// "XEmbed system tray": a window on the X server that the icon's window is
// put inside. Dusk's tray shows StatusNotifierItems (the D-Bus kind) only,
// so with no XEmbed tray Wine showed its icons in a small window of their
// own. This takes the XEmbed tray's place on Xwayland, keeps each icon's
// window off screen, copies its picture whenever it changes, and offers it to
// Dusk's tray as a StatusNotifierItem; clicks on it in the panel are sent
// back to the icon's window. (The same idea as KDE's xembedsniproxy.)
//
// It's its own process, so nothing here can take the desktop down with it.
#include <poll.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <unistd.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/xcb.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

xcb_connection_t *X = nullptr;
xcb_screen_t *screen = nullptr;
sd_event *loop = nullptr;
uint8_t damage_event_base = 0;
xcb_visualid_t argb_visual = 0;
xcb_colormap_t argb_colormap = 0;
constexpr int kSize = 24;   // the icon window's size; the panel scales the picture

xcb_atom_t atom(const char *name) {
	auto *r = xcb_intern_atom_reply(X, xcb_intern_atom(X, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr);
	const xcb_atom_t a = r ? r->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
	std::free(r);
	return a;
}

xcb_atom_t A_OPCODE, A_XEMBED, A_XEMBED_INFO, A_WM_NAME, A_NET_WM_NAME, A_UTF8;

std::string window_text(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type) {
	auto *r = xcb_get_property_reply(X, xcb_get_property(X, 0, w, prop, type, 0, 256), nullptr);
	std::string s;
	if (r && xcb_get_property_value_length(r) > 0) {
		s.assign(static_cast<const char *>(xcb_get_property_value(r)), xcb_get_property_value_length(r));
	}
	std::free(r);
	return s;
}

// The program's name for the icon: _NET_WM_NAME, WM_NAME, else its class.
std::string window_title(xcb_window_t w) {
	std::string t = window_text(w, A_NET_WM_NAME, A_UTF8);
	if (t.empty()) t = window_text(w, A_WM_NAME, XCB_ATOM_STRING);
	if (t.empty()) {
		const std::string cls = window_text(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
		t = cls.substr(0, cls.find('\0'));
	}
	return t;
}

struct Icon {
	xcb_window_t window = 0, container = 0;
	xcb_damage_damage_t damage = 0;
	sd_bus *bus = nullptr;
	sd_bus_slot *slot = nullptr;
	int w = kSize, h = kSize;
	std::vector<uint8_t> argb;   // big-endian ARGB, as StatusNotifierItem wants
	std::string title;
	sd_event_source *pending = nullptr;   // a picture to take soon (changes come in bursts)
	bool ignore_unmap = false;   // the unmap that moving it into our window causes
};

std::map<xcb_window_t, std::unique_ptr<Icon>> icons;

// ---- the picture ---------------------------------------------------------------

void grab(Icon &ic) {
	auto *g = xcb_get_geometry_reply(X, xcb_get_geometry(X, ic.window), nullptr);
	if (!g) return;
	// The geometry comes from an untrusted X client (any client may resize its
	// own window after docking): clamp it to tray size and do the size math in
	// 64-bit, or a huge w * h * 4 overflows int and slips past the length
	// check below.
	const size_t w = std::min<size_t>(std::max<size_t>(g->width, 1), 512);
	const size_t h = std::min<size_t>(std::max<size_t>(g->height, 1), 512);
	const int depth = g->depth;
	std::free(g);
	if (depth != 24 && depth != 32) return;
	auto *img = xcb_get_image_reply(X, xcb_get_image(X, XCB_IMAGE_FORMAT_Z_PIXMAP, ic.window, 0, 0,
		static_cast<uint16_t>(w), static_cast<uint16_t>(h), ~0u), nullptr);
	if (!img) return;
	const uint8_t *px = xcb_get_image_data(img);
	const size_t want = w * h * 4;
	if (xcb_get_image_data_length(img) < static_cast<int>(want)) {
		std::free(img);
		return;
	}
	std::vector<uint8_t> out(want);
	bool any = false;
	for (size_t i = 0; i < w * h; i++) {
		// Little-endian BGRA (BGRX at depth 24), premultiplied at depth 32.
		const uint8_t b = px[i * 4], gr = px[i * 4 + 1], r = px[i * 4 + 2];
		uint8_t a = depth == 32 ? px[i * 4 + 3] : 255;
		uint8_t rr = r, gg = gr, bb = b;
		if (depth == 32 && a > 0 && a < 255) {
			rr = static_cast<uint8_t>(std::min(255, r * 255 / a));
			gg = static_cast<uint8_t>(std::min(255, gr * 255 / a));
			bb = static_cast<uint8_t>(std::min(255, b * 255 / a));
		}
		out[i * 4] = a;
		out[i * 4 + 1] = rr;
		out[i * 4 + 2] = gg;
		out[i * 4 + 3] = bb;
		any = any || a > 0;
	}
	std::free(img);
	if (!any) return;   // nothing drawn yet
	if (out == ic.argb && w == ic.w && h == ic.h) return;
	ic.argb = std::move(out);
	ic.w = w;
	ic.h = h;
	if (ic.bus) {
		sd_bus_emit_signal(ic.bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewIcon", "");
		sd_bus_flush(ic.bus);
	}
}

int grab_later(sd_event_source *s, uint64_t, void *data) {
	auto *ic = static_cast<Icon *>(data);
	ic->pending = nullptr;
	sd_event_source_unref(s);
	grab(*ic);
	return 0;
}

void schedule_grab(Icon &ic) {
	if (ic.pending) return;
	uint64_t now = 0;
	sd_event_now(loop, CLOCK_MONOTONIC, &now);
	sd_event_add_time(loop, &ic.pending, CLOCK_MONOTONIC, now + 60 * 1000, 0, grab_later, &ic);
}

// ---- clicks --------------------------------------------------------------------------

void click(Icon &ic, int button, int x, int y) {
	xcb_window_t w = ic.window;
	for (int type : {XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE}) {
		xcb_button_press_event_t ev{};
		ev.response_type = static_cast<uint8_t>(type);
		ev.detail = static_cast<xcb_button_t>(button);
		ev.time = XCB_CURRENT_TIME;
		ev.root = screen->root;
		ev.event = w;
		ev.child = XCB_NONE;
		ev.root_x = static_cast<int16_t>(x);
		ev.root_y = static_cast<int16_t>(y);
		ev.event_x = static_cast<int16_t>(ic.w / 2);
		ev.event_y = static_cast<int16_t>(ic.h / 2);
		ev.state = type == XCB_BUTTON_RELEASE ? static_cast<uint16_t>(XCB_BUTTON_MASK_1 << (button - 1)) : 0;
		ev.same_screen = 1;
		xcb_send_event(X, 0, w, type == XCB_BUTTON_PRESS ? XCB_EVENT_MASK_BUTTON_PRESS : XCB_EVENT_MASK_BUTTON_RELEASE,
			reinterpret_cast<const char *>(&ev));
	}
	xcb_flush(X);
	schedule_grab(ic);
}

// ---- the StatusNotifierItem ---------------------------------------------------------

Icon *icon_of(void *data) {
	return static_cast<Icon *>(data);
}

int m_activate(sd_bus_message *m, void *data, sd_bus_error *) {
	int x = 0, y = 0;
	sd_bus_message_read(m, "ii", &x, &y);
	click(*icon_of(data), 1, x, y);
	return sd_bus_reply_method_return(m, "");
}
int m_secondary(sd_bus_message *m, void *data, sd_bus_error *) {
	int x = 0, y = 0;
	sd_bus_message_read(m, "ii", &x, &y);
	click(*icon_of(data), 2, x, y);
	return sd_bus_reply_method_return(m, "");
}
int m_menu(sd_bus_message *m, void *data, sd_bus_error *) {
	int x = 0, y = 0;
	sd_bus_message_read(m, "ii", &x, &y);
	click(*icon_of(data), 3, x, y);
	return sd_bus_reply_method_return(m, "");
}
int m_scroll(sd_bus_message *m, void *data, sd_bus_error *) {
	int delta = 0;
	const char *dir = nullptr;
	sd_bus_message_read(m, "is", &delta, &dir);
	click(*icon_of(data), delta > 0 ? 4 : 5, 0, 0);
	return sd_bus_reply_method_return(m, "");
}

int p_string(sd_bus *, const char *, const char *, const char *prop, sd_bus_message *reply, void *data, sd_bus_error *) {
	const Icon *ic = icon_of(data);
	const std::string p = prop;
	std::string v;
	if (p == "Category") v = "ApplicationStatus";
	else if (p == "Id") v = ic->title.empty() ? "xembed-" + std::to_string(ic->window) : ic->title;
	else if (p == "Title") v = ic->title;
	else if (p == "Status") v = "Active";
	else if (p == "IconName" || p == "AttentionIconName" || p == "OverlayIconName" || p == "IconThemePath") v = "";
	return sd_bus_message_append(reply, "s", v.c_str());
}
int p_menu(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *, sd_bus_error *) {
	return sd_bus_message_append(reply, "o", "/NO_DBUSMENU");
}
int p_ismenu(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *, sd_bus_error *) {
	return sd_bus_message_append(reply, "b", 0);
}
int p_window(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *data, sd_bus_error *) {
	return sd_bus_message_append(reply, "i", static_cast<int32_t>(icon_of(data)->window));
}
int p_pixmap(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *data, sd_bus_error *) {
	const Icon *ic = icon_of(data);
	sd_bus_message_open_container(reply, 'a', "(iiay)");
	if (!ic->argb.empty()) {
		sd_bus_message_open_container(reply, 'r', "iiay");
		sd_bus_message_append(reply, "ii", ic->w, ic->h);
		sd_bus_message_append_array(reply, 'y', ic->argb.data(), ic->argb.size());
		sd_bus_message_close_container(reply);
	}
	return sd_bus_message_close_container(reply);
}
int p_empty_pixmap(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *, sd_bus_error *) {
	sd_bus_message_open_container(reply, 'a', "(iiay)");
	return sd_bus_message_close_container(reply);
}
int p_tooltip(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *data, sd_bus_error *) {
	const Icon *ic = icon_of(data);
	sd_bus_message_open_container(reply, 'r', "sa(iiay)ss");
	sd_bus_message_append(reply, "s", "");
	sd_bus_message_open_container(reply, 'a', "(iiay)");
	sd_bus_message_close_container(reply);
	sd_bus_message_append(reply, "ss", ic->title.c_str(), "");
	return sd_bus_message_close_container(reply);
}

const sd_bus_vtable kVtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Activate", "ii", "", m_activate, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SecondaryActivate", "ii", "", m_secondary, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ContextMenu", "ii", "", m_menu, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Scroll", "is", "", m_scroll, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("Category", "s", p_string, 0, 0),
	SD_BUS_PROPERTY("Id", "s", p_string, 0, 0),
	SD_BUS_PROPERTY("Title", "s", p_string, 0, 0),
	SD_BUS_PROPERTY("Status", "s", p_string, 0, 0),
	SD_BUS_PROPERTY("IconName", "s", p_string, 0, 0),
	SD_BUS_PROPERTY("IconThemePath", "s", p_string, 0, 0),
	SD_BUS_PROPERTY("OverlayIconName", "s", p_string, 0, 0),
	SD_BUS_PROPERTY("AttentionIconName", "s", p_string, 0, 0),
	SD_BUS_PROPERTY("WindowId", "i", p_window, 0, 0),
	SD_BUS_PROPERTY("IconPixmap", "a(iiay)", p_pixmap, 0, 0),
	SD_BUS_PROPERTY("OverlayIconPixmap", "a(iiay)", p_empty_pixmap, 0, 0),
	SD_BUS_PROPERTY("AttentionIconPixmap", "a(iiay)", p_empty_pixmap, 0, 0),
	SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", p_tooltip, 0, 0),
	SD_BUS_PROPERTY("ItemIsMenu", "b", p_ismenu, 0, 0),
	SD_BUS_PROPERTY("Menu", "o", p_menu, 0, 0),
	SD_BUS_SIGNAL("NewIcon", "", 0),
	SD_BUS_SIGNAL("NewTitle", "", 0),
	SD_BUS_SIGNAL("NewStatus", "s", 0),
	SD_BUS_VTABLE_END,
};

// ---- docking and undocking ----------------------------------------------------------

void undock(xcb_window_t window) {
	auto it = icons.find(window);
	if (it == icons.end()) return;
	Icon &ic = *it->second;
	if (ic.pending) sd_event_source_unref(ic.pending);
	if (ic.slot) sd_bus_slot_unref(ic.slot);
	if (ic.bus) {
		sd_bus_detach_event(ic.bus);
		sd_bus_flush_close_unref(ic.bus);   // its name goes: the tray drops the item
	}
	if (ic.damage) xcb_damage_destroy(X, ic.damage);
	if (ic.container) xcb_destroy_window(X, ic.container);
	xcb_flush(X);
	icons.erase(it);
}

void dock(xcb_window_t window) {
	if (icons.count(window)) return;
	auto ic = std::make_unique<Icon>();
	ic->window = window;
	ic->title = window_title(window);

	// Its own window to sit in, far off screen (Xwayland keeps drawing it;
	// Dusk never shows it), kept as a picture we can read (Composite).
	const bool argb = argb_visual != 0;
	ic->container = xcb_generate_id(X);
	const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK |
		(argb ? XCB_CW_COLORMAP : 0);
	const uint32_t values[] = {0, 0, 1, XCB_EVENT_MASK_NO_EVENT, argb_colormap};
	xcb_create_window(X, argb ? 32 : XCB_COPY_FROM_PARENT, ic->container, screen->root, -20000, -20000, kSize, kSize, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, argb ? argb_visual : screen->root_visual, mask, values);
	const char cls[] = "dusk-xembed\0dusk-xembed";
	xcb_change_property(X, XCB_PROP_MODE_REPLACE, ic->container, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof cls, cls);
	// A notification-type window: compositors don't give it the keyboard.
	const xcb_atom_t kind = atom("_NET_WM_WINDOW_TYPE_NOTIFICATION");
	xcb_change_property(X, XCB_PROP_MODE_REPLACE, ic->container, atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, 32, 1, &kind);
	xcb_composite_redirect_subwindows(X, ic->container, XCB_COMPOSITE_REDIRECT_MANUAL);
	xcb_map_window(X, ic->container);

	if (auto *wa = xcb_get_window_attributes_reply(X, xcb_get_window_attributes(X, window), nullptr)) {
		ic->ignore_unmap = wa->map_state != XCB_MAP_STATE_UNMAPPED;
		std::free(wa);
	}
	const uint32_t ev_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(X, window, XCB_CW_EVENT_MASK, &ev_mask);
	xcb_reparent_window(X, window, ic->container, 0, 0);
	const uint32_t size[] = {kSize, kSize};
	xcb_configure_window(X, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
	xcb_map_window(X, window);
	// XEMBED_EMBEDDED_NOTIFY: it's in.
	xcb_client_message_event_t em{};
	em.response_type = XCB_CLIENT_MESSAGE;
	em.format = 32;
	em.window = window;
	em.type = A_XEMBED;
	em.data.data32[0] = XCB_CURRENT_TIME;
	em.data.data32[1] = 0;
	em.data.data32[3] = ic->container;
	em.data.data32[4] = 0;
	xcb_send_event(X, 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&em));
	ic->damage = xcb_generate_id(X);
	xcb_damage_create(X, ic->damage, window, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
	xcb_flush(X);

	// On the session bus: a connection (and so a name) per icon; closing it
	// is how the tray learns it's gone.
	if (sd_bus_open_user(&ic->bus) >= 0) {
		sd_bus_add_object_vtable(ic->bus, &ic->slot, "/StatusNotifierItem", "org.kde.StatusNotifierItem", kVtable,
			ic.get());
		sd_bus_attach_event(ic->bus, loop, 0);
		sd_bus_call_method_async(ic->bus, nullptr, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem", nullptr, nullptr, "s", "/StatusNotifierItem");
	}
	Icon &ref = *ic;
	icons[window] = std::move(ic);
	schedule_grab(ref);
}

// ---- the X side ----------------------------------------------------------------------

xcb_window_t manager = 0;

bool become_tray() {
	const std::string sel_name = "_NET_SYSTEM_TRAY_S" + std::to_string(0);
	const xcb_atom_t sel = atom(sel_name.c_str());
	auto *owner = xcb_get_selection_owner_reply(X, xcb_get_selection_owner(X, sel), nullptr);
	const bool taken = owner && owner->owner != XCB_NONE;
	std::free(owner);
	if (taken) {
		std::fprintf(stderr, "dusk-xembed: another tray is running\n");
		return false;
	}
	manager = xcb_generate_id(X);
	xcb_create_window(X, XCB_COPY_FROM_PARENT, manager, screen->root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
		screen->root_visual, 0, nullptr);
	const uint32_t orientation = 0;   // horizontal
	xcb_change_property(X, XCB_PROP_MODE_REPLACE, manager, atom("_NET_SYSTEM_TRAY_ORIENTATION"), XCB_ATOM_CARDINAL, 32, 1,
		&orientation);
	if (argb_visual) {
		xcb_change_property(X, XCB_PROP_MODE_REPLACE, manager, atom("_NET_SYSTEM_TRAY_VISUAL"), XCB_ATOM_VISUALID, 32, 1,
			&argb_visual);
	}
	xcb_set_selection_owner(X, manager, sel, XCB_CURRENT_TIME);
	// MANAGER: apps waiting for a tray know there is one now.
	xcb_client_message_event_t ev{};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = screen->root;
	ev.type = atom("MANAGER");
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = sel;
	ev.data.data32[2] = manager;
	xcb_send_event(X, 0, screen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<const char *>(&ev));
	xcb_flush(X);
	return true;
}

void find_argb_visual() {
	for (auto d = xcb_screen_allowed_depths_iterator(screen); d.rem; xcb_depth_next(&d)) {
		if (d.data->depth != 32) continue;
		for (auto v = xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v)) {
			if (v.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
				argb_visual = v.data->visual_id;
				argb_colormap = xcb_generate_id(X);
				xcb_create_colormap(X, XCB_COLORMAP_ALLOC_NONE, argb_colormap, screen->root, argb_visual);
				return;
			}
		}
	}
}

int on_x(sd_event_source *, int, uint32_t, void *) {
	while (xcb_generic_event_t *e = xcb_poll_for_event(X)) {
		const uint8_t type = e->response_type & 0x7f;
		if (type == XCB_CLIENT_MESSAGE) {
			auto *cm = reinterpret_cast<xcb_client_message_event_t *>(e);
			if (cm->type == A_OPCODE && cm->data.data32[1] == 0) dock(cm->data.data32[2]);   // SYSTEM_TRAY_REQUEST_DOCK
		} else if (type == XCB_DESTROY_NOTIFY) {
			undock(reinterpret_cast<xcb_destroy_notify_event_t *>(e)->window);
		} else if (type == XCB_UNMAP_NOTIFY) {
			auto *u = reinterpret_cast<xcb_unmap_notify_event_t *>(e);
			auto it = icons.find(u->window);
			if (it != icons.end()) {
				if (it->second->ignore_unmap) it->second->ignore_unmap = false;
				else undock(u->window);   // hidden by its app
			}
		} else if (type == XCB_REPARENT_NOTIFY) {
			auto *r = reinterpret_cast<xcb_reparent_notify_event_t *>(e);
			auto it = icons.find(r->window);
			if (it != icons.end() && r->parent != it->second->container) undock(r->window);
		} else if (type == XCB_PROPERTY_NOTIFY) {
			auto *p = reinterpret_cast<xcb_property_notify_event_t *>(e);
			auto it = icons.find(p->window);
			if (it != icons.end() && (p->atom == A_WM_NAME || p->atom == A_NET_WM_NAME)) {
				it->second->title = window_title(p->window);
				if (it->second->bus) sd_bus_emit_signal(it->second->bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
					"NewTitle", "");
			}
		} else if (type == XCB_SELECTION_CLEAR) {
			std::fprintf(stderr, "dusk-xembed: another tray took over\n");
			sd_event_exit(loop, 0);
		} else if (damage_event_base && type == damage_event_base + XCB_DAMAGE_NOTIFY) {
			auto *d = reinterpret_cast<xcb_damage_notify_event_t *>(e);
			xcb_damage_subtract(X, d->damage, XCB_NONE, XCB_NONE);
			auto it = icons.find(d->drawable);
			if (it != icons.end()) schedule_grab(*it->second);
		}
		std::free(e);
	}
	xcb_flush(X);
	if (xcb_connection_has_error(X)) sd_event_exit(loop, 0);   // Xwayland went away
	return 0;
}

} // namespace

int main() {
	X = xcb_connect(nullptr, nullptr);
	if (!X || xcb_connection_has_error(X)) {
		std::fprintf(stderr, "dusk-xembed: no X server (DISPLAY)\n");
		return 1;
	}
	screen = xcb_setup_roots_iterator(xcb_get_setup(X)).data;
	A_OPCODE = atom("_NET_SYSTEM_TRAY_OPCODE");
	A_XEMBED = atom("_XEMBED");
	A_XEMBED_INFO = atom("_XEMBED_INFO");
	A_WM_NAME = XCB_ATOM_WM_NAME;
	A_NET_WM_NAME = atom("_NET_WM_NAME");
	A_UTF8 = atom("UTF8_STRING");
	// Composite (to read the icons' pictures) and Damage (to know when they change).
	auto *cv = xcb_composite_query_version_reply(X, xcb_composite_query_version(X, 0, 4), nullptr);
	auto *dv = xcb_damage_query_version_reply(X, xcb_damage_query_version(X, 1, 1), nullptr);
	const xcb_query_extension_reply_t *dq = xcb_get_extension_data(X, &xcb_damage_id);
	if (!cv || !dv || !dq || !dq->present) {
		std::fprintf(stderr, "dusk-xembed: the X server lacks Composite or Damage\n");
		return 1;
	}
	std::free(cv);
	std::free(dv);
	damage_event_base = dq->first_event;
	find_argb_visual();
	if (!become_tray()) return 0;

	sd_event_default(&loop);
	sd_event_add_io(loop, nullptr, xcb_get_file_descriptor(X), EPOLLIN, on_x, nullptr);
	sd_event_add_post(loop, nullptr, [](sd_event_source *, void *) -> int {
		xcb_flush(X);
		return 0;
	}, nullptr);
	on_x(nullptr, 0, 0, nullptr);
	sd_event_loop(loop);
	while (!icons.empty()) undock(icons.begin()->first);
	xcb_disconnect(X);
	return 0;
}
