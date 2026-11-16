// The clipboard history: Dusk remembers what you copy (text, pictures and
// files), and you can pick an older entry to make it the clipboard again —
// then paste it anywhere as usual (Ctrl+V, or right-click > Paste). It is
// opened from the clipboard on a panel, or with Super+V.
//
// Dusk reads each new clipboard itself (no helper programs), keeps the
// history in memory only, and skips what password managers mark as secret.
#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "popover.hpp"
#include "server.hpp"

namespace dusk {

struct ClipEntry {
	enum class Kind { Text, Image, Files };
	uint64_t id = 0;
	Kind kind = Kind::Text;
	std::shared_ptr<const std::string> data;   // UTF-8 text, PNG bytes, or a text/uri-list
	std::vector<std::string> files;            // Files: local paths (or URIs that aren't files)
	bool cut = false;                          // Files: moved, not copied, when pasted
	int image_w = 0, image_h = 0;              // Image
	std::time_t time = 0;
};

struct ClipReader;
struct ClipSource;

class Clipboard {
public:
	explicit Clipboard(Server *server);
	~Clipboard();
	Clipboard(const Clipboard &) = delete;
	Clipboard &operator=(const Clipboard &) = delete;

	const std::deque<ClipEntry> &history() const { return history_; }
	// The entry that is the clipboard right now (0: something not in the list).
	uint64_t current() const { return current_; }
	// Makes an entry the clipboard again.
	void use(uint64_t id);
	void remove(uint64_t id);
	void clear();
	// Settings changed (history on/off, its size).
	void settings_changed();

	// Files and folders for the desktop's Copy and Cut.
	void copy_files(const std::vector<std::string> &paths, bool cut);
	// A data source for dragging these files into an app (wlroots owns it
	// once the drag has started).
	wlr_data_source *drag_source(const std::vector<std::string> &paths);
	// Whether the clipboard holds files (for the desktop's Paste).
	bool has_files() const;
	// The files on the clipboard, read in the background.
	void get_files(std::function<void(std::vector<std::string> paths, bool cut)> done);

	// A small picture of an image entry, `px` device pixels at most (cached).
	cairo_surface_t *thumbnail(const ClipEntry &entry, int px);

	std::function<void()> changed;   // the list or the current entry changed

	// Reads one type of what `source` offers, in the background (also used
	// for files dropped onto the desktop).
	void read(wlr_data_source *source, const std::string &mime, size_t limit,
		std::function<void(std::string)> done);

	// Used by the readers and our own data source.
	void reader_done(ClipReader *reader);
	void source_gone(ClipSource *source);
	wl_event_loop *loop() const { return server_->loop; }

private:
	void handle_set_selection(void *data);
	void add(ClipEntry entry);
	// Makes it the seat's selection. `fresh`: a new serial (you picked it);
	// otherwise the last one, so an app's next copy is never turned down as
	// older than ours (that stopped copying in some apps).
	void offer(const ClipEntry &entry, bool fresh = true);
	void restore_newest();                  // the newest entry, back in the clipboard
	wl_event_source *restore_timer_ = nullptr;
	static int handle_restore(void *data);
	void trim();
	void notify();
	void save();
	void load();
	bool saved_any_ = false;

	Server *server_;
	Listener on_set_selection_;
	std::deque<ClipEntry> history_;
	uint64_t next_id_ = 1;
	uint64_t current_ = 0;
	uint64_t generation_ = 0;          // which selection a finished read belongs to
	wl_event_source *read_timer_ = nullptr;   // reads a new selection a moment after it's copied
	static int handle_read_timer(void *data);
	void read_selection();
	ClipSource *own_ = nullptr;        // our data source, while it's the selection
	std::vector<ClipReader *> readers_;
	std::map<uint64_t, cairo_surface_t *> thumbs_;
};

// "file:///a%20b\r\nfile:///c" -> {"/a b", "/c"} (other URIs are kept as they are).
std::vector<std::string> parse_uri_list(const std::string &text);

// The list, as a card that opens from the panel's clipboard (or at the
// pointer with Super+V).
class ClipboardCard : public Popover {
public:
	explicit ClipboardCard(Server *server);
	void update();   // the history changed
	void show_tab(int tab) { tab_ = tab; }
	// While it's open: Up/Down pick an entry (the newest is picked when it
	// opens), Enter puts it in the clipboard, Delete forgets it.
	bool handle_key(xkb_keysym_t sym);

protected:
	void draw(cairo_t *cr, float scale) override;
	void on_open() override;
	void activate(const std::string &id, uint32_t button, double px, double py) override;
	void on_scroll(double delta) override;
	void on_hover() override;

private:
	void fit_size();
	int row_height(const ClipEntry &e) const;
	int scroll_ = 0, max_scroll_ = 0;
	int tab_ = 0;   // 0 clipboard, 1 notifications
	int sel_ = 0;   // the picked entry (0 = the newest)
	void scroll_to_selected();
};

} // namespace dusk
