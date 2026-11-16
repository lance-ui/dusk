#include "popover.hpp"

#include "lyrics.hpp"
#include "mediaview.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <linux/input-event-codes.h>

#include "draw.hpp"
#include "glyphs.hpp"
#include "system.hpp"

namespace dusk {

namespace {

bool inside(const wlr_box &b, double x, double y) {
	return x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height;
}

[[maybe_unused]] std::string mmss(double seconds) {
	const int s = std::max(0, static_cast<int>(seconds));
	char buf[16];
	std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
	return buf;
}

} // namespace

// ============================================================================
// Popover
// ============================================================================

Popover::Popover(Server *server, int width, int height)
		: server_(server), w_(width), h_(height), layer_(server->layer_menu, &tag),
		fx_(server->anim.get(), [this] { redraw(); }), hl_(layer_, server->anim.get()) {
	layer_.set_enabled(false);
}

Popover::~Popover() {
	server_->anim->cancel(anim_);
}

void Popover::resize(int width, int height) {
	w_ = width;
	h_ = height;
}

void Popover::open(Output *output, wlr_box anchor, const std::string &edge) {
	if (!output) {
		return;
	}
	output_ = output;
	anchor_ = anchor;
	edge_ = edge;
	open_ = true;
	hover_.clear();
	on_open();
	redraw();
	layer_.set_enabled(true);
	layer_.raise();
	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(t_, 1.0, c.animate(c.anim_menu) ? c.ms(240) : 0,
		[this] { place(); }, [this] { anim_ = 0; });
	server_->redraw_panels();
}

void Popover::close() {
	if (!open_) {
		return;
	}
	open_ = false;
	dragging_.clear();
	on_close();
	const Config &c = server_->config;
	server_->anim->cancel(anim_);
	anim_ = server_->anim->tween(t_, 0.0, c.animate(c.anim_menu) ? c.ms(170) : 0,
		[this] { place(); },
		[this] {
			anim_ = 0;
			if (!open_) {
				layer_.set_enabled(false);
			}
		});
	server_->redraw_panels();
}

void Popover::output_gone(Output *output) {
	if (output_ == output) {
		open_ = false;
		server_->anim->cancel(anim_);
		anim_ = 0;
		t_ = 0;
		layer_.set_enabled(false);
		output_ = nullptr;
	}
}

void Popover::refresh() {
	if (open_ || t_ > 0) {
		redraw();
	}
}

// Beside the button, on the side away from its panel's edge; it zooms out
// of the corner nearest the button.
void Popover::place() {
	if (!output_) {
		return;
	}
	const wlr_box u = output_->usable();
	const int gap = 10;
	int x, y;
	if (edge_ == "below") {
		// Under the anchor (a title bar button), or above it if there's no room.
		x = anchor_.x + anchor_.width / 2 - w_ / 2;
		y = anchor_.y + anchor_.height + 6;
		if (y + h_ > u.y + u.height - gap) y = anchor_.y - h_ - 6;
	} else if (edge_ == "point") {
		// At the pointer, flipped to stay on screen.
		x = anchor_.x + 2;
		y = anchor_.y + 2;
		if (x + w_ > u.x + u.width - gap) x = anchor_.x - w_ - 2;
		if (y + h_ > u.y + u.height - gap) y = anchor_.y - h_ - 2;
	} else if (anchor_.width <= 0) {
		x = u.x + (u.width - w_) / 2;
		y = u.y + (u.height - h_) / 2;
	} else if (edge_ == "top" || edge_ == "bottom") {
		x = anchor_.x + anchor_.width / 2 - w_ / 2;
		y = edge_ == "top" ? u.y + gap : u.y + u.height - h_ - gap;
	} else {
		y = anchor_.y + anchor_.height / 2 - h_ / 2;
		x = edge_ == "left" ? u.x + gap : u.x + u.width - w_ - gap;
	}
	x += offset_x_;
	y += offset_y_;
	x = std::clamp(x, u.x + gap, std::max(u.x + gap, u.x + u.width - w_ - gap));
	y = std::clamp(y, u.y + gap, std::max(u.y + gap, u.y + u.height - h_ - gap));
	box_ = wlr_box{x, y, w_, h_};
	if (anchor_.width > 0 || edge_ == "point" || edge_ == "below") {
		origin_x_ = std::clamp<double>(anchor_.x + anchor_.width / 2.0 - x, 0, w_);
		origin_y_ = std::clamp<double>(anchor_.y + anchor_.height / 2.0 - y, 0, h_);
	} else {
		origin_x_ = w_ / 2.0;
		origin_y_ = h_ / 2.0;
	}
	const double t = std::clamp(t_, 0.0, 1.1);
	layer_.show(x, y, std::min(1.0, t * 1.4), 0.86 + 0.14 * t, origin_x_, origin_y_);
}

bool Popover::contains(double lx, double ly) const {
	return open_ && inside(box_, lx, ly);
}

void Popover::redraw() {
	if (!output_ || drawing_) {
		return;
	}
	drawing_ = true;
	const float scale = output_->output->scale;
	Canvas canvas(w_, h_, scale);
	cairo_t *cr = canvas.cr();
	hots_.clear();
	ui_card(cr, 0, 0, w_, h_, theme::ui_radius, theme::menu_bg);
	draw(cr, scale);
	layer_.present(canvas, wlr_box{0, 0, w_, h_}, theme::ui_radius);
	drawing_ = false;
	place();
	update_highlight();
}

void Popover::add_hot(const std::string &id, double x, double y, double w, double h,
		double radius) {
	hots_.push_back(Hot{id, wlr_box{static_cast<int>(x), static_cast<int>(y), static_cast<int>(w),
		static_cast<int>(h)}, radius});
}

double Popover::hover(const std::string &) {
	return 0.0;
}

void Popover::update_highlight() {
	for (const Hot &h : hots_) {
		if (h.id == hover_ && h.radius >= 0) {
			hl_.show(h.box, h.radius, with_alpha(theme::text, theme::dark ? 0.10 : 0.07));
			return;
		}
	}
	hl_.hide();
}

std::string Popover::hot_at(double px, double py) const {
	for (auto it = hots_.rbegin(); it != hots_.rend(); ++it) {
		if (inside(it->box, px, py)) {
			return it->id;
		}
	}
	return "";
}

void Popover::motion(double lx, double ly) {
	const double px = lx - box_.x, py = ly - box_.y;
	if (!dragging_.empty()) {
		on_drag(dragging_, px, py);
		return;
	}
	const std::string now = hot_at(px, py);
	if (now != hover_) {
		hover_ = now;
		update_highlight();
		on_hover();
	}
}

void Popover::leave() {
	if (!hover_.empty() && dragging_.empty()) {
		hover_.clear();
		update_highlight();
		on_hover();
	}
}

void Popover::press(double lx, double ly, uint32_t button) {
	const double px = lx - box_.x, py = ly - box_.y;
	const std::string id = hot_at(px, py);
	if (button == BTN_LEFT && draggable(id)) {
		dragging_ = id;
		server_->popover_grab = this;
		on_drag(id, px, py);
		return;
	}
	if (!id.empty()) {
		hl_.flash();
		activate(id, button, px, py);
	}
}

void Popover::release() {
	if (!dragging_.empty()) {
		const std::string id = dragging_;
		dragging_.clear();
		activate(id, BTN_LEFT, -1, -1);   // "let go"
	}
	server_->popover_grab = nullptr;
}

void Popover::scroll(double, double, double delta) {
	on_scroll(delta);
}

// ============================================================================
// Calendar
// ============================================================================

Calendar::Calendar(Server *server) : Popover(server, 336, 356) {}

void Calendar::on_open() {
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	year_ = local.tm_year + 1900;
	month_ = local.tm_mon;
	slide_ = 0;
}

void Calendar::change_month(int delta) {
	month_ += delta;
	while (month_ < 0) { month_ += 12; year_--; }
	while (month_ > 11) { month_ -= 12; year_++; }
	// The new month slides in from the side it came from.
	const Config &c = server_->config;
	server_->anim->cancel(slide_anim_);
	slide_ = delta > 0 ? 1.0 : -1.0;
	slide_anim_ = server_->anim->tween(slide_, 0.0, c.animate(c.anim_menu) ? c.ms(220) : 0,
		[this] { redraw(); }, [this] { slide_anim_ = 0; });
	redraw();
}

void Calendar::on_scroll(double delta) {
	if (std::fabs(delta) >= 1) {
		change_month(delta > 0 ? 1 : -1);
	}
}

void Calendar::activate(const std::string &id, uint32_t button, double, double) {
	if (button != BTN_LEFT) {
		return;
	}
	if (id == "prev") change_month(-1);
	else if (id == "next") change_month(1);
	else if (id == "today") {
		std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_r(&now, &local);
		const int delta = (local.tm_year + 1900 - year_) * 12 + (local.tm_mon - month_);
		if (delta != 0) change_month(delta);
	}
}

void Calendar::draw(cairo_t *cr, float scale) {
	const int pad = 18;
	const double r = theme::ui_radius;
	const bool monday = server_->config.week_monday;

	// A soft accent glow at the top, with a small bar on the edge.
	{
		cairo_save(cr);
		rounded_rect(cr, 2, 2, w_ - 4, h_ - 4, inner_radius(r, 2));
		cairo_clip(cr);
		cairo_pattern_t *glow = cairo_pattern_create_radial(w_ / 2.0, -20, 0, w_ / 2.0, -20, w_ * 0.55);
		const Color &a = theme::accent;
		cairo_pattern_add_color_stop_rgba(glow, 0, a.r, a.g, a.b, theme::dark ? 0.38 : 0.22);
		cairo_pattern_add_color_stop_rgba(glow, 1, a.r, a.g, a.b, 0);
		cairo_set_source(cr, glow);
		cairo_paint(cr);
		cairo_pattern_destroy(glow);
		fill_rounded(cr, w_ / 2.0 - 36, 2, 72, 4, 2, theme::accent);
		cairo_restore(cr);
	}

	// Header: arrows and the month.
	std::tm first{};
	first.tm_year = year_ - 1900;
	first.tm_mon = month_;
	first.tm_mday = 1;
	first.tm_hour = 12;
	std::mktime(&first);
	char title[64];
	std::strftime(title, sizeof title, "%B %Y", &first);
	const int bs = 36;
	for (const auto &[id, x] : {std::pair<std::string, int>{"prev", pad},
			std::pair<std::string, int>{"next", w_ - pad - bs}}) {
		const double h = hover(id);
		cairo_new_path(cr);
		cairo_arc(cr, x + bs / 2.0, pad + bs / 2.0, bs / 2.0 - 0.5, 0, 2 * M_PI);
		set_color(cr, with_alpha(theme::text, 0.05 + 0.10 * h));
		cairo_fill_preserve(cr);
		set_color(cr, with_alpha(theme::text, 0.12));
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		draw_named_icon(cr, server_->icons, id == "prev" ? "chevron-left" : "chevron-right",
			x + bs / 2.0 - 8, pad + bs / 2.0 - 8, 16, theme::text, scale);
		add_hot(id, x, pad, bs, bs, bs / 2.0);
	}
	{
		const double h = hover("today");
		const int tw = text_width(title, theme::font_large) + 24;
		if (h > 0.01) {
			fill_rounded(cr, (w_ - tw) / 2.0, pad + 2, tw, bs - 4, (bs - 4) / 2.0,
				with_alpha(theme::text, 0.08 * h));
		}
		draw_text(cr, title, pad + bs, pad, w_ - 2 * (pad + bs), bs, theme::font_large, theme::text,
			Align::Center);
		add_hot("today", (w_ - tw) / 2.0, pad + 2, tw, bs - 4, (bs - 4) / 2.0);
	}

	// Weekday names.
	const int grid_x = pad - 2, grid_w = w_ - 2 * grid_x;
	const double cw = grid_w / 7.0;
	const int names_y = pad + bs + 12;
	static const char *kMon[] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
	static const char *kSun[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
	for (int i = 0; i < 7; i++) {
		draw_text(cr, monday ? kMon[i] : kSun[i], grid_x + i * cw, names_y, cw, 20, theme::font_small,
			theme::text_dim, Align::Center);
	}

	// Days. Leading/trailing days of the neighbouring months are faint.
	const int first_wday = first.tm_wday;   // 0 = Sunday
	const int lead = monday ? (first_wday + 6) % 7 : first_wday;
	auto days_in = [](int y, int m) {
		static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
		const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
		return m == 1 && leap ? 29 : d[m];
	};
	const int n = days_in(year_, month_);
	const int prev_n = days_in(month_ == 0 ? year_ - 1 : year_, (month_ + 11) % 12);
	std::time_t now = std::time(nullptr);
	std::tm today{};
	localtime_r(&now, &today);
	const bool this_month = today.tm_year + 1900 == year_ && today.tm_mon == month_;

	const int rows_y = names_y + 26;
	const double rh = (h_ - rows_y - pad + 4) / 6.0;
	const double shift = slide_ * 40;
	const double fade = 1.0 - std::min(1.0, std::fabs(slide_));
	cairo_save(cr);
	cairo_rectangle(cr, 4, rows_y - 4, w_ - 8, h_ - rows_y);
	cairo_clip(cr);
	for (int cell = 0; cell < 42; cell++) {
		const int day = cell - lead + 1;
		const bool other = day < 1 || day > n;
		const int shown = day < 1 ? prev_n + day : day > n ? day - n : day;
		if (other && cell >= 35 && lead + n <= 35) {
			continue;   // a sixth row of only next month: leave it out
		}
		const double cx = grid_x + (cell % 7) * cw + cw / 2 + shift;
		const double cy = rows_y + (cell / 7) * rh + rh / 2;
		const bool is_today = this_month && !other && day == today.tm_mday;
		if (is_today) {
			// The accent dot with a soft glow, like light on a screen.
			cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy + 2, 8, cx, cy + 2, 28);
			const Color &a = theme::accent;
			cairo_pattern_add_color_stop_rgba(g, 0, a.r, a.g, a.b, 0.45 * fade);
			cairo_pattern_add_color_stop_rgba(g, 1, a.r, a.g, a.b, 0);
			cairo_set_source(cr, g);
			cairo_arc(cr, cx, cy + 2, 28, 0, 2 * M_PI);
			cairo_fill(cr);
			cairo_pattern_destroy(g);
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, std::min(cw, rh) / 2 - 2, 0, 2 * M_PI);
			set_color(cr, with_alpha(theme::accent, fade));
			cairo_fill(cr);
		}
		Color c = is_today ? theme::on_accent : other ? with_alpha(theme::text_dim, 0.45) : theme::text;
		c.a *= fade;
		draw_text(cr, std::to_string(shown), cx - cw / 2, cy - rh / 2, cw, rh, theme::font_large, c,
			Align::Center);
	}
	cairo_restore(cr);
}

// ============================================================================
// Media card
// ============================================================================

namespace {
constexpr int kMediaW = 380, kMediaH = 216, kLyricsH = 206;
}

MediaCard::MediaCard(Server *server) : Popover(server, kMediaW, kMediaH) {
	timer_ = wl_event_loop_add_timer(server->loop, &MediaCard::handle_timer, this);
	view_ = std::make_unique<MediaView>(server);
}

MediaCard::~MediaCard() {
	wl_event_source_remove(timer_);
	if (ticker_) {
		server_->anim->remove_ticker(ticker_);
	}
	view_.reset();
}

int MediaCard::handle_timer(void *data) {
	auto *self = static_cast<MediaCard *>(data);
	if (!self->open_) {
		return 0;
	}
	self->server_->status->refresh_media();
	wl_event_source_timer_update(self->timer_, 1000);
	return 0;
}

void MediaCard::on_open() {
	server_->status->refresh_media();
	wl_event_source_timer_update(timer_, 1000);
	view_->set_shown(true);
	// The wave, the title and the time move while it's open.
	if (!ticker_) {
		ticker_ = server_->anim->add_ticker([this](double dt) {
			if (!open_) {
				view_->set_shown(false);
				ticker_ = 0;
				return false;
			}
			view_->step(dt);
			since_draw_ += dt;
			if (since_draw_ >= 33) {
				since_draw_ = 0;
				redraw();
			}
			return true;
		});
	}
}

void MediaCard::on_close() {
	wl_event_source_timer_update(timer_, 0);
	view_->set_shown(false);
}

void MediaCard::on_drag(const std::string &, double px, double) {
	view_->drag(px);
	redraw();
}

void MediaCard::activate(const std::string &id, uint32_t button, double px, double py) {
	SystemStatus &st = *server_->status;
	Lyrics *ly = server_->lyrics.get();
	if (id == "mp:seek") {
		view_->end_drag();
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}
	const wlr_box b = box();
	if (id == "mp:output") {
		sound_output_menu(server_, output_, b.x + px, b.y + py);
	} else if (id == "mp:lyrics") {
		lyrics_open_ = !lyrics_open_;
		resize(kMediaW, kMediaH + (lyrics_open_ ? kLyricsH : 0));
	} else if (id == "ly:show") {
		server_->write_setting("lyrics_show", server_->config.lyrics_show ? "false" : "true");
	} else if (ly && id == "ly:earlier") {
		ly->nudge_delay(-250);
	} else if (ly && id == "ly:later") {
		ly->nudge_delay(250);
	} else if (ly && id == "ly:slower") {
		ly->nudge_speed(-1);
	} else if (ly && id == "ly:faster") {
		ly->nudge_speed(1);
	} else if (ly && id == "ly:reset") {
		ly->reset_song_timing();
	} else if (ly && id == "ly:refresh") {
		ly->retry(false);
	} else if (ly && id == "ly:retry") {
		ly->retry(false);
	} else if (ly && id == "ly:listen") {
		ly->retry(true);
	} else if (id == "mp:source") {
		media_source_menu(server_, output_, b.x + px, b.y + py);
		return;
	} else if (id == "mp:card") {
		return;
	} else if (id.rfind("mp:", 0) == 0) {
		st.media(id.substr(3).c_str());
	}
	redraw();
}

void MediaCard::draw(cairo_t *cr, float scale) {
	const int inset = theme::ui_line == 2 ? 2 : theme::ui_line == 1 ? 1 : 0;
	const double r = inner_radius(theme::ui_radius, inset);
	std::vector<MediaHot> hots;
	// With the lyrics open the card keeps its square bottom corners on the panel under it.
	view_->draw(cr, inset, inset, w_ - 2 * inset, kMediaH - inset - (lyrics_open_ ? 0 : inset), r, scale,
		MediaView::Look::Samsung, &hots);
	for (const MediaHot &h : hots) {
		add_hot(h.id, h.box.x, h.box.y, h.box.width, h.box.height, h.radius);
	}
	if (lyrics_open_) {
		draw_lyrics(cr, kMediaH, kLyricsH, scale);
	}
}

namespace {
struct B2 {
	const char *id, *label, *icon;
};
} // namespace

// Under the card: the lines around now, and the timing for this song.
void MediaCard::draw_lyrics(cairo_t *cr, double y, double h, float scale) {
	Lyrics *ly = server_->lyrics.get();
	const double pad = 16;
	const double w = w_ - 2 * pad;
	if (!ly || !ly->available()) {
		const bool off = !server_->config.lyrics;
		const std::string msg = off ? "Lyrics are off (Settings › Media & lyrics)"
			: ly && ly->searching() ? "Looking for lyrics…" : "No synced lyrics for this song yet";
		draw_text(cr, msg, pad, y + 14, w, 24, theme::font, theme::text, Align::Center);
		// Why: what the search said last (not on lrclib.net, SongRec missing…).
		if (ly && !off && !ly->status().empty()) {
			PangoLayout *pl = pango_cairo_create_layout(cr);
			PangoFontDescription *fd = pango_font_description_from_string(theme::font_small);
			pango_layout_set_font_description(pl, fd);
			pango_font_description_free(fd);
			pango_layout_set_text(pl, ly->status().c_str(), -1);
			pango_layout_set_width(pl, static_cast<int>(w * PANGO_SCALE));
			pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
			pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
			pango_layout_set_height(pl, -3);
			pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
			cairo_set_source_rgba(cr, theme::text_dim.r, theme::text_dim.g, theme::text_dim.b, theme::text_dim.a);
			cairo_move_to(cr, pad, y + 44);
			pango_cairo_show_layout(cr, pl);
			g_object_unref(pl);
		}
		if (ly && !off && !ly->searching()) {
			const B2 buttons[] = {{"ly:retry", "Search again", "search"}, {"ly:listen", "Listen for it", "mic-vocal"}};
			const double gap = 8, bw = (w - gap) / 2, by = y + h - 50;
			for (int k = 0; k < 2; k++) {
				const double bx = pad + k * (bw + gap);
				fill_rounded(cr, bx, by, bw, 32, 16, k == 0 ? theme::accent : theme::tile_off);
				const Color fg = k == 0 ? theme::on_accent : theme::text;
				const double tw = text_width(buttons[k].label, theme::font_small);
				draw_named_icon(cr, server_->icons, buttons[k].icon, bx + (bw - tw) / 2 - 22, by + 8, 16, fg, scale);
				draw_text(cr, buttons[k].label, bx + (bw - tw) / 2, by, tw + 4, 32, theme::font_small, fg);
				add_hot(buttons[k].id, bx, by, bw, 32, 16);
			}
		}
		return;
	}
	// The lyrics as on the screen: in their style, colour and translation, on
	// a dark backdrop (their white letters read on it in light mode too).
	{
		const Color c = ly->colour();
		fill_rounded(cr, pad - 6, y + 4, w + 12, 104, 12, mix(rgb(0x111114), c, 0.10));
		ly->draw_in(cr, pad - 2, y + 6, w + 4, 100, scale);
	}
	// Timing: earlier / later, slower / faster, for this song.
	char info[96];
	std::snprintf(info, sizeof info, "This song: %+.2f s · %d%% speed", ly->song_delay() / 1000.0, ly->song_speed());
	draw_text(cr, info, pad, y + 112, w, 18, theme::font_small, theme::text_dim, Align::Center);
	// Refresh: wrong song, or badly timed? Look for the lyrics again.
	{
		const double bx = pad + w - 26, byy = y + 108;
		fill_rounded(cr, bx, byy, 26, 26, 13, theme::tile_off);
		draw_named_icon(cr, server_->icons, "refresh", bx + 6, byy + 6, 14, theme::text, scale);
		add_hot("ly:refresh", bx, byy, 26, 26, 13);
	}
	struct B {
		const char *id, *label;
	};
	const B buttons[] = {{"ly:earlier", "Earlier"}, {"ly:later", "Later"}, {"ly:slower", "Slower"},
		{"ly:faster", "Faster"}, {"ly:reset", "Reset"}};
	const double gap = 6, bw = (w - 4 * gap) / 5, by = y + 134;
	for (int k = 0; k < 5; k++) {
		const double bx = pad + k * (bw + gap);
		fill_rounded(cr, bx, by, bw, 28, 14, theme::tile_off);
		draw_text(cr, buttons[k].label, bx, by, bw, 28, theme::font_small, theme::text, Align::Center);
		add_hot(buttons[k].id, bx, by, bw, 28, 14);
	}
	const bool on = server_->config.lyrics_show;
	const std::string show = on ? "Showing on the screen" : "Show on the screen";
	const double sw = text_width(show, theme::font_small) + 40;
	fill_rounded(cr, (w_ - sw) / 2, by + 36, sw, 26, 13, on ? theme::accent : theme::tile_off);
	draw_named_icon(cr, server_->icons, "mic-vocal", (w_ - sw) / 2 + 10, by + 42, 14,
		on ? theme::on_accent : theme::text, scale);
	draw_text(cr, show, (w_ - sw) / 2 + 28, by + 36, sw - 34, 26, theme::font_small,
		on ? theme::on_accent : theme::text);
	add_hot("ly:show", (w_ - sw) / 2, by + 36, sw, 26, 13);
}

} // namespace dusk

namespace dusk {

// ============================================================================
// Right-click menu
// ============================================================================

namespace {
constexpr int kMenuPad = 6;
constexpr int kRowH = 32;
constexpr int kHeaderH = 26;
constexpr int kSepH = 9;

int row_height(const MenuItem &m) {
	switch (m.kind) {
	case MenuItem::Kind::Separator: return kSepH;
	case MenuItem::Kind::Header: return kHeaderH;
	default: return kRowH;
	}
}
} // namespace

ContextMenu::ContextMenu(Server *server) : Popover(server, 220, 100) {}

void ContextMenu::show(Output *output, wlr_box anchor, const std::string &edge,
		std::vector<MenuItem> items) {
	items_ = std::move(items);
	bool marks = false, icons = false;
	int text = 0, h = 2 * kMenuPad;
	for (const MenuItem &m : items_) {
		h += row_height(m);
		marks = marks || m.kind == MenuItem::Kind::Check || m.kind == MenuItem::Kind::Radio;
		icons = icons || !m.icon.empty();
		text = std::max(text, text_width(m.label, m.kind == MenuItem::Kind::Header
			? theme::font_small : theme::font));
	}
	const int lead = (marks || icons) ? 30 : 0;
	const int w = std::clamp(text + lead + 2 * kMenuPad + 28, 180, 340);
	resize(w, h);
	if (is_open()) {
		redraw();
	}
	open(output, anchor, edge);
}

void ContextMenu::draw(cairo_t *cr, float scale) {
	bool lead = false;
	for (const MenuItem &m : items_) {
		lead = lead || !m.icon.empty() || m.kind == MenuItem::Kind::Check ||
			m.kind == MenuItem::Kind::Radio;
	}
	const double r = inner_radius(theme::ui_radius, kMenuPad);
	int y = kMenuPad;
	for (size_t i = 0; i < items_.size(); i++) {
		const MenuItem &m = items_[i];
		const int rh = row_height(m);
		if (m.kind == MenuItem::Kind::Separator) {
			cairo_set_source_rgba(cr, theme::text.r, theme::text.g, theme::text.b, 0.10);
			cairo_rectangle(cr, kMenuPad + 8, y + kSepH / 2.0 - 0.5, w_ - 2 * kMenuPad - 16, 1);
			cairo_fill(cr);
		} else if (m.kind == MenuItem::Kind::Header) {
			draw_text(cr, m.label, kMenuPad + 12, y + 4, w_ - 2 * kMenuPad - 24, rh - 4,
				theme::font_small, theme::text_dim);
		} else {
			const Color fg = !m.enabled ? with_alpha(theme::text, 0.35)
				: m.danger ? theme::danger : theme::text;
			double tx = kMenuPad + 12;
			if (lead) {
				const double cx = kMenuPad + 12 + 9, cy = y + rh / 2.0;
				if ((m.kind == MenuItem::Kind::Check || m.kind == MenuItem::Kind::Radio) && m.on) {
					if (m.kind == MenuItem::Kind::Radio) {
						cairo_set_source_rgba(cr, theme::accent.r, theme::accent.g, theme::accent.b, 1);
						cairo_new_path(cr);
						cairo_arc(cr, cx, cy, 4, 0, 2 * M_PI);
						cairo_fill(cr);
					} else {
						draw_glyph(cr, "check", cx, cy, 16, theme::accent);
					}
				} else if (m.kind == MenuItem::Kind::Radio) {
					cairo_set_source_rgba(cr, theme::text.r, theme::text.g, theme::text.b, 0.25);
					cairo_set_line_width(cr, 1.2);
					cairo_new_path(cr);   // or the ring joins up with the last thing drawn
					cairo_arc(cr, cx, cy, 4.5, 0, 2 * M_PI);
					cairo_stroke(cr);
				} else if (!m.icon.empty()) {
					draw_named_icon(cr, server_->icons, m.icon, cx - 8, cy - 8, 16, fg, scale);
				}
				tx += 28;
			}
			draw_text(cr, m.label, tx, y, w_ - tx - kMenuPad - 10, rh, theme::font, fg);
			if (m.enabled) {
				add_hot("item:" + std::to_string(i), kMenuPad, y, w_ - 2 * kMenuPad, rh, r);
			}
		}
		y += rh;
	}
}

void ContextMenu::activate(const std::string &id, uint32_t, double, double) {
	if (id.rfind("item:", 0) != 0) {
		return;
	}
	const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 5));
	if (i >= items_.size()) {
		return;
	}
	// Copy first: running it can rebuild panels, or open another menu.
	std::function<void()> run = items_[i].run;
	close();
	if (run) {
		run();
	}
}

} // namespace dusk

namespace dusk {

// ============================================================================
// Snap layouts
// ============================================================================

namespace {
constexpr int kThumbW = 84, kThumbH = 52, kThumbGap = 10, kSnapPad = 12;
}

SnapLayouts::SnapLayouts(Server *server) : Popover(server, 200, 80) {}

void SnapLayouts::fit_size() {
	const int n = std::max<int>(1, static_cast<int>(server_->config.snap_layouts.size()));
	// One row of up to 6; more wrap.
	const int per_row = std::min(n, 6);
	const int rows = (n + per_row - 1) / per_row;
	resize(2 * kSnapPad + per_row * kThumbW + (per_row - 1) * kThumbGap,
		2 * kSnapPad + rows * kThumbH + (rows - 1) * kThumbGap);
}

void SnapLayouts::draw(cairo_t *cr, float) {
	const auto &layouts = server_->config.snap_layouts;
	const int per_row = std::min<int>(std::max<int>(1, static_cast<int>(layouts.size())), 6);
	const double r = inner_radius(theme::ui_radius, kSnapPad);
	for (size_t li = 0; li < layouts.size(); li++) {
		const int col = static_cast<int>(li) % per_row, row = static_cast<int>(li) / per_row;
		const double tx = kSnapPad + col * (kThumbW + kThumbGap);
		const double ty = kSnapPad + row * (kThumbH + kThumbGap);
		// The little screen.
		rounded_rect(cr, tx, ty, kThumbW, kThumbH, std::min(r, 9.0));
		set_color(cr, with_alpha(theme::text, 0.06));
		cairo_fill(cr);
		for (size_t zi = 0; zi < layouts[li].size(); zi++) {
			const auto &z = layouts[li][zi];
			const double zx = tx + 3 + z[0] * (kThumbW - 6), zy = ty + 3 + z[1] * (kThumbH - 6);
			const double zw = z[2] * (kThumbW - 6) - 3, zh = z[3] * (kThumbH - 6) - 3;
			const std::string id = "z:" + std::to_string(li) + ":" + std::to_string(zi);
			const bool lit = hover_ == id;
			rounded_rect(cr, zx + 1.5, zy + 1.5, zw, zh, 4.5);
			set_color(cr, lit ? theme::accent : with_alpha(theme::text, theme::dark ? 0.20 : 0.16));
			cairo_fill(cr);
			add_hot(id, zx, zy, zw + 3, zh + 3, -1);
		}
	}
}

bool SnapLayouts::zone_of(const std::string &id, View::Zone *zone) const {
	if (id.rfind("z:", 0) != 0) {
		return false;
	}
	const size_t colon = id.find(':', 2);
	const size_t li = static_cast<size_t>(std::atoi(id.c_str() + 2));
	const size_t zi = static_cast<size_t>(std::atoi(id.c_str() + colon + 1));
	const auto &layouts = server_->config.snap_layouts;
	if (li >= layouts.size() || zi >= layouts[li].size()) {
		return false;
	}
	const auto &z = layouts[li][zi];
	*zone = View::Zone{z[0], z[1], z[2], z[3]};
	return true;
}

bool SnapLayouts::drag_motion(View *view, Output *output, double lx, double ly, View::Zone *zone) {
	if (!output || !server_->config.snap_bar || server_->config.snap_layouts.empty()) {
		drag_end();
		return false;
	}
	const wlr_box u = output->usable();
	const bool inside = is_open() && output_ == output && contains(lx, ly);
	// Appears when the window is brought to the top of the screen in the
	// middle (about where the bar shows), not anywhere along the top: the
	// rest of the top edge maximises, its corners tile to a quarter.
	const double reach = std::max(160.0, std::min(u.width * 0.18, 320.0));
	const bool near_top = ly < u.y + 60 && std::abs(lx - (u.x + u.width / 2.0)) < reach;
	if (!inside && !near_top) {
		drag_end();
		return false;
	}
	if (!is_open() || output_ != output || menu_mode_) {
		menu_mode_ = false;
		view_ = view;
		fit_size();
		open(output, wlr_box{u.x + u.width / 2, u.y, 1, 1}, "top");
	}
	// Which zone is under the pointer (Popover::motion finds it and
	// redraws through on_hover).
	motion(lx, ly);
	return zone_of(hover_, zone);
}

void SnapLayouts::drag_end() {
	if (is_open() && !menu_mode_) {
		close();
	}
}

void SnapLayouts::open_for(View *view, Output *output, wlr_box anchor) {
	if (!view || !output || server_->config.snap_layouts.empty()) {
		return;
	}
	menu_mode_ = true;
	view_ = view;
	fit_size();
	if (is_open()) {
		redraw();
	}
	open(output, anchor, "below");
}

void SnapLayouts::on_hover() {
	redraw();   // the hovered zone lights up in the accent colour
	if (!menu_mode_ || !view_) {
		return;
	}
	View::Zone z;
	if (zone_of(hover_, &z) && output_) {
		server_->show_snap_preview(view_->zone_frame(z, output_), view_);
	} else {
		server_->hide_snap_preview();
	}
}

void SnapLayouts::activate(const std::string &id, uint32_t, double, double) {
	View::Zone z;
	if (!menu_mode_ || !view_ || !zone_of(id, &z)) {
		return;
	}
	View *v = view_;
	Output *out = output_;
	close();
	v->tile_to(z, out);
	server_->focus(v);
}

void SnapLayouts::on_close() {
	if (menu_mode_) {
		server_->hide_snap_preview();
	}
}

void SnapLayouts::view_gone(View *view) {
	if (view_ == view) {
		view_ = nullptr;
		if (is_open()) close();
	}
}

} // namespace dusk
