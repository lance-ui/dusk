// Wallpapers: pictures are decoded and scaled on a worker thread (a big
// photo takes a moment, and Dusk mustn't stutter meanwhile), and a new
// wallpaper comes in with a transition: fade, slide, push, zoom, wipe or
// grow (Settings > Appearance > Wallpaper > Transition; or a random one).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <cairo.h>

#include "draw.hpp"

struct wl_event_source;

namespace dusk {

class Server;

// The accent "From wallpaper" (server.cpp; safe on any thread).
bool accent_from_surface(cairo_surface_t *picture, Color &out);

class WallpaperWorker {
public:
	struct Job {
		std::string output;      // connector name
		uint64_t generation = 0;
		std::string path, mode;  // mode: fill, fit, center, stretch
		int width = 0, height = 0;   // logical
		float scale = 1.0f;
		bool accent = false;     // also pick an accent colour from it
	};
	struct Result {
		Job job;
		cairo_surface_t *surface = nullptr;   // pixel size, owned by the receiver
		bool has_accent = false;
		Color accent{};
	};

	explicit WallpaperWorker(Server *server);
	~WallpaperWorker();
	WallpaperWorker(const WallpaperWorker &) = delete;
	WallpaperWorker &operator=(const WallpaperWorker &) = delete;

	// Newer requests for the same output replace older ones still waiting.
	void request(const Job &job);
	std::function<void(Result &)> done;   // on Dusk's own thread

private:
	void run();
	static int handle_ready(int fd, uint32_t mask, void *data);

	Server *server_;
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<Job> jobs_;
	std::deque<Result> results_;
	bool stop_ = false;
	int pipe_[2] = {-1, -1};
	wl_event_source *source_ = nullptr;

	// The last picture decoded (thread only): resizes and other screens reuse it.
	std::string cached_path_;
	cairo_surface_t *cached_ = nullptr;
};

} // namespace dusk
