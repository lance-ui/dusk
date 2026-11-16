// The lock screen.
//
//   - The clock (digital, hours over minutes, an outline, or a clock face)
//     and the date over your wallpaper (blurred, or a picture of its own, or
//     a colour), your picture and name, and a password field; what's playing
//     and the Wi-Fi / battery, like a phone. All of it set in Settings ›
//     Lock screen, where the presets and the preview are.
//   - Apps carry on underneath but get no keys or clicks. The password is
//     checked by PAM (the same check as logging in), off the main thread.
//
// It locks with Super+L or Lock in the power menu, when the screen turns
// off by itself, before the computer sleeps, and when the system asks
// (loginctl lock-session). The login screen itself is GNOME's (GDM).
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <systemd/sd-bus.h>

#include "draw.hpp"
#include "server.hpp"

namespace dusk {

class LockScreen {
public:
	explicit LockScreen(Server *server);
	~LockScreen();
	LockScreen(const LockScreen &) = delete;
	LockScreen &operator=(const LockScreen &) = delete;

	bool active() const { return active_; }
	void lock();
	// Settings, the wallpaper or the screens changed: draw it again.
	void refresh();
	// Music or the battery changed: those parts again.
	void status_changed();
	// Notifications came or went: the list under the password.
	void notifications_changed();
	void output_gone(Output *output);

	// While it's active it gets every key and click.
	bool key(xkb_keysym_t sym, const std::string &text, uint32_t modifiers);
	// Caps Lock went on or off (from the keyboard's state, the moment it changes).
	void set_caps_lock(bool on);
	void motion(double lx, double ly);
	void press(double lx, double ly, uint32_t button);

private:
	// Each screen: a background made once (blurred and darkened already: no
	// blur running every frame), the clock, and on the main screen the
	// password form. Separate small pictures, so typing redraws only the form.
	struct Screen {
		Output *output = nullptr;
		wlr_scene_tree *tree = nullptr;
		wlr_scene_rect *base = nullptr;     // a plain colour under it all
		wlr_scene_buffer *bg = nullptr;
		wlr_scene_buffer *clock = nullptr;
		wlr_scene_buffer *form = nullptr;
		wlr_scene_buffer *media = nullptr;   // what's playing (locked, main screen)
		wlr_scene_buffer *status = nullptr;  // Wi-Fi and battery, top right (main screen)
		std::string bg_key;                 // what bg was made from
		wlr_box form_box{};                 // layout coordinates
	};
	struct User {
		std::string name, real, face;
	};
	struct NotifyGroup {
		std::string app, icon;
		size_t count = 0;
	};
	std::vector<NotifyGroup> notification_groups() const;
	size_t notify_groups_shown_ = 0;
	// Where things go on a screen (layout coordinates).
	struct Layout {
		double pt = 0, dpt = 0, mpt = 0;   // the clock's, the date's and the message's sizes
		double time_h = 0;                 // the time's own height
		Align align = Align::Center;
		wlr_box clock{}, media{}, form{};
		int pad = 8;                       // room inside the form's picture
	};
	struct Hot {
		std::string id;
		wlr_box box;
	};

	void build();
	void destroy_screens();
	Screen *form_screen() const;          // the main screen: where the password goes
	void make_background(Screen &s);
	void draw_clock(Screen &s);
	void draw_form(Screen &s);
	Layout layout(const Screen &s) const;
	bool media_wanted() const;
	void find_face();
	void draw_media(Screen &s);
	void draw_status(Screen &s);
	void redraw_form();                   // typing, hover: just the form (and bar)
	void redraw();                        // everything but the backgrounds
	void submit();
	void unlock();
	void auth_finished();
	static int handle_pipe(int fd, uint32_t mask, void *data);
	static int handle_clock(void *data);
	std::string time_text() const;

	// logind: lock when asked, and before sleeping.
	void watch_logind();
	void take_sleep_delay();
	static int handle_bus_fd(int fd, uint32_t mask, void *data);
	static int on_prepare_for_sleep(sd_bus_message *m, void *data, sd_bus_error *err);
	static int on_lock_signal(sd_bus_message *m, void *data, sd_bus_error *err);

	Server *server_;
	bool active_ = false;
	std::vector<std::unique_ptr<Screen>> screens_;
	std::vector<Hot> form_hots_, media_hots_;   // layout coordinates
	std::string media_shown_;   // what the music card shows (drawn again only when it changes)
	bool media_on_ = false;     // the music card is up (the form keeps clear of it)
	wlr_box media_bar_{};       // the music's progress bar (click to jump)
	std::string hover_;

	std::string password_;
	std::string message_;
	bool message_is_error_ = false;
	bool checking_ = false;
	std::string shown_time_;
	// "Just the clock until a key is pressed": the form is hidden behind a
	// curtain, and comes back down after a while with nothing typed.
	bool curtain_ = false;
	// The field: picked (accent ring and a blinking cursor), showing what's
	// typed (the eye), and whether Caps Lock is on.
	bool focused_ = true;
	bool reveal_ = false;
	size_t caret_ = 0;   // where typing goes, in bytes (always at a character's start)
	bool caret_on_ = true;
	bool caps_lock_ = false;
	wl_event_source *blink_timer_ = nullptr;
	static int handle_blink(void *data);
	double last_input_ = 0;
	void raise_curtain();

	User user_;
	cairo_surface_t *face_ = nullptr;
	std::string face_for_;

	// The password check runs here; its answer comes back through the pipe.
	std::thread worker_;
	std::mutex result_mutex_;
	bool result_ok_ = false;
	std::string result_message_;
	int pipe_[2] = {-1, -1};
	wl_event_source *pipe_source_ = nullptr;
	wl_event_source *clock_timer_ = nullptr;

	sd_bus *system_bus_ = nullptr;
	wl_event_source *bus_source_ = nullptr;
	sd_bus_slot *sleep_slot_ = nullptr;
	sd_bus_slot *lock_slot_ = nullptr;
	int sleep_fd_ = -1;   // logind waits for us before sleeping while we hold it
	wl_event_source *sleep_close_timer_ = nullptr;   // releases sleep_fd_ a moment later
};

} // namespace dusk
