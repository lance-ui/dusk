// On-screen display for volume, microphone, brightness, Caps/Num Lock and
// keyboard layout changes. The level bar is a row of slanted segments,
// inspired by the LED light strips and cockpit gauges of sports cars.
#pragma once

#include <string>

#include <memory>

#include "server.hpp"
#include "ui.hpp"

namespace dusk {

class Osd {
public:
	enum class Kind { Volume, Mic, Brightness, CapsLock, NumLock, Layout };

	explicit Osd(Server *server);
	~Osd();
	Osd(const Osd &) = delete;
	Osd &operator=(const Osd &) = delete;

	// `level` 0..100, or -1 for no bar. `off` greys it out (muted, lock off).
	void show(Kind kind, int level, bool off, const std::string &text = "");
	void hide();

	// The volume and brightness bars take the pointer while they are up:
	// press and drag across the segments, or scroll, to set the level.
	bool grabs_input(double lx, double ly) const;
	void pointer_set(double lx, double ly);
	void pointer_scroll(double delta);
	static bool accepts_input(wlr_scene_buffer *buffer, double *sx, double *sy);

private:
	static int handle_timeout(void *data);
	void redraw();
	void place();
	// True when what is shown is a level bar that can be set (volume,
	// brightness), not just an on/off state or a keyboard layout.
	bool interactive() const;
	// So the pointer can find us (see accepts_input).
	NodeTag tag_ = {NodeTag::Kind::Osd, this};

	Server *server_;
	// The same card as menus and notifications: their blur and shadow.
	std::unique_ptr<UiLayer> layer_;
	wl_event_source *timer_;
	uint64_t anim_ = 0;
	bool visible_ = false;
	double alpha_ = 0;

	Kind kind_ = Kind::Volume;
	int level_ = 0;
	bool off_ = false;
	std::string text_;
	int width_ = 0, height_ = 0;
	int pos_x_ = 0, pos_y_ = 0;   // where the card sits on the screen (place)
};

} // namespace dusk
