// The on-screen keyboard: for a touch screen, or a tablet folded back. It
// sits along the bottom of the screen above the windows, types into the app
// that has the keyboard focus (never taking it), and has a page of emoji.
// On a wide screen it splits in two halves at the sides, like Samsung's,
// so both thumbs reach.
//
// Keys go through a real keyboard's keymap (the layout set in Settings);
// anything that layout can't type (emoji, other alphabets) is typed with a
// keymap made just for it for a moment, the way wtype does.
#pragma once

#include <string>
#include <vector>

#include "popover.hpp"

struct xkb_context;

namespace dusk {

class Osk : public Popover {
public:
	explicit Osk(Server *server);
	~Osk() override;
	void toggle(Output *output);
	void show(Output *output);

protected:
	void draw(cairo_t *cr, float scale) override;
	void on_open() override;
	void activate(const std::string &id, uint32_t button, double px, double py) override;
	void on_drag(const std::string &id, double px, double py) override;
	bool draggable(const std::string &id) const override;
	void on_scroll(double delta) override;

private:
	enum class Mode { Letters, Symbols, Emoji, Clipboard };
	struct Key {
		std::string id;      // "c:<text>" types it; else an action
		std::string label;
		double units = 1;
		bool special = false;
		bool gap_after = false;   // the split goes after this key
	};
	void fit();                  // size for the screen
	std::vector<std::vector<Key>> rows() const;
	void press_key(const std::string &id);

	// Typing.
	bool type_text(const std::string &utf8);
	bool send_keycode(uint32_t evdev, bool shift);
	bool type_codepoints(const std::vector<uint32_t> &cps);
	struct wlr_keyboard *target() const;

	Mode mode_ = Mode::Letters;
	bool shift_ = false, caps_ = false;
	double last_shift_ = 0;
	bool split_ = false;
	int category_ = 0, page_ = 0;
	int emoji_cols_ = 10, emoji_rows_ = 4;
	wl_event_source *repeat_ = nullptr;
	std::string held_;   // the key held down (Backspace repeats)
	double grab_x_ = 0, grab_y_ = 0;   // where the handle was taken (moving it)
	int clip_scroll_ = 0;              // the clipboard page: how far down
	void paste_entry(uint64_t id);
	void type_string(const std::string &utf8);
	bool send_combo(uint32_t evdev, bool ctrl, bool shift);
	static int handle_repeat(void *data);
	xkb_context *xkb_ = nullptr;
};

} // namespace dusk
