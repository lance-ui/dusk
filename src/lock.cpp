#include "lock.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ccentre.hpp"
#include "crash.hpp"
#include "draw.hpp"
#include "glyphs.hpp"
#include "launcher.hpp"
#include "notify.hpp"
#include "osk.hpp"
#include "overview.hpp"
#include "system.hpp"

namespace dusk {

namespace {

constexpr int kFieldW = 320, kFieldH = 48, kFace = 104;
constexpr size_t kLockNotifyRows = 3;   // more apps than this: "N more from other apps"

bool no_input(wlr_scene_buffer *, double *, double *) {
	return false;
}

bool inside(const wlr_box &b, double x, double y) {
	return x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height;
}

bool readable(const std::string &path) {
	return !path.empty() && access(path.c_str(), R_OK) == 0;
}

// ---- Checking the password: PAM (locked) ----------------------------------------------

int pam_conversation(int n, const pam_message **msg, pam_response **resp, void *data) {
	const auto *password = static_cast<const std::string *>(data);
	auto *r = static_cast<pam_response *>(calloc(static_cast<size_t>(n), sizeof(pam_response)));
	if (!r) return PAM_BUF_ERR;
	for (int i = 0; i < n; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
			r[i].resp = strdup(password->c_str());
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

bool pam_check(const std::string &user, const std::string &password) {
	// Dusk's own PAM file (./build.sh installs it: "auth include login"),
	// or the login one itself. Never a name with no file: PAM would use
	// "other", which says no to everything, and you couldn't unlock.
	const char *service = access("/etc/pam.d/dusk-lock", R_OK) == 0 ? "dusk-lock" : "login";
	const pam_conv conv{pam_conversation, const_cast<std::string *>(&password)};
	pam_handle_t *handle = nullptr;
	if (pam_start(service, user.c_str(), &conv, &handle) != PAM_SUCCESS) {
		return false;
	}
	int r = pam_authenticate(handle, 0);
	if (r == PAM_SUCCESS) {
		pam_setcred(handle, PAM_REFRESH_CRED);   // e.g. a new Kerberos ticket; harmless otherwise
	}
	pam_end(handle, r);
	return r == PAM_SUCCESS;
}

} // namespace

// ---- Lifetime -----------------------------------------------------------------------------

LockScreen::LockScreen(Server *server) : server_(server) {
	if (pipe2(pipe_, O_CLOEXEC | O_NONBLOCK) == 0) {
		pipe_source_ = wl_event_loop_add_fd(server_->loop, pipe_[0], WL_EVENT_READABLE, &LockScreen::handle_pipe, this);
	}
	clock_timer_ = wl_event_loop_add_timer(server_->loop, &LockScreen::handle_clock, this);
	blink_timer_ = wl_event_loop_add_timer(server_->loop, &LockScreen::handle_blink, this);
	if (const passwd *pw = getpwuid(getuid())) {
		user_.name = pw->pw_name;
		const std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
		user_.real = gecos.substr(0, gecos.find(','));
	}
	find_face();
	watch_logind();
}

// Your picture: ~/.face, or the one GNOME's settings keep.
void LockScreen::find_face() {
	user_.face.clear();
	const passwd *pw = getpwuid(getuid());
	const std::string home = pw && pw->pw_dir ? pw->pw_dir : "";
	for (const std::string &f : {home + "/.face", "/var/lib/AccountsService/icons/" + user_.name}) {
		if (readable(f)) {
			user_.face = f;
			break;
		}
	}
}

LockScreen::~LockScreen() {
	if (worker_.joinable()) worker_.join();
	destroy_screens();
	if (face_) cairo_surface_destroy(face_);
	if (pipe_source_) wl_event_source_remove(pipe_source_);
	if (clock_timer_) wl_event_source_remove(clock_timer_);
	if (blink_timer_) wl_event_source_remove(blink_timer_);
	if (sleep_close_timer_) wl_event_source_remove(sleep_close_timer_);
	for (int fd : pipe_) {
		if (fd >= 0) ::close(fd);
	}
	if (bus_source_) wl_event_source_remove(bus_source_);
	if (sleep_slot_) sd_bus_slot_unref(sleep_slot_);
	if (lock_slot_) sd_bus_slot_unref(lock_slot_);
	if (system_bus_) sd_bus_flush_close_unref(system_bus_);
	if (sleep_fd_ >= 0) ::close(sleep_fd_);
}

void LockScreen::lock() {
	if (active_) return;
	active_ = true;
	crumb("locked");
	password_.clear();
	message_.clear();
	message_is_error_ = false;
	hover_.clear();
	curtain_ = server_->config.lock_curtain;
	last_input_ = now_ms();
	focused_ = true;
	reveal_ = false;
	caret_on_ = true;
	caps_lock_ = false;
	if (wlr_keyboard *kb = wlr_seat_get_keyboard(server_->seat)) {
		caps_lock_ = kb->modifiers.locked & WLR_MODIFIER_CAPS;
	}
	find_face();   // it may have been set (or changed) since Dusk started
	// Nothing open on top of the apps, and the apps get no keys or clicks.
	server_->close_popups();
	if (server_->osk) server_->osk->close();
	if (server_->launcher) server_->launcher->close();
	if (server_->control_centre) server_->control_centre->close();
	if (server_->overview && server_->overview->is_open()) server_->overview->close();
	wlr_seat_keyboard_notify_clear_focus(server_->seat);
	wlr_seat_pointer_notify_clear_focus(server_->seat);
	server_->set_cursor("default");
	build();
	wl_event_source_timer_update(clock_timer_, 1000);
	wl_event_source_timer_update(blink_timer_, 530);
}

// The cursor in the field blinks (just the form is drawn again).
int LockScreen::handle_blink(void *data) {
	auto *self = static_cast<LockScreen *>(data);
	if (!self->active_) return 0;
	if (self->focused_ && !self->curtain_ && !self->checking_) {
		self->caret_on_ = !self->caret_on_;
		self->redraw_form();
	}
	wl_event_source_timer_update(self->blink_timer_, 530);
	return 0;
}

void LockScreen::unlock() {
	active_ = false;
	crumb("unlocked");
	std::fill(password_.begin(), password_.end(), '\0');
	password_.clear();
	caret_ = 0;
	destroy_screens();
	wl_event_source_timer_update(clock_timer_, 0);
	wl_event_source_timer_update(blink_timer_, 0);
	// Back to where you were.
	if (View *v = server_->focused) {
		server_->focus_surface(v->surface());
	}
	server_->process_motion(0);
}

void LockScreen::refresh() {
	if (!active_) return;
	// Screens that came or went; the rest keeps its nodes (only what changed
	// is made again).
	bool changed = false;
	for (Output *o : server_->outputs) {
		if (!o->enabled()) continue;
		const bool have = std::any_of(screens_.begin(), screens_.end(), [o](const auto &s) { return s->output == o; });
		if (!have) changed = true;
	}
	for (auto &s : screens_) {
		if (!s->output->enabled()) changed = true;
	}
	if (changed) {
		destroy_screens();
		build();
		return;
	}
	for (auto &s : screens_) make_background(*s);
	redraw();
}

void LockScreen::output_gone(Output *output) {
	for (auto it = screens_.begin(); it != screens_.end(); ++it) {
		if ((*it)->output == output) {
			wlr_scene_node_destroy(&(*it)->tree->node);
			screens_.erase(it);
			break;
		}
	}
	if (active_) redraw();
}

// ---- Building and drawing -----------------------------------------------------------------

namespace {

// A quick blur for a small picture: three box blurs (close to a Gaussian),
// across then down, on premultiplied ARGB.
void box_blur(cairo_surface_t *img, int radius) {
	if (radius < 1) return;
	cairo_surface_flush(img);
	const int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
	const int stride = cairo_image_surface_get_stride(img) / 4;
	auto *px = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(img));
	std::vector<uint32_t> line(static_cast<size_t>(std::max(w, h)));
	auto pass = [&](int count, int len, auto at) {
		for (int i = 0; i < count; i++) {
			for (int j = 0; j < len; j++) line[static_cast<size_t>(j)] = *at(i, j);
			uint32_t sum[4] = {0, 0, 0, 0};
			auto add = [&](uint32_t v, int sign) {
				for (int c = 0; c < 4; c++) sum[c] += sign * static_cast<int>((v >> (c * 8)) & 0xff);
			};
			auto get = [&](int j) { return line[static_cast<size_t>(std::clamp(j, 0, len - 1))]; };
			for (int j = -radius; j <= radius; j++) add(get(j), 1);
			const int n = 2 * radius + 1;
			for (int j = 0; j < len; j++) {
				uint32_t v = 0;
				for (int c = 0; c < 4; c++) v |= std::min<uint32_t>(255, sum[c] / n) << (c * 8);
				*at(i, j) = v;
				add(get(j + radius + 1), 1);
				add(get(j - radius), -1);
			}
		}
	};
	for (int k = 0; k < 3; k++) {
		pass(h, w, [&](int y, int x) { return &px[y * stride + x]; });
		pass(w, h, [&](int x, int y) { return &px[y * stride + x]; });
	}
	cairo_surface_mark_dirty(img);
}

} // namespace

LockScreen::Screen *LockScreen::form_screen() const {
	for (const auto &s : screens_) {
		if (s->output->is_primary()) return s.get();
	}
	return screens_.empty() ? nullptr : screens_.front().get();
}

void LockScreen::destroy_screens() {
	for (auto &s : screens_) {
		wlr_scene_node_destroy(&s->tree->node);
	}
	screens_.clear();
	form_hots_.clear();
}

void LockScreen::build() {
	for (Output *o : server_->outputs) {
		if (!o->enabled()) continue;
		const wlr_box b = o->box();
		auto s = std::make_unique<Screen>();
		s->output = o;
		s->tree = wlr_scene_tree_create(server_->layer_lock);
		const float base[4] = {0.063f, 0.063f, 0.094f, 1.0f};
		s->base = wlr_scene_rect_create(s->tree, b.width, b.height, base);
		wlr_scene_node_set_position(&s->base->node, b.x, b.y);
		for (wlr_scene_buffer **node : {&s->bg, &s->clock, &s->form, &s->media, &s->status}) {
			*node = wlr_scene_buffer_create(s->tree, nullptr);
			(*node)->point_accepts_input = no_input;
		}
		make_background(*s);
		screens_.push_back(std::move(s));
	}
	redraw();
}

// The background, made once: the wallpaper (or a picture, or a colour)
// covering the screen, blurred and darkened as set. Made again only when one
// of those changes.
void LockScreen::make_background(Screen &s) {
	const Config &c = server_->config;
	const wlr_box b = s.output->box();
	const float scale = s.output->output->scale;
	wlr_buffer *wall = s.output->wallpaper ? s.output->wallpaper->buffer : nullptr;
	char color[16];
	std::snprintf(color, sizeof color, "%.3f%.3f%.3f", c.lock_color.r, c.lock_color.g, c.lock_color.b);
	const bool picture = (c.lock_background == "picture" || c.lock_background == "picture_blur") && readable(c.lock_picture);
	const bool use_wall = !picture && c.lock_background != "color";
	std::string key = c.lock_background + "|" + std::to_string(c.lock_blur) + "|" + std::to_string(c.lock_dim) + "|" +
		color + "|" + std::to_string(b.width) + "x" + std::to_string(b.height) + "@" + std::to_string(scale) + "|" +
		(picture ? c.lock_picture : "") + "|" + (use_wall ? std::to_string(reinterpret_cast<uintptr_t>(wall)) : "");
	wlr_scene_node_set_position(&s.bg->node, b.x, b.y);
	if (key == s.bg_key) return;
	s.bg_key = key;

	Canvas canvas(b.width, b.height, scale);
	cairo_t *cr = canvas.cr();
	set_color(cr, c.lock_background == "color" ? c.lock_color : rgb(0x101018));
	cairo_paint(cr);

	// The picture to start from: the wallpaper as Dusk already has it, or the
	// lock screen's own picture.
	cairo_surface_t *src = nullptr;
	void *data = nullptr;
	bool wall_open = false;
	if (picture) {
		src = load_image_sized(c.lock_picture, static_cast<int>(b.width * scale * 2), static_cast<int>(b.height * scale * 2));
	} else if (use_wall && wall) {
		uint32_t format = 0;
		size_t stride = 0;
		if (wlr_buffer_begin_data_ptr_access(wall, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
			wall_open = true;
			src = cairo_image_surface_create_for_data(static_cast<unsigned char *>(data), CAIRO_FORMAT_ARGB32,
				wall->width, wall->height, static_cast<int>(stride));
		}
	}
	if (use_wall && !src) {
		wlr_log(WLR_INFO, "Lock screen: no wallpaper picture to use yet on %s (%s)", s.output->name().c_str(),
			wall ? "can't read it" : "not loaded yet");
	}
	if (src && cairo_surface_status(src) == CAIRO_STATUS_SUCCESS) {
		const double iw = cairo_image_surface_get_width(src), ih = cairo_image_surface_get_height(src);
		const bool blur = (c.lock_background == "blur" || c.lock_background == "picture_blur") && c.lock_blur > 0;
		if (blur) {
			// Small, blurred, then stretched back up smoothly: soft, and quick.
			const int sw = std::max(16, b.width / 8), sh = std::max(16, b.height / 8);
			cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
			cairo_t *sc = cairo_create(small);
			const double k = std::max(sw / iw, sh / ih);
			cairo_translate(sc, (sw - iw * k) / 2, (sh - ih * k) / 2);
			cairo_scale(sc, k, k);
			cairo_set_source_surface(sc, src, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(sc), CAIRO_FILTER_GOOD);
			cairo_paint(sc);
			cairo_destroy(sc);
			box_blur(small, std::max(1, static_cast<int>(std::lround(c.lock_blur / 100.0 * 6))));
			cairo_save(cr);
			cairo_scale(cr, static_cast<double>(b.width) / sw, static_cast<double>(b.height) / sh);
			cairo_set_source_surface(cr, small, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
			cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
			cairo_paint(cr);
			cairo_restore(cr);
			cairo_surface_destroy(small);
		} else {
			cairo_save(cr);
			const double k = std::max(b.width / iw, b.height / ih);
			cairo_translate(cr, (b.width - iw * k) / 2, (b.height - ih * k) / 2);
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, src, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	if (src) cairo_surface_destroy(src);
	if (wall_open) wlr_buffer_end_data_ptr_access(wall);
	// Darker, so the clock and the form stand out.
	set_color(cr, rgb(0x000000, c.lock_dim / 100.0));
	cairo_paint(cr);
	canvas.present(s.bg);
}

std::string LockScreen::time_text() const {
	char buf[32];
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
	localtime_r(&now, &tm);
	std::strftime(buf, sizeof buf, server_->config.clock_24h ? "%H:%M" : "%l:%M", &tm);
	std::string t = buf;
	while (!t.empty() && t[0] == ' ') t.erase(0, 1);
	return t;
}

namespace {

std::string font_of(const Config &c, const char *style, double pt) {
	const std::string family = c.font.empty() ? "Sans" : c.font;
	return family + (*style ? std::string(" ") + style : "") + " " + std::to_string(static_cast<int>(std::lround(pt)));
}

// The clock's own font (Settings › Lock screen › Clock font), else the interface's.
std::string clock_font(const Config &c, double pt) {
	std::string family = c.lock_clock_font.empty() ? c.font : c.lock_clock_font;
	if (family.empty()) family = "Sans";
	return family + (c.lock_clock_weight == "bold" ? " Bold " : " Light ") + std::to_string(static_cast<int>(std::lround(pt)));
}

// Text with a soft shadow, so it reads on any picture.
void shadowed(cairo_t *cr, const std::string &t, double x, double y, double w, double h, const std::string &font,
		const Color &col, Align a) {
	draw_text(cr, t, x, y + 1.5, w, h, font.c_str(), rgb(0x000000, 0.32), a);
	draw_text(cr, t, x, y, w, h, font.c_str(), col, a);
}

// Just the outline of the letters (the "Outline" clock).
void outlined(cairo_t *cr, const std::string &t, double x, double y, double w, double h, const std::string &font,
		const Color &col, Align a, double line) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font.c_str());
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, t.c_str(), -1);
	int tw = 0, th = 0;
	pango_layout_get_pixel_size(layout, &tw, &th);
	const double tx = a == Align::Left ? x : a == Align::Center ? x + (w - tw) / 2.0 : x + w - tw;
	const double ty = y + (h - th) / 2.0;
	cairo_save(cr);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_new_path(cr);
	cairo_move_to(cr, tx, ty + 1.5);
	pango_cairo_layout_path(cr, layout);
	set_color(cr, rgb(0x000000, 0.28));
	cairo_set_line_width(cr, line + 1);
	cairo_stroke(cr);
	cairo_move_to(cr, tx, ty);
	pango_cairo_layout_path(cr, layout);
	set_color(cr, col);
	cairo_set_line_width(cr, line);
	cairo_stroke(cr);
	cairo_restore(cr);
	g_object_unref(layout);
}

constexpr int kMediaW = 380, kMediaH = 92;

} // namespace

// Where everything goes on a screen, from the settings: the clock (with the
// date and the message), the music card and the password form. Worked out
// in one place so nothing overlaps whichever way they're set.
LockScreen::Layout LockScreen::layout(const Screen &s) const {
	const Config &c = server_->config;
	const wlr_box b = s.output->box();
	const double W = b.width, H = b.height;
	Layout L;
	const double size = c.lock_clock_size == "small" ? 0.7 : c.lock_clock_size == "large" ? 1.35 : 1.0;
	L.pt = std::clamp(H * 0.068 * size, 30.0, 170.0);
	L.dpt = std::clamp(H * 0.015, 12.0, 22.0);
	L.mpt = std::clamp(H * 0.011, 10.0, 16.0);
	const bool clock = c.lock_clock != "hidden";
	const std::string &style = c.lock_clock_style;
	if (clock) {
		if (style == "stacked") {
			const double p2 = L.pt * 1.3;
			L.time_h = p2 * 1.3 * 2 - p2 * 0.34;
		} else if (style == "analog") {
			L.time_h = L.pt * 2.5 + 16;
		} else {
			L.time_h = L.pt * 1.75;
		}
	}
	const double date_h = clock && c.lock_date ? L.dpt * 2 : 0;
	const double msg_h = c.lock_message.empty() ? 0 : L.mpt * 2.6;
	const int ch = static_cast<int>(L.time_h + date_h + msg_h + 12);
	const std::string &where = c.lock_clock;
	L.align = where == "center" || where == "hidden" ? Align::Center : Align::Left;
	if (!clock && msg_h <= 0) {
		L.clock = wlr_box{b.x, b.y + static_cast<int>(H * 0.08), 0, 0};
	} else if (where == "left") {
		L.clock = wlr_box{b.x + static_cast<int>(W * 0.06), b.y + static_cast<int>(H * 0.07), static_cast<int>(W * 0.6), ch};
	} else if (where == "corner") {
		L.clock = wlr_box{b.x + static_cast<int>(W * 0.06), b.y + static_cast<int>(H * 0.92) - ch, static_cast<int>(W * 0.6), ch};
	} else {
		const double top = style == "analog" || style == "stacked" ? 0.08 : 0.11;
		L.clock = wlr_box{b.x, b.y + static_cast<int>(H * top), b.width, ch};
	}

	// The music: under the clock (top left when the clock is in the corner).
	const int mx = L.align == Align::Center ? b.x + (b.width - kMediaW) / 2 : b.x + static_cast<int>(W * 0.06);
	const int my = where == "corner" ? b.y + static_cast<int>(H * 0.08) : L.clock.y + L.clock.height + 18;
	L.media = wlr_box{mx, my, kMediaW, kMediaH};

	// The form: its size, then where it goes, kept clear of what's above it.
	const int pad = c.lock_card ? 26 : 8;
	L.pad = pad;
	const int fw = c.lock_card ? 400 : 420;
	const size_t groups = notification_groups().size();
	const int fh = pad * 2 + (c.lock_avatar ? kFace + 14 : 0) + (c.lock_name ? 44 : 6) + kFieldH + 14 + 22 +
		(groups ? 10 + static_cast<int>(std::min<size_t>(groups, kLockNotifyRows)) * 34 : 0);
	int fx, fy;
	if (c.lock_form == "right") {
		fx = b.x + b.width - fw - static_cast<int>(W * 0.07);
		fy = b.y + (b.height - fh) / 2;
	} else {
		fx = b.x + (b.width - fw) / 2;
		const double at = c.lock_form == "lower" ? 0.66 : 0.52;
		fy = b.y + static_cast<int>(H * at - (c.lock_avatar ? kFace / 2.0 : 0)) - pad;
		// Below the clock and the music when they're in the same column.
		if (L.align == Align::Center) {
			int above = L.clock.y + L.clock.height + 20;
			if (media_on_ && where != "corner") above = L.media.y + L.media.height + 22;
			fy = std::max(fy, above);
		}
	}
	fy = std::min(fy, b.y + b.height - fh - 16);
	L.form = wlr_box{fx, fy, fw, fh};
	return L;
}

// The apps with notifications waiting (newest app first), and how many each.
std::vector<LockScreen::NotifyGroup> LockScreen::notification_groups() const {
	std::vector<NotifyGroup> out;
	if (!server_->config.lock_notifications || !server_->notifications) return out;
	const auto &list = server_->notifications->list();
	for (const Notification &n : list) {   // newest first
		auto g = std::find_if(out.begin(), out.end(), [&](const NotifyGroup &x) { return x.app == n.app; });
		if (g == out.end()) {
			// Only the app's own icon: a notification's picture can be private
			// (someone's photo, a message's image).
			out.push_back({n.app, n.app_icon, 1});
		} else {
			g->count++;
		}
	}
	return out;
}

void LockScreen::notifications_changed() {
	if (!active_) return;
	const size_t now = notification_groups().size();
	if (now != notify_groups_shown_) {
		// The form grows or shrinks: everything moves.
		notify_groups_shown_ = now;
		redraw();
	} else {
		redraw_form();
	}
}

void LockScreen::redraw() {
	notify_groups_shown_ = notification_groups().size();
	shown_time_ = time_text();
	for (auto &s : screens_) {
		draw_clock(*s);
		for (wlr_scene_buffer *n : {s->form, s->media, s->status}) wlr_scene_node_set_enabled(&n->node, false);
	}
	media_on_ = media_wanted();
	redraw_form();
	media_shown_.clear();
	status_changed();
}

bool LockScreen::media_wanted() const {
	const SystemStatus *st = server_->status.get();
	return server_->config.lock_media && st && st->media_available && !st->media_title.empty();
}

void LockScreen::status_changed() {
	if (!active_) return;
	// Music starting or stopping can move the form (it stays clear of the card).
	const bool on = media_wanted();
	if (on != media_on_) {
		media_on_ = on;
		redraw_form();
	}
	if (Screen *s = form_screen()) {
		draw_media(*s);
		draw_status(*s);
	}
}

// What's playing, under the clock: the cover, the title and artist, and
// previous / play-pause / next.
void LockScreen::draw_media(Screen &s) {
	const SystemStatus *st = server_->status.get();
	const bool show = media_wanted();
	// Where it's got to, now (it moves on while playing).
	double pos = st ? st->media_position : 0;
	if (st && st->media_playing) pos += (now_ms() - st->media_read_at) / 1000.0;
	if (st && st->media_length > 0) pos = std::clamp(pos, 0.0, st->media_length);
	const std::string key = show ? st->media_title + "|" + st->media_artist + "|" + st->media_art + "|" +
		(st->media_playing ? "1" : "0") + "|" + hover_ + "|" + std::to_string(static_cast<int>(pos)) : "";
	if (key == media_shown_ && !key.empty()) return;
	media_shown_ = key;
	media_hots_.clear();
	if (!show) {
		wlr_scene_node_set_enabled(&s.media->node, false);
		return;
	}
	const Config &c = server_->config;
	const Layout L = layout(s);
	const int w = kMediaW, h = kMediaH, x = L.media.x, y = L.media.y;
	Canvas canvas(w, h, s.output->output->scale);
	cairo_t *cr = canvas.cr();
	fill_rounded(cr, 0, 0, w, h, 20, rgb(0x000000, 0.28));
	rounded_rect(cr, 0.5, 0.5, w - 1, h - 1, 20);
	set_color(cr, rgb(0xffffff, 0.12));
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	const int art = 56;
	bool drew_art = false;
	if (!st->media_art.empty() && readable(st->media_art)) {
		if (cairo_surface_t *img = load_image_sized(st->media_art, art * 2, art * 2)) {
			cairo_save(cr);
			rounded_rect(cr, 10, 10, art, art, 12);
			cairo_clip(cr);
			const double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
			const double k = std::max(art / iw, art / ih);
			cairo_translate(cr, 10 + (art - iw * k) / 2, 10 + (art - ih * k) / 2);
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, img, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
			cairo_surface_destroy(img);
			drew_art = true;
		}
	}
	if (!drew_art) {
		fill_rounded(cr, 10, 10, art, art, 12, rgb(0xffffff, 0.14));
		draw_glyph(cr, "speaker-on", 10 + art / 2.0, 10 + art / 2.0, 20, rgb(0xffffff, 0.8));
	}
	const double tx = 10 + art + 12, tw = w - tx - 3 * 34 - 14;
	draw_text(cr, st->media_title, tx, 12, tw, 24, font_of(c, "Bold", 11).c_str(), rgb(0xffffff));
	draw_text(cr, st->media_artist, tx, 34, tw, 20, font_of(c, "", 10).c_str(), rgb(0xffffff, 0.75));
	// How far in: a thin bar along the bottom (click it to jump there), with
	// the time on each side.
	if (st->media_length > 0) {
		auto clock = [](double t) {
			const int sec = static_cast<int>(t);
			char buf[16];
			std::snprintf(buf, sizeof buf, "%d:%02d", sec / 60, sec % 60);
			return std::string(buf);
		};
		const double by = h - 16, bx0 = 10 + 38, bw = w - 10 - 38 - 48;
		const std::string fs = font_of(c, "", 8);
		draw_text(cr, clock(pos), 10, by - 8, 36, 16, fs.c_str(), rgb(0xffffff, 0.7), Align::Right);
		draw_text(cr, clock(st->media_length), bx0 + bw + 6, by - 8, 40, 16, fs.c_str(), rgb(0xffffff, 0.7));
		const bool on = hover_ == "media-seek";
		fill_rounded(cr, bx0, by - (on ? 3 : 2), bw, on ? 6 : 4, 3, rgb(0xffffff, 0.22));
		const double done = bw * pos / st->media_length;
		fill_rounded(cr, bx0, by - (on ? 3 : 2), std::max(on ? 6.0 : 4.0, done), on ? 6 : 4, 3, theme::accent);
		media_bar_ = wlr_box{x + static_cast<int>(bx0), y + static_cast<int>(by - 8), static_cast<int>(bw), 16};
		media_hots_.push_back({"media-seek", media_bar_});
	}
	double bx = w - 14 - 3 * 34;
	for (const char *id : {"previous", "play-pause", "next"}) {
		const bool main = std::string(id) == "play-pause";
		const std::string hid = std::string("media:") + id;
		const double cx = bx + 17, cy = h / 2.0;
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, main ? 17 : 15, 0, 2 * M_PI);
		set_color(cr, main ? (hover_ == hid ? mix(theme::accent, rgb(0xffffff), 0.18) : theme::accent)
			: rgb(0xffffff, hover_ == hid ? 0.22 : 0.0));
		cairo_fill(cr);
		const char *glyph = main ? (st->media_playing ? "pause" : "play") : id;
		draw_glyph(cr, glyph, cx, cy, 15, main ? theme::on_accent : rgb(0xffffff));
		media_hots_.push_back({hid, wlr_box{x + static_cast<int>(bx), y + static_cast<int>(cy - 17), 34, 34}});
		bx += 34;
	}
	canvas.present(s.media);
	wlr_scene_node_set_position(&s.media->node, x, y);
	wlr_scene_node_set_enabled(&s.media->node, true);
}

// Top right, like a phone: Wi-Fi, sound and the battery.
void LockScreen::draw_status(Screen &s) {
	const SystemStatus *st = server_->status.get();
	const Config &c = server_->config;
	if (!st || !c.lock_status) {
		wlr_scene_node_set_enabled(&s.status->node, false);
		return;
	}
	const wlr_box b = s.output->box();
	std::vector<std::string> icons;
	if (st->net_kind == "ethernet") icons.push_back("wired");
	else if (st->net_kind == "wifi") icons.push_back(wifi_icon(st->wifi_signal));
	if (st->has_audio) icons.push_back(speaker_icon(st->volume, st->muted));
	const std::string pct = st->has_battery ? std::to_string(st->battery) + "%" : "";
	const std::string f = font_of(c, "Bold", 10);
	const int w = static_cast<int>(icons.size()) * 26 + (st->has_battery ? 26 + text_width(pct, f.c_str()) + 6 : 0) + 8;
	const int h = 28;
	if (w <= 8) {
		wlr_scene_node_set_enabled(&s.status->node, false);
		return;
	}
	Canvas canvas(w, h, s.output->output->scale);
	cairo_t *cr = canvas.cr();
	double x = 4;
	for (const std::string &icon : icons) {
		draw_glyph(cr, icon, x + 10, h / 2.0, 16, rgb(0xffffff));
		x += 26;
	}
	if (st->has_battery) {
		draw_glyph(cr, battery_icon(st->battery, st->charging), x + 10, h / 2.0, 16, rgb(0xffffff));
		x += 24;
		draw_text(cr, pct, x, 0, w - x, h, f.c_str(), rgb(0xffffff));
	}
	canvas.present(s.status);
	wlr_scene_node_set_position(&s.status->node, b.x + b.width - w - 20, b.y + 16);
	wlr_scene_node_set_enabled(&s.status->node, true);
}

void LockScreen::redraw_form() {
	form_hots_.clear();
	if (Screen *s = form_screen()) {
		if (curtain_) {
			// Just the clock: any key or click brings the rest.
			wlr_scene_node_set_enabled(&s->form->node, false);
			return;
		}
		draw_form(*s);
	}
}

void LockScreen::raise_curtain() {
	last_input_ = now_ms();
	if (!curtain_) return;
	curtain_ = false;
	redraw_form();
}

// The clock: digital, hours over minutes (stacked), just the outline, or a
// clock face; with the date and your message under it.
void LockScreen::draw_clock(Screen &s) {
	const Config &c = server_->config;
	const Layout L = layout(s);
	if (L.clock.width <= 0 || L.clock.height <= 0) {
		wlr_scene_node_set_enabled(&s.clock->node, false);
		return;
	}
	const int width = L.clock.width, height = L.clock.height;
	Canvas canvas(width, height, s.output->output->scale);
	cairo_t *cr = canvas.cr();
	const Align a = L.align;
	const Color col = c.lock_clock_accent ? theme::accent : rgb(0xffffff);
	const double pt = L.pt;
	double y = 0;
	if (c.lock_clock != "hidden") {
		const std::string &style = c.lock_clock_style;
		const std::time_t now = std::time(nullptr);
		std::tm tm{};
		localtime_r(&now, &tm);
		if (style == "stacked") {
			// Big hours over the minutes; the minutes in the accent colour.
			const double p2 = pt * 1.3, lh = p2 * 1.3;
			char hh[8], mm[8];
			std::strftime(hh, sizeof hh, c.clock_24h ? "%H" : "%I", &tm);
			std::strftime(mm, sizeof mm, "%M", &tm);
			const std::string f = clock_font(c, p2);
			shadowed(cr, hh, 0, 0, width, lh, f, col, a);
			shadowed(cr, mm, 0, lh - p2 * 0.34, width, lh, f,
				c.lock_clock_accent ? mix(theme::accent, rgb(0xffffff), 0.45) : theme::accent, a);
		} else if (style == "analog") {
			const double d = pt * 2.5, r = d / 2;
			const double cx = a == Align::Center ? width / 2.0 : r + 4, cy = r + 8;
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy + 2, r, 0, 2 * M_PI);
			set_color(cr, rgb(0x000000, 0.18));
			cairo_fill(cr);
			cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
			set_color(cr, rgb(0x000000, 0.24));
			cairo_fill_preserve(cr);
			set_color(cr, rgb(0xffffff, 0.28));
			cairo_set_line_width(cr, 2);
			cairo_stroke(cr);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			for (int i = 0; i < 12; i++) {
				const double ang = i * M_PI / 6;
				const bool big = i % 3 == 0;
				const double r1 = r * (big ? 0.80 : 0.86), r2 = r * 0.92;
				cairo_move_to(cr, cx + std::sin(ang) * r1, cy - std::cos(ang) * r1);
				cairo_line_to(cr, cx + std::sin(ang) * r2, cy - std::cos(ang) * r2);
				set_color(cr, rgb(0xffffff, big ? 0.9 : 0.5));
				cairo_set_line_width(cr, big ? std::max(3.0, r * 0.035) : 2);
				cairo_stroke(cr);
			}
			auto hand = [&](double ang, double len, double lw, const Color &hc) {
				cairo_move_to(cr, cx - std::sin(ang) * r * 0.1, cy + std::cos(ang) * r * 0.1);
				cairo_line_to(cr, cx + std::sin(ang) * len, cy - std::cos(ang) * len);
				set_color(cr, rgb(0x000000, 0.25));
				cairo_set_line_width(cr, lw + 2);
				cairo_stroke(cr);
				cairo_move_to(cr, cx - std::sin(ang) * r * 0.1, cy + std::cos(ang) * r * 0.1);
				cairo_line_to(cr, cx + std::sin(ang) * len, cy - std::cos(ang) * len);
				set_color(cr, hc);
				cairo_set_line_width(cr, lw);
				cairo_stroke(cr);
			};
			hand(((tm.tm_hour % 12) + tm.tm_min / 60.0) * M_PI / 6, r * 0.52, std::max(5.0, r * 0.07), col);
			hand(tm.tm_min * M_PI / 30, r * 0.78, std::max(3.5, r * 0.045),
				c.lock_clock_accent ? rgb(0xffffff) : theme::accent);
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, std::max(4.0, r * 0.05), 0, 2 * M_PI);
			set_color(cr, rgb(0xffffff));
			cairo_fill(cr);
		} else if (style == "outline") {
			outlined(cr, shown_time_, 0, 0, width, pt * 1.75, clock_font(c, pt), col, a, std::max(1.5, pt / 40));
		} else {
			shadowed(cr, shown_time_, 0, 0, width, pt * 1.75, clock_font(c, pt), col, a);
		}
		y = L.time_h;
		if (c.lock_date) {
			char buf[64];
			std::strftime(buf, sizeof buf, "%A %e %B", &tm);
			std::string date = buf;
			date.erase(std::unique(date.begin(), date.end(), [](char p, char q) { return p == ' ' && q == ' '; }), date.end());
			const double up = style == "digital" || style == "outline" ? pt * 0.15 : 0;
			shadowed(cr, date, 0, y - up, width, L.dpt * 2, font_of(c, "", L.dpt), rgb(0xffffff, 0.85), a);
			y += L.dpt * 2;
		}
	}
	if (!c.lock_message.empty()) {
		shadowed(cr, c.lock_message, 0, y + 4, width, L.mpt * 2.2, font_of(c, "", L.mpt), rgb(0xffffff, 0.8), a);
	}
	canvas.present(s.clock);
	wlr_scene_node_set_position(&s.clock->node, L.clock.x, L.clock.y);
	wlr_scene_node_set_enabled(&s.clock->node, true);
}

// Who (picture and name) and the password field, on the main screen (on a
// frosted card if that's on).
void LockScreen::draw_form(Screen &s) {
	const Config &c = server_->config;
	const Layout L = layout(s);
	s.form_box = L.form;
	const int fw = L.form.width, fh = L.form.height, pad = L.pad;
	const bool face = c.lock_avatar;
	const bool name = c.lock_name;
	Canvas canvas(fw, fh, s.output->output->scale);
	cairo_t *cr = canvas.cr();
	if (c.lock_card) {
		fill_rounded(cr, 0, 0, fw, fh, 30, rgb(0x000000, 0.34));
		rounded_rect(cr, 0.5, 0.5, fw - 1, fh - 1, 30);
		set_color(cr, rgb(0xffffff, 0.14));
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	const Color white = rgb(0xffffff), soft = rgb(0xffffff, 0.8);
	const double cx = fw / 2.0;
	double y = 0;
	auto hot = [&](const std::string &id, double x, double hy, double w, double h) {
		form_hots_.push_back({id, wlr_box{s.form_box.x + static_cast<int>(x), s.form_box.y + static_cast<int>(hy + pad),
			static_cast<int>(w), static_cast<int>(h)}});
	};
	// (Room round the picture: its outline must not touch the edge, or the
	// top of the circle would be cut flat.)
	cairo_translate(cr, 0, pad);
	// The picture's shape: a circle, or a square with round corners.
	const bool round = c.lock_avatar_shape != "rounded";
	auto face_path = [&](double grow) {
		cairo_new_path(cr);
		if (round) cairo_arc(cr, cx, kFace / 2.0, kFace / 2.0 + grow, 0, 2 * M_PI);
		else rounded_rect(cr, cx - kFace / 2.0 - grow, -grow, kFace + 2 * grow, kFace + 2 * grow, kFace * 0.24 + grow);
	};
	const User &u = user_;
	if (face) {
		// Changed in Settings: same file, newer picture.
		struct stat st{};
		const std::string face_key = u.face + "|" + (stat(u.face.c_str(), &st) == 0 ? std::to_string(st.st_mtime) : "");
		if (face_for_ != face_key) {
			face_for_ = face_key;
			if (face_) cairo_surface_destroy(face_);
			face_ = readable(u.face) ? load_image_sized(u.face, kFace * 2, kFace * 2) : nullptr;
		}
		cairo_save(cr);
		face_path(0);
		cairo_clip(cr);
		if (face_) {
			const double iw = cairo_image_surface_get_width(face_), ih = cairo_image_surface_get_height(face_);
			const double k = std::max(kFace / iw, kFace / ih);
			cairo_translate(cr, cx - iw * k / 2, kFace / 2.0 - ih * k / 2);
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, face_, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
		} else {
			set_color(cr, theme::accent);
			cairo_paint(cr);
			const std::string &who = u.real.empty() ? u.name : u.real;
			const std::string initial(1, static_cast<char>(std::toupper(static_cast<unsigned char>(who.empty() ? '?' : who[0]))));
			draw_text(cr, initial, cx - kFace / 2.0, 0, kFace, kFace, font_of(c, "Bold", 30).c_str(), theme::on_accent,
				Align::Center);
		}
		cairo_restore(cr);
		face_path(1);
		set_color(cr, rgb(0xffffff, 0.35));
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
		y = kFace + 14;
	}
	if (name) shadowed(cr, u.real.empty() ? u.name : u.real, 0, y, fw, 30, font_of(c, "Bold", 15), white, Align::Center);
	y += name ? 44 : 6;

	// The field: a soft pill (an accent ring when it's picked), the password
	// as dots (or as it is, with the eye), a blinking cursor, and a round
	// button to go.
	const double fx = cx - kFieldW / 2.0;
	const bool lit = focused_ || hover_ == "field";
	fill_rounded(cr, fx, y, kFieldW, kFieldH, kFieldH / 2.0, rgb(0xffffff, lit ? 0.22 : 0.16));
	rounded_rect(cr, fx + 0.75, y + 0.75, kFieldW - 1.5, kFieldH - 1.5, kFieldH / 2.0);
	if (focused_) {
		set_color(cr, theme::accent);
		cairo_set_line_width(cr, 2);
	} else {
		set_color(cr, rgb(0xffffff, password_.empty() ? 0.22 : 0.45));
		cairo_set_line_width(cr, 1);
	}
	cairo_stroke(cr);
	hot("field", fx, y, kFieldW - kFieldH - 34, kFieldH);
	const double tx = fx + 20, tw = kFieldW - 20 - kFieldH - 40;   // the room for the text
	double caret_x = tx;
	const std::string f12 = font_of(c, "", 12);
	if (checking_) {
		draw_text(cr, "Checking…", tx, y, tw, kFieldH, f12.c_str(), soft);
	} else if (password_.empty()) {
		draw_text(cr, focused_ ? "" : "Password", tx, y, tw, kFieldH, f12.c_str(), rgb(0xffffff, 0.55));
	} else if (reveal_) {
		// What's typed: the part round the cursor when it's longer than the field.
		const size_t caret = std::min(caret_, password_.size());
		size_t start = 0;
		while (start < caret && text_width(password_.substr(start, caret - start), f12.c_str()) > tw - 6) {
			start++;
			while (start < caret && (static_cast<unsigned char>(password_[start]) & 0xc0) == 0x80) start++;
		}
		std::string shown_text = password_.substr(start);
		while (!shown_text.empty() && text_width(shown_text, f12.c_str()) > tw - 2) {
			size_t cut = shown_text.size() - 1;
			while (cut > 0 && (static_cast<unsigned char>(shown_text[cut]) & 0xc0) == 0x80) cut--;
			shown_text.erase(cut);
		}
		draw_text(cr, shown_text, tx, y, tw, kFieldH, f12.c_str(), white);
		caret_x = tx + text_width(password_.substr(start, caret - start), f12.c_str()) + 1;
	} else {
		size_t chars = 0, before = 0;
		for (size_t i = 0; i < password_.size(); i++) {
			if ((static_cast<unsigned char>(password_[i]) & 0xc0) == 0x80) continue;
			chars++;
			if (i < caret_) before++;
		}
		const double gap = 14, dot_r = 3.6;   // clear space between the dots
		const size_t room = static_cast<size_t>((tw - 6) / gap);
		// More dots than fit: the ones round the cursor.
		const size_t first = before > room ? before - room : 0;
		const size_t shown = std::min(chars - first, room);
		set_color(cr, white);
		for (size_t i = 0; i < shown; i++) {
			cairo_new_path(cr);
			cairo_arc(cr, tx + dot_r + 1 + i * gap, y + kFieldH / 2.0, dot_r, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		caret_x = tx + dot_r + 1 + (before - first) * gap - gap / 2.0 - 1;
	}
	if (focused_ && caret_on_ && !checking_) {
		cairo_rectangle(cr, caret_x, y + 13, 2, kFieldH - 26);
		set_color(cr, theme::accent);
		cairo_fill(cr);
	}
	// The eye: show or hide what's typed.
	{
		const double ex = fx + kFieldW - kFieldH - 22, ey = y + kFieldH / 2.0;
		if (hover_ == "reveal") {
			cairo_new_path(cr);
			cairo_arc(cr, ex, ey, 15, 0, 2 * M_PI);
			set_color(cr, rgb(0xffffff, 0.16));
			cairo_fill(cr);
		}
		draw_glyph(cr, reveal_ ? "eye-off" : "eye", ex, ey, 16, password_.empty() ? rgb(0xffffff, 0.45) : soft);
		hot("reveal", ex - 16, ey - 16, 32, 32);
	}
	{
		const double bx = fx + kFieldW - kFieldH / 2.0 - 4, by = y + kFieldH / 2.0, br = kFieldH / 2.0 - 6;
		cairo_new_path(cr);
		cairo_arc(cr, bx, by, br, 0, 2 * M_PI);
		set_color(cr, password_.empty() ? rgb(0xffffff, hover_ == "go" ? 0.28 : 0.18)
			: hover_ == "go" ? mix(theme::accent, rgb(0xffffff), 0.15) : theme::accent);
		cairo_fill(cr);
		draw_glyph(cr, "chevron-right", bx, by, 16, password_.empty() ? soft : theme::on_accent);
		hot("go", bx - br, by - br, 2 * br, 2 * br);
	}
	y += kFieldH + 14;
	// Under it: what went wrong, or Caps Lock.
	if (!message_.empty()) {
		shadowed(cr, message_, 0, y, fw, 22, font_of(c, "", 11), message_is_error_ ? rgb(0xffb4ab) : soft, Align::Center);
	} else if (caps_lock_) {
		shadowed(cr, "Caps Lock is on", 0, y, fw, 22, font_of(c, "", 11), rgb(0xffe08a), Align::Center);
	}
	y += 22;
	// Waiting notifications: which apps and how many, never what they say.
	const auto groups = notification_groups();
	if (!groups.empty()) {
		y += 10;
		const std::string f11 = font_of(c, "", 11);
		const float scale = s.output->output->scale;
		const size_t rows = std::min<size_t>(groups.size(), kLockNotifyRows);
		size_t more = 0;
		for (size_t i = kLockNotifyRows; i < groups.size(); i++) more += groups[i].count;
		for (size_t i = 0; i < rows; i++) {
			const NotifyGroup &g = groups[i];
			std::string text = g.app.empty() ? "Notification" : g.app;
			if (i + 1 == rows && more > 0) {
				text = std::to_string(more + g.count) + " more from other apps";
			} else {
				text += g.count == 1 ? " · 1 new" : " · " + std::to_string(g.count) + " new";
			}
			const bool last_more = i + 1 == rows && more > 0;
			const double pw = std::min<double>(fw - 40, text_width(text, f11.c_str()) + (last_more ? 32 : 58));
			const double px = cx - pw / 2.0;
			fill_rounded(cr, px, y, pw, 28, 14, rgb(0x000000, 0.30));
			if (!last_more) {
				draw_icon(cr, server_->icons, g.icon, g.app, px + 8, y + 5, 18, scale);
				draw_text(cr, text, px + 34, y, pw - 44, 28, f11.c_str(), soft);
			} else {
				draw_text(cr, text, px + 16, y, pw - 32, 28, f11.c_str(), soft);
			}
			y += 34;
		}
	}
	canvas.present(s.form);
	wlr_scene_node_set_position(&s.form->node, s.form_box.x, s.form_box.y);
	wlr_scene_node_set_enabled(&s.form->node, true);
}

// ---- Input -----------------------------------------------------------------------------------

void LockScreen::set_caps_lock(bool on) {
	if (caps_lock_ == on) return;
	caps_lock_ = on;
	if (active_) redraw();
}

bool LockScreen::key(xkb_keysym_t sym, const std::string &text, uint32_t modifiers) {
	if (!active_) return false;
	// The music keys work while it's locked.
	if (server_->status) {
		const char *media = sym == XKB_KEY_XF86AudioPlay || sym == XKB_KEY_XF86AudioPause ? "play-pause"
			: sym == XKB_KEY_XF86AudioNext ? "next" : sym == XKB_KEY_XF86AudioPrev ? "previous" : nullptr;
		if (media) {
			server_->status->media(media);
			return true;
		}
	}
	if (checking_) return true;
	const bool was_down = curtain_;
	raise_curtain();
	// The key that lifts the curtain only lifts it (unless it's a letter: then
	// it's the first one of the password).
	if (was_down && (text.empty() || static_cast<unsigned char>(text[0]) < 0x20)) return true;
	const bool ctrl = modifiers & WLR_MODIFIER_CTRL;
	// Typing goes to the field (it's picked again), the cursor shows at once.
	// (Caps Lock itself: set_caps_lock() follows the keyboard's own state.)
	focused_ = true;
	caret_on_ = true;
	wl_event_source_timer_update(blink_timer_, 530);
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		submit();
		return true;
	}
	// The cursor goes over whole characters, never into the middle of one.
	auto prev_char = [this](size_t at) {
		while (at > 0) {
			at--;
			if ((static_cast<unsigned char>(password_[at]) & 0xc0) != 0x80) break;
		}
		return at;
	};
	auto next_char = [this](size_t at) {
		if (at >= password_.size()) return password_.size();
		at++;
		while (at < password_.size() && (static_cast<unsigned char>(password_[at]) & 0xc0) == 0x80) at++;
		return at;
	};
	caret_ = std::min(caret_, password_.size());
	if (sym == XKB_KEY_BackSpace) {
		if (caret_ > 0) {
			const size_t from = ctrl ? 0 : prev_char(caret_);   // Ctrl: everything before the cursor
			password_.erase(from, caret_ - from);
			caret_ = from;
		}
	} else if (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete) {
		if (caret_ < password_.size()) password_.erase(caret_, next_char(caret_) - caret_);
	} else if (sym == XKB_KEY_Left || sym == XKB_KEY_KP_Left) {
		caret_ = ctrl ? 0 : prev_char(caret_);
	} else if (sym == XKB_KEY_Right || sym == XKB_KEY_KP_Right) {
		caret_ = ctrl ? password_.size() : next_char(caret_);
	} else if (sym == XKB_KEY_Home || sym == XKB_KEY_KP_Home) {
		caret_ = 0;
	} else if (sym == XKB_KEY_End || sym == XKB_KEY_KP_End) {
		caret_ = password_.size();
	} else if (sym == XKB_KEY_Escape || (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U))) {
		password_.clear();
		caret_ = 0;
	} else if (!ctrl && !text.empty() && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f &&
			password_.size() < 512) {
		password_.insert(caret_, text);
		caret_ += text.size();
		if (message_is_error_) message_.clear();
	} else {
		return true;
	}
	redraw_form();   // only the form: the rest stays as it is
	return true;
}

void LockScreen::motion(double lx, double ly) {
	std::string h;
	for (const auto *list : {&form_hots_, &media_hots_}) {
		for (const Hot &hot : *list) {
			if (inside(hot.box, lx, ly)) h = hot.id;
		}
	}
	if (h == hover_) return;   // nothing to do while the pointer just moves
	hover_ = h;
	redraw_form();
	if (Screen *s = form_screen()) draw_media(*s);
	server_->set_cursor(h.empty() ? "default" : "pointer");
}

void LockScreen::press(double lx, double ly, uint32_t button) {
	if (button != BTN_LEFT || checking_) return;
	if (curtain_) {
		raise_curtain();
		return;
	}
	last_input_ = now_ms();
	std::string id;
	for (const auto *list : {&form_hots_, &media_hots_}) {
		for (const Hot &hot : *list) {
			if (inside(hot.box, lx, ly)) id = hot.id;
		}
	}
	if (id == "go") {
		submit();
	} else if (id == "field") {
		focused_ = true;
		caret_on_ = true;
		redraw_form();
	} else if (id == "reveal") {
		reveal_ = !reveal_;
		focused_ = true;
		redraw_form();
	} else if (id == "media-seek") {
		if (server_->status && server_->status->media_length > 0 && media_bar_.width > 0) {
			const double f = std::clamp((lx - media_bar_.x) / media_bar_.width, 0.0, 1.0);
			server_->status->seek(f * server_->status->media_length);
		}
	} else if (id.rfind("media:", 0) == 0) {
		if (server_->status) server_->status->media(id.substr(6).c_str());
	} else if (id.empty()) {
		// Clicked on the picture around it: the field lets go (typing picks it again).
		if (focused_) {
			focused_ = false;
			redraw_form();
		}
	}
}

// ---- Checking --------------------------------------------------------------------------------

void LockScreen::submit() {
	if (checking_ || password_.empty() || user_.name.empty()) return;
	checking_ = true;
	message_.clear();
	redraw_form();
	if (worker_.joinable()) worker_.join();
	const std::string user = user_.name;
	std::string password = password_;
	// Guaranteed to stick (std::fill can be optimized away before the free).
	explicit_bzero(password_.data(), password_.size());
	password_.clear();
	caret_ = 0;
	worker_ = std::thread([this, user, password]() mutable {
		const bool ok = pam_check(user, password);
		explicit_bzero(password.data(), password.size());
		{
			std::lock_guard<std::mutex> g(result_mutex_);
			result_ok_ = ok;
			result_message_ = ok ? "" : "Wrong password, try again";
		}
		const char one = 1;
		if (pipe_[1] >= 0) {
			const ssize_t ignored = ::write(pipe_[1], &one, 1);
			(void)ignored;
		}
	});
}

int LockScreen::handle_pipe(int fd, uint32_t, void *data) {
	char buf[16];
	while (::read(fd, buf, sizeof buf) > 0) {
	}
	static_cast<LockScreen *>(data)->auth_finished();
	return 0;
}

void LockScreen::auth_finished() {
	if (worker_.joinable()) worker_.join();
	bool ok;
	std::string message;
	{
		std::lock_guard<std::mutex> g(result_mutex_);
		ok = result_ok_;
		message = result_message_;
	}
	checking_ = false;
	if (ok) {
		unlock();
		return;
	}
	message_ = message;
	message_is_error_ = true;
	redraw_form();
}

int LockScreen::handle_clock(void *data) {
	auto *self = static_cast<LockScreen *>(data);
	if (!self->active_) return 0;
	if (self->server_->config.lock_curtain && !self->curtain_ && self->password_.empty() && !self->checking_ &&
			now_ms() - self->last_input_ > 20000) {
		self->curtain_ = true;   // nothing typed for a while: back to just the clock
		self->redraw_form();
	}
	if (self->server_->status && self->server_->status->media_playing) self->status_changed();   // the music's time
	const std::string t = self->time_text();
	if (t != self->shown_time_) {
		self->shown_time_ = t;
		for (auto &s : self->screens_) self->draw_clock(*s);   // just the clock
	}
	wl_event_source_timer_update(self->clock_timer_, 1000);
	return 0;
}

// ---- logind: lock when asked, and before sleeping ----------------------------------------------

void LockScreen::watch_logind() {
	if (sd_bus_open_system(&system_bus_) < 0) {
		system_bus_ = nullptr;
		return;
	}
	sd_bus_match_signal(system_bus_, &sleep_slot_, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "PrepareForSleep", &LockScreen::on_prepare_for_sleep, this);
	if (const char *id = getenv("XDG_SESSION_ID"); id && *id) {
		char *path = nullptr;
		if (sd_bus_path_encode("/org/freedesktop/login1/session", id, &path) >= 0 && path) {
			sd_bus_match_signal(system_bus_, &lock_slot_, "org.freedesktop.login1", path,
				"org.freedesktop.login1.Session", "Lock", &LockScreen::on_lock_signal, this);
			free(path);
		}
	}
	bus_source_ = wl_event_loop_add_fd(server_->loop, sd_bus_get_fd(system_bus_), WL_EVENT_READABLE,
		&LockScreen::handle_bus_fd, this);
	take_sleep_delay();
	while (sd_bus_process(system_bus_, nullptr) > 0) {
	}
}

// Asks logind to wait a moment before sleeping, so the screen is locked
// first (and nobody sees your windows for a second when it wakes).
void LockScreen::take_sleep_delay() {
	if (!system_bus_ || sleep_fd_ >= 0 || !server_->config.lock_sleep) return;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = nullptr;
	if (sd_bus_call_method(system_bus_, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "Inhibit", &err, &reply, "ssss", "sleep", "Dusk",
			"Locks the screen first", "delay") >= 0) {
		int fd = -1;
		if (sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0) {
			sleep_fd_ = fcntl(fd, F_DUPFD_CLOEXEC, 3);
		}
	}
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
}

int LockScreen::handle_bus_fd(int, uint32_t, void *data) {
	auto *self = static_cast<LockScreen *>(data);
	while (self->system_bus_ && sd_bus_process(self->system_bus_, nullptr) > 0) {
	}
	return 0;
}

int LockScreen::on_prepare_for_sleep(sd_bus_message *m, void *data, sd_bus_error *) {
	auto *self = static_cast<LockScreen *>(data);
	int going = 0;
	if (sd_bus_message_read(m, "b", &going) < 0) return 0;
	if (going) {
		if (self->server_->config.lock_sleep) self->lock();
		// A moment for the locked screen to be drawn, then it can sleep.
		// (A member, not a static: the timer must never outlive this object
		// and fire into freed memory.)
		if (!self->sleep_close_timer_) {
			self->sleep_close_timer_ = wl_event_loop_add_timer(self->server_->loop, [](void *d) {
				auto *s = static_cast<LockScreen *>(d);
				if (s->sleep_fd_ >= 0) {
					::close(s->sleep_fd_);
					s->sleep_fd_ = -1;
				}
				return 0;
			}, self);
		}
		wl_event_source_timer_update(self->sleep_close_timer_, 250);
	} else {
		self->take_sleep_delay();   // for next time
	}
	return 0;
}

int LockScreen::on_lock_signal(sd_bus_message *, void *data, sd_bus_error *) {
	static_cast<LockScreen *>(data)->lock();
	return 0;
}

} // namespace dusk
