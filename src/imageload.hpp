// Pictures (album art and the like) read in the background: reading and
// scaling a big one on the main thread froze everything for a moment.
#pragma once

#include <cairo.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "wlr.hpp"

namespace dusk {

class ImageLoader {
public:
	explicit ImageLoader(wl_event_loop *loop);
	~ImageLoader();
	ImageLoader(const ImageLoader &) = delete;
	ImageLoader &operator=(const ImageLoader &) = delete;

	// Reads `path` scaled to fit width × height; `done` gets it on the main
	// thread (nullptr if it couldn't be read) and owns it.
	void request(const std::string &path, int width, int height, std::function<void(cairo_surface_t *)> done);

private:
	struct Job {
		std::string path;
		int w = 0, h = 0;
		std::function<void(cairo_surface_t *)> done;
		cairo_surface_t *result = nullptr;
	};
	void work();
	static int handle_ready(int fd, uint32_t mask, void *data);

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<Job> jobs_, results_;
	bool stop_ = false;
	int pipe_[2] = {-1, -1};
	wl_event_source *source_ = nullptr;
};

} // namespace dusk
