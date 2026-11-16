// How loud what's playing is right now, and when a beat lands: read from
// the sound going to the speakers (the default output's monitor), so the
// media card's wave can move with the music. Runs only while something
// that shows it is on screen (acquire / release).
#pragma once

#include <sys/types.h>

#include <array>

#include "wlr.hpp"

namespace dusk {

class Server;

class AudioLevel {
public:
	explicit AudioLevel(Server *server);
	~AudioLevel();
	AudioLevel(const AudioLevel &) = delete;
	AudioLevel &operator=(const AudioLevel &) = delete;

	// Something on screen wants the level (counted; the reader stops when
	// nobody does).
	void acquire();
	void release();

	bool live() const;     // sound is being read (else the wave just sways)
	double level() const;  // 0..1, follows the loudness (quick up, slower down)
	double beat() const;   // 1 just after a beat, falling to 0
	double pulse() const;  // 0..1: how far the bass stands above its recent average (kicks)
	// The spectrum: bar i of n (0..1, bass to treble), smoothed (quick up, slow down).
	// `ch`: 0 left, 1 right, -1 both (the louder of the two).
	static constexpr int kBars = 24;
	double bar(int i, int n, int ch = -1) const;

private:
	void start();
	void stop();
	void feed(const unsigned char *data, size_t n);
	static int handle_read(int fd, uint32_t mask, void *data);

	Server *server_;
	int users_ = 0;
	pid_t pid_ = -1;
	int fd_ = -1;
	wl_event_source *source_ = nullptr;
	unsigned char carry_[4] = {0};   // a part of a frame (left + right, 2 bytes each)
	int carry_n_ = 0;

	// Analysis, per 10 ms block.
	double block_sum_ = 0;
	int block_n_ = 0;
	double env_ = 0;         // loudness envelope
	double avg_ = 0;         // slow average (what a beat stands out from)
	double peak_ = 0.05;     // automatic gain: the loudest lately
	double last_beat_ = -1e9;
	double last_data_ = -1e9;
	double low_ = 0, bass_sum_ = 0;     // the bass, per block
	double bass_env_ = 0, bass_avg_ = 0.01, pulse_ = 0;
	void spectrum();
	void spectrum_of(int ch);
	std::array<std::array<double, 1024>, 2> ring_{};   // the last samples, left and right (for the spectrum)
	int ring_pos_ = 0, hop_n_ = 0;
	std::array<std::array<double, kBars>, 2> bars_{}, band_peak_{};
};

} // namespace dusk
