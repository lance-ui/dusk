#include "clipboard.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

#include "draw.hpp"
#include "glyphs.hpp"
#include "notify.hpp"

namespace dusk {

namespace {

constexpr size_t kTextLimit = 1 << 20;        // 1 MB of text
constexpr size_t kImageLimit = 24 << 20;      // 24 MB picture
constexpr size_t kListLimit = 256 << 10;      // a file list
constexpr int kTransferMs = 5000;             // an app that never answers

const char *kTextTypes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};

std::vector<std::string> mime_types(wlr_data_source *source) {
	std::vector<std::string> out;
	if (!source) {
		return out;
	}
	char **types = static_cast<char **>(source->mime_types.data);
	const size_t n = source->mime_types.size / sizeof(char *);
	for (size_t i = 0; i < n; i++) {
		if (types[i]) out.push_back(types[i]);
	}
	return out;
}

bool offers(const std::vector<std::string> &types, const std::string &mime) {
	return std::find(types.begin(), types.end(), mime) != types.end();
}

int hex_value(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// "file:///home/me/My%20File" -> "/home/me/My File"; other URIs stay as they are.
std::string uri_to_path(const std::string &uri) {
	if (uri.rfind("file://", 0) != 0) {
		return uri;
	}
	std::string rest = uri.substr(7);
	const size_t slash = rest.find('/');
	rest = slash == std::string::npos ? "/" : rest.substr(slash);   // drop a host name
	std::string out;
	for (size_t i = 0; i < rest.size(); i++) {
		if (rest[i] == '%' && i + 2 < rest.size() && hex_value(rest[i + 1]) >= 0 && hex_value(rest[i + 2]) >= 0) {
			out += static_cast<char>(hex_value(rest[i + 1]) * 16 + hex_value(rest[i + 2]));
			i += 2;
		} else {
			out += rest[i];
		}
	}
	return out;
}

std::string path_to_uri(const std::string &path) {
	static const char *hex = "0123456789ABCDEF";
	std::string out = "file://";
	for (unsigned char c : path) {
		if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

} // namespace

std::vector<std::string> parse_uri_list(const std::string &text) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(start, end - start);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if (!line.empty() && line[0] != '#') {
			out.push_back(uri_to_path(line));
		}
		start = end + 1;
	}
	return out;
}

namespace {

bool blank(const std::string &s) {
	return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

std::string ago(std::time_t when) {
	const long d = static_cast<long>(std::time(nullptr) - when);
	if (d < 60) return "Just now";
	if (d < 3600) return std::to_string(d / 60) + " min ago";
	if (d < 86400) return std::to_string(d / 3600) + " h ago";
	return std::to_string(d / 86400) + " d ago";
}

std::string base_name(const std::string &path) {
	const size_t slash = path.find_last_of('/', path.size() > 1 ? path.size() - 2 : 0);
	std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	if (!name.empty() && name.back() == '/') name.pop_back();
	return name.empty() ? path : name;
}

// Writes `data` to an app's pipe a piece at a time, without blocking Dusk.
struct ClipWriter {
	wl_event_source *source = nullptr, *timer = nullptr;
	int fd = -1;
	std::shared_ptr<const std::string> data;
	size_t done = 0;

	void finish() {
		if (source) wl_event_source_remove(source);
		if (timer) wl_event_source_remove(timer);
		close(fd);
		delete this;
	}
	static int writable(int fd, uint32_t mask, void *p) {
		auto *w = static_cast<ClipWriter *>(p);
		if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
			w->finish();
			return 0;
		}
		while (w->done < w->data->size()) {
			const size_t chunk = std::min<size_t>(64 * 1024, w->data->size() - w->done);
			const ssize_t n = write(fd, w->data->data() + w->done, chunk);
			if (n > 0) {
				w->done += static_cast<size_t>(n);
			} else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
				return 0;   // the app's pipe is full: carry on when it has read some
			} else {
				break;      // it went away (EPIPE)
			}
		}
		w->finish();
		return 0;
	}
	static int timed_out(void *p) {
		static_cast<ClipWriter *>(p)->finish();
		return 0;
	}
};

void start_write(wl_event_loop *loop, int fd, std::shared_ptr<const std::string> data) {
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	auto *w = new ClipWriter;
	w->fd = fd;
	w->data = std::move(data);
	w->source = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE, &ClipWriter::writable, w);
	w->timer = wl_event_loop_add_timer(loop, &ClipWriter::timed_out, w);
	wl_event_source_timer_update(w->timer, 30000);
}

} // namespace

// Reads what an app offers on the clipboard, in the background.
struct ClipReader {
	Clipboard *owner = nullptr;
	int fd = -1;
	wl_event_source *source = nullptr, *timer = nullptr;
	std::string data;
	size_t limit = 0;
	bool failed = false;
	std::function<void(std::string)> done;

	void stop() {
		if (source) wl_event_source_remove(source);
		if (timer) wl_event_source_remove(timer);
		source = timer = nullptr;
		if (fd >= 0) close(fd);
		fd = -1;
	}
	static int readable(int fd, uint32_t mask, void *p) {
		auto *r = static_cast<ClipReader *>(p);
		char buffer[16384];
		for (;;) {
			const ssize_t n = read(fd, buffer, sizeof buffer);
			if (n > 0) {
				r->data.append(buffer, static_cast<size_t>(n));
				if (r->data.size() > r->limit) {
					r->failed = true;   // too big to keep
					break;
				}
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EINTR) && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
				return 0;   // more to come
			}
			break;   // the end
		}
		r->owner->reader_done(r);
		return 0;
	}
	static int timed_out(void *p) {
		auto *r = static_cast<ClipReader *>(p);
		r->failed = true;
		r->owner->reader_done(r);
		return 0;
	}
};

// Our own clipboard offer: an entry from the history, given to whichever
// app pastes.
struct ClipSource {
	wlr_data_source base;   // must stay the first member
	Clipboard *owner = nullptr;
	wl_event_loop *loop = nullptr;
	std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> offers;

	static void send(wlr_data_source *source, const char *mime, int32_t fd) {
		auto *self = reinterpret_cast<ClipSource *>(source);
		for (const auto &[type, bytes] : self->offers) {
			if (type == mime) {
				start_write(self->loop, fd, bytes);
				return;
			}
		}
		close(fd);
	}
	static void destroy(wlr_data_source *source) {
		auto *self = reinterpret_cast<ClipSource *>(source);
		if (self->owner) self->owner->source_gone(self);
		delete self;
	}
	static const wlr_data_source_impl *impl() {
		static const wlr_data_source_impl i = [] {
			wlr_data_source_impl x{};
			x.send = &ClipSource::send;
			x.destroy = &ClipSource::destroy;
			return x;
		}();
		return &i;
	}
	// Dragging: with dnd_finish, wlroots frees the source itself once the
	// drop is done (or cancelled).
	static void dnd_finish(wlr_data_source *) {}
	static const wlr_data_source_impl *drag_impl() {
		static const wlr_data_source_impl i = [] {
			wlr_data_source_impl x{};
			x.send = &ClipSource::send;
			x.destroy = &ClipSource::destroy;
			x.dnd_finish = &ClipSource::dnd_finish;
			return x;
		}();
		return &i;
	}
};

// ============================================================================
// Clipboard
// ============================================================================

Clipboard::Clipboard(Server *server) : server_(server) {
	load();
	read_timer_ = wl_event_loop_add_timer(server->loop, &Clipboard::handle_read_timer, this);
	on_set_selection_.connect<&Clipboard::handle_set_selection>(&server->seat->events.set_selection, this);
	restore_timer_ = wl_event_loop_add_timer(server->loop, &Clipboard::handle_restore, this);
	// Kept from last time: the newest is in the clipboard from the start.
	if (!history_.empty() && !server->seat->selection_source) {
		restore_newest();
	}
}

int Clipboard::handle_restore(void *data) {
	auto *self = static_cast<Clipboard *>(data);
	// Still empty (not an app clearing it on the way to its new copy).
	if (!self->server_->seat->selection_source) self->restore_newest();
	return 0;
}

void Clipboard::restore_newest() {
	if (history_.empty() || !server_->config.clipboard_history) return;
	current_ = history_.front().id;
	offer(history_.front(), false);
	notify();
}

Clipboard::~Clipboard() {
	on_set_selection_.disconnect();
	if (read_timer_) wl_event_source_remove(read_timer_);
	if (restore_timer_) wl_event_source_remove(restore_timer_);
	for (ClipReader *r : readers_) {
		r->stop();
		delete r;
	}
	readers_.clear();
	if (own_) {
		own_->owner = nullptr;   // it may outlive us (the seat still holds it)
		own_ = nullptr;
	}
	for (auto &[id, surface] : thumbs_) {
		cairo_surface_destroy(surface);
	}
}

void Clipboard::notify() {
	save();
	if (changed) changed();
}

namespace {
std::string clip_file() {
	const char *xdg = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	return (xdg && *xdg ? std::string(xdg) + "/dusk" : std::string(home ? home : "/tmp") + "/.local/state/dusk") +
		"/clipboard";
}
} // namespace

// Settings > Desktop & clipboard > "Keep after restarting" (on by default):
// text, files and the newest pictures are saved (only for you: 0600).
void Clipboard::save() {
	const std::string file = clip_file();
	if (!server_->config.clipboard_keep || !server_->config.clipboard_history) {
		if (saved_any_) {
			std::remove(file.c_str());
			saved_any_ = false;
		}
		return;
	}
	const std::string dir = file.substr(0, file.rfind('/'));
	for (size_t i = 1; i <= dir.size(); i++) {
		if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0700);
	}
	const std::string tmp = file + ".tmp";
	const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return;
	std::string out;
	int pictures = 0;
	for (const ClipEntry &e : history_) {
		if (!e.data) continue;
		if (e.kind == ClipEntry::Kind::Image) {
			// Pictures too (the newest few, up to 12 MB each).
			if (e.data->size() > 12 * 1024 * 1024 || ++pictures > 8) continue;
		} else if (e.data->size() > 256 * 1024) {
			continue;
		}
		out += std::string(e.kind == ClipEntry::Kind::Text ? "T " : e.kind == ClipEntry::Kind::Image ? "I " : e.cut ? "X " : "F ") +
			std::to_string(static_cast<long long>(e.time)) + " " + std::to_string(e.data->size()) + "\n" + *e.data + "\n";
	}
	const ssize_t wrote = write(fd, out.data(), out.size());
	close(fd);
	if (wrote == static_cast<ssize_t>(out.size())) {
		std::rename(tmp.c_str(), file.c_str());
		saved_any_ = true;
	} else {
		std::remove(tmp.c_str());
	}
}

void Clipboard::load() {
	if (!server_->config.clipboard_keep || !server_->config.clipboard_history) {
		return;
	}
	FILE *f = std::fopen(clip_file().c_str(), "rb");
	if (!f) return;
	saved_any_ = true;
	char head[96];
	while (std::fgets(head, sizeof head, f)) {
		char kind = 0;
		long long time = 0;
		size_t len = 0;
		if (std::sscanf(head, "%c %lld %zu", &kind, &time, &len) != 3 ||
				len > (kind == 'I' ? 12u * 1024 * 1024 : 256u * 1024)) break;
		std::string data(len, '\0');
		if (len && std::fread(data.data(), 1, len, f) != len) break;
		std::fgetc(f);   // the newline after it
		ClipEntry e;
		e.id = next_id_++;
		e.time = static_cast<std::time_t>(time);
		e.kind = kind == 'T' ? ClipEntry::Kind::Text : kind == 'I' ? ClipEntry::Kind::Image : ClipEntry::Kind::Files;
		e.cut = kind == 'X';
		if (e.kind == ClipEntry::Kind::Image) {
			cairo_surface_t *probe = load_image_bytes(data, 8, 8, &e.image_w, &e.image_h);
			if (!probe) continue;
			cairo_surface_destroy(probe);
		}
		if (e.kind == ClipEntry::Kind::Files) {
			std::string uris = data;
			if (uris.size() >= 4 && uris.compare(uris.size() - 4, 4, "#cut") == 0) uris.resize(uris.size() - 4);
			e.files = parse_uri_list(uris);
		}
		e.data = std::make_shared<const std::string>(std::move(data));
		history_.push_back(std::move(e));
	}
	std::fclose(f);
	trim();
}

void Clipboard::source_gone(ClipSource *source) {
	if (own_ == source) {
		own_ = nullptr;
	}
}

void Clipboard::reader_done(ClipReader *reader) {
	reader->stop();
	readers_.erase(std::remove(readers_.begin(), readers_.end(), reader), readers_.end());
	std::function<void(std::string)> done = std::move(reader->done);
	std::string data = std::move(reader->data);
	const bool failed = reader->failed;
	delete reader;
	if (done && !failed) {
		done(std::move(data));
	}
}

void Clipboard::read(wlr_data_source *source, const std::string &mime, size_t limit,
		std::function<void(std::string)> done) {
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		return;
	}
	// Only our end doesn't block; the app gets an ordinary pipe to write to.
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	auto *r = new ClipReader;
	r->owner = this;
	r->fd = fds[0];
	r->limit = limit;
	r->done = std::move(done);
	readers_.push_back(r);
	r->source = wl_event_loop_add_fd(server_->loop, fds[0], WL_EVENT_READABLE, &ClipReader::readable, r);
	r->timer = wl_event_loop_add_timer(server_->loop, &ClipReader::timed_out, r);
	wl_event_source_timer_update(r->timer, kTransferMs);
	// The app writes into the other end (wlroots closes our copy of it).
	wlr_data_source_send(source, mime.c_str(), fds[1]);
}

// Something new was copied: read it into the history.
void Clipboard::handle_set_selection(void *) {
	wlr_data_source *source = server_->seat->selection_source;
	const uint64_t gen = ++generation_;
	if (source && own_ && source == &own_->base) {
		return;   // ours: use() already marked which entry it is
	}
	current_ = 0;
	(void)gen;
	if (!source) {
		// Emptied (the app it came from closed): the newest entry is the
		// clipboard again, so pasting still gives the latest thing copied.
		// A moment later, and only if it's still empty: some apps clear it
		// just before setting their new copy.
		if (server_->config.clipboard_history && !history_.empty()) {
			wl_event_source_timer_update(restore_timer_, 600);
		}
		notify();
		return;
	}
	wl_event_source_timer_update(restore_timer_, 0);   // an app's new copy: nothing to put back
	notify();
	if (!server_->config.clipboard_history) {
		return;
	}
	// Read it into the history a moment later: right after copying you
	// usually paste, and the app shouldn't be busy handing its text to us
	// as well just then (pasting a long selection sometimes failed).
	wl_event_source_timer_update(read_timer_, 350);
}

int Clipboard::handle_read_timer(void *data) {
	static_cast<Clipboard *>(data)->read_selection();
	return 0;
}

void Clipboard::read_selection() {
	wlr_data_source *source = server_->seat->selection_source;
	const uint64_t gen = generation_;
	if (!source || (own_ && source == &own_->base) || !server_->config.clipboard_history) {
		return;
	}
	const std::vector<std::string> types = mime_types(source);
	// Password managers ask for their secrets not to be kept.
	if (offers(types, "x-kde-passwordManagerHint")) {
		return;
	}
	auto accept = [this, gen](ClipEntry e) {
		if (gen != generation_) {
			return;   // something newer was copied meanwhile
		}
		e.time = std::time(nullptr);
		add(std::move(e));
	};
	if (offers(types, "text/uri-list")) {
		read(source, "text/uri-list", kListLimit, [accept](std::string text) {
			ClipEntry e;
			e.kind = ClipEntry::Kind::Files;
			e.files = parse_uri_list(text);
			if (e.files.empty()) return;
			if (std::none_of(e.files.begin(), e.files.end(), [](const std::string &f) { return !f.empty() && f[0] == '/'; })) {
				// Only web links (a browser): keep them as text.
				std::string links;
				for (const std::string &f : e.files) links += (links.empty() ? "" : "\n") + f;
				e.kind = ClipEntry::Kind::Text;
				e.files.clear();
				e.data = std::make_shared<const std::string>(std::move(links));
			} else {
				e.data = std::make_shared<const std::string>(std::move(text));
			}
			accept(std::move(e));
		});
		return;
	}
	for (const char *image : {"image/png"}) {
		if (offers(types, image)) {
			read(source, image, kImageLimit, [accept](std::string bytes) {
				ClipEntry e;
				e.kind = ClipEntry::Kind::Image;
				cairo_surface_t *probe = load_image_bytes(bytes, 8, 8, &e.image_w, &e.image_h);
				if (!probe) return;   // not a picture after all
				cairo_surface_destroy(probe);
				e.data = std::make_shared<const std::string>(std::move(bytes));
				accept(std::move(e));
			});
			return;
		}
	}
	for (const char *text : kTextTypes) {
		if (offers(types, text)) {
			read(source, text, kTextLimit, [accept](std::string bytes) {
				if (bytes.empty() || blank(bytes)) return;
				ClipEntry e;
				e.kind = ClipEntry::Kind::Text;
				e.data = std::make_shared<const std::string>(std::move(bytes));
				accept(std::move(e));
			});
			return;
		}
	}
}

void Clipboard::add(ClipEntry entry) {
	// The same thing copied again moves to the top.
	for (auto it = history_.begin(); it != history_.end(); ++it) {
		if (it->kind == entry.kind && it->data && entry.data && *it->data == *entry.data) {
			entry.id = it->id;
			history_.erase(it);
			break;
		}
	}
	if (entry.id == 0) {
		entry.id = next_id_++;
	}
	current_ = entry.id;
	history_.push_front(std::move(entry));
	trim();
	notify();
}

void Clipboard::trim() {
	const size_t keep = static_cast<size_t>(std::max(1, server_->config.clipboard_size));
	while (history_.size() > keep) {
		const uint64_t id = history_.back().id;
		if (auto it = thumbs_.find(id); it != thumbs_.end()) {
			cairo_surface_destroy(it->second);
			thumbs_.erase(it);
		}
		history_.pop_back();
	}
}

void Clipboard::settings_changed() {
	save();
	if (!server_->config.clipboard_history) {
		if (!history_.empty()) clear();   // off means nothing is kept
		return;
	}
	const size_t before = history_.size();
	trim();
	if (history_.size() != before) notify();
}

// Builds our offer for an entry and makes it the clipboard.
void Clipboard::offer(const ClipEntry &e, bool fresh) {
	auto *s = new ClipSource;
	s->owner = this;
	s->loop = server_->loop;
	auto add_type = [s](const std::string &mime, std::shared_ptr<const std::string> bytes) {
		s->offers.push_back({mime, std::move(bytes)});
	};
	if (e.kind == ClipEntry::Kind::Text) {
		for (const char *t : kTextTypes) add_type(t, e.data);
	} else if (e.kind == ClipEntry::Kind::Image) {
		add_type("image/png", e.data);
	} else {
		std::string uris, plain;
		for (const std::string &f : e.files) {
			const std::string uri = f.empty() || f[0] != '/' ? f : path_to_uri(f);
			uris += uri + "\r\n";
			plain += (plain.empty() ? "" : "\n") + f;
		}
		std::string gnome = e.cut ? "cut" : "copy";
		for (const std::string &f : e.files) {
			gnome += "\n" + (f.empty() || f[0] != '/' ? f : path_to_uri(f));
		}
		add_type("text/uri-list", std::make_shared<const std::string>(uris));
		add_type("x-special/gnome-copied-files", std::make_shared<const std::string>(gnome));
		add_type("application/x-kde-cutselection", std::make_shared<const std::string>(e.cut ? "1" : "0"));
		auto text = std::make_shared<const std::string>(plain);
		for (const char *t : kTextTypes) add_type(t, text);
	}
	wlr_data_source_init(&s->base, ClipSource::impl());
	for (const auto &[type, bytes] : s->offers) {
		auto **slot = static_cast<char **>(wl_array_add(&s->base.mime_types, sizeof(char *)));
		if (slot) *slot = strdup(type.c_str());
	}
	if (own_) {
		own_->owner = nullptr;   // the old one is being replaced
	}
	own_ = s;
	wlr_seat_set_selection(server_->seat, &s->base,
		fresh ? wl_display_next_serial(server_->display) : server_->seat->selection_serial);
}

void Clipboard::use(uint64_t id) {
	auto it = std::find_if(history_.begin(), history_.end(), [id](const ClipEntry &e) { return e.id == id; });
	if (it == history_.end()) {
		return;
	}
	ClipEntry e = *it;
	history_.erase(it);
	history_.push_front(e);   // it's the newest again
	current_ = id;
	offer(e);
	notify();
}

void Clipboard::remove(uint64_t id) {
	history_.erase(std::remove_if(history_.begin(), history_.end(),
		[id](const ClipEntry &e) { return e.id == id; }), history_.end());
	if (auto it = thumbs_.find(id); it != thumbs_.end()) {
		cairo_surface_destroy(it->second);
		thumbs_.erase(it);
	}
	notify();
}

void Clipboard::clear() {
	history_.clear();
	for (auto &[id, surface] : thumbs_) {
		cairo_surface_destroy(surface);
	}
	thumbs_.clear();
	notify();
}

wlr_data_source *Clipboard::drag_source(const std::vector<std::string> &paths) {
	auto *s = new ClipSource;
	s->loop = server_->loop;
	std::string uris, plain, gnome = "copy";
	for (const std::string &f : paths) {
		uris += path_to_uri(f) + "\r\n";
		plain += (plain.empty() ? "" : "\n") + f;
		gnome += "\n" + path_to_uri(f);
	}
	s->offers.push_back({"text/uri-list", std::make_shared<const std::string>(uris)});
	s->offers.push_back({"x-special/gnome-copied-files", std::make_shared<const std::string>(gnome)});
	auto text = std::make_shared<const std::string>(plain);
	for (const char *t : kTextTypes) s->offers.push_back({t, text});
	wlr_data_source_init(&s->base, ClipSource::drag_impl());
	for (const auto &[type, bytes] : s->offers) {
		auto **slot = static_cast<char **>(wl_array_add(&s->base.mime_types, sizeof(char *)));
		if (slot) *slot = strdup(type.c_str());
	}
	// The app decides: move (the default in file managers) or copy (Ctrl).
	s->base.actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	return &s->base;
}

void Clipboard::copy_files(const std::vector<std::string> &paths, bool cut) {
	if (paths.empty()) {
		return;
	}
	ClipEntry e;
	e.kind = ClipEntry::Kind::Files;
	e.files = paths;
	e.cut = cut;
	e.time = std::time(nullptr);
	std::string uris;
	for (const std::string &p : paths) uris += path_to_uri(p) + "\r\n";
	e.data = std::make_shared<const std::string>(uris + (cut ? "#cut" : ""));
	++generation_;
	offer(e);
	if (server_->config.clipboard_history) {
		add(e);
	} else {
		notify();
	}
}

bool Clipboard::has_files() const {
	wlr_data_source *source = server_->seat->selection_source;
	return source && offers(mime_types(source), "text/uri-list");
}

void Clipboard::get_files(std::function<void(std::vector<std::string>, bool)> done) {
	wlr_data_source *source = server_->seat->selection_source;
	if (!source) {
		return;
	}
	if (own_ && source == &own_->base) {
		for (const ClipEntry &e : history_) {
			if (e.id == current_ && e.kind == ClipEntry::Kind::Files) {
				done(e.files, e.cut);
				return;
			}
		}
	}
	const std::vector<std::string> types = mime_types(source);
	if (offers(types, "x-special/gnome-copied-files")) {
		// "cut" or "copy", then one URI a line (GNOME's Files, and others).
		read(source, "x-special/gnome-copied-files", kListLimit, [done](std::string text) {
			const size_t nl = text.find('\n');
			const bool cut = text.substr(0, nl) == "cut";
			done(parse_uri_list(nl == std::string::npos ? "" : text.substr(nl + 1)), cut);
		});
	} else if (offers(types, "text/uri-list")) {
		const bool kde_cut = offers(types, "application/x-kde-cutselection");
		read(source, "text/uri-list", kListLimit, [this, done, kde_cut](std::string text) {
			if (!kde_cut) {
				done(parse_uri_list(text), false);
				return;
			}
			// Dolphin says whether it was cut separately.
			wlr_data_source *now = server_->seat->selection_source;
			if (!now) {
				done(parse_uri_list(text), false);
				return;
			}
			read(now, "application/x-kde-cutselection", 16, [done, text](std::string flag) {
				done(parse_uri_list(text), !flag.empty() && flag[0] == '1');
			});
		});
	}
}

cairo_surface_t *Clipboard::thumbnail(const ClipEntry &entry, int px) {
	if (entry.kind != ClipEntry::Kind::Image || !entry.data) {
		return nullptr;
	}
	if (auto it = thumbs_.find(entry.id); it != thumbs_.end()) {
		return it->second;
	}
	cairo_surface_t *t = load_image_bytes(*entry.data, px, px);
	if (t) {
		thumbs_[entry.id] = t;
	}
	return t;
}

// ============================================================================
// The card
// ============================================================================

namespace {
constexpr int kCardW = 380;
constexpr int kPad = 14;
constexpr int kHead = 40;
constexpr int kRowGap = 6;
constexpr int kMaxH = 500;
} // namespace

ClipboardCard::ClipboardCard(Server *server) : Popover(server, kCardW, 220) {}

int ClipboardCard::row_height(const ClipEntry &e) const {
	switch (e.kind) {
	case ClipEntry::Kind::Image: return 68;
	case ClipEntry::Kind::Files: return e.files.size() > 1 ? 62 : 54;
	default: return 62;
	}
}

void ClipboardCard::fit_size() {
	int h = kPad + kHead + kPad;
	const Clipboard *cb = server_->clipboard.get();
	if (!cb || cb->history().empty() || !server_->config.clipboard_history) {
		h += 150;
	} else {
		for (const ClipEntry &e : cb->history()) {
			h += row_height(e) + kRowGap;
			if (h > kMaxH) break;
		}
	}
	resize(kCardW, std::clamp(h, 200, kMaxH));
}

void ClipboardCard::update() {
	if (!is_open()) {
		return;
	}
	fit_size();
	refresh();
}

void ClipboardCard::on_open() {
	scroll_ = 0;
	sel_ = 0;   // the newest, whatever it is (text, a picture, files)
	fit_size();
}

void ClipboardCard::on_hover() {
	// Pointing at an entry picks it, so the keys carry on from there.
	if (const Clipboard *cb = server_->clipboard.get(); cb && hover_.rfind("row:", 0) == 0) {
		const uint64_t id = std::strtoull(hover_.c_str() + 4, nullptr, 10);
		int i = 0;
		for (const ClipEntry &e : cb->history()) {
			if (e.id == id) { sel_ = i; break; }
			i++;
		}
	}
	redraw();   // the hovered entry shows its remove button
}

void ClipboardCard::scroll_to_selected() {
	const Clipboard *cb = server_->clipboard.get();
	if (!cb) return;
	const int view_h = h_ - 2 * kPad - kHead;
	int y = 0, i = 0;
	for (const ClipEntry &e : cb->history()) {
		const int rh = row_height(e);
		if (i == sel_) {
			if (y < scroll_) scroll_ = y;
			else if (y + rh > scroll_ + view_h) scroll_ = y + rh - view_h;
			break;
		}
		y += rh + kRowGap;
		i++;
	}
	scroll_ = std::clamp(scroll_, 0, std::max(0, max_scroll_));
}

bool ClipboardCard::handle_key(xkb_keysym_t sym) {
	Clipboard *cb = server_->clipboard.get();
	if (!is_open() || !cb || cb->history().empty()) return false;
	const int n = static_cast<int>(cb->history().size());
	sel_ = std::clamp(sel_, 0, n - 1);
	auto picked = [&]() -> uint64_t {
		auto it = cb->history().begin();
		std::advance(it, sel_);
		return it->id;
	};
	switch (sym) {
	case XKB_KEY_Down:
	case XKB_KEY_Tab:
		sel_ = std::min(n - 1, sel_ + 1);
		break;
	case XKB_KEY_Up:
	case XKB_KEY_ISO_Left_Tab:
		sel_ = std::max(0, sel_ - 1);
		break;
	case XKB_KEY_Home:
		sel_ = 0;
		break;
	case XKB_KEY_End:
		sel_ = n - 1;
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		cb->use(picked());
		close();
		return true;
	case XKB_KEY_Delete:
	case XKB_KEY_BackSpace:
		cb->remove(picked());
		sel_ = std::min(sel_, static_cast<int>(cb->history().size()) - 1);
		return true;
	default:
		return false;
	}
	scroll_to_selected();
	redraw();
	return true;
}

void ClipboardCard::on_scroll(double delta) {
	const int before = scroll_;
	scroll_ = std::clamp(scroll_ + static_cast<int>(std::lround(delta * 3)), 0, max_scroll_);
	if (scroll_ != before) redraw();
}

void ClipboardCard::activate(const std::string &id, uint32_t button, double, double) {
	if (id == "tab:0" || id == "tab:1") {
		tab_ = id == "tab:1" ? 1 : 0;
		scroll_ = 0;
		fit_size();
		redraw();
		return;
	}
	Clipboard *cb = server_->clipboard.get();
	if (!cb) {
		return;
	}
	if (id == "clear") {
		cb->clear();
		return;
	}
	if (id == "enable") {
		server_->write_setting("clipboard_history", "true");
		return;
	}
	if (id == "settings") {
		close();
		server_->open_settings("desktop");
		return;
	}
	if (id.rfind("del:", 0) == 0) {
		cb->remove(std::strtoull(id.c_str() + 4, nullptr, 10));
		return;
	}
	if (id.rfind("row:", 0) == 0) {
		const uint64_t entry = std::strtoull(id.c_str() + 4, nullptr, 10);
		if (button == BTN_RIGHT || button == BTN_MIDDLE) {
			cb->remove(entry);
			return;
		}
		// It's the clipboard now: paste it anywhere (Ctrl+V, or right-click > Paste).
		cb->use(entry);
		close();
	}
}

void ClipboardCard::draw(cairo_t *cr, float scale) {
	Clipboard *cb = server_->clipboard.get();
	const bool on = server_->config.clipboard_history;
	const bool any = cb && !cb->history().empty();

	// Header: the title, then Clear and Settings. (Notification history is
	// in the control centre.)
	tab_ = 0;
	draw_text(cr, "Clipboard", kPad + 6, kPad, 160, 30, theme::font_bold, theme::text_dim);
	int bx = w_ - kPad;
	auto button = [&](const char *hot, const char *label) {
		const int bw = text_width(label, theme::font_small) + 24;
		bx -= bw;
		fill_rounded(cr, bx, kPad + 2, bw, 26, 13, theme::field_bg);
		draw_text(cr, label, bx, kPad + 2, bw, 26, theme::font_small, theme::text, Align::Center);
		add_hot(hot, bx, kPad + 2, bw, 26, 13);
		bx -= 6;
	};
	if (any) button("clear", "Clear");
	button("settings", "Settings");

	const wlr_box area{kPad, kPad + kHead, w_ - 2 * kPad, h_ - 2 * kPad - kHead};
	if (!on || !any) {
		const double cy = area.y + area.height / 2.0 - 30;
		draw_named_icon(cr, server_->icons, "clipboard", w_ / 2.0 - 14, cy - 14, 28,
			with_alpha(theme::text_dim, 0.7), scale);
		draw_text(cr, on ? "Nothing copied yet" : "Clipboard history is off", area.x, cy + 22, area.width, 22,
			theme::font, theme::text, Align::Center);
		draw_paragraph(cr, on ? "Copy something and it appears here. Pick it later to paste it anywhere."
			: "Dusk doesn't keep what you copy.", area.x + 24, cy + 46, area.width - 48, 2,
			theme::font_small, theme::text_dim);
		if (!on) {
			const int bw = 110;
			fill_rounded(cr, (w_ - bw) / 2.0, cy + 82, bw, 30, 15, theme::accent);
			draw_text(cr, "Turn it on", (w_ - bw) / 2.0, cy + 82, bw, 30, theme::font_small, theme::on_accent,
				Align::Center);
			add_hot("enable", (w_ - bw) / 2.0, cy + 82, bw, 30, 15);
		}
		max_scroll_ = 0;
		return;
	}

	cairo_save(cr);
	cairo_rectangle(cr, area.x - 2, area.y, area.width + 4, area.height);
	cairo_clip(cr);
	int y = area.y - scroll_;
	int total = 0;
	const double r = inner_radius(theme::ui_radius, kPad);
	int index = -1;
	for (const ClipEntry &e : cb->history()) {
		index++;
		const int rh = row_height(e);
		const std::string row = "row:" + std::to_string(e.id);
		const std::string del = "del:" + std::to_string(e.id);
		if (y + rh > area.y && y < area.y + area.height) {
			const bool now = e.id == cb->current();
			fill_rounded(cr, area.x, y, area.width, rh, r, now ? with_alpha(theme::accent, 0.16) : theme::card_bg);
			if (index == sel_) {
				// The picked one (the newest when it opens): Enter puts it in the clipboard.
				rounded_rect(cr, area.x + 1, y + 1, area.width - 2, rh - 2, std::max(1.0, r - 1));
				set_color(cr, theme::accent);
				cairo_set_line_width(cr, 2);
				cairo_stroke(cr);
			}
			if (now) {
				fill_rounded(cr, area.x + 3, y + 10, 3, rh - 20, 1.5, theme::accent);
			}
			const int tx = area.x + 14, tw = area.width - 28 - 30;
			std::string sub = ago(e.time);
			if (e.kind == ClipEntry::Kind::Text) {
				// The text itself, two lines at most, with the line breaks shown as spaces.
				std::string text = *e.data;
				if (text.size() > 600) text = text.substr(0, 600);
				std::replace(text.begin(), text.end(), '\n', ' ');
				std::replace(text.begin(), text.end(), '\t', ' ');
				draw_paragraph(cr, text, tx, y + 9, tw, 2, theme::font, theme::text);
				const size_t lines = std::count(e.data->begin(), e.data->end(), '\n');
				if (lines > 0) sub += "  ·  " + std::to_string(lines + 1) + " lines";
			} else if (e.kind == ClipEntry::Kind::Image) {
				const int th = rh - 16, tw2 = th * 4 / 3;
				if (cairo_surface_t *t = cb->thumbnail(e, static_cast<int>(tw2 * scale))) {
					const double iw = cairo_image_surface_get_width(t), ih = cairo_image_surface_get_height(t);
					const double s = std::min(tw2 / iw, th / ih);
					cairo_save(cr);
					rounded_rect(cr, tx, y + 8, tw2, th, 8);
					cairo_clip(cr);
					set_color(cr, theme::field_bg);
					cairo_paint(cr);
					cairo_translate(cr, tx + (tw2 - iw * s) / 2, y + 8 + (th - ih * s) / 2);
					cairo_scale(cr, s, s);
					cairo_set_source_surface(cr, t, 0, 0);
					cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
					cairo_paint(cr);
					cairo_restore(cr);
				}
				draw_text(cr, "Picture", tx + tw2 + 12, y + 12, tw - tw2 - 12, 20, theme::font_bold, theme::text);
				sub = std::to_string(e.image_w) + " × " + std::to_string(e.image_h) + "  ·  " + sub;
				draw_text(cr, sub, tx + tw2 + 12, y + 34, tw - tw2 - 12, 18, theme::font_small,
					now ? theme::accent : theme::text_dim);
				sub.clear();
			} else {
				const bool one = e.files.size() == 1;
				draw_named_icon(cr, server_->icons, one ? "file" : "folder", tx, y + 12, 18, theme::text_dim, scale);
				std::string names;
				for (size_t i = 0; i < e.files.size() && i < 4; i++) {
					names += (i ? ", " : "") + base_name(e.files[i]);
				}
				if (e.files.size() > 4) names += " and " + std::to_string(e.files.size() - 4) + " more";
				draw_paragraph(cr, names, tx + 28, y + 10, tw - 28, one ? 1 : 2, theme::font, theme::text);
				sub = (one ? std::string("File") : std::to_string(e.files.size()) + " files") +
					(e.cut ? " (cut)" : "") + "  ·  " + sub;
				draw_text(cr, sub, tx + 28, y + rh - 24, tw - 28, 18, theme::font_small,
					now ? theme::accent : theme::text_dim);
				sub.clear();
			}
			if (!sub.empty()) {
				draw_text(cr, now ? "In the clipboard now  ·  " + sub : sub, tx, y + rh - 24, tw, 18,
					theme::font_small, now ? theme::accent : theme::text_dim);
			}
			const int top = std::max(y, area.y), bottom = std::min(y + rh, area.y + area.height);
			add_hot(row, area.x, top, area.width, bottom - top, r);
			// Remove: shown while pointed at.
			if ((hover_ == row || hover_ == del) && y + 8 >= area.y) {
				const double cx = area.x + area.width - 22, cy = y + 20;
				cairo_new_path(cr);
				cairo_arc(cr, cx, cy, 12, 0, 2 * M_PI);
				set_color(cr, theme::tile_off);
				cairo_fill(cr);
				draw_named_icon(cr, server_->icons, "close", cx - 6, cy - 6, 12, theme::text, scale);
				add_hot(del, cx - 12, cy - 12, 24, 24, 12);
			}
		}
		y += rh + kRowGap;
		total += rh + kRowGap;
	}
	cairo_restore(cr);
	max_scroll_ = std::max(0, total - kRowGap - area.height);
	scroll_ = std::clamp(scroll_, 0, max_scroll_);
}

} // namespace dusk
