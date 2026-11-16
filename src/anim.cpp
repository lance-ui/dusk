#include "anim.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace dusk {

namespace {
// Fallback tick when no output is drawing (e.g. all screens are off).
constexpr int kFallbackMs = 16;
} // namespace

double now_ms() {
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

double ease_out_cubic(double t) {
	t = std::clamp(t, 0.0, 1.0);
	const double u = 1.0 - t;
	return 1.0 - u * u * u;
}

namespace {
int curve = 0;   // 0 smooth, 1 snappy, 2 bouncy
} // namespace

void set_curve(const std::string &name) {
	curve = name == "snappy" ? 1 : name == "bouncy" ? 2 : 0;
}

double ease(double t) {
	t = std::clamp(t, 0.0, 1.0);
	if (curve == 1) {
		const double u = 1.0 - t;
		return 1.0 - u * u * u * u * u;
	}
	if (curve == 2) {
		// "Back" easing: goes ~6% past the end, then settles.
		const double c1 = 1.2, c3 = c1 + 1.0;
		const double u = t - 1.0;
		return 1.0 + c3 * u * u * u + c1 * u * u;
	}
	return ease_out_cubic(t);
}

Animator::Animator(wl_event_loop *loop) : loop_(loop) {
	timer_ = wl_event_loop_add_timer(loop_, &Animator::tick, this);
}

Animator::~Animator() {
	if (timer_) {
		wl_event_source_remove(timer_);
	}
}

uint64_t Animator::start(double duration_ms, Update update, Done done) {
	const uint64_t id = next_id_++;
	if (duration_ms <= 0) {
		if (update) update(1.0);
		if (done) done();
		return id;
	}
	if (update) {
		update(0.0);
	}
	anims_.push_back(Anim{id, now_ms(), duration_ms, std::move(update), std::move(done)});
	wl_event_source_timer_update(timer_, kFallbackMs * 3);
	if (wake) {
		wake();
	}
	return id;
}

uint64_t Animator::tween(double &value, double to, double full_ms, std::function<void()> changed,
		Done done) {
	const double from = value;
	// Values here are 0..1 (opacity, slide progress), so the distance left
	// tells how much of the full duration is still needed.
	const double distance = std::min(1.0, std::fabs(to - from));
	double *target = &value;
	return start(full_ms * distance, [target, from, to, changed](double t) {
		*target = from + (to - from) * t;
		if (changed) changed();
	}, std::move(done));
}

uint64_t Animator::add_ticker(Ticker tick) {
	const uint64_t id = next_id_++;
	tickers_.push_back({id, std::move(tick)});
	wl_event_source_timer_update(timer_, kFallbackMs * 3);
	if (wake) {
		wake();
	}
	return id;
}

void Animator::remove_ticker(uint64_t id) {
	tickers_.erase(std::remove_if(tickers_.begin(), tickers_.end(),
		[id](const auto &t) { return t.first == id; }), tickers_.end());
}

void Animator::cancel(uint64_t id, bool finish) {
	if (id == 0) {
		return;
	}
	for (auto it = anims_.begin(); it != anims_.end(); ++it) {
		if (it->id == id) {
			Anim a = std::move(*it);
			anims_.erase(it);
			if (finish) {
				if (a.update) a.update(1.0);
				if (a.done) a.done();
			}
			return;
		}
	}
}

bool Animator::running(uint64_t id) const {
	return std::any_of(anims_.begin(), anims_.end(),
		[id](const Anim &a) { return a.id == id; });
}

int Animator::tick(void *data) {
	auto *self = static_cast<Animator *>(data);
	// Only step from the timer when the outputs haven't for a while.
	if (now_ms() - self->last_step_ >= kFallbackMs * 2) {
		self->step();
	}
	if (self->active()) {
		wl_event_source_timer_update(self->timer_, kFallbackMs * 3);
	}
	return 0;
}

void Animator::step() {
	const double now = now_ms();
	if (now - last_step_ < 1.0 && active()) {
		return;   // a second output in the same refresh
	}
	const double dt = std::clamp(now - last_step_, 1.0, 50.0);
	last_step_ = now;

	// Tickers (things that follow a moving target).
	if (!tickers_.empty()) {
		std::vector<uint64_t> ids;
		for (auto &t : tickers_) {
			ids.push_back(t.first);
		}
		for (uint64_t id : ids) {
			auto it = std::find_if(tickers_.begin(), tickers_.end(),
				[id](const auto &t) { return t.first == id; });
			if (it == tickers_.end()) {
				continue;
			}
			Ticker tick = it->second;   // may remove itself
			if (!tick(dt)) {
				remove_ticker(id);
			}
		}
	}
	std::vector<Anim> finished;
	// Copy the ids first: callbacks may start or cancel animations.
	std::vector<uint64_t> ids;
	for (const Anim &a : anims_) {
		ids.push_back(a.id);
	}
	for (uint64_t id : ids) {
		auto it = std::find_if(anims_.begin(), anims_.end(),
			[id](const Anim &a) { return a.id == id; });
		if (it == anims_.end()) {
			continue;
		}
		const double t = (now - it->start) / it->duration;
		if (t >= 1.0) {
			Anim a = std::move(*it);
			anims_.erase(it);
			if (a.update) a.update(1.0);
			finished.push_back(std::move(a));
		} else if (it->update) {
			Update update = it->update;   // the callback may erase the entry
			update(ease(t));
		}
	}
	for (Anim &a : finished) {
		if (a.done) a.done();
	}
}

} // namespace dusk
