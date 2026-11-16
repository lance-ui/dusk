#include "mediaview.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unistd.h>

#include "anim.hpp"
#include "audiolevel.hpp"
#include "glyphs.hpp"
#include "lyrics.hpp"
#include "popover.hpp"
#include "server.hpp"
#include "system.hpp"

namespace dusk {

std::string media_clock(double seconds) {
	const long s = std::max(0L, static_cast<long>(seconds));
	char buf[24];
	if (s >= 3600) {
		std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
	} else {
		std::snprintf(buf, sizeof buf, "%ld:%02ld", s / 60, s % 60);
	}
	return buf;
}

namespace {

// "spotify" -> "Spotify"; browsers' instance names ("firefox.instance_2_31") cut short.
std::string player_label(const std::string &player) {
	if (player.empty()) return "This computer";
	std::string p = player.substr(0, player.find('.'));
	static const std::pair<const char *, const char *> kNames[] = {
		{"firefox", "Firefox"}, {"chromium", "Chromium"}, {"chrome", "Chrome"}, {"brave", "Brave"},
		{"spotify", "Spotify"}, {"vlc", "VLC"}, {"mpv", "mpv"}, {"rhythmbox", "Rhythmbox"},
		{"elisa", "Elisa"}, {"amberol", "Amberol"}, {"io", "Music"}, {"edge", "Edge"}, {"msedge", "Edge"},
	};
	for (const auto &[k, v] : kNames) {
		if (p == k) return v;
	}
	if (!p.empty()) p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
	return p;
}

// The title: a step bigger than text, semibold (like Samsung's).
const char *title_font() {
	static std::string f;
	std::string fam = theme::font_store[0];
	const size_t sp = fam.rfind(' ');
	double size = 10;
	if (sp != std::string::npos) {
		size = std::atof(fam.c_str() + sp + 1);
		fam = fam.substr(0, sp);
	}
	char buf[32];
	std::snprintf(buf, sizeof buf, " Semibold %.1f", size + 2);
	f = fam + buf;
	return f.c_str();
}

void paint_cover(cairo_t *cr, cairo_surface_t *img, double x, double y, double w, double h) {
	const double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
	const double s = std::max(w / iw, h / ih);
	cairo_save(cr);
	cairo_translate(cr, x + (w - iw * s) / 2, y + (h - ih * s) / 2);
	cairo_scale(cr, s, s);
	cairo_set_source_surface(cr, img, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_paint(cr);
	cairo_restore(cr);
}

// The art, or a quiet placeholder, in a rounded square.
void draw_thumb(cairo_t *cr, IconLoader &icons, cairo_surface_t *art, double x, double y, double s, double r,
		const Color &bg, const Color &fg, float scale) {
	cairo_save(cr);
	rounded_rect(cr, x, y, s, s, r);
	cairo_clip(cr);
	if (art) {
		paint_cover(cr, art, x, y, s, s);
	} else {
		set_color(cr, bg);
		cairo_paint(cr);
		const double is = s * 0.42;
		draw_named_icon(cr, icons, "disc-3", x + (s - is) / 2, y + (s - is) / 2, is, fg, scale);
	}
	cairo_restore(cr);
}

} // namespace

MediaView::MediaView(Server *server) : server_(server) {}

MediaView::~MediaView() {
	*alive_ = false;
	set_shown(false);
	if (art_bg_) cairo_surface_destroy(art_bg_);
	if (art_) cairo_surface_destroy(art_);
}

void MediaView::set_shown(bool shown) {
	shown_ = shown;
	const bool want = shown && server_->status && server_->status->media_playing &&
		server_->config.media_wave_beat && server_->audio_level;
	if (want != listening_) {
		listening_ = want;
		if (want) server_->audio_level->acquire();
		else if (server_->audio_level) server_->audio_level->release();
	}
}

// The art's average colour, made light and a little more colourful (peach
// on warm art, pale blue on cool art...). Alpha 0: no art.
Color art_colour(cairo_surface_t *small) {
	if (!small) return Color{0, 0, 0, 0};
	cairo_surface_flush(small);
	const unsigned char *d = cairo_image_surface_get_data(small);
	const int iw = cairo_image_surface_get_width(small), ih = cairo_image_surface_get_height(small);
	const int stride = cairo_image_surface_get_stride(small);
	double r = 0, g = 0, b = 0, n = 0;
	for (int yy = 0; d && yy < ih; yy++) {
		for (int xx = 0; xx < iw; xx++) {
			const uint32_t px = *reinterpret_cast<const uint32_t *>(d + yy * stride + xx * 4);
			const double a = ((px >> 24) & 0xff) / 255.0;
			if (a < 0.5) continue;
			r += ((px >> 16) & 0xff) / 255.0 / a;
			g += ((px >> 8) & 0xff) / 255.0 / a;
			b += (px & 0xff) / 255.0 / a;
			n++;
		}
	}
	if (n <= 0) return Color{0, 0, 0, 0};
	Color avg{r / n, g / n, b / n, 1};
	const double grey = (avg.r + avg.g + avg.b) / 3;
	avg = Color{std::clamp(grey + (avg.r - grey) * 1.8, 0.0, 1.0), std::clamp(grey + (avg.g - grey) * 1.8, 0.0, 1.0),
		std::clamp(grey + (avg.b - grey) * 1.8, 0.0, 1.0), 1};
	return mix(avg, rgb(0xffffff), 0.5);
}

Color art_colour(const std::string &path) {
	if (path.empty()) return Color{0, 0, 0, 0};
	cairo_surface_t *small = load_image_sized(path, 40, 40);
	const Color c = art_colour(small);
	if (small) cairo_surface_destroy(small);
	return c;
}

void MediaView::load_art(float scale) {
	const SystemStatus &st = *server_->status;
	const int px = static_cast<int>(std::lround(120 * scale));
	if (st.media_art == art_path_ && art_px_ >= px) return;
	art_path_ = st.media_art;
	art_px_ = px;
	if (art_bg_) cairo_surface_destroy(art_bg_);
	if (art_) cairo_surface_destroy(art_);
	art_bg_ = art_ = nullptr;
	art_tint_ = Color{0, 0, 0, 0};
	if (art_path_.empty()) return;
	if (!server_->images) {
		art_ = load_image_sized(art_path_, px, px);
		art_bg_ = load_image_sized(art_path_, 40, 40);
		art_tint_ = art_colour(art_bg_);
		return;
	}
	// In the background (a big cover froze the card for a moment); drawn
	// when it arrives. (A newer song by then: the old picture is dropped.)
	const std::string path = art_path_;
	const unsigned gen = ++art_gen_;
	auto ready = [this, path, gen, alive = std::weak_ptr<bool>(alive_)](cairo_surface_t *img, bool small) {
		const auto a = alive.lock();
		if (!a || !*a || gen != art_gen_ || path != art_path_) {
			if (img) cairo_surface_destroy(img);
			return;
		}
		cairo_surface_t *&slot = small ? art_bg_ : art_;
		if (slot) cairo_surface_destroy(slot);
		slot = img;
		if (small) art_tint_ = art_colour(art_bg_);   // the progress bar's colour: from the art
		if (server_->status && server_->status->changed) server_->status->changed();   // drawn again with it
	};
	server_->images->request(path, px, px, [ready](cairo_surface_t *img) { ready(img, false); });
	server_->images->request(path, 40, 40, [ready](cairo_surface_t *img) { ready(img, true); });   // stretched over the card: soft
}

double MediaView::position() const {
	const SystemStatus &st = *server_->status;
	if (drag_s_ >= 0) return drag_s_;
	double p = st.media_position;
	if (st.media_playing) p += (now_ms() - st.media_read_at) / 1000.0;
	return st.media_length > 0 ? std::clamp(p, 0.0, st.media_length) : std::max(0.0, p);
}

void MediaView::drag(double x) {
	const SystemStatus &st = *server_->status;
	if (st.media_length <= 0 || seek_.width <= 0) return;
	drag_s_ = std::clamp((x - seek_.x) / seek_.width, 0.0, 1.0) * st.media_length;
}

void MediaView::end_drag() {
	if (drag_s_ >= 0) server_->status->seek(drag_s_);
	drag_s_ = -1;
}

bool MediaView::moving() const {
	const SystemStatus &st = *server_->status;
	return shown_ && st.media_available && (st.media_playing || marquee_on_ || amp_ > 0.05);
}

bool MediaView::step(double dt) {
	const SystemStatus &st = *server_->status;
	set_shown(shown_);   // starts / stops listening as playback starts and stops
	if (!shown_ || !st.media_available) return false;
	dt = std::min(dt, 100.0);
	const bool anim = server_->config.animations;
	AudioLevel *al = server_->audio_level.get();
	const bool live = listening_ && al && al->live();
	const double target_level = live ? al->level() : 0;
	// The hills are the music's recent past: every 40 ms how hard it hits
	// (mostly the bass, so kicks stand up), flowing away to the left from the
	// knob. Without sound to listen to, a slow sway.
	level_ += (target_level - level_) * std::min(1.0, dt / 120.0);
	double target_amp = st.media_playing && anim ? 1.0 : 0.0;
	amp_ += (target_amp - amp_) * std::min(1.0, dt / 220.0);
	if (st.media_playing && anim) {
		phase_ += dt / 1000.0 * 0.12;
		sample_t_ += dt;
		while (sample_t_ >= kSampleMs) {
			sample_t_ -= kSampleMs;
			double v;
			if (live) {
				const double hit = std::max(al->pulse(), 0.85 * al->beat());
				v = std::clamp(0.12 + 0.30 * level_ + 0.75 * hit, 0.0, 1.0);
			} else {
				// Nothing to listen to: gentle made-up hills.
				const double k = static_cast<double>(samples_++) * 0.22;
				v = 0.32 + 0.18 * std::sin(k) + 0.12 * std::sin(k * 2.7 + 1.0);
			}
			history_.push_front(v);
			if (history_.size() > 96) history_.pop_back();
		}
	}
	// The visualizer: the music's spectrum (24 bars, already smoothed), or
	// Arqen's made-up beat when there's nothing to hear (a kick about twice a
	// second, a shimmer on top); paused, everything rests low and still.
	{
		const double rest = 0.06;
		std::array<std::array<double, 24>, 2> raw{};
		if (st.media_playing && anim) {
			if (live) {
				for (int ch = 0; ch < 2; ch++) {
					for (int i = 0; i < 24; i++) {
						raw[static_cast<size_t>(ch)][static_cast<size_t>(i)] = std::max(rest, al->bar(i, 24, ch));
					}
				}
			} else {
				fake_t_ += dt / 1000.0;
				const double kick = std::pow(std::max(0.0, std::sin(fake_t_ * M_PI * 4)), 5);
				for (int ch = 0; ch < 2; ch++) {
					for (int i = 0; i < 24; i++) {
						const double low = 1 - i / 34.0;
						// The two sides differ a little, like a real stereo mix.
						raw[static_cast<size_t>(ch)][static_cast<size_t>(i)] = std::min(1.0, 0.1 + kick * low * 0.9 +
							0.3 * std::fabs(std::sin(fake_t_ * (2.5 + i * 0.45) + i * 1.3 + ch * 0.9)) * (0.35 + i / 40.0));
					}
				}
			}
		} else {
			for (auto &side : raw) side.fill(rest);
		}
		// Quick up, slow down (live bars are smoothed already: only a touch more).
		const double up = 1 - std::pow(live ? 0.1 : 0.35, dt / 33.0), down = 1 - std::pow(live ? 0.6 : 0.84, dt / 33.0);
		const double slow_up = 1 - std::pow(0.7, dt / 33.0), slow_down = 1 - std::pow(0.93, dt / 33.0);
		for (size_t ch = 0; ch < 2; ch++) {
			for (size_t i = 0; i < 24; i++) {
				const double v = raw[ch][i], o = viz_[ch][i], so = viz_slow_[ch][i];
				viz_[ch][i] = o + (v - o) * (v > o ? up : down);
				viz_slow_[ch][i] = so + (v - so) * (v > so ? slow_up : slow_down);
				// A beat: the band jumps well above its usual level. Steady
				// sound (a held note, a pad) leaves the line flat.
				const double avg = viz_avg_[ch][i];
				const double jump = std::clamp((v - avg - 0.06) * 2.6, 0.0, 1.0);
				viz_avg_[ch][i] = avg + (v - avg) * (1 - std::pow(0.96, dt / 33.0));
				viz_kick_[ch][i] = std::max(viz_kick_[ch][i] * std::pow(0.80, dt / 33.0), jump);
			}
		}
	}

	// What the hills show: the music's hit right now, quick up and gently down.
	{
		double now = 0;
		if (st.media_playing && anim) {
			now = live ? std::clamp(0.10 + 0.30 * level_ + 0.80 * std::max(al->pulse(), 0.85 * al->beat()), 0.0, 1.0)
				: (history_.empty() ? 0.3 : history_[0]);
		}
		energy_ += (now - energy_) * std::min(1.0, dt / (now > energy_ ? 35.0 : 190.0));
		energy_slow_ += (now - energy_slow_) * std::min(1.0, dt / (now > energy_slow_ ? 80.0 : 360.0));
	}

	if (marquee_on_ && anim) {
		if (marquee_wait_ > 0) {
			marquee_wait_ -= dt;
		} else {
			marquee_off_ += dt / 1000.0 * 34;
			if (marquee_off_ >= marquee_w_ + 48) {
				marquee_off_ = 0;
				marquee_wait_ = 2200;
			}
		}
	}
	return moving();
}

// The title: as it is when it fits; otherwise sliding along, faded at the ends.
void MediaView::draw_title(cairo_t *cr, const std::string &text, double x, double y, double w, double h,
		const char *font, const Color &color) {
	if (text != marquee_text_) {
		marquee_text_ = text;
		marquee_off_ = 0;
		marquee_wait_ = 2200;
	}
	const int tw = text_width(text, font);
	marquee_w_ = tw;
	marquee_on_ = server_->config.media_marquee && tw > w + 1;
	if (!marquee_on_) {
		draw_text(cr, text, x, y, w, h, font, color);
		return;
	}
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_push_group(cr);
	const double gap = 48;
	draw_text(cr, text, x - marquee_off_, y, tw + 4, h, font, color);
	draw_text(cr, text, x - marquee_off_ + tw + gap, y, tw + 4, h, font, color);
	cairo_pop_group_to_source(cr);
	// Soft edges: the text fades out at the right, and at the left while moving.
	cairo_pattern_t *mask = cairo_pattern_create_linear(x, 0, x + w, 0);
	const double edge = std::min(0.12, 18.0 / w);
	cairo_pattern_add_color_stop_rgba(mask, 0, 0, 0, 0, marquee_off_ > 0.5 ? 0 : 1);
	cairo_pattern_add_color_stop_rgba(mask, edge, 0, 0, 0, 1);
	cairo_pattern_add_color_stop_rgba(mask, 1 - edge, 0, 0, 0, 1);
	cairo_pattern_add_color_stop_rgba(mask, 1, 0, 0, 0, 0);
	cairo_mask(cr, mask);
	cairo_pattern_destroy(mask);
	cairo_restore(cr);
}

// Bar i of n from the 24 (fewer: the loudest of each group; more: in between).
double MediaView::side_value(int ch, double pos, bool slow) const {
	const auto &v = (slow ? viz_slow_ : viz_)[static_cast<size_t>(ch & 1)];
	const double p = std::clamp(pos, 0.0, 1.0) * 23;
	const int a = static_cast<int>(p);
	const double f = p - a, e = (1 - std::cos(f * M_PI)) / 2;   // eased between bands: no corners
	return v[static_cast<size_t>(a)] * (1 - e) + v[static_cast<size_t>(std::min(23, a + 1))] * e;
}

double MediaView::viz_value(int i, int n) const {
	// Left half: the left channel, treble at the edge, bass in the middle;
	// right half the same for the right channel.
	const double u = (i + 0.5) / n;   // 0..1 across
	if (u < 0.5) return side_value(0, (0.5 - u) * 2);
	return side_value(1, (u - 0.5) * 2);
}

// Behind everything on the card, like Arqen's media card: the beat drawn as
// bars (or a mirror, dots, a wave). Nothing moves sideways: each bar has its
// place and only its height follows the music.
void MediaView::draw_sound(cairo_t *cr, double x, double y, double w, double h) {
	const std::string &kind = server_->config.media_viz;
	if (kind == "off") return;
	if (kind == "hills") {
		draw_hills(cr, x, y, w, h);
		return;
	}
	const std::string &tint_kind = server_->config.media_viz_color;
	const Color art = art_tint_.a > 0 ? art_tint_ : mix(theme::accent, rgb(0xffffff), 0.45);
	auto col = [&](int i, int n) -> Color {
		if (tint_kind == "rainbow") {
			// hsl(i/n, 75%, 62%)
			const double hh = static_cast<double>(i) / std::max(1, n) * 6, s = 0.75, l = 0.62;
			const double c = (1 - std::fabs(2 * l - 1)) * s, xx = c * (1 - std::fabs(std::fmod(hh, 2.0) - 1)), m = l - c / 2;
			double r = 0, g = 0, b = 0;
			if (hh < 1) { r = c; g = xx; } else if (hh < 2) { r = xx; g = c; } else if (hh < 3) { g = c; b = xx; }
			else if (hh < 4) { g = xx; b = c; } else if (hh < 5) { r = xx; b = c; } else { r = c; b = xx; }
			return Color{r + m, g + m, b + m, 1};
		}
		if (tint_kind == "accent") return theme::accent;
		if (tint_kind == "text") return plain_ ? theme::text : rgb(0xffffff);
		return art;
	};
	// A band along the bottom, inset like Arqen's (8 px), most of the card tall.
	const double inset = 8;
	const double bx = x + inset, bw = w - 2 * inset;
	const double bh = (h - 2 * inset) * (plain_ ? 0.7 : 0.62), by = y + h - inset - bh;
	const double alpha = 0.30;
	cairo_save(cr);
	cairo_push_group(cr);
	if (kind == "bars" || kind == "mirror") {
		const int n = w < 260 ? 10 : 12;
		const double gap = std::max(2.0, std::min(4.0, bw / (n * 4.0)));
		const double barw = std::max(2.0, (bw - gap * (n - 1)) / n);
		for (int i = 0; i < n; i++) {
			const double v = viz_value(i, n);
			const double bhh = std::max(barw, v * bh);
			const double px = bx + i * (barw + gap);
			const double py = kind == "mirror" ? by + (bh - bhh) / 2 : by + bh - bhh;
			fill_rounded(cr, px, py, barw, bhh, barw / 2, col(i, n));
		}
	} else if (kind == "dots") {
		const int n = 12;
		int rows = static_cast<int>(bh / 9);
		if (rows % 2 == 0) rows--;
		rows = std::clamp(rows, 3, 9);
		const double dd = std::max(2.0, std::min(bw / n * 0.7, bh / rows * 0.78));
		const double mid = (rows - 1) / 2.0;
		for (int c = 0; c < n; c++) {
			const double v = viz_value(c, n);
			const Color k = col(c, n);
			for (int r = 0; r < rows; r++) {
				const double lit = std::clamp(v * (mid + 1) + 0.15 - std::fabs(r - mid), 0.0, 1.0);
				cairo_new_path(cr);
				cairo_arc(cr, bx + (c + 0.5) * (bw / n), by + (r + 0.5) * (bh / rows), dd / 2, 0, 2 * M_PI);
				cairo_set_source_rgba(cr, k.r, k.g, k.b, 0.14 + 0.86 * lit);
				cairo_fill(cr);
			}
		}
	} else if (kind == "wave") {
		// A flat line, like a heart monitor's, that spikes where there's a
		// beat: each band (left channel on the left, right on the right,
		// bass in the middle) kicks up a sharp peak when it suddenly jumps,
		// with a small dip either side, and settles back flat.
		const int n = 16;
		const double lw = std::max(1.6, std::min(2.6, bh * 0.10));
		const double amp = std::max(2.0, std::min(bh * 0.80, 34.0));
		const double base = by + bh * 0.70;
		const double gap = bw / n, sigma = gap * 0.26;
		double kick[n];
		for (int i = 0; i < n; i++) {
			const double u = (i + 0.5) / n;
			const int ch = u < 0.5 ? 0 : 1;
			const double pos = u < 0.5 ? (0.5 - u) * 2 : (u - 0.5) * 2;
			const double p = pos * 23;
			const int a = static_cast<int>(p);
			const double f = p - a;
			const auto &k = viz_kick_[static_cast<size_t>(ch)];
			kick[i] = k[static_cast<size_t>(a)] * (1 - f) + k[static_cast<size_t>(std::min(23, a + 1))] * f;
		}
		const int steps = std::max(60, static_cast<int>(bw / 1.5));
		cairo_new_path(cr);
		for (int j = 0; j <= steps; j++) {
			const double px = bx + bw * j / steps;
			double y = 0;
			for (int i = 0; i < n; i++) {
				if (kick[i] < 0.01) continue;
				const double d = (px - (bx + (i + 0.5) * gap)) / sigma;
				if (std::fabs(d) > 4) continue;
				// A peak with a shallow dip each side (and alternating up/down
				// a little, so neighbours don't merge into one hump).
				y += kick[i] * (1 - 0.55 * d * d) * std::exp(-d * d / 2) * (i % 2 ? 0.85 : 1.0);
			}
			const double py = base - std::clamp(y, -0.35, 1.1) * amp;
			if (j == 0) cairo_move_to(cr, px, py);
			else cairo_line_to(cr, px, py);
		}
		const Color k = col(n / 2, n);
		cairo_set_source_rgba(cr, k.r, k.g, k.b, 1);
		cairo_set_line_width(cr, lw);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke(cr);
	}
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, (kind == "wave" ? 0.55 : kind == "dots" ? 0.45 : alpha + 0.12) * (plain_ ? 0.55 : 1.0));
	cairo_restore(cr);
}

// Hills: the sound as a landscape. Across the card is the spectrum in
// stereo (the left channel on the left, the right on the right, the bass
// meeting in the middle), drawn as a smooth hillside: the front follows the
// music closely, the one behind it more slowly. Nothing moves sideways; the
// land rises where the music is loud.
void MediaView::draw_hills(cairo_t *cr, double x, double y, double w, double h) {
	const Color tint = art_tint_.a > 0 ? art_tint_ : mix(theme::accent, rgb(0xffffff), 0.45);
	const double base = y + h;
	const double tall = h * 0.42;
	for (int layer = 1; layer >= 0; layer--) {
		const bool slow = layer == 1;
		cairo_new_path(cr);
		cairo_move_to(cr, x - 2, base);
		for (double px = x - 2; px <= x + w + 2; px += 2) {
			const double u = std::clamp((px - x) / w, 0.0, 1.0);
			const double v = u < 0.5 ? side_value(0, (0.5 - u) * 2, slow) : side_value(1, (u - 0.5) * 2, slow);
			// Low ground everywhere, rising with the music; the back hills a little higher and softer.
			const double hgt = (0.08 + 0.92 * v) * (slow ? 1.08 : 0.85);
			cairo_line_to(cr, px, base - tall * hgt);
		}
		cairo_line_to(cr, x + w + 2, base);
		cairo_close_path(cr);
		cairo_pattern_t *g = cairo_pattern_create_linear(0, base - tall, 0, base);
		const double a = slow ? 0.16 : 0.26;
		cairo_pattern_add_color_stop_rgba(g, 0, tint.r, tint.g, tint.b, a * 0.25);
		cairo_pattern_add_color_stop_rgba(g, 0.5, tint.r, tint.g, tint.b, a);
		cairo_pattern_add_color_stop_rgba(g, 1, tint.r, tint.g, tint.b, a * 1.3);
		cairo_set_source(cr, g);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
	}
}

// The progress line, like Samsung's: a thick bar, the part played in a light
// colour taken from the album art, and a round knob.
void MediaView::draw_wave(cairo_t *cr, double x0, double y, double w, const Color &played, const Color &rest,
		std::vector<MediaHot> *hots) {
	const SystemStatus &st = *server_->status;
	seek_ = wlr_box{static_cast<int>(x0), static_cast<int>(y - 14), static_cast<int>(w), 28};
	const double frac = st.media_length > 0 ? std::clamp(position() / st.media_length, 0.0, 1.0) : 0;
	const double th = 7;                 // the bar's thickness
	const double kx = x0 + w * frac;
	const Color tint = art_tint_.a > 0 ? art_tint_ : mix(theme::accent, rgb(0xffffff), 0.45);
	(void)played;
	// The rest of the song.
	fill_rounded(cr, x0, y - th / 2, w, th, th / 2, rest);
	if (kx > x0 + 1) {
		fill_rounded(cr, x0, y - th / 2, std::max(th, kx - x0 + th / 2), th, th / 2, tint);
	}
	// The knob.
	const double kr = dragging() ? 9.5 : 8;
	cairo_new_path(cr);
	cairo_arc(cr, kx, y, kr, 0, 2 * M_PI);
	set_color(cr, mix(tint, rgb(0xffffff), 0.55));
	cairo_fill_preserve(cr);
	set_color(cr, rgb(0x000000, 0.25));
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	if (hots && st.media_length > 0) {
		hots->push_back(MediaHot{"mp:seek", wlr_box{static_cast<int>(x0 - 8), static_cast<int>(y - 16),
			static_cast<int>(w + 16), 32}, -1});
	}
}

void MediaView::draw(cairo_t *cr, double x, double y, double w, double h, double radius, float scale, Look look,
		std::vector<MediaHot> *hots) {
	load_art(scale);
	if (look == Look::Samsung) {
		draw_samsung(cr, x, y, w, h, radius, scale, hots);
	} else {
		draw_plain(cr, x, y, w, h, radius, scale, look, hots);
	}
}

void MediaView::draw_samsung(cairo_t *cr, double x, double y, double w, double h, double r, float scale,
		std::vector<MediaHot> *hots) {
	const SystemStatus &st = *server_->status;
	IconLoader &icons = server_->icons;
	// 0.35, calm: a plain card in the control centre's colours; the cover
	// sits beside the title, the accent marks play / pause and the progress.
	const Color white = theme::text, soft = theme::text_dim;
	const Color chip = with_alpha(theme::text, theme::dark ? 0.07 : 0.06);
	fill_rounded(cr, x, y, w, h, r, theme::card_bg);
	if (theme::dark) {
		rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r);
		set_color(cr, with_alpha(theme::text, 0.05));
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}

	const bool lyrics = server_->lyrics && server_->config.lyrics && st.media_available;
	const bool found = lyrics && server_->lyrics->available();
	const std::string title = st.media_available ? (st.media_title.empty() ? "Playing" : st.media_title)
		: "Nothing playing";
	const double pad = h >= 140 ? 18 : 14;

	auto control_row = [&](double cx, double cy, double gap, double big, double small) {
		if (!st.media_available) return;
		const struct {
			const char *id, *icon;
			double dx, size;
		} kButtons[] = {
			{"mp:previous", "previous", -gap, small},
			{"mp:play-pause", st.media_playing ? "pause" : "play", 0, big},
			{"mp:next", "next", gap, small},
		};
		for (const auto &b : kButtons) {
			if (b.size <= 0) continue;   // only play / pause
			const bool main = b.dx == 0;
			if (main) {
				// Play / pause: a round accent button.
				const double d = b.size + 20;
				cairo_new_path(cr);
				cairo_arc(cr, cx, cy, d / 2, 0, 2 * M_PI);
				set_color(cr, theme::accent);
				cairo_fill(cr);
			}
			const double is = main ? b.size - 6 : b.size;
			draw_named_icon(cr, icons, b.icon, cx + b.dx - is / 2, cy - is / 2, is, main ? theme::on_accent : white, scale);
			if (hots) {
				const double hit = b.size + 18;
				hots->push_back(MediaHot{b.id, wlr_box{static_cast<int>(cx + b.dx - hit / 2),
					static_cast<int>(cy - hit / 2), static_cast<int>(hit), static_cast<int>(hit)}, hit / 2});
			}
		}
	};

	if (w < 170) {
		// Small: just the art, play / pause in the middle (and next / previous
		// above and below when it's tall), the progress along the bottom.
		const double cx = x + w / 2, cy = y + h / 2;
		if (!st.media_available) {
			draw_named_icon(cr, icons, "disc-3", cx - 12, cy - 12, 24, soft, scale);
		} else if (h >= 150) {
			const struct {
				const char *id, *icon;
				double dy, size;
			} kButtons[] = {
				{"mp:previous", "previous", -h / 3.2, 20}, {"mp:play-pause", st.media_playing ? "pause" : "play", 0, 26},
				{"mp:next", "next", h / 3.2, 20},
			};
			for (const auto &bt : kButtons) {
				draw_named_icon(cr, icons, bt.icon, cx - bt.size / 2, cy + bt.dy - bt.size / 2, bt.size, white, scale);
				if (hots) {
					const double hit = std::min(w, bt.size + 18);
					hots->push_back(MediaHot{bt.id, wlr_box{static_cast<int>(cx - hit / 2), static_cast<int>(cy + bt.dy - hit / 2),
						static_cast<int>(hit), static_cast<int>(hit)}, hit / 2});
				}
			}
		} else {
			control_row(cx, cy, 0, 26, 0);
		}
		if (st.media_available && st.media_length > 0) {
			const double frac = std::clamp(position() / st.media_length, 0.0, 1.0);
			const double bw = w - 24, by = y + h - 12;
			fill_rounded(cr, x + 12, by, bw, 4, 2, with_alpha(theme::text, 0.14));
			fill_rounded(cr, x + 12, by, std::max(4.0, bw * frac), 4, 2, theme::accent);
		}
	} else if (h >= 140) {
		// Full: app and output on top, title and artist, the wave and times,
		// the controls along the bottom.
		const double top = y + pad - 2;
		// The media source: the app's icon and name, a chip that picks
		// another player when several are playing.
		const std::string from = player_label(st.media_player);
		const size_t count = st.player_list().size();
		const int cw = text_width(from, theme::font_small) + 16 + 8 + (count > 1 ? 16 : 0) + 14;
		fill_rounded(cr, x + pad - 6, top - 4, cw, 26, std::min(13.0, theme::small_radius(13)), chip);
		const std::string base = st.media_player.substr(0, st.media_player.find('.'));
		const std::string icon = base.empty() ? "" : server_->apps.icon_for(base);
		if (!icon.empty()) draw_icon(cr, icons, icon, from, x + pad, top + 1, 16, scale);
		else draw_named_icon(cr, icons, "play", x + pad + 3, top + 4, 10, soft, scale);
		draw_text(cr, from, x + pad + 22, top - 4, cw - 28, 26, theme::font_small, white);
		if (count > 1) {
			draw_named_icon(cr, icons, "chevron-down", x + pad - 6 + cw - 22, top + 2, 14, soft, scale);
		}
		if (hots) {
			hots->push_back(MediaHot{"mp:source", wlr_box{static_cast<int>(x + pad - 6), static_cast<int>(top - 4),
				cw, 26}, 13});
		}
		const std::string out = "Media output";
		const int ow = text_width(out, theme::font_small) + 24;
		const double ox = x + w - pad - ow;
		fill_rounded(cr, ox, top - 4, ow, 26, std::min(13.0, theme::small_radius(13)), chip);
		draw_text(cr, out, ox, top - 4, ow, 26, theme::font_small, white, Align::Center);
		if (hots) hots->push_back(MediaHot{"mp:output", wlr_box{static_cast<int>(ox), static_cast<int>(top - 4), ow, 26}, 13});
		if (lyrics) {
			const double lx = ox - 32;
			cairo_new_path(cr);
			cairo_arc(cr, lx + 13, top + 9, 13, 0, 2 * M_PI);
			const bool lit = server_->config.lyrics_show && found;
			set_color(cr, lit ? theme::accent : chip);
			cairo_fill(cr);
			draw_named_icon(cr, icons, "mic-vocal", lx + 6, top + 2, 14,
				lit ? theme::on_accent : found ? white : with_alpha(theme::text, 0.45), scale);
			if (hots) hots->push_back(MediaHot{"mp:lyrics", wlr_box{static_cast<int>(lx), static_cast<int>(top - 4), 26, 26}, 13});
		}
		// The cover beside the title and artist.
		const double as = 52, ay = top + 34;
		draw_thumb(cr, icons, art_, x + pad, ay, as, std::min(12.0, r * 0.6), chip, soft, scale);
		const double tx = x + pad + as + 12, tw = x + w - pad - tx;
		draw_title(cr, title, tx, ay + 4, tw, 24, title_font(), white);
		draw_text(cr, st.media_available ? st.media_artist : std::string(), tx, ay + 28, tw, 18, theme::font_small, soft);
		const double wy = y + h - 92;
		const Color rest = with_alpha(theme::text, 0.16);
		if (st.media_available && st.media_length > 0) {
			draw_wave(cr, x + pad + 2, wy, w - 2 * pad - 4, theme::accent, rest, hots);
			draw_text(cr, media_clock(position()), x + pad, wy + 9, 60, 14, theme::font_small, soft);
			draw_text(cr, media_clock(st.media_length), x + w - pad - 60, wy + 9, 60, 14, theme::font_small, soft,
				Align::Right);
		} else if (st.media_available) {
			draw_wave(cr, x + pad + 2, wy, w - 2 * pad - 4, theme::accent, rest, nullptr);
		}
		control_row(x + w / 2, y + h - 34, 72, 24, 22);
	} else if (h >= 84) {
		// Medium: title and artist with the controls beside them, the wave under.
		const double ctl_w = st.media_available ? 120 : 0;
		draw_title(cr, title, x + pad, y + pad - 4, w - 2 * pad - ctl_w, 22, theme::font_bold, white);
		draw_text(cr, st.media_available ? st.media_artist : std::string(), x + pad, y + pad + 17, w - 2 * pad - ctl_w, 16, theme::font_small, soft);
		control_row(x + w - pad - ctl_w / 2, y + pad + 12, 38, 22, 18);
		if (st.media_available) {
			draw_wave(cr, x + pad + 2, y + h - pad - 4, w - 2 * pad - 4, theme::accent, with_alpha(theme::text, 0.16),
				st.media_length > 0 ? hots : nullptr);
		}
	} else {
		// One row: the cover, title and artist, play/pause; a thin wave along the bottom.
		const double as = h - 16;
		draw_thumb(cr, icons, art_, x + 8, y + 8, as, std::max(6.0, r - 8), chip, soft, scale);
		const double tx = x + 8 + as + 10;
		const double ctl = st.media_available ? (w >= 220 ? 104 : 40) : 0;
		const double tw = x + w - tx - ctl - 6;
		draw_title(cr, title, tx, y + h / 2 - 18, tw, 20, theme::font_bold, white);
		draw_text(cr, st.media_available ? st.media_artist : std::string(), tx, y + h / 2 + 1, tw, 16, theme::font_small, soft);
		if (st.media_available) {
			if (w >= 220) control_row(x + w - 6 - ctl / 2, y + h / 2 - 2, 34, 20, 16);
			else control_row(x + w - 6 - ctl / 2, y + h / 2 - 2, 0, 20, 0);
			if (st.media_length > 0) {
				const double frac = std::clamp(position() / st.media_length, 0.0, 1.0);
				cairo_save(cr);
				rounded_rect(cr, x, y, w, h, r);
				cairo_clip(cr);
				cairo_rectangle(cr, x, y + h - 3, w * frac, 3);
				set_color(cr, theme::accent);
				cairo_fill(cr);
				cairo_restore(cr);
			}
		}
	}
	if (hots && st.media_available) {
		// The rest of the card: right-click options, a click does nothing.
		hots->insert(hots->begin(), MediaHot{"mp:card", wlr_box{static_cast<int>(x), static_cast<int>(y),
			static_cast<int>(w), static_cast<int>(h)}, -2});
	}
}

// The other looks: on the card colours of their control centre.
void MediaView::draw_plain(cairo_t *cr, double x, double y, double w, double h, double r, float scale, Look look,
		std::vector<MediaHot> *hots) {
	const SystemStatus &st = *server_->status;
	IconLoader &icons = server_->icons;
	const std::string title = st.media_available ? (st.media_title.empty() ? "Playing" : st.media_title)
		: look == Look::Mac ? "Not Playing" : "Nothing playing";
	auto button = [&](const char *id, const char *icon, double cx, double cy, double size, bool round, const Color &fg) {
		if (round) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, size * 0.9, 0, 2 * M_PI);
			set_color(cr, theme::tile_off);
			cairo_fill(cr);
		}
		draw_named_icon(cr, icons, icon, cx - size / 2, cy - size / 2, size, fg, scale);
		if (hots) {
			const double hit = size + 16;
			hots->push_back(MediaHot{id, wlr_box{static_cast<int>(cx - hit / 2), static_cast<int>(cy - hit / 2),
				static_cast<int>(hit), static_cast<int>(hit)}, hit / 2});
		}
	};
	const char *play = st.media_playing ? "pause" : "play";
	fill_rounded(cr, x, y, w, h, r, theme::card_bg);
	// The beat behind it too, when the card is big enough.
	if (st.media_available && h >= 84 && w >= 170 && look != Look::Gnome) {
		cairo_save(cr);
		rounded_rect(cr, x, y, w, h, r);
		cairo_clip(cr);
		plain_ = true;
		draw_sound(cr, x, y, w, h);
		plain_ = false;
		cairo_restore(cr);
	}
	if (look == Look::Windows) {
		// Windows 11: the app on top, title and artist, the cover at the right,
		// the controls centred underneath.
		const double pad = 16, as = std::min(64.0, h - 64);
		draw_text(cr, player_label(st.media_player), x + pad, y + 10, w / 2, 18, theme::font_small, theme::text_dim);
		draw_thumb(cr, icons, art_, x + w - pad - as, y + 14, as, 6, theme::tile_off, theme::text_dim, scale);
		draw_title(cr, title, x + pad, y + 32, w - 3 * pad - as, 22, theme::font_bold, theme::text);
		draw_text(cr, st.media_available ? st.media_artist : std::string(), x + pad, y + 54, w - 3 * pad - as, 18, theme::font_small, theme::text_dim);
		if (st.media_available) {
			const double cy = y + h - 24;
			button("mp:previous", "previous", x + w / 2 - 56, cy, 16, false, theme::text);
			button("mp:play-pause", play, x + w / 2, cy, 18, false, theme::text);
			button("mp:next", "next", x + w / 2 + 56, cy, 16, false, theme::text);
		}
	} else if (look == Look::Gnome) {
		// GNOME: one row: cover, title over artist, small controls at the end.
		const double as = std::min(44.0, h - 16);
		draw_thumb(cr, icons, art_, x + 10, y + (h - as) / 2, as, 8, theme::tile_off, theme::text_dim, scale);
		const double tx = x + 10 + as + 12, ctl = st.media_available ? 104 : 0;
		draw_title(cr, title, tx, y + h / 2 - 19, x + w - tx - ctl - 8, 20, theme::font_bold, theme::text);
		draw_text(cr, st.media_available ? st.media_artist : std::string(), tx, y + h / 2 + 1, x + w - tx - ctl - 8, 18, theme::font_small, theme::text_dim);
		if (st.media_available) {
			const double cx = x + w - 10 - ctl / 2, cy = y + h / 2;
			button("mp:previous", "previous", cx - 34, cy, 15, false, theme::text);
			button("mp:play-pause", play, cx, cy, 16, false, theme::text);
			button("mp:next", "next", cx + 34, cy, 15, false, theme::text);
		}
	} else {
		// macOS: the cover at the top left, title and artist, controls below.
		const double pad = 14, as = std::min(46.0, h * 0.34);
		draw_thumb(cr, icons, art_, x + pad, y + pad, as, 8, theme::tile_off, theme::text_dim, scale);
		draw_title(cr, title, x + pad, y + pad + as + 6, w - 2 * pad, 20, theme::font_bold, theme::text);
		draw_text(cr, st.media_available ? st.media_artist : std::string(), x + pad, y + pad + as + 25, w - 2 * pad, 17, theme::font_small, theme::text_dim);
		const double cy = y + h - 22;
		const Color fg = st.media_available ? theme::text : theme::text_dim;
		button("mp:previous", "previous", x + w / 2 - 38, cy, 16, false, fg);
		button("mp:play-pause", play, x + w / 2, cy, 20, false, fg);
		button("mp:next", "next", x + w / 2 + 38, cy, 16, false, fg);
	}
	if (hots && st.media_available) {
		hots->insert(hots->begin(), MediaHot{"mp:card", wlr_box{static_cast<int>(x), static_cast<int>(y),
			static_cast<int>(w), static_cast<int>(h)}, -2});
	}
}

// Where sound goes: every output, the one in use ticked; with the ones
// that need their sound card switched first, like a monitor's or a TV's
// HDMI / DisplayPort (they're "off" until picked, so PipeWire alone doesn't
// list them). From dusk-audio-outputs; wpctl's list if that isn't there.
void sound_output_menu(Server *srv, Output *out, double lx, double ly) {
	const std::string helper = srv->helper_path("dusk-audio-outputs");
	auto show = [srv, out, lx, ly](std::vector<MenuItem> outputs) {
		std::vector<MenuItem> items;
		items.push_back(MenuItem::header("Play sound on"));
		if (outputs.empty()) items.push_back(MenuItem::action("No sound outputs found", [] {}));
		for (MenuItem &m : outputs) items.push_back(std::move(m));
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::check("Mute", srv->status->muted, [srv] { srv->status->toggle_mute(); }));
		for (const char *mixer : {"pwvucontrol", "pavucontrol"}) {
			if (access((std::string("/usr/bin/") + mixer).c_str(), X_OK) == 0) {
				items.push_back(MenuItem::action("Sound settings…", [srv, m = std::string(mixer)] { srv->spawn(m); }, "gear"));
				break;
			}
		}
		srv->menu->show(out, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point", std::move(items));
	};
	auto icon_for = [](const std::string &label) {
		std::string l = label;
		std::transform(l.begin(), l.end(), l.begin(), ::tolower);
		if (l.find("hdmi") != std::string::npos || l.find("displayport") != std::string::npos ||
				l.find(" tv") != std::string::npos) return std::string("monitor");
		if (l.find("headphone") != std::string::npos || l.find("headset") != std::string::npos ||
				l.find("bluez") != std::string::npos || l.find("buds") != std::string::npos) return std::string("headphones");
		if (l.find("usb") != std::string::npos) return std::string("usb");
		return std::string("speaker");
	};
	if (!helper.empty()) {
		srv->run_capture(shell_quote(helper) + " 2>/dev/null", [srv, show, helper, icon_for](const std::string &text) {
			std::vector<MenuItem> outputs;
			std::istringstream in(text);
			std::string line;
			while (std::getline(in, line)) {
				std::vector<std::string> f;
				size_t at = 0, tab;
				while ((tab = line.find('\t', at)) != std::string::npos) {
					f.push_back(line.substr(at, tab - at));
					at = tab + 1;
				}
				f.push_back(line.substr(at));
				if (f.size() == 4 && f[0] == "SINK") {
					MenuItem m = MenuItem::radio(f[3], f[2] == "1", [srv, helper, name = f[1]] {
						srv->run_capture(shell_quote(helper) + " --sink " + shell_quote(name) + " >/dev/null 2>&1",
							[srv](const std::string &) { srv->status->refresh(); });
					});
					outputs.push_back(std::move(m));
				} else if (f.size() == 4 && f[0] == "PORT") {
					// Not on yet: picking it switches its card over.
					outputs.push_back(MenuItem::action(f[3], [srv, helper, card = f[1], profile = f[2]] {
						srv->run_capture(shell_quote(helper) + " --port " + shell_quote(card) + " " + shell_quote(profile) +
							" >/dev/null 2>&1", [srv](const std::string &) { srv->status->refresh(); });
					}, icon_for(f[3])));
				}
			}
			show(std::move(outputs));
		});
		return;
	}
	srv->run_capture("wpctl status 2>/dev/null", [srv, show](const std::string &text) {
		std::vector<MenuItem> outputs;
		bool audio = false, sinks = false;
		std::istringstream in(text);
		std::string line;
		while (std::getline(in, line)) {
			if (line == "Audio") audio = true;
			else if (line == "Video" || line == "Settings") audio = false;
			if (!audio) continue;
			if (line.find("Sinks:") != std::string::npos) {
				sinks = true;
				continue;
			}
			if (sinks && (line.find("├─") != std::string::npos || line.find("└─") != std::string::npos)) {
				sinks = false;
			}
			if (!sinks) continue;
			// " │  *   57. Built-in Audio Analog Stereo        [vol: 0.40]"
			const bool current = line.find('*') != std::string::npos;
			size_t i = 0;
			while (i < line.size() && !std::isdigit(static_cast<unsigned char>(line[i]))) i++;
			const size_t dot = line.find('.', i);
			if (i >= line.size() || dot == std::string::npos) continue;
			const std::string id = line.substr(i, dot - i);
			std::string name = line.substr(dot + 1);
			name = name.substr(0, name.find(" [vol"));
			while (!name.empty() && name.front() == ' ') name.erase(0, 1);
			while (!name.empty() && name.back() == ' ') name.pop_back();
			if (name.empty()) continue;
			outputs.push_back(MenuItem::radio(name, current, [srv, id] {
				srv->run_capture("wpctl set-default " + id + " >/dev/null 2>&1",
					[srv](const std::string &) { srv->status->refresh(); });
			}));
		}
		show(std::move(outputs));
	});
}

// The players: pick which one the cards show (and control), or open its window.
void media_source_menu(Server *srv, Output *out, double lx, double ly) {
	const SystemStatus &st = *srv->status;
	std::vector<MenuItem> items;
	items.push_back(MenuItem::header("Media source"));
	for (const auto &[name, title] : st.player_list()) {
		const std::string label = player_label(name) + (title.empty() ? "" : " · " + title);
		items.push_back(MenuItem::radio(label, name == st.media_player, [srv, n = name] { srv->status->pick_player(n); }));
	}
	if (!st.media_player.empty()) {
		items.push_back(MenuItem::separator());
		items.push_back(MenuItem::action("Open " + player_label(st.media_player), [srv] { open_player_window(srv); }, "open"));
	}
	srv->menu->show(out, wlr_box{static_cast<int>(lx), static_cast<int>(ly), 1, 1}, "point", std::move(items));
}

bool open_player_window(Server *srv) {
	const std::string player = srv->status->media_player.substr(0, srv->status->media_player.find('.'));
	if (player.empty()) return false;
	for (View *v : srv->views) {
		std::string app = v->app_id();
		std::transform(app.begin(), app.end(), app.begin(), ::tolower);
		if (app.find(player) != std::string::npos) {
			srv->close_popups();
			if (v->minimized) v->restore_from_minimize();
			else srv->focus(v);
			return true;
		}
	}
	return false;
}

} // namespace dusk
