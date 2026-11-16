#include "lyrics.hpp"

#include "notify.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <pango/pangocairo.h>

#include "anim.hpp"
#include "audiolevel.hpp"
#include "server.hpp"
#include "system.hpp"
#include "mediaview.hpp"

namespace dusk {

namespace {

bool no_input(wlr_scene_buffer *, double *, double *) {
	return false;
}

std::string cache_dir() {
	const char *xdg = std::getenv("XDG_CACHE_HOME");
	const char *home = std::getenv("HOME");
	return (xdg && *xdg ? std::string(xdg) : std::string(home ? home : "/tmp") + "/.cache") + "/dusk/lyrics";
}

// "[01:02.50]" / "<01:02.50>" -> seconds (or -1).
double stamp(const std::string &s) {
	int m = 0;
	double sec = 0;
	if (std::sscanf(s.c_str(), "%d:%lf", &m, &sec) != 2) return -1;
	return m * 60 + sec;
}

std::string trim(const std::string &s) {
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// The interface font's family, for big lyric text.
std::string family() {
	std::string f = theme::font_store[0];
	const size_t sp = f.rfind(' ');
	f = sp == std::string::npos ? "Sans" : f.substr(0, sp);
	return f.empty() ? "Sans" : f;
}

std::string font(double size, const char *weight = "Bold") {
	char buf[24];
	std::snprintf(buf, sizeof buf, "%.1f", size);
	return family() + " " + weight + " " + buf;
}

PangoLayout *layout(cairo_t *cr, const std::string &text, const std::string &f, int wrap_w = -1) {
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d = pango_font_description_from_string(f.c_str());
	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, text.c_str(), -1);
	if (wrap_w > 0) {
		pango_layout_set_width(l, wrap_w * PANGO_SCALE);
		pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
		pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
	}
	return l;
}

void size_of(PangoLayout *l, double *w, double *h) {
	int pw = 0, ph = 0;
	pango_layout_get_pixel_size(l, &pw, &ph);
	*w = pw;
	*h = ph;
}

// The biggest size (down to `min_k` of `size`) at which `text` fits on one line in `max_w`.
// The biggest size (up to `size`) at which `text` fits on one line in
// `max_w` (long lines get smaller rather than run off the edge).
double fit(cairo_t *cr, const std::string &text, double size, double max_w, double min_k = 0.38,
		const char *weight = "Bold") {
	for (double k = 1.0; k >= min_k; k -= 0.04) {
		PangoLayout *l = layout(cr, text, font(size * k, weight));
		double w, h;
		size_of(l, &w, &h);
		g_object_unref(l);
		if (w <= max_w) return size * k;
	}
	return size * min_k;
}

// The biggest size (up to `size`) at which `text`, wrapped in `max_w`,
// takes at most `lines` lines.
double fit_lines(cairo_t *cr, const std::string &text, double size, double max_w, int lines,
		const char *weight = "Bold", double min_k = 0.45) {
	for (double k = 1.0; k >= min_k; k -= 0.04) {
		PangoLayout *l = layout(cr, text, font(size * k, weight), static_cast<int>(max_w));
		const int n = pango_layout_get_line_count(l);
		g_object_unref(l);
		if (n <= lines) return size * k;
	}
	return size * min_k;
}

enum class Ink { Plain, Outline, Glow };

// Text that reads on any wallpaper: a soft dark shadow under it; or a thick
// outline (comic); or a coloured glow.
void paint(cairo_t *cr, PangoLayout *l, double x, double y, const Color &fill, Ink ink = Ink::Plain,
		const Color &extra = {}, double strength = 1.0) {
	cairo_save(cr);
	if (ink == Ink::Plain) {
		for (int i = 0; i < 2; i++) {
			cairo_move_to(cr, x, y + 1.5 + i);
			cairo_set_source_rgba(cr, 0, 0, 0, 0.28 * fill.a);
			pango_cairo_show_layout(cr, l);
		}
	} else {
		cairo_move_to(cr, x, y);
		pango_cairo_layout_path(cr, l);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		if (ink == Ink::Outline) {
			cairo_set_source_rgba(cr, extra.r, extra.g, extra.b, extra.a * fill.a);
			cairo_set_line_width(cr, 7 * strength);
			cairo_stroke(cr);
		} else {
			for (int pass = 3; pass >= 1; pass--) {
				cairo_set_source_rgba(cr, extra.r, extra.g, extra.b, 0.16 * strength * fill.a);
				cairo_set_line_width(cr, pass * 6.0 * strength);
				cairo_stroke_preserve(cr);
			}
			cairo_new_path(cr);
		}
	}
	cairo_move_to(cr, x, y);
	set_color(cr, fill);
	pango_cairo_show_layout(cr, l);
	cairo_restore(cr);
}

double ease_back(double t) {
	t = std::clamp(t, 0.0, 1.0);
	const double c = 1.7;
	return 1 + (c + 1) * std::pow(t - 1, 3) + c * std::pow(t - 1, 2);
}

// How many bytes of `s` make up its first n characters (UTF-8).
size_t utf8_prefix(const std::string &s, size_t n) {
	size_t i = 0;
	while (i < s.size() && n > 0) {
		i++;
		while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) i++;
		n--;
	}
	return i;
}

size_t utf8_length(const std::string &s) {
	size_t n = 0;
	for (unsigned char c : s) n += (c & 0xc0) != 0x80;
	return n;
}

} // namespace

Lyrics::Lyrics(Server *server) : server_(server) {
	tree_ = wlr_scene_tree_create(server->layer_desktop);
	buffer_ = wlr_scene_buffer_create(tree_, nullptr);
	buffer_->point_accepts_input = no_input;
	wlr_scene_node_set_enabled(&tree_->node, false);
	timer_ = wl_event_loop_add_timer(server->loop, &Lyrics::handle_timer, this);
	retry_timer_ = wl_event_loop_add_timer(server->loop, [](void *data) {
		auto *self = static_cast<Lyrics *>(data);
		if (!self->key_.empty() && self->lines_.empty() && !self->searching_ && self->server_->config.lyrics) {
			if (self->ear_mode_) {
				// A mix between songs (or one SongRec didn't know): listen again.
				const std::string name = std::to_string(std::hash<std::string>{}(self->key_));
				std::error_code ec;
				std::filesystem::remove(cache_dir() + "/" + name + ".miss", ec);
				self->fetch(true);
			} else {
				self->fetch();
			}
		}
		return 0;
	}, this);
}

Lyrics::~Lyrics() {
	if (listening_ && server_->audio_level) server_->audio_level->release();
	if (timer_) wl_event_source_remove(timer_);
	if (retry_timer_) wl_event_source_remove(retry_timer_);
	wlr_scene_node_destroy(&tree_->node);
}

int Lyrics::handle_timer(void *data) {
	static_cast<Lyrics *>(data)->update();
	return 0;
}

void Lyrics::schedule(double ms) {
	wl_event_source_timer_update(timer_, std::max(1, static_cast<int>(ms)));
}

void Lyrics::output_gone(Output *output) {
	if (output_ == output) {
		output_ = nullptr;
		shown_ = false;
		alpha_ = 0;
		wlr_scene_node_set_enabled(&tree_->node, false);
	}
}

// ---- Finding them ---------------------------------------------------------------------------

void Lyrics::media_changed() {
	const SystemStatus &st = *server_->status;
	const std::string key = st.media_available ? st.media_artist + "\n" + st.media_title : "";
	if (key != key_) {
		key_ = key;
		lines_.clear();
		index_ = -2;
		song_delay_ = 0;
		song_speed_ = 100;
		searching_ = false;
		status_.clear();
		file_.clear();
		retries_ = 0;
		ear_mode_ = false;
		by_ear_ = false;
		wl_event_source_timer_update(retry_timer_, 0);
		trans_.clear();
		original_.clear();
		translating_ = false;
		trans_key_.clear();
		if (!key.empty() && server_->config.lyrics) fetch();
	}
	update();
}

void Lyrics::apply_config() {
	if (!key_.empty() && lines_.empty() && !searching_ && server_->config.lyrics) fetch();
	start_translation();   // (only does something if the language or the way it's shown changed)
	index_ = -2;   // redraw in the new style
	update();
}

namespace {
// The last thing dusk-lyrics said (its log), for "why no lyrics?".
std::string last_said(const std::string &path) {
	std::ifstream f(path);
	std::string line, last, ear;
	while (std::getline(f, line)) {
		if (line.empty()) continue;
		last = line;
		// What listening did (or why it couldn't) matters more than "not found".
		if (line.find("SongRec") != std::string::npos || line.find("record") != std::string::npos ||
				line.find("silent") != std::string::npos) {
			ear = line;
		}
	}
	return ear.empty() || ear == last ? last : ear + " · " + last;
}
} // namespace

void Lyrics::retry(bool listen) {
	if (key_.empty()) return;
	const std::string name = std::to_string(std::hash<std::string>{}(key_));
	std::error_code ec;
	std::filesystem::remove(cache_dir() + "/" + name + ".miss", ec);
	// Forget what was found (it may be the wrong song, or badly timed) and look again.
	std::filesystem::remove(cache_dir() + "/" + name + ".lrc", ec);
	std::filesystem::remove(cache_dir() + "/" + name + ".adj", ec);
	song_delay_ = 0;
	song_speed_ = 100;
	original_.clear();
	trans_.clear();
	trans_key_.clear();
	by_ear_ = false;
	lines_.clear();
	index_ = -2;
	searching_ = false;
	announce_ = true;
	fetch(listen);
	if (server_->status->changed) server_->status->changed();
	update();
}

void Lyrics::fetch(bool listen_first) {
	const SystemStatus &st = *server_->status;
	if (st.media_title.empty() || searching_) return;
	const std::string name = std::to_string(std::hash<std::string>{}(key_));
	std::error_code ec;
	std::filesystem::create_directories(cache_dir(), ec);
	file_ = cache_dir() + "/" + name + ".lrc";
	// This song's own timing, if it was nudged before.
	if (std::ifstream adj(cache_dir() + "/" + name + ".adj"); adj) {
		adj >> song_delay_ >> song_speed_;
		if (song_speed_ < 50 || song_speed_ > 200) song_speed_ = 100;
	}
	if (std::ifstream f(file_); f) {
		std::stringstream ss;
		ss << f.rdbuf();
		load(ss.str());
		status_ = lines_.empty() ? "The saved lyrics for this song are empty" : "";
		return;
	}
	// Not found before: not asked again for a few hours (Search again does).
	const std::string miss = cache_dir() + "/" + name + ".miss";   // (".none" from 0.26.0 is ignored: the search is better now)
	struct stat sb{};
	if (stat(miss.c_str(), &sb) == 0 && std::time(nullptr) - sb.st_mtime < 6 * 3600) {
		const std::string why = last_said(miss);
		status_ = (why.empty() ? std::string("Not found earlier") : why) + " (Search again to retry)";
		return;
	}
	searching_ = true;
	const bool recognise = server_->config.lyrics_recognize;
	status_ = listen_first && recognise ? "Listening with SongRec…" : "Looking it up on lrclib.net…";
	char dur[32], pos[32];
	std::snprintf(dur, sizeof dur, "%.0f", st.media_length);
	double at = st.media_position;
	if (st.media_playing) at += (now_ms() - st.media_read_at) / 1000.0;
	std::snprintf(pos, sizeof pos, "%.2f", std::max(0.0, at));
	const std::string log = cache_dir() + "/last.log";
	const std::string cmd = shell_quote(server_->helper_path("dusk-lyrics")) +
		" --title " + shell_quote(st.media_title) + " --artist " + shell_quote(st.media_artist) +
		" --album " + shell_quote(st.media_album) + " --duration " + dur + " --position " + pos +
		(recognise ? " --recognize" : "") + (listen_first && recognise ? " --listen-first" : "") +
		" --out " + shell_quote(file_) + " >/dev/null 2>" + shell_quote(log) + "; echo \"EXIT:$?\"";
	std::fprintf(stderr, "dusk: lyrics: looking for \"%s\" by \"%s\"%s\n", st.media_title.c_str(),
		st.media_artist.c_str(), recognise ? " (SongRec if needed)" : "");
	const std::string key = key_, file = file_;
	server_->run_capture(cmd, [this, key, file, miss, log](const std::string &out) {
		const std::string said = last_said(log);
		std::fprintf(stderr, "dusk: lyrics: %s\n", said.empty() ? out.c_str() : said.c_str());
		if (key != key_) return;   // another song by now
		searching_ = false;
		if (std::ifstream f(file); f) {
			std::stringstream ss;
			ss << f.rdbuf();
			load(ss.str());
			status_.clear();
		} else if (out.find("EXIT:1") != std::string::npos) {
			std::ofstream(miss) << said << '\n';   // really not there (not just offline)
			status_ = said.empty() ? "No synced lyrics found" : said;
			if (ear_mode_) {
				status_ += " — listening again in a minute";
				wl_event_source_timer_update(retry_timer_, 60000);
			}
		} else if (out.find("EXIT:2") != std::string::npos) {
			// The sites are down or unreachable (not "no lyrics"): try again in a
			// while, a few times, while this song plays.
			status_ = said.empty() ? "Couldn't reach the lyrics sites" : said;
			if (retries_ < 4) {
				const int wait_s = 45 << retries_;   // 45 s, 1.5 min, 3 min, 6 min
				retries_++;
				wl_event_source_timer_update(retry_timer_, wait_s * 1000);
				status_ += " — trying again in " + (wait_s < 60 ? std::to_string(wait_s) + " s"
					: std::to_string(wait_s / 60) + " min");
			}
		} else {
			status_ = said.empty() ? "The lyrics helper didn't run (is Dusk installed?)" : said;
		}
		if (announce_ && server_->notifications) {
			announce_ = false;
			const bool down = out.find("EXIT:2") != std::string::npos;
			note_ = server_->notifications->post(!lines_.empty() ? "Lyrics found"
				: down ? "Lyrics sites unreachable" : "No lyrics for this song",
				lines_.empty() ? status_ : server_->status->media_title, "audio-x-generic-symbolic", 0, note_);
		}
		if (server_->status->changed) server_->status->changed();   // the media card's lyrics button
		update();
	});
}

void Lyrics::load(const std::string &text) {
	lines_.clear();
	double offset = 0;
	by_ear_ = text.find("[by:dusk-songrec]") != std::string::npos;
	if (by_ear_) ear_mode_ = true;
	std::istringstream in(text);
	std::string raw;
	while (std::getline(in, raw)) {
		std::string line = trim(raw);
		std::vector<double> times;
		while (line.size() > 2 && line[0] == '[') {
			const size_t end = line.find(']');
			if (end == std::string::npos) break;
			const std::string tag = line.substr(1, end - 1);
			if (tag.rfind("offset:", 0) == 0) {
				offset = std::atof(tag.c_str() + 7) / 1000.0;   // + = sooner
			} else if (const double t = stamp(tag); t >= 0) {
				times.push_back(t);
			}
			line = trim(line.substr(end + 1));
		}
		if (times.empty()) continue;
		// Word timings: "<00:12.30> word <00:12.80> word".
		std::vector<LyricWord> words;
		std::string plain;
		if (line.find('<') != std::string::npos) {
			size_t i = 0;
			double wt = -1;
			while (i < line.size()) {
				if (line[i] == '<') {
					const size_t e = line.find('>', i);
					if (e == std::string::npos) break;
					wt = stamp(line.substr(i + 1, e - i - 1));
					i = e + 1;
					continue;
				}
				const size_t next = line.find('<', i);
				const std::string word = line.substr(i, next == std::string::npos ? std::string::npos : next - i);
				if (!trim(word).empty() && wt >= 0) words.push_back(LyricWord{wt - offset, trim(word)});
				plain += word;
				i = next == std::string::npos ? line.size() : next;
			}
			line = trim(plain);
		}
		for (double t : times) lines_.push_back(LyricLine{t - offset, line, words});
	}
	std::sort(lines_.begin(), lines_.end(), [](const LyricLine &a, const LyricLine &b) { return a.t < b.t; });
	index_ = -2;
	original_.clear();
	trans_.clear();
	trans_key_.clear();
	start_translation();
}

// ---- Translation -------------------------------------------------------------------------

std::string Lyrics::translate_to() const {
	const Config &c = server_->config;
	if (!c.lyrics_translate) return "";
	if (!c.lyrics_translate_to.empty()) return c.lyrics_translate_to;
	// Your system's language: "fr_FR.UTF-8" -> "fr" (Chinese keeps its script).
	for (const char *var : {"LC_MESSAGES", "LC_ALL", "LANG"}) {
		const char *v = std::getenv(var);
		if (!v || !*v || std::string(v) == "C" || std::string(v) == "POSIX") continue;
		std::string l = v;
		l = l.substr(0, l.find('.'));
		if (l == "zh_CN" || l == "zh_SG") return "zh-CN";
		if (l == "zh_TW" || l == "zh_HK") return "zh-TW";
		return l.substr(0, l.find('_'));
	}
	return "en";
}

void Lyrics::start_translation() {
	const std::vector<LyricLine> &orig = original_.empty() ? lines_ : original_;
	const std::string lang = translate_to();
	const std::string key = lang.empty() || orig.empty() ? "" : key_ + "|" + lang;
	const std::string show_key = key + "|" + server_->config.lyrics_translate_show;
	if (show_key == trans_key_) return;
	const bool same_text = !trans_key_.empty() && trans_key_.substr(0, trans_key_.rfind('|')) == key;
	trans_key_ = show_key;
	if (key.empty()) {
		trans_.clear();
		apply_translation();
		return;
	}
	if (same_text && !trans_.empty()) {   // only how it's shown changed
		apply_translation();
		return;
	}
	trans_.clear();
	apply_translation();
	// Named after the words themselves (a mix's songs share its title).
	std::string all;
	for (const LyricLine &l : orig) all += l.text + '\n';
	const std::string name = std::to_string(std::hash<std::string>{}(all));
	const std::string out = cache_dir() + "/" + name + ".tr-" + lang + ".txt";
	auto read_back = [this, out]() {
		std::ifstream f(out);
		std::vector<std::string> got;
		std::string line;
		while (std::getline(f, line)) got.push_back(line);
		const std::vector<LyricLine> &o = original_.empty() ? lines_ : original_;
		if (got.size() == o.size()) trans_ = std::move(got);
		apply_translation();
	};
	if (std::ifstream(out)) {
		read_back();
		return;
	}
	if (std::ifstream(out + ".same")) return;   // already in that language
	if (translating_) return;
	translating_ = true;
	const std::string in = cache_dir() + "/" + name + ".src.txt";
	{
		std::ofstream f(in);
		for (const LyricLine &l : orig) f << l.text << '\n';
	}
	const std::string cmd = shell_quote(server_->helper_path("dusk-lyrics")) + " --translate " + shell_quote(lang) +
		" --in " + shell_quote(in) + " --out " + shell_quote(out) + " >/dev/null 2>>" +
		shell_quote(cache_dir() + "/last.log") + "; echo \"EXIT:$?\"";
	const std::string song = key_;
	server_->run_capture(cmd, [this, song, out, in, read_back](const std::string &res) {
		translating_ = false;
		std::error_code ec;
		std::filesystem::remove(in, ec);
		if (song != key_) return;
		if (res.find("EXIT:3") != std::string::npos) {
			std::ofstream(out + ".same").put('\n');
			return;
		}
		if (res.find("EXIT:0") == std::string::npos) {
			std::fprintf(stderr, "dusk: lyrics: couldn't translate (see ~/.cache/dusk/lyrics/last.log)\n");
			trans_key_.clear();   // try again on the next change
			return;
		}
		read_back();
		index_ = -2;
		update();
	});
}

const std::string *Lyrics::translation(int i) const {
	if (server_->config.lyrics_translate_show != "under" || i < 0 || i >= static_cast<int>(trans_.size())) return nullptr;
	const std::string &t = trans_[static_cast<size_t>(i)];
	const std::string &o = (original_.empty() ? lines_ : original_)[static_cast<size_t>(i)].text;
	return t.empty() || t == o ? nullptr : &t;
}

// "Instead": the lines themselves become the translation (word timing goes);
// "under": the lines stay, and the translation is drawn below the one sung.
void Lyrics::apply_translation() {
	const bool instead = !trans_.empty() && server_->config.lyrics_translate_show == "instead";
	if (instead) {
		if (original_.empty()) original_ = lines_;
		lines_ = original_;
		for (size_t i = 0; i < lines_.size() && i < trans_.size(); i++) {
			if (!trans_[i].empty()) {
				lines_[i].text = trans_[i];
				lines_[i].words.clear();
			}
		}
	} else if (!original_.empty()) {
		lines_ = original_;
		original_.clear();
	}
	index_ = -2;
}

// ---- Timing ------------------------------------------------------------------------------------

double Lyrics::song_time() const {
	const SystemStatus &st = *server_->status;
	double p = st.media_position;
	if (st.media_playing) p += (now_ms() - st.media_read_at) / 1000.0;
	const double speed = server_->config.lyrics_speed / 100.0 * song_speed_ / 100.0;
	const double delay = (server_->config.lyrics_delay + song_delay_) / 1000.0;
	return p * speed - delay;
}

int Lyrics::current() const {
	if (lines_.empty()) return -1;
	const double t = song_time();
	auto it = std::upper_bound(lines_.begin(), lines_.end(), t,
		[](double v, const LyricLine &l) { return v < l.t; });
	return static_cast<int>(it - lines_.begin()) - 1;
}

void Lyrics::nudge_delay(int ms) {
	song_delay_ = std::clamp(song_delay_ + ms, -10000, 10000);
	save_song_timing();
	update();
}

void Lyrics::nudge_speed(int percent) {
	song_speed_ = std::clamp(song_speed_ + percent, 70, 140);
	save_song_timing();
	update();
}

void Lyrics::reset_song_timing() {
	song_delay_ = 0;
	song_speed_ = 100;
	save_song_timing();
	update();
}

void Lyrics::save_song_timing() const {
	if (key_.empty()) return;
	const std::string name = std::to_string(std::hash<std::string>{}(key_));
	const std::string path = cache_dir() + "/" + name + ".adj";
	if (song_delay_ == 0 && song_speed_ == 100) {
		unlink(path.c_str());
		return;
	}
	std::ofstream(path) << song_delay_ << " " << song_speed_ << "\n";
}

// ---- On the screen ------------------------------------------------------------------------------

void Lyrics::place() {
	if (!output_) return;
	const wlr_box u = output_->usable();
	const std::string &pos = server_->config.lyrics_position;
	// One of nine places (Settings › Media & lyrics: click where on the screen).
	const bool top = pos.rfind("top", 0) == 0, bottom = pos.rfind("bottom", 0) == 0;
	const bool left = pos.find("left") != std::string::npos, right = pos.find("right") != std::string::npos;
	const int y = top ? u.y + 36 : bottom ? u.y + u.height - h_ - 36 : u.y + (u.height - h_) / 2;
	const int x = left ? u.x + 36 : right ? u.x + u.width - w_ - 36 : u.x + (u.width - w_) / 2;
	wlr_scene_node_set_position(&tree_->node, x, y);
}

void Lyrics::update() {
	const Config &c = server_->config;
	const SystemStatus &st = *server_->status;
	// Over the windows, or on the desktop under them.
	wlr_scene_tree *want_parent = c.lyrics_on_top ? server_->layer_overlay : server_->layer_desktop;
	if (tree_->node.parent != want_parent) {
		wlr_scene_node_reparent(&tree_->node, want_parent);
	}
	const bool want = c.lyrics && c.lyrics_show && !lines_.empty() && st.media_available && st.media_playing &&
		!server_->game_mode && !server_->locked();
	if (want && !output_) {
		output_ = server_->focused_output();
	}
	const double now = now_ms();
	const double dt = last_draw_ > 0 ? std::min(100.0, now - last_draw_) : 33;
	const double target = want && output_ ? 1.0 : 0.0;
	const bool fading = std::fabs(alpha_ - target) > 0.01;
	if (fading) {
		alpha_ += (target - alpha_) * std::min(1.0, dt / 160.0);
		if (std::fabs(alpha_ - target) <= 0.01) alpha_ = target;
	}
	// The glow style pulses with the beat: listen while it's on screen.
	const bool listen = target > 0 && c.lyrics_style == "glow" && server_->audio_level;
	if (listen != listening_) {
		listening_ = listen;
		if (listen) server_->audio_level->acquire();
		else if (server_->audio_level) server_->audio_level->release();
	}
	if (alpha_ <= 0.001 && target == 0) {
		alpha_ = 0;
		shown_ = false;
		wlr_scene_node_set_enabled(&tree_->node, false);
		last_draw_ = 0;
		return;   // playing again (or a new song) arrives as media_changed
	}
	// Under the windows, and a maximised or full-screen one covers this
	// screen: nothing to see, so no drawing (it redrew 30 times a second
	// behind it); a look now and then to catch up.
	if (!c.lyrics_on_top && output_ && target > 0 && alpha_ >= 0.99) {
		for (View *v : server_->views) {
			if (v->visible() && (v->maximized || v->fullscreen) && server_->output_for_view(v) == output_) {
				schedule(400);
				return;
			}
		}
	}
	shown_ = true;
	wlr_scene_node_set_enabled(&tree_->node, true);
	// Found by listening, in something longer (a compilation, a mix): once
	// this song is over (or before it began), listen for the next one.
	if (by_ear_ && !searching_ && !lines_.empty() && st.media_playing && now - relisten_at_ > 45000) {
		const double t = song_time();
		if (t > lines_.back().t + 25 || t < lines_.front().t - 45) {
			relisten_at_ = now;
			const std::string name = std::to_string(std::hash<std::string>{}(key_));
			std::error_code ec;
			std::filesystem::remove(cache_dir() + "/" + name + ".lrc", ec);
			std::filesystem::remove(cache_dir() + "/" + name + ".miss", ec);
			lines_.clear();
			original_.clear();
			trans_.clear();
			trans_key_.clear();
			index_ = -2;
			fetch(true);
			if (server_->status->changed) server_->status->changed();
		}
	}
	const int idx = current();
	const std::string &style = c.lyrics_style;
	if (idx != index_) {
		from_index_ = index_;
		index_ = idx;
		changed_at_ = now;
	}
	const double since = now - changed_at_;
	// Styles that move all the time; the others only around a line change.
	const bool animated = style == "karaoke" || style == "bounce" || style == "typewriter" || style == "glow" ||
		since < 450 || fading || std::fabs(scroll_ - index_) > 0.01;
	redraw();
	last_draw_ = now;
	if (animated) {
		schedule(33);
	} else {
		// Next: when the next line starts.
		double wait = 500;
		if (index_ + 1 < static_cast<int>(lines_.size())) {
			const double speed = c.lyrics_speed / 100.0 * song_speed_ / 100.0;
			wait = (lines_[static_cast<size_t>(index_ + 1)].t - song_time()) / std::max(0.1, speed) * 1000.0;
		}
		schedule(std::clamp(wait, 20.0, 500.0));
	}
}

void Lyrics::redraw() {
	if (!output_) return;
	const wlr_box u = output_->usable();
	const double k = server_->config.lyrics_size / 100.0;
	w_ = std::max(200, std::min(u.width - 80, static_cast<int>(1000 * std::max(0.7, k))));
	// At a side of the screen: that side's part of it.
	const std::string &pos = server_->config.lyrics_position;
	if (pos.find("left") != std::string::npos || pos.find("right") != std::string::npos) {
		w_ = std::max(200, std::min(w_, static_cast<int>(u.width * 0.42)));
	}
	h_ = style_height(k);
	// The translation under the line being sung: room for it at the bottom.
	const bool under = server_->config.lyrics_translate_show == "under" && !trans_.empty();
	const int extra = under ? static_cast<int>(std::lround(40 * k)) : 0;
	h_ += extra;
	const float scale = output_->output->scale;
	Canvas canvas(w_, h_, scale);
	draw_lines(canvas.cr(), w_, h_ - extra, scale);
	if (under) draw_translation(canvas.cr(), w_, h_ - extra, extra, k);
	canvas.present(buffer_);
	wlr_scene_buffer_set_opacity(buffer_, static_cast<float>(alpha_));
	place();
}

int Lyrics::style_height(double k) const {
	const std::string &style = server_->config.lyrics_style;
	return static_cast<int>(std::lround((style == "scroll" ? 300 : style == "classic" ? 190 : style == "subtitle" ? 96
		: 150) * k));
}

Color Lyrics::colour() {
	const std::string &want = server_->config.lyrics_color;
	if (want == "white") return rgb(0xffffff);
	if (want == "art") {
		const std::string &path = server_->status->media_art;
		if (path != art_path_) {
			art_path_ = path;
			art_colour_ = art_colour(path);
		}
		if (art_colour_.a > 0) return art_colour_;
	}
	return theme::accent;
}

void Lyrics::draw_translation(cairo_t *cr, double w, double top, double extra, double k) {
	const std::string *tr = translation(index_);
	if (!tr) return;
	const double fade = std::clamp((now_ms() - changed_at_) / 260.0, 0.0, 1.0);
	PangoLayout *l = layout(cr, *tr, font(16 * k, "Italic"), static_cast<int>(w - 40));
	pango_layout_set_height(l, -2);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	double tw = 0, th = 0;
	size_of(l, &tw, &th);
	const double y = top + (extra - th) / 2 + (1 - fade) * 6;
	// A soft shadow so it reads on any wallpaper or window.
	cairo_set_source_rgba(cr, 0, 0, 0, 0.45 * fade);
	cairo_move_to(cr, 20 + 1, y + 1.5);
	pango_cairo_show_layout(cr, l);
	const Color tint = mix(rgb(0xffffff), colour(), 0.35);
	cairo_set_source_rgba(cr, tint.r, tint.g, tint.b, 0.92 * fade);
	cairo_move_to(cr, 20, y);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
}

void Lyrics::draw_in(cairo_t *cr, double x, double y, double w, double h, float scale) {
	if (lines_.empty() || w < 40 || h < 20) return;
	// Keep the line being sung up to date (the overlay may not be showing).
	const int idx = current();
	if (idx != index_) {
		from_index_ = index_;
		index_ = idx;
		changed_at_ = now_ms();
	}
	if (server_->config.lyrics_style == "scroll" && std::fabs(scroll_ - index_) > 0.01) {
		scroll_ += (index_ - scroll_) * 0.25;
	}
	const double k = server_->config.lyrics_size / 100.0;
	const bool under = server_->config.lyrics_translate_show == "under" && translation(index_);
	const double extra = under ? 40 * k : 0;
	const double natural = style_height(k) + extra;
	// Shrunk to fit the box, but never so small it can't be read: tall
	// styles (scrolling, classic) are then laid out in less height instead,
	// showing fewer of the lines around the one being sung.
	const double f = std::clamp(h / natural, server_->config.lyrics_style == "scroll" ? 0.62 : 0.5, 1.0);
	const double W = w / f, H = std::min(natural, h / f);
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_translate(cr, x, y + (h - H * f) / 2);
	cairo_scale(cr, f, f);
	draw_lines(cr, W, H - extra, scale);
	if (under) draw_translation(cr, W, H - extra, extra, k);
	cairo_restore(cr);
}

void Lyrics::draw_lines(cairo_t *cr, double w, double h, float) {
	const Config &c = server_->config;
	const std::string &style = c.lyrics_style;
	const double k = c.lyrics_size / 100.0;
	const double now = now_ms();
	const double since = now - changed_at_;
	const double change = std::clamp(since / 320.0, 0.0, 1.0);   // 0 -> 1 as a new line arrives
	const double t = song_time();
	const int n = static_cast<int>(lines_.size());
	auto text_of = [&](int i) -> std::string {
		return i >= 0 && i < n ? lines_[static_cast<size_t>(i)].text : std::string();
	};
	const int i = index_;
	const std::string now_text = i < 0 ? "♪" : text_of(i).empty() ? "♪" : text_of(i);
	// The colour (Settings: the accent, the album art's, or plain white): the
	// highlights in it, the words themselves touched with it.
	const Color accent = colour();
	const bool plain = c.lyrics_color == "white";
	const Color white = plain ? rgb(0xffffff) : mix(rgb(0xffffff), accent, 0.22);
	const Color dim = with_alpha(white, 0.55);
	// How far through this line the singing is.
	auto progress = [&]() {
		if (i < 0 || i >= n) return 0.0;
		const LyricLine &l = lines_[static_cast<size_t>(i)];
		const double end = i + 1 < n ? lines_[static_cast<size_t>(i + 1)].t : l.t + 5;
		if (!l.words.empty()) {
			// Word timing: whole words light up as they're sung.
			size_t done = 0;
			for (const LyricWord &wd : l.words) if (wd.t <= t) done++;
			return std::clamp(static_cast<double>(done) / l.words.size(), 0.0, 1.0);
		}
		return std::clamp((t - l.t) / std::max(0.4, (end - l.t) * 0.85), 0.0, 1.0);
	};
	const double big = 26 * k, small = 15 * k;

	if (style == "classic") {
		// The line being sung, big; the one before above and the next below, faint.
		const double slide = (1 - ease_out_cubic(change)) * 18;
		PangoLayout *prev = layout(cr, text_of(i - 1), font(small, "Semibold"), static_cast<int>(w - 40));
		PangoLayout *cur = layout(cr, now_text, font(fit_lines(cr, now_text, big, w - 40, 2)), static_cast<int>(w - 40));
		PangoLayout *next = layout(cr, text_of(i + 1), font(small, "Semibold"), static_cast<int>(w - 40));
		// The faint ones: one line each, cut short if long.
		for (PangoLayout *f : {prev, next}) {
			pango_layout_set_height(f, -1);
			pango_layout_set_ellipsize(f, PANGO_ELLIPSIZE_END);
		}
		double pw, ph, cw, ch, nw, nh;
		size_of(prev, &pw, &ph);
		size_of(cur, &cw, &ch);
		size_of(next, &nw, &nh);
		const double cy = (h - ch) / 2 + slide;
		// (Wrapped layouts centre themselves in their 40 px narrower width.)
		paint(cr, prev, 20, cy - ph - 10, with_alpha(dim, 0.45 * change));
		paint(cr, cur, 20, cy, with_alpha(white, 0.25 + 0.75 * change));
		paint(cr, next, 20, cy + ch + 10, with_alpha(dim, 0.5));
		g_object_unref(prev);
		g_object_unref(cur);
		g_object_unref(next);
	} else if (style == "karaoke") {
		// One line, filling with the accent colour as it's sung; the next under it.
		// (Long lines: smaller, then on two lines that fill one after the other.)
		const double box = w - 40;
		const double size = fit_lines(cr, now_text, big, box, 1, "Bold", 0.7) < big * 0.7 + 0.01
			? fit_lines(cr, now_text, big, box, 2) : fit(cr, now_text, big, box, 0.7);
		PangoLayout *cur = layout(cr, now_text, font(size), static_cast<int>(box));
		double cw, ch;
		size_of(cur, &cw, &ch);
		const double x = 20, y = h * 0.42 - ch / 2;
		paint(cr, cur, x, y, rgb(0xffffff, 0.9));
		// The fill: along each line in turn, by how much of the line is sung.
		double total = 0;
		std::vector<PangoRectangle> rects;
		PangoLayoutIter *it = pango_layout_get_iter(cur);
		do {
			PangoRectangle ink, logical;
			pango_layout_iter_get_line_extents(it, &ink, &logical);
			rects.push_back(logical);
			total += logical.width / static_cast<double>(PANGO_SCALE);
		} while (pango_layout_iter_next_line(it));
		pango_layout_iter_free(it);
		double left = total * progress();
		cairo_save(cr);
		cairo_new_path(cr);
		for (const PangoRectangle &r : rects) {
			const double lx = r.x / static_cast<double>(PANGO_SCALE), ly = r.y / static_cast<double>(PANGO_SCALE);
			const double lw = r.width / static_cast<double>(PANGO_SCALE), lh = r.height / static_cast<double>(PANGO_SCALE);
			const double take = std::clamp(left, 0.0, lw);
			if (take > 0) cairo_rectangle(cr, x + lx, y + ly - 2, take, lh + 4);
			left -= lw;
		}
		cairo_clip(cr);
		cairo_move_to(cr, x, y);
		set_color(cr, accent);
		pango_cairo_show_layout(cr, cur);
		cairo_restore(cr);
		g_object_unref(cur);
		PangoLayout *next = layout(cr, text_of(i + 1), font(small, "Semibold"), static_cast<int>(w - 40));
		double nw, nh;
		size_of(next, &nw, &nh);
		paint(cr, next, 20, y + ch + 12, dim);
		g_object_unref(next);
	} else if (style == "subtitle") {
		// Like a film's subtitles: white on a dark box.
		PangoLayout *cur = layout(cr, now_text, font(fit_lines(cr, now_text, 19 * k, w - 80, 2, "Semibold"), "Semibold"),
			static_cast<int>(w - 80));
		double cw, ch;
		size_of(cur, &cw, &ch);
		const double bw = cw + 36, bh = ch + 18;
		const double a = 0.4 + 0.6 * change;
		fill_rounded(cr, (w - bw) / 2, h - bh - 8, bw, bh, std::min<double>(bh / 2.0, theme::small_radius(12)), rgb(0x000000, 0.62 * a));
		cairo_move_to(cr, 40, h - bh - 8 + 9);
		set_color(cr, with_alpha(white, a));
		pango_cairo_show_layout(cr, cur);
		g_object_unref(cur);
	} else if (style == "glow") {
		// A neon line in the accent colour; the glow pulses with the beat.
		const double size = fit_lines(cr, now_text, big * 1.1, w - 60, 2);
		PangoLayout *cur = layout(cr, now_text, font(size), static_cast<int>(w - 60));
		double cw, ch;
		size_of(cur, &cw, &ch);
		cw = w - 60;
		const double beat = server_->audio_level ? server_->audio_level->beat() : 0;
		paint(cr, cur, (w - cw) / 2, (h - ch) / 2, with_alpha(white, 0.3 + 0.7 * change), Ink::Glow, accent,
			0.8 + 0.7 * beat);
		g_object_unref(cur);
	} else if (style == "scroll") {
		// A column that glides up line by line, like a music app's lyrics page.
		// Lines that wrap take the room they need (they used to overlap).
		scroll_ += (i - scroll_) * 0.22;
		if (std::fabs(i - scroll_) < 0.01) scroll_ = i;
		const double gap = 12 * k;
		const int from = std::max(0, i - 5), to = std::min(n, i + 6);
		struct Row {
			PangoLayout *l;
			double h;
		};
		std::map<int, Row> rows;
		for (int j = from; j < to; j++) {
			const bool on = j == i;
			PangoLayout *l = layout(cr, text_of(j), font(on ? 22 * k : 18 * k, on ? "Bold" : "Semibold"),
				static_cast<int>(w - 40));
			pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
			double lw, lh;
			size_of(l, &lw, &lh);
			rows[j] = Row{l, lh};
		}
		// Each line's centre, from the one being sung.
		std::map<int, double> pos;
		if (rows.count(i)) {
			pos[i] = 0;
			for (int j = i + 1; j < to; j++) pos[j] = pos[j - 1] + (rows[j - 1].h + rows[j].h) / 2 + gap;
			for (int j = i - 1; j >= from; j--) pos[j] = pos[j + 1] - (rows[j + 1].h + rows[j].h) / 2 - gap;
		}
		// Where the column is while gliding from one line to the next.
		const int s0 = static_cast<int>(std::floor(scroll_));
		const double sf = scroll_ - s0;
		double offset = 0;
		if (pos.count(s0) && pos.count(s0 + 1)) offset = pos[s0] + sf * (pos[s0 + 1] - pos[s0]);
		else if (pos.count(s0)) offset = pos[s0];
		cairo_save(cr);
		cairo_rectangle(cr, 0, 0, w, h);
		cairo_clip(cr);
		for (auto &[j, r] : rows) {
			const double y = h / 2 + (pos.count(j) ? pos[j] : 0) - offset;
			const double d = std::fabs(j - scroll_);
			const double a = std::clamp(1.0 - d * 0.32, 0.0, 1.0) * (j == i ? 1.0 : 0.6);
			if (y + r.h / 2 > 0 && y - r.h / 2 < h) paint(cr, r.l, 20, y - r.h / 2, with_alpha(white, a));
			g_object_unref(r.l);
		}
		cairo_restore(cr);
	} else if (style == "bounce") {
		// Each word pops up as it's sung.
		std::vector<std::string> words;
		std::vector<double> at;
		if (i >= 0 && i < n && !lines_[static_cast<size_t>(i)].words.empty()) {
			for (const LyricWord &wd : lines_[static_cast<size_t>(i)].words) {
				words.push_back(wd.text);
				at.push_back(wd.t);
			}
		} else {
			std::istringstream ws(now_text);
			std::string wd;
			while (ws >> wd) words.push_back(wd);
			const double start = i >= 0 && i < n ? lines_[static_cast<size_t>(i)].t : t;
			const double end = i + 1 < n ? lines_[static_cast<size_t>(i + 1)].t : start + 4;
			const double span = std::max(0.5, (end - start) * 0.7);
			for (size_t j = 0; j < words.size(); j++) at.push_back(start + span * j / std::max<size_t>(1, words.size()));
		}
		// Long lines: smaller, then the words in two rows.
		const double size = fit_lines(cr, now_text, big, w - 40, 2);
		std::vector<double> ww;
		double lh = 0;
		PangoLayout *space = layout(cr, " ", font(size));
		double sw, sh;
		size_of(space, &sw, &sh);
		g_object_unref(space);
		for (const std::string &wd : words) {
			PangoLayout *l = layout(cr, wd, font(size));
			double a, b;
			size_of(l, &a, &b);
			g_object_unref(l);
			ww.push_back(a);
			lh = std::max(lh, b);
		}
		// Rows: as many words as fit across, then the next row.
		std::vector<std::pair<size_t, size_t>> rows;   // [from, to)
		std::vector<double> row_w;
		for (size_t j = 0; j < words.size();) {
			size_t e = j;
			double rw = 0;
			while (e < words.size() && (e == j || rw + sw + ww[e] <= w - 40)) {
				rw += (e == j ? 0 : sw) + ww[e];
				e++;
			}
			rows.push_back({j, e});
			row_w.push_back(rw);
			j = e;
		}
		const double gap = 4;
		const double block = rows.size() * lh + (rows.empty() ? 0 : (rows.size() - 1) * gap);
		for (size_t r = 0; r < rows.size(); r++) {
			double x = (w - row_w[r]) / 2;
			const double y = (h - block) / 2 + r * (lh + gap);
			for (size_t j = rows[r].first; j < rows[r].second; j++) {
				const double age = (t - at[j]) * 1000.0;
				if (age >= 0) {
					const double sc = 0.55 + 0.45 * ease_back(age / 260.0);
					const double a = std::clamp(age / 120.0, 0.0, 1.0);
					PangoLayout *l = layout(cr, words[j], font(size));
					cairo_save(cr);
					cairo_translate(cr, x + ww[j] / 2, y + lh);
					cairo_scale(cr, sc, sc);
					const bool newest = j + 1 == words.size() || t < at[j + 1];
					paint(cr, l, -ww[j] / 2, -lh, with_alpha(newest ? mix(white, accent, 0.55) : white, a));
					cairo_restore(cr);
					g_object_unref(l);
				}
				x += ww[j] + sw;
			}
		}
	} else if (style == "typewriter") {
		// Typed out letter by letter, with a blinking cursor.
		const size_t len = utf8_length(now_text);
		const size_t shown = static_cast<size_t>(std::lround(std::clamp(progress() * 1.6, 0.0, 1.0) * len));
		const std::string part = now_text.substr(0, utf8_prefix(now_text, shown));
		// Smaller for long lines, so the whole line fits in two rows.
		int msize = static_cast<int>(22 * k);
		for (; msize > 9; msize--) {
			PangoLayout *probe = layout(cr, now_text, "Monospace Bold " + std::to_string(msize), static_cast<int>(w - 40));
			const int lines = pango_layout_get_line_count(probe);
			g_object_unref(probe);
			if (lines <= 2) break;
		}
		const std::string mono = "Monospace Bold " + std::to_string(msize);
		PangoLayout *full = layout(cr, now_text, mono, static_cast<int>(w - 40));
		pango_layout_set_alignment(full, PANGO_ALIGN_LEFT);
		double fw, fh;
		size_of(full, &fw, &fh);
		g_object_unref(full);
		PangoLayout *l = layout(cr, part, mono, static_cast<int>(w - 40));
		pango_layout_set_alignment(l, PANGO_ALIGN_LEFT);
		const double x = (w - fw) / 2, y = (h - fh) / 2;
		paint(cr, l, x, y, white);
		if (static_cast<long>(now / 450) % 2 == 0 || shown < len) {
			PangoRectangle cur;
			pango_layout_get_cursor_pos(l, static_cast<int>(part.size()), &cur, nullptr);
			fill_rounded(cr, x + cur.x / PANGO_SCALE + 2, y + cur.y / PANGO_SCALE + 2, 3,
				cur.height / PANGO_SCALE - 4, 1.5, accent);
		}
		g_object_unref(l);
	} else {
		// "comic": thick black outline, white fill, a hard accent shadow, a little
		// tilt, and a pop when the line changes.
		// Smaller for long lines, and wrapped onto two if it still doesn't fit.
		const double box = w - 70;
		const double size = fit_lines(cr, now_text, big * 1.15, box, 2, "Heavy");
		PangoLayout *cur = layout(cr, now_text, font(size, "Heavy"), static_cast<int>(box));
		double cw, ch;
		size_of(cur, &cw, &ch);
		cw = box;   // laid out (and centred) in the whole box
		const double pop = 0.82 + 0.18 * ease_back(since / 280.0);
		cairo_save(cr);
		cairo_translate(cr, w / 2, h * 0.42);
		cairo_rotate(cr, -0.035);
		cairo_scale(cr, pop, pop);
		cairo_move_to(cr, -cw / 2 + 5, -ch / 2 + 5);
		pango_cairo_layout_path(cr, cur);
		set_color(cr, accent);
		cairo_set_line_width(cr, 7);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke_preserve(cr);
		cairo_fill(cr);
		paint(cr, cur, -cw / 2, -ch / 2, white, Ink::Outline, rgb(0x111111), 1.0);
		cairo_restore(cr);
		g_object_unref(cur);
		PangoLayout *next = layout(cr, text_of(i + 1), font(small, "Bold"), static_cast<int>(w - 60));
		pango_layout_set_height(next, -1);   // one line, cut short if long
		pango_layout_set_ellipsize(next, PANGO_ELLIPSIZE_END);
		double nw, nh;
		size_of(next, &nw, &nh);
		const double ny = std::max(h * 0.42 + ch / 2 + 10, h - nh - 6);
		paint(cr, next, 30, ny, rgb(0xffffff, 0.85), Ink::Outline, rgb(0x111111), 0.5);
		g_object_unref(next);
	}
}

} // namespace dusk
