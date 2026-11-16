#include "audiolevel.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "anim.hpp"
#include "server.hpp"

namespace dusk {

namespace {
constexpr int kRate = 16000;          // samples a second: enough for the spectrum up to 8 kHz
constexpr int kBlock = kRate / 100;   // 10 ms
constexpr int kFft = 1024;            // the spectrum's window (64 ms)
constexpr int kHop = 512;             // a new spectrum every 32 ms (~30 a second)

// In-place radix-2 FFT (re, im of length n, a power of two).
void fft(std::vector<double> &re, std::vector<double> &im) {
	const size_t n = re.size();
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) {
			std::swap(re[i], re[j]);
			std::swap(im[i], im[j]);
		}
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const double ang = -2 * M_PI / static_cast<double>(len);
		const double wr = std::cos(ang), wi = std::sin(ang);
		for (size_t i = 0; i < n; i += len) {
			double cr = 1, ci = 0;
			for (size_t k = 0; k < len / 2; k++) {
				const size_t a = i + k, b = i + k + len / 2;
				const double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				const double ncr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = ncr;
			}
		}
	}
}
}

AudioLevel::AudioLevel(Server *server) : server_(server) {}

AudioLevel::~AudioLevel() {
	stop();
}

void AudioLevel::acquire() {
	if (users_++ == 0) start();
}

void AudioLevel::release() {
	if (users_ <= 0) return;
	if (--users_ == 0) stop();
}

void AudioLevel::start() {
	if (pid_ > 0) return;
	int fds[2];
	if (pipe(fds) != 0) return;
	const pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		signal(SIGPIPE, SIG_DFL);
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		const int null = open("/dev/null", O_WRONLY);
		if (null >= 0) dup2(null, STDERR_FILENO);
		// The sound going to the speakers, quietly, as small numbers. "dusk-levels"
		// is how the microphone indicator knows it isn't a recording.
		execl("/bin/sh", "/bin/sh", "-c",
			// parec first (pulseaudio-utils); if it's missing or fails, PipeWire's own.
			"command -v parec >/dev/null && parec --client-name=dusk-levels --stream-name=dusk-levels "
			"-d @DEFAULT_MONITOR@ --raw --format=s16le --rate=16000 --channels=2 --latency-msec=30; "
			"command -v pw-record >/dev/null && "
			"exec pw-record -P '{ stream.capture.sink = true, application.name = \"dusk-levels\" }' "
			"--rate 16000 --channels 2 --format s16 -",
			static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	pid_ = pid;
	fd_ = fds[0];
	fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
	fcntl(fd_, F_SETFD, FD_CLOEXEC);
	source_ = wl_event_loop_add_fd(server_->loop, fd_, WL_EVENT_READABLE, &AudioLevel::handle_read, this);
	wlr_log(WLR_INFO, "Listening to the music for the media wave");
	carry_n_ = 0;
	block_sum_ = 0;
	block_n_ = 0;
}

void AudioLevel::stop() {
	if (source_) wl_event_source_remove(source_);
	source_ = nullptr;
	if (fd_ >= 0) close(fd_);
	fd_ = -1;
	if (pid_ > 0) {
		kill(-pid_, SIGTERM);
		kill(pid_, SIGTERM);
		waitpid(pid_, nullptr, 0);
	}
	pid_ = -1;
	env_ = 0;
	last_data_ = -1e9;
}

int AudioLevel::handle_read(int fd, uint32_t mask, void *data) {
	auto *self = static_cast<AudioLevel *>(data);
	unsigned char buf[4096];
	for (;;) {
		const ssize_t n = read(fd, buf, sizeof buf);
		if (n > 0) {
			self->feed(buf, static_cast<size_t>(n));
			continue;
		}
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
			// The reader ended (no sound server, or neither tool installed):
			// the wave sways on its own.
			const int users = self->users_;
			wlr_log(WLR_INFO, "The media wave can't hear the music (install pulseaudio-utils for parec)");
			self->stop();
			self->users_ = users;
		}
		break;
	}
	(void)mask;
	return 0;
}

void AudioLevel::feed(const unsigned char *data, size_t n) {
	auto sample = [](unsigned char lo, unsigned char hi) {
		return static_cast<int16_t>(static_cast<uint16_t>(lo | (hi << 8))) / 32768.0;
	};
	const double now = now_ms();
	last_data_ = now;
	// Stereo frames: left then right, 2 bytes each (a frame can be split between reads).
	auto frame = [&](const unsigned char *f) {
		const double l = sample(f[0], f[1]), r = sample(f[2], f[3]);
		const double s = 0.5 * (l + r);
		block_sum_ += s * s;
		// The bass (kick drums, bass lines): what the beat is felt in.
		low_ += (s - low_) * 0.057;  // one-pole low-pass, ~150 Hz at 16 kHz
		ring_[0][static_cast<size_t>(ring_pos_)] = l;
		ring_[1][static_cast<size_t>(ring_pos_)] = r;
		ring_pos_ = (ring_pos_ + 1) % kFft;
		if (++hop_n_ >= kHop) {
			hop_n_ = 0;
			spectrum();
		}
		bass_sum_ += low_ * low_;
		if (++block_n_ < kBlock) return;
		const double rms = std::sqrt(block_sum_ / block_n_);
		const double bass = std::sqrt(bass_sum_ / block_n_);
		block_sum_ = 0;
		bass_sum_ = 0;
		block_n_ = 0;
		// Kicks: the bass jumping above its recent average.
		bass_env_ = bass > bass_env_ ? bass : bass_env_ * 0.86 + bass * 0.14;
		bass_avg_ += (bass - bass_avg_) * 0.015;
		pulse_ = std::clamp((bass_env_ / std::max(bass_avg_, 0.004) - 0.75) / 1.5, 0.0, 1.0);
		// Quick to rise, slower to fall: it moves with the music without jitter.
		env_ = rms > env_ ? env_ + (rms - env_) * 0.6 : env_ * 0.90 + rms * 0.10;
		avg_ += (rms - avg_) * 0.02;
		peak_ = std::max({env_, peak_ * 0.9985, 0.02});
		// A beat: clearly louder than lately, not too soon after the last.
		if (rms > avg_ * 1.45 && rms > 0.015 && now - last_beat_ > 180) {
			last_beat_ = now;
		}
	};
	size_t i = 0;
	while (carry_n_ > 0 && carry_n_ < 4 && i < n) carry_[carry_n_++] = data[i++];
	if (carry_n_ == 4) {
		frame(carry_);
		carry_n_ = 0;
	}
	for (; i + 3 < n; i += 4) frame(data + i);
	while (i < n) carry_[carry_n_++] = data[i++];
}

// The spectrum, in 24 bars from the bass to the treble (like Arqen's, which
// uses cava): each bar quick to rise and slow to fall, blended a little with
// its neighbours, so the shapes drawn from it are steady.
void AudioLevel::spectrum() {
	spectrum_of(0);
	spectrum_of(1);
}

void AudioLevel::spectrum_of(int ch) {
	const auto &ring = ring_[static_cast<size_t>(ch)];
	auto &bars = bars_[static_cast<size_t>(ch)];
	auto &band_peak = band_peak_[static_cast<size_t>(ch)];
	std::vector<double> re(kFft), im(kFft, 0.0);
	for (int i = 0; i < kFft; i++) {
		const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (kFft - 1));   // Hann window
		re[static_cast<size_t>(i)] = ring[static_cast<size_t>((ring_pos_ + i) % kFft)] * w;
	}
	fft(re, im);
	std::array<double, kBars> raw{};
	const double hz_per_bin = static_cast<double>(kRate) / kFft;
	for (int b = 0; b < kBars; b++) {
		// Log-spaced bands, 40 Hz .. 7.5 kHz.
		const double f0 = 40 * std::pow(7500.0 / 40, static_cast<double>(b) / kBars);
		const double f1 = 40 * std::pow(7500.0 / 40, static_cast<double>(b + 1) / kBars);
		const int k0 = std::max(1, static_cast<int>(f0 / hz_per_bin));
		const int k1 = std::max(k0 + 1, static_cast<int>(f1 / hz_per_bin));
		double m = 0;
		for (int k = k0; k < k1 && k < kFft / 2; k++) {
			m = std::max(m, std::hypot(re[static_cast<size_t>(k)], im[static_cast<size_t>(k)]));
		}
		// Loudness-like, then each bar's own automatic gain (the treble is quieter).
		const double v = std::log10(1 + m * 4);
		const size_t u = static_cast<size_t>(b);
		band_peak[u] = std::max({v, band_peak[u] * 0.996, 0.05});
		raw[u] = std::clamp(v / band_peak[u], 0.0, 1.0);
	}
	for (int i = 0; i < kBars; i++) {
		const size_t u = static_cast<size_t>(i);
		const double v = 0.5 * raw[u] + 0.25 * raw[static_cast<size_t>(std::max(0, i - 1))] +
			0.25 * raw[static_cast<size_t>(std::min(kBars - 1, i + 1))];
		const double o = bars[u];
		bars[u] = v > o ? o + (v - o) * 0.65 : o + (v - o) * 0.16;
	}
}

double AudioLevel::bar(int i, int n, int ch) const {
	if (!live() || n <= 0) return 0;
	if (ch < 0) return std::max(bar(i, n, 0), bar(i, n, 1));
	const auto &bars = bars_[static_cast<size_t>(ch & 1)];
	if (n == kBars) return bars[static_cast<size_t>(std::clamp(i, 0, kBars - 1))];
	if (n < kBars) {
		// The loudest of its group.
		const double per = static_cast<double>(kBars) / n;
		const int a = static_cast<int>(i * per), b = std::max(a + 1, static_cast<int>((i + 1) * per));
		double m = 0;
		for (int k = a; k < b && k < kBars; k++) m = std::max(m, bars[static_cast<size_t>(k)]);
		return m;
	}
	const double pos = std::clamp((i + 0.5) / n * kBars - 0.5, 0.0, kBars - 1.0);
	const int a = static_cast<int>(pos);
	const double f = pos - a;
	return bars[static_cast<size_t>(a)] * (1 - f) + bars[static_cast<size_t>(std::min(kBars - 1, a + 1))] * f;
}

bool AudioLevel::live() const {
	return pid_ > 0 && now_ms() - last_data_ < 500;
}

double AudioLevel::level() const {
	if (!live()) return 0;
	return std::clamp(env_ / peak_, 0.0, 1.0);
}

double AudioLevel::pulse() const {
	if (!live()) return 0;
	return pulse_;
}

double AudioLevel::beat() const {
	if (!live()) return 0;
	const double since = now_ms() - last_beat_;
	return since < 0 ? 0 : std::exp(-since / 140.0);
}

} // namespace dusk
