#include "imageload.hpp"

#include <fcntl.h>
#include <unistd.h>

#include "draw.hpp"

namespace dusk {

ImageLoader::ImageLoader(wl_event_loop *loop) {
	if (pipe2(pipe_, O_CLOEXEC | O_NONBLOCK) == 0) {
		source_ = wl_event_loop_add_fd(loop, pipe_[0], WL_EVENT_READABLE, &ImageLoader::handle_ready, this);
	}
	thread_ = std::thread([this] { work(); });
}

ImageLoader::~ImageLoader() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable()) thread_.join();
	if (source_) wl_event_source_remove(source_);
	for (Job &j : results_) {
		if (j.result) cairo_surface_destroy(j.result);
	}
	for (int fd : pipe_) {
		if (fd >= 0) close(fd);
	}
}

void ImageLoader::request(const std::string &path, int width, int height,
		std::function<void(cairo_surface_t *)> done) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		jobs_.push_back(Job{path, width, height, std::move(done), nullptr});
	}
	wake_.notify_one();
}

void ImageLoader::work() {
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
			if (stop_) return;
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		job.result = load_image_sized(job.path, job.w, job.h);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			results_.push_back(std::move(job));
		}
		const char c = 1;
		if (write(pipe_[1], &c, 1) < 0) {
			// (full: the main thread already has a wake-up waiting)
		}
	}
}

int ImageLoader::handle_ready(int fd, uint32_t, void *data) {
	auto *self = static_cast<ImageLoader *>(data);
	char buf[64];
	while (read(fd, buf, sizeof buf) > 0) {
	}
	std::deque<Job> ready;
	{
		std::lock_guard<std::mutex> lock(self->mutex_);
		ready.swap(self->results_);
	}
	for (Job &j : ready) {
		if (j.done) j.done(j.result);
		else if (j.result) cairo_surface_destroy(j.result);
	}
	return 0;
}

} // namespace dusk
