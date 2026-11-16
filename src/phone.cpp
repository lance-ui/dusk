// A phone or tablet as a second screen: Dusk makes a screen with no display
// behind it (a "headless" output, arranged like any other in Settings >
// Displays) and wayvnc shows it over VNC, so a VNC app on the phone can see
// and use it: over Wi-Fi (password protected), or over USB through adb.
// The other way round (the phone's screen on the computer) is scrcpy,
// started from Settings.
#include "server.hpp"

#include <algorithm>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <fstream>

#include "notify.hpp"

namespace dusk {

namespace {

std::string runtime_file(const char *name) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	return std::string(dir ? dir : "/tmp") + "/dusk/" + name;
}

std::string config_dir() {
	return Config::directory() + "/wayvnc";
}

void kill_from_pidfile(const std::string &file) {
	std::ifstream in(file);
	long pid = 0;
	if (in >> pid && pid > 1) {
		kill(static_cast<pid_t>(pid), SIGTERM);
	}
	std::remove(file.c_str());
}

} // namespace

void Server::stop_phone_screen() {
	kill_from_pidfile(runtime_file("wayvnc.pid"));
	if (phone_output) {
		wlr_output *o = phone_output;
		phone_output = nullptr;
		wlr_output_destroy(o);   // its windows move to another screen
	}
	std::remove(runtime_file("phone").c_str());
}

void Server::sync_phone_screen() {
	const Config &c = config;
	const std::string want = c.phone_screen && headless
		? c.phone_size + "|" + c.phone_link + "|" + c.phone_password + "|" + std::to_string(c.phone_port) : "";
	if (want == phone_applied) {
		return;
	}
	stop_phone_screen();
	phone_applied = want;
	if (want.empty()) {
		return;
	}
	int w = 2400, h = 1080;
	if (std::sscanf(c.phone_size.c_str(), "%dx%d", &w, &h) != 2) {
		w = 2400;
		h = 1080;
	}
	w = std::clamp(w, 320, 7680);
	h = std::clamp(h, 320, 7680);
	phone_output = wlr_headless_add_output(headless, static_cast<unsigned>(w), static_cast<unsigned>(h));
	if (!phone_output) {
		return;
	}
	const std::string name = phone_output->name ? phone_output->name : "HEADLESS-1";

	// wayvnc's settings: over Wi-Fi only with a password (RSA-AES, which
	// RealVNC Viewer, AVNC and bVNC speak); over USB only on this computer.
	const bool usb = c.phone_link == "usb";
	const std::string dir = config_dir();
	mkdir(Config::directory().c_str(), 0700);
	mkdir(dir.c_str(), 0700);
	const std::string conf = dir + "/config", key = dir + "/rsa_key.pem";
	{
		// Created with its permissions from the start: the config holds the
		// VNC password, and a 0644 window before chmod() would leak it.
		const int fd = ::open(conf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) {
			std::string text = "address=" + std::string(usb ? "127.0.0.1" : "0.0.0.0") + "\n";
			text += "port=" + std::to_string(c.phone_port) + "\n";
			if (!usb && !c.phone_password.empty()) {
				text += "enable_auth=true\nusername=dusk\npassword=" + c.phone_password + "\n";
				text += "rsa_private_key_file=" + key + "\n";
			}
			ssize_t ignored = write(fd, text.data(), text.size());
			(void)ignored;
			::close(fd);
		}
	}
	chmod(conf.c_str(), 0600);
	if (!usb && c.phone_password.empty()) {
		// Never open to the whole network without a password.
		if (notifications) {
			notifications->post("Phone screen needs a password",
				"Set one in Settings > Phone & tablet to use it over Wi-Fi, or connect with USB.",
				"dialog-warning-symbolic");
		}
		return;
	}
	mkdir(runtime_file("").c_str(), 0700);
	std::string cmd = "command -v wayvnc >/dev/null || exit 0; ";
	if (!usb) {
		cmd += "[ -f " + shell_quote(key) + " ] || ssh-keygen -q -m pem -t rsa -b 2048 -N '' -f " + shell_quote(key) +
			" >/dev/null 2>&1; ";
	}
	cmd += "echo $$ > " + shell_quote(runtime_file("wayvnc.pid")) + "; exec wayvnc --config=" + shell_quote(conf) +
		" --output=" + shell_quote(name);
	spawn(cmd);
	if (usb) {
		// The phone reaches it as its own localhost:port.
		spawn("command -v adb >/dev/null && adb reverse tcp:" + std::to_string(c.phone_port) + " tcp:" +
			std::to_string(c.phone_port) + " >/dev/null 2>&1");
	}
	// For Settings: what to type into the phone's VNC app.
	std::ofstream info(runtime_file("phone"), std::ios::trunc);
	info << "output=" << name << "\nport=" << c.phone_port << "\nlink=" << (usb ? "usb" : "wifi") << "\n";
}

} // namespace dusk
