// Synced lyrics for what's playing: looked up by the song's name (and, if
// SongRec is installed, by listening when the name doesn't find it), kept
// in ~/.cache/dusk/lyrics, and shown on the screen in time with the music
// in one of eight styles. The timing can be nudged (Settings, or the media
// card's lyrics view for one song): later or earlier, and faster or slower
// for sped-up or slowed versions of a song.
#pragma once

#include <string>
#include <vector>

#include "draw.hpp"
#include "wlr.hpp"

namespace dusk {

class Server;
struct Output;

struct LyricWord {
	double t;           // seconds into the song
	std::string text;
};

struct LyricLine {
	double t;
	std::string text;
	std::vector<LyricWord> words;   // only when the lyrics time each word
};

class Lyrics {
public:
	explicit Lyrics(Server *server);
	~Lyrics();
	Lyrics(const Lyrics &) = delete;
	Lyrics &operator=(const Lyrics &) = delete;

	// The song or playback changed (track, play/pause, seek).
	void media_changed();
	// Settings changed (style, place, on/off, timing).
	void apply_config();
	void output_gone(Output *output);

	bool available() const { return !lines_.empty(); }
	bool searching() const { return searching_; }
	// Why there are none, or what it's doing ("Listening with SongRec…").
	const std::string &status() const { return status_; }
	// Look again now (forgets "not found"); `listen`: ask SongRec first.
	void retry(bool listen = false);
	const std::vector<LyricLine> &lines() const { return lines_; }
	// The lyrics in their style (and translation), fitted into a box: the
	// panel's media card shows them like the screen does.
	void draw_in(cairo_t *cr, double x, double y, double w, double h, float scale);
	Color colour();                 // what the lyrics are coloured with (Settings: accent, the art, white)
	int current() const;            // the line being sung (-1: before the first)
	double song_time() const;       // where the lyrics are, with the timing adjustments

	// This song's own timing, on top of Settings' (kept for next time).
	void nudge_delay(int ms);
	void nudge_speed(int percent);
	void reset_song_timing();
	int song_delay() const { return song_delay_; }
	int song_speed() const { return song_speed_; }

private:
	void fetch(bool listen_first = false);
	// Translation (Settings › Media & lyrics): into another language, shown
	// under each line or instead of it.
	void start_translation();
	void apply_translation();
	std::string translate_to() const;   // the language code ("" when off)
	const std::string *translation(int i) const;
	void load(const std::string &text);
	void save_song_timing() const;
	void update();                  // shows, hides, redraws
	void redraw();
	void place();
	void schedule(double ms);
	static int handle_timer(void *data);

	// The styles.
	void draw_lines(cairo_t *cr, double w, double h, float scale);
	void draw_translation(cairo_t *cr, double w, double top, double extra, double k);
	int style_height(double k) const;
	std::string art_path_;
	Color art_colour_{0, 0, 0, 0};

	Server *server_;
	wlr_scene_tree *tree_ = nullptr;
	wlr_scene_buffer *buffer_ = nullptr;
	wl_event_source *timer_ = nullptr;
	wl_event_source *retry_timer_ = nullptr;   // the lyrics sites were down: try again later
	int retries_ = 0;
	Output *output_ = nullptr;

	std::string key_;               // artist + title of the song the lyrics are for
	std::string file_;              // its cache file
	bool searching_ = false;
	std::string status_;
	std::vector<std::string> trans_;       // one per line (same order), when translated
	std::vector<LyricLine> original_;      // the lines as sung, while showing the translation instead
	bool translating_ = false;
	bool ear_mode_ = false;                // this video/mix has needed listening: keep listening between songs
	bool by_ear_ = false;                  // found by listening (SongRec): listen again when this song ends
	double relisten_at_ = 0;               // when it last listened again (now_ms)
	std::string trans_key_;                // what was asked for: song, language, how it's shown
	bool announce_ = false;         // asked for (Search again): say how it went
	uint32_t note_ = 0;
	std::vector<LyricLine> lines_;
	int song_delay_ = 0;            // ms
	int song_speed_ = 100;          // percent

	bool shown_ = false;
	bool listening_ = false;        // to the beat (the glow style)
	double alpha_ = 0;
	int index_ = -2;                // the line drawn last
	int from_index_ = -1;           // the line before it (for the change)
	double changed_at_ = 0;         // when the line changed (now_ms)
	double scroll_ = 0;             // the scroll style's position (lines)
	int w_ = 0, h_ = 0;
	double last_draw_ = 0;
};

} // namespace dusk
