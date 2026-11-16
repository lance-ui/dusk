// Screen recording (the screenshot tool's video side, or Shift+Ctrl+Alt+R):
// a screen, a window or an area, with or without sound, into
// Videos/Screencasts. While it records, a red pill with the time shows on
// the panels' status area; click it (or press the keys again) to stop.
#pragma once

#include <sys/types.h>

#include <string>

#include "wlr.hpp"

namespace dusk {

class Server;
struct Output;

class Recorder {
public:
	explicit Recorder(Server *server);
	~Recorder();
	Recorder(const Recorder &) = delete;
	Recorder &operator=(const Recorder &) = delete;

	bool active() const { return pid_ > 0; }
	bool starting() const { return (active() && started_ == 0) || counting_down_; }
	bool counting_down() const { return counting_down_; }
	double elapsed() const;         // seconds since it started recording
	std::string elapsed_text() const;

	// `box` in layout coordinates (width 0: all of `output`). After `delay` seconds.
	void start(wlr_box box, Output *output, int delay);
	void stop();
	void toggle();                  // the shortcut: this screen, or stop

private:
	void begin(wlr_box box, std::string output_name);
	void finished(bool ok);
	static int handle_output(int fd, uint32_t mask, void *data);
	static int handle_tick(void *data);

	Server *server_;
	pid_t pid_ = -1;
	int fd_ = -1;
	wl_event_source *source_ = nullptr;
	wl_event_source *tick_ = nullptr;
	wl_event_source *kill_timer_ = nullptr;   // SIGKILL escalation after stop
	std::string out_buf_;
	std::string file_;
	std::string error_;
	double started_ = 0;            // now_ms() when it really began (0: not yet)
	bool counting_down_ = false;
	bool stop_requested_ = false;
};

} // namespace dusk
