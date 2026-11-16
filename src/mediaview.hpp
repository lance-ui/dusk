// What's playing, drawn like Samsung's media card: the album art filling
// the card (darkened), the app and an output button on top, the title
// (scrolling sideways when it's too long) and artist, a wavy progress line
// that moves with the music's beat, the times, and the controls. Shared by
// the control centre's tile and the panel's media popover, which only say
// where to draw it. Also the other control centre looks' versions
// (Windows, GNOME, macOS).
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "draw.hpp"
#include "wlr.hpp"

namespace dusk {

class Server;
struct Output;
struct MenuItem;

struct MediaHot {
	std::string id;   // mp:previous, mp:play-pause, mp:next, mp:seek, mp:output, mp:lyrics, mp:source
	wlr_box box;
	double radius;
};

class MediaView {
public:
	enum class Look { Samsung, Windows, Gnome, Mac };

	explicit MediaView(Server *server);
	~MediaView();
	MediaView(const MediaView &) = delete;
	MediaView &operator=(const MediaView &) = delete;

	// Draws the card into (x, y, w, h); what can be clicked goes into `hots`
	// (in the same coordinates).
	void draw(cairo_t *cr, double x, double y, double w, double h, double radius, float scale, Look look,
		std::vector<MediaHot> *hots);

	// It's on screen (or not): the beat is only listened to while it is.
	void set_shown(bool shown);
	// Time passing: the wave and the scrolling title move. True while it
	// needs to be drawn again soon.
	bool step(double dt_ms);
	bool moving() const;

	// Dragging the position along the line (0..1 of the song); end_drag seeks.
	void drag(double x);
	void end_drag();
	bool dragging() const { return drag_s_ >= 0; }

	double position() const;   // seconds, now (between the player's reports)

private:
	void load_art(float scale);
	void draw_samsung(cairo_t *cr, double x, double y, double w, double h, double r, float scale,
		std::vector<MediaHot> *hots);
	void draw_plain(cairo_t *cr, double x, double y, double w, double h, double r, float scale, Look look,
		std::vector<MediaHot> *hots);
	void draw_title(cairo_t *cr, const std::string &text, double x, double y, double w, double h, const char *font,
		const Color &color);
	void draw_sound(cairo_t *cr, double x, double y, double w, double h);
	void draw_hills(cairo_t *cr, double x, double y, double w, double h);
	void draw_wave(cairo_t *cr, double x0, double y, double w, const Color &played, const Color &rest,
		std::vector<MediaHot> *hots);

	Server *server_;
	bool shown_ = false;
	bool listening_ = false;
	std::string art_path_;
	cairo_surface_t *art_bg_ = nullptr;   // small: stretched, it's soft
	cairo_surface_t *art_ = nullptr;      // the sharp cover
	int art_px_ = 0;
	unsigned art_gen_ = 0;
	std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   // (for pictures arriving later)

	double phase_ = 0;       // the wave's movement (cycles)
	double amp_ = 0;         // its height now (eases)
	double level_ = 0;       // the music's loudness, smoothed
	double beat_ = 0;        // beats, smoothed
	double energy_ = 0, energy_slow_ = 0;   // how hard it hits now (the hills' height)
	// The visualizer's bars (Arqen's beat): 24, bass to treble, 0..1. They
	// never move sideways; only their height follows the music.
	std::array<std::array<double, 24>, 2> viz_{};   // left, right
	std::array<std::array<double, 24>, 2> viz_slow_{};   // the same, slower (the hills' back layer)
	// The wave's beats: each band's usual level (about a second's average),
	// and a spike when it suddenly jumps above that, fading quickly.
	std::array<std::array<double, 24>, 2> viz_avg_{};
	std::array<std::array<double, 24>, 2> viz_kick_{};
	double fake_t_ = 0;
	bool plain_ = false;     // drawing on a plain card (not over the cover)      // the made-up beat's clock (nothing to listen to)
	// Bar i of n across the card, in stereo: the left channel on the left
	// half and the right on the right, the bass meeting in the middle.
	double viz_value(int i, int n) const;
	double side_value(int ch, double pos, bool slow = false) const;   // pos 0 (bass) .. 1 (treble)
	std::deque<double> history_;   // how hard the music hit, every 40 ms (newest first)
	double sample_t_ = 0;
	unsigned long samples_ = 0;
	static constexpr double kSampleMs = 40;
	Color art_tint_{0, 0, 0, 0};   // the bar's colour, from the art (alpha 0: none)

	// The title, when it doesn't fit: it slides along, waits at the start.
	std::string marquee_text_;
	bool marquee_on_ = false;
	double marquee_w_ = 0;
	double marquee_off_ = 0, marquee_wait_ = 1800;

	wlr_box seek_{};         // the progress line, last drawn
	double drag_s_ = -1;
};

// The album art's colour (its average, lighter and more colourful); alpha 0: none.
Color art_colour(const std::string &path);
Color art_colour(cairo_surface_t *small);

// The media card's menu items after play / next / previous: lyrics, their
// style, the beat visualizer, settings (control centre and panel alike).
void media_menu_items(Server *srv, Output *out, wlr_box at, const std::string &edge, std::vector<MenuItem> &items);

// A menu of the sound outputs (speakers, headphones...) to pick where sound goes.
void sound_output_menu(Server *server, Output *output, double lx, double ly);

// Which player the cards follow (when several play), and opening its window.
void media_source_menu(Server *server, Output *output, double lx, double ly);
bool open_player_window(Server *server);

// "3:07" from seconds.
std::string media_clock(double seconds);

} // namespace dusk
