#include "glyphs.hpp"

#include "lucide.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dusk {

namespace {

// Theme icon names to try for each of our names (only used when Settings >
// Appearance > Icons is set to the icon theme's).
std::vector<std::string> theme_names(const std::string &name) {
	auto starts = [&](const char *p) { return name.rfind(p, 0) == 0; };
	if (starts("wifi-")) {
		static const char *levels[] = {"none", "weak", "ok", "good", "excellent"};
		const int l = std::clamp(name.back() - '0', 0, 4);
		return {std::string("network-wireless-signal-") + levels[l] + "-symbolic",
			"network-wireless-symbolic"};
	}
	if (name == "wired") return {"network-wired-symbolic"};
	if (name == "offline") return {"network-wireless-offline-symbolic", "network-offline-symbolic"};
	if (name == "airplane") return {"airplane-mode-symbolic", "flightmode-on-symbolic"};
	if (name == "speaker-muted") return {"audio-volume-muted-symbolic"};
	if (starts("speaker-")) {
		static const char *levels[] = {"muted", "low", "medium", "high"};
		const int l = std::clamp(name.back() - '0', 0, 3);
		return {std::string("audio-volume-") + levels[l] + "-symbolic"};
	}
	if (starts("battery-")) {
		// battery-<n> or battery-<n>-charging, n rounded to tens
		const bool charging = name.find("charging") != std::string::npos;
		const int n = std::atoi(name.c_str() + 8);
		const int tens = std::clamp((n + 5) / 10 * 10, 0, 100);
		const std::string level = "battery-level-" + std::to_string(tens);
		return {level + (charging ? (tens == 100 ? "-charged-symbolic" : "-charging-symbolic")
			: "-symbolic"), charging ? "battery-good-charging-symbolic" : "battery-good-symbolic"};
	}
	if (name == "bluetooth") return {"bluetooth-active-symbolic", "bluetooth-symbolic"};
	if (name == "bluetooth-off") return {"bluetooth-disabled-symbolic", "bluetooth-symbolic"};
	if (name == "moon") return {"weather-clear-night-symbolic", "dark-mode-symbolic"};
	if (name == "sun") return {"display-brightness-symbolic", "weather-clear-symbolic"};
	if (name == "nightlight") return {"night-light-symbolic", "weather-clear-night-symbolic"};
	if (name == "bell") return {"preferences-system-notifications-symbolic",
		"notifications-symbolic"};
	if (name == "bell-off") return {"notifications-disabled-symbolic"};
	if (name == "coffee") return {"caffeine-cup-full-symbolic", "my-caffeine-on-symbolic"};
	if (name == "gauge-power-saver") return {"power-profile-power-saver-symbolic"};
	if (name == "gauge-performance") return {"power-profile-performance-symbolic"};
	if (starts("gauge")) return {"power-profile-balanced-symbolic"};
	if (name == "mic") return {"microphone-sensitivity-high-symbolic", "audio-input-microphone-symbolic"};
	if (name == "mic-off") return {"microphone-disabled-symbolic", "microphone-sensitivity-muted-symbolic"};
	if (name == "gear") return {"emblem-system-symbolic", "preferences-system-symbolic"};
	if (name == "power") return {"system-shutdown-symbolic"};
	if (name == "play") return {"media-playback-start-symbolic"};
	if (name == "pause") return {"media-playback-pause-symbolic"};
	if (name == "next") return {"media-skip-forward-symbolic"};
	if (name == "previous") return {"media-skip-backward-symbolic"};
	if (name == "chevron-right") return {"go-next-symbolic"};
	if (name == "chevron-left") return {"go-previous-symbolic"};
	if (name == "lock") return {"system-lock-screen-symbolic", "changes-prevent-symbolic"};
	if (name == "check") return {"object-select-symbolic"};
	if (name == "close") return {"window-close-symbolic"};
	if (name == "desktop") return {"user-desktop-symbolic", "computer-symbolic"};
	if (name == "logout") return {"system-log-out-symbolic"};
	if (name == "restart") return {"system-reboot-symbolic", "view-refresh-symbolic"};
	if (name == "suspend") return {"weather-clear-night-symbolic", "media-playback-pause-symbolic"};
	if (name == "refresh") return {"view-refresh-symbolic"};
	if (name == "edit") return {"document-edit-symbolic", "edit-symbolic"};
	if (name == "screenshot") return {"camera-photo-symbolic", "applets-screenshooter-symbolic"};
	if (name == "plus") return {"list-add-symbolic"};
	if (name == "minus") return {"list-remove-symbolic"};
	if (name == "speaker-on") return {"audio-speakers-symbolic", "audio-volume-high-symbolic"};
	if (name == "clipboard" || name == "paste") return {"edit-paste-symbolic"};
	if (name == "copy") return {"edit-copy-symbolic"};
	if (name == "trash") return {"user-trash-symbolic"};
	if (name == "folder") return {"folder-symbolic"};
	if (name == "folder-new") return {"folder-new-symbolic"};
	if (name == "file") return {"text-x-generic-symbolic"};
	if (name == "terminal") return {"utilities-terminal-symbolic"};
	if (name == "open") return {"document-open-symbolic"};
	if (name == "info") return {"help-about-symbolic"};
	if (name == "image") return {"image-x-generic-symbolic"};
	if (name == "apps") return {"view-app-grid-symbolic"};
	if (name == "search") return {"system-search-symbolic"};
	if (name == "area") return {"selection-mode-symbolic", "edit-select-all-symbolic"};
	if (name == "window") return {"window-symbolic", "focus-windows-symbolic"};
	if (name == "pointer") return {"input-mouse-symbolic"};
	if (name == "timer") return {"timer-symbolic", "alarm-symbolic"};
	return {};
}

} // namespace

std::string wifi_icon(int signal) {
	return "wifi-" + std::to_string(std::clamp((signal + 12) / 25, 0, 4));
}

std::string speaker_icon(int volume, bool muted) {
	if (muted || volume <= 0) {
		return "speaker-muted";
	}
	return "speaker-" + std::to_string(volume < 34 ? 1 : volume < 67 ? 2 : 3);
}

std::string battery_icon(int percent, bool charging) {
	return "battery-" + std::to_string(percent) + (charging ? "-charging" : "");
}

void draw_named_icon(cairo_t *cr, IconLoader &icons, const std::string &name,
		double x, double y, double size, const Color &color, float scale) {
	if (!theme::own_icons) {
		for (const std::string &n : theme_names(name)) {
			if (draw_symbolic(cr, icons, {n.c_str()}, x, y, size, color, scale)) {
				return;
			}
		}
	}
	draw_glyph(cr, name, x + size / 2, y + size / 2, size, color);
}

// Dusk's own icons are Lucide's (https://lucide.dev): one even line weight,
// round ends, outlines. Our names map onto theirs here; levels (Wi-Fi,
// battery) and solid media buttons are added on top. Any Lucide name in
// lucide_icons.hpp works as it is too.
namespace {

const char *lucide_name(const std::string &name) {
	static const std::pair<const char *, const char *> kMap[] = {
		{"offline", "wifi-off"}, {"wired", "ethernet-port"}, {"airplane", "plane"},
		{"speaker-muted", "volume-x"}, {"speaker-on", "volume-2"}, {"speaker-0", "volume-x"},
		{"speaker-1", "volume"}, {"speaker-2", "volume-1"}, {"speaker-3", "volume-2"},
		{"moon", "sun-moon"}, {"suspend", "moon"}, {"nightlight", "moon-star"},
		{"gauge-power-saver", "leaf"}, {"gauge-performance", "zap"}, {"gauge-balanced", "gauge"},
		{"gear", "settings"}, {"next", "skip-forward"}, {"previous", "skip-back"},
		{"close", "x"}, {"desktop", "monitor"}, {"display", "monitor"}, {"logout", "log-out"},
		{"restart", "rotate-cw"}, {"refresh", "refresh-cw"}, {"edit", "pencil"}, {"screenshot", "scan"},
		{"area", "square-dashed"}, {"window", "app-window"}, {"pointer", "mouse-pointer-2"},
		{"paste", "clipboard-paste"}, {"folder-new", "folder-plus"}, {"terminal", "square-terminal"},
		{"open", "external-link"}, {"apps", "layout-grid"}, {"history", "rotate-ccw-clock"},
		{"phone", "smartphone"}, {"tray", "layers"},
	};
	for (const auto &[ours, theirs] : kMap) {
		if (name == ours) return theirs;
	}
	if (name.rfind("gauge", 0) == 0) return "gauge";
	return nullptr;
}

} // namespace

void draw_glyph(cairo_t *cr, const std::string &name, double cx, double cy, double size,
		const Color &color) {
	cairo_save(cr);
	// Lucide's 24 px grid has a 2 px margin: draw it a little larger so its
	// shapes match the size of the old 16 px ones.
	const double s = size * 1.18;
	const double x = cx - s / 2, y = cy - s / 2;
	auto starts = [&](const char *p) { return name.rfind(p, 0) == 0; };
	auto paint = [&](const char *n, double alpha, bool fill = false) {
		cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a * alpha);
		lucide::draw(cr, n, x, y, s, 2.0, fill);
	};

	if (starts("wifi-")) {
		// Signal: the full fan faint, the lit part over it.
		static const char *kLevels[] = {"wifi-zero", "wifi-low", "wifi-low", "wifi-high", "wifi"};
		const int level = std::clamp(name.back() - '0', 0, 4);
		if (level < 4) paint("wifi", 0.3);
		paint(kLevels[level], 1.0);
	} else if (starts("battery-")) {
		const int n = std::clamp(std::atoi(name.c_str() + 8), 0, 100);
		const bool charging = name.find("charging") != std::string::npos;
		if (charging) {
			paint("battery-charging", 1.0);
		} else {
			paint("battery", 1.0);
			// The level inside the cell (Lucide's is x 2..18, y 6..18).
			const double k = s / 24.0;
			const double w = 12.0 * n / 100.0;
			if (w > 0.3) {
				if (n <= 15) cairo_set_source_rgba(cr, 0.90, 0.28, 0.30, color.a);
				else cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
				rounded_rect(cr, x + 4 * k, y + 8 * k, std::max(1.2, w) * k, 8 * k, 1 * k);
				cairo_fill(cr);
			}
		}
	} else if (name == "play" || name == "pause" || name == "next" || name == "previous") {
		paint(lucide_name(name) ? lucide_name(name) : name.c_str(), 1.0, true);   // solid, like media keys
	} else if (const char *mapped = lucide_name(name)) {
		paint(mapped, 1.0);
	} else if (lucide::exists(name.c_str())) {
		paint(name.c_str(), 1.0);
	} else {
		paint("circle-dot", 1.0);
	}
	cairo_restore(cr);
}

} // namespace dusk
