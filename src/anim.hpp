// A tiny animation engine. Animations advance once per screen refresh (the
// outputs call step() right before drawing), so motion stays smooth and in
// step with the display. A slow timer keeps things going if no screen is
// drawing.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "wlr.hpp"

namespace dusk {

double now_ms();
double ease_out_cubic(double t);
// The easing chosen in Settings: smooth (cubic), snappy (quint), bouncy
// (overshoots a little and settles).
void set_curve(const std::string &name);
double ease(double t);

class Animator {
public:
	using Update = std::function<void(double)>;   // eased progress, 0..1
	using Done = std::function<void()>;

	explicit Animator(wl_event_loop *loop);
	~Animator();
	Animator(const Animator &) = delete;
	Animator &operator=(const Animator &) = delete;

	// Starts an animation and returns its id. A duration of 0 (animations
	// off) runs it to the end straight away.
	uint64_t start(double duration_ms, Update update, Done done = {});
	// Animates a value from wherever it is now to `to`. `value` must stay
	// alive while it runs (it is a member of the animated object). The
	// duration shrinks when there is less distance to cover, so reversing
	// half way through doesn't jump or crawl.
	uint64_t tween(double &value, double to, double full_ms, std::function<void()> changed,
		Done done = {});
	// Stops an animation. With `finish`, it jumps to the end first.
	void cancel(uint64_t id, bool finish = false);
	bool running(uint64_t id) const;
	bool active() const { return !anims_.empty() || !tickers_.empty(); }

	// Runs `tick(ms since last frame)` every frame until it returns false.
	using Ticker = std::function<bool(double)>;
	uint64_t add_ticker(Ticker tick);
	void remove_ticker(uint64_t id);

	// Advances all animations to "now". Called by each output before it
	// draws a frame (several outputs in one refresh is fine).
	void step();
	// Called when animations start, so the outputs schedule a frame.
	std::function<void()> wake;

private:
	struct Anim {
		uint64_t id;
		double start, duration;
		Update update;
		Done done;
	};
	static int tick(void *data);

	wl_event_loop *loop_;
	wl_event_source *timer_ = nullptr;
	std::vector<Anim> anims_;
	std::vector<std::pair<uint64_t, Ticker>> tickers_;
	uint64_t next_id_ = 1;
	double last_step_ = 0;
};

} // namespace dusk
