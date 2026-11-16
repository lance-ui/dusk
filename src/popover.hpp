// Small cards that pop out of a panel button: the calendar (from the clock)
// and the media card (from the now-playing chip). They zoom out of the
// button that opened them and fade away when you click elsewhere.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "server.hpp"
#include "ui.hpp"

namespace dusk {

class Popover {
public:
	Popover(Server *server, int width, int height);
	virtual ~Popover();
	Popover(const Popover &) = delete;
	Popover &operator=(const Popover &) = delete;

	bool is_open() const { return open_; }
	bool is_open_on(const Output *output) const { return open_ && output_ == output; }
	void open(Output *output, wlr_box anchor, const std::string &edge);
	void close();
	void output_gone(Output *output);
	void refresh();   // redraw if showing

	bool contains(double lx, double ly) const;
	void motion(double lx, double ly);
	void leave();
	void press(double lx, double ly, uint32_t button);
	void release();
	void scroll(double lx, double ly, double delta);

	NodeTag tag{NodeTag::Kind::Popover, this};

protected:
	struct Hot {
		std::string id;
		wlr_box box;
		double radius;   // of its hover highlight; < 0 = none
	};
	virtual void draw(cairo_t *cr, float scale) = 0;   // the card is already filled
	virtual void on_open() {}
	virtual void on_close() {}
	virtual void activate(const std::string &id, uint32_t button, double px, double py) {}
	virtual void on_drag(const std::string &id, double px, double py) {}
	virtual void on_scroll(double delta) {}
	virtual void on_hover() {}   // hover_ changed
	virtual bool draggable(const std::string &id) const { return false; }

	void redraw();
	void add_hot(const std::string &id, double x, double y, double w, double h,
		double radius = 12);
	double hover(const std::string &id);   // drawn hover amount (0: the GPU highlight does it)
	void update_highlight();
	void resize(int width, int height);

	Server *server_;
	Output *output_ = nullptr;
	int w_, h_;
	UiLayer layer_;
	Fades fx_;
	Highlight hl_;
	std::vector<Hot> hots_;
	std::string hover_;
	std::string dragging_;
	bool open_ = false;
	// Moved by hand (the on-screen keyboard's handle): added to where it goes.
	int offset_x_ = 0, offset_y_ = 0;
	wlr_box box() const { return box_; }
	void place();

private:
	std::string hot_at(double px, double py) const;

	double t_ = 0;          // 0 closed .. 1 open
	uint64_t anim_ = 0;
	wlr_box anchor_{};
	std::string edge_ = "bottom";
	wlr_box box_{};
	double origin_x_ = 0, origin_y_ = 0;   // zoom from here (card coords)
	bool drawing_ = false;
};

// A month view: today highlighted, arrows (or the scroll wheel) to change month.
class Calendar : public Popover {
public:
	explicit Calendar(Server *server);

protected:
	void draw(cairo_t *cr, float scale) override;
	void on_open() override;
	void activate(const std::string &id, uint32_t button, double px, double py) override;
	void on_scroll(double delta) override;

private:
	void change_month(int delta);
	int year_ = 2026, month_ = 0;   // month 0..11
	double slide_ = 0;              // -1..1 while changing month
	uint64_t slide_anim_ = 0;
};

// What's playing: album art behind, title, a wavy progress line you can
// drag, and the controls. Opens from the panel's now-playing chip.
class MediaView;

class MediaCard : public Popover {
public:
	explicit MediaCard(Server *server);
	~MediaCard() override;

protected:
	void draw(cairo_t *cr, float scale) override;
	void on_open() override;
	void on_close() override;
	void activate(const std::string &id, uint32_t button, double px, double py) override;
	void on_drag(const std::string &id, double px, double py) override;
	bool draggable(const std::string &id) const override { return id == "mp:seek"; }

private:
	static int handle_timer(void *data);
	void draw_lyrics(cairo_t *cr, double y, double h, float scale);

	wl_event_source *timer_;
	uint64_t ticker_ = 0;
	double since_draw_ = 0;      // redraws about 30 times a second while it moves
	std::unique_ptr<MediaView> view_;
	bool lyrics_open_ = false;   // the lyrics and their timing, under the card
};

// One line of a right-click menu.
struct MenuItem {
	enum class Kind { Action, Check, Radio, Separator, Header };
	Kind kind = Kind::Action;
	std::string label;
	std::string icon;          // optional named icon (glyphs)
	bool on = false;           // Check / Radio: ticked
	bool enabled = true;
	bool danger = false;       // drawn in red (close, remove)
	std::function<void()> run;

	static MenuItem action(std::string label, std::function<void()> run, std::string icon = {}) {
		MenuItem m;
		m.label = std::move(label);
		m.run = std::move(run);
		m.icon = std::move(icon);
		return m;
	}
	static MenuItem check(std::string label, bool on, std::function<void()> run) {
		MenuItem m;
		m.kind = Kind::Check;
		m.label = std::move(label);
		m.on = on;
		m.run = std::move(run);
		return m;
	}
	static MenuItem radio(std::string label, bool on, std::function<void()> run) {
		MenuItem m = check(std::move(label), on, std::move(run));
		m.kind = Kind::Radio;
		return m;
	}
	static MenuItem header(std::string label) {
		MenuItem m;
		m.kind = Kind::Header;
		m.label = std::move(label);
		return m;
	}
	static MenuItem separator() {
		MenuItem m;
		m.kind = Kind::Separator;
		return m;
	}
};

// A right-click menu: shows next to a panel (edge "top"/"bottom"/...) or
// at the pointer (edge "point").
class ContextMenu : public Popover {
public:
	explicit ContextMenu(Server *server);
	void show(Output *output, wlr_box anchor, const std::string &edge, std::vector<MenuItem> items);

protected:
	void draw(cairo_t *cr, float scale) override;
	void activate(const std::string &id, uint32_t button, double px, double py) override;

private:
	std::vector<MenuItem> items_;
};

// Snap layouts, like Windows 11: little screens split into zones. Shown at
// the top of the screen while dragging a window (drop it on a zone), and
// under the maximise button or with Super+Z (click a zone).
class SnapLayouts : public Popover {
public:
	explicit SnapLayouts(Server *server);

	// Dragging `view` with the pointer at (lx, ly) on `output`: shows the
	// bar near the top. Returns true and the zone if the pointer is on one.
	bool drag_motion(View *view, Output *output, double lx, double ly, View::Zone *zone);
	void drag_end();
	// The menu for a window, under `anchor` (layout coords).
	void open_for(View *view, Output *output, wlr_box anchor);
	bool menu_open() const { return is_open() && menu_mode_; }
	void view_gone(View *view);

protected:
	void draw(cairo_t *cr, float scale) override;
	void activate(const std::string &id, uint32_t button, double px, double py) override;
	void on_hover() override;
	void on_close() override;

private:
	void fit_size();
	bool zone_of(const std::string &id, View::Zone *zone) const;
	View *view_ = nullptr;
	bool menu_mode_ = false;
};

} // namespace dusk
