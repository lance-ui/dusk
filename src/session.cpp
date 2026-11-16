// Things a desktop session starts besides the compositor: the keyring
// (saved passwords), a password prompt for admin actions (polkit), and the
// apps set to start at login (XDG autostart).
#include "session.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "server.hpp"

namespace dusk {

namespace {

bool executable(const std::string &path) {
	return access(path.c_str(), X_OK) == 0;
}

// Is `name` a program somewhere on PATH (or an absolute path that exists)?
bool on_path(const std::string &name) {
	if (name.empty()) return false;
	if (name.find('/') != std::string::npos) return executable(name);
	const char *path = std::getenv("PATH");
	std::stringstream ss(path ? path : "/usr/local/bin:/usr/bin:/bin");
	std::string dir;
	while (std::getline(ss, dir, ':')) {
		if (!dir.empty() && executable(dir + "/" + name)) return true;
	}
	return false;
}

std::vector<std::string> split(const std::string &s, char sep) {
	std::vector<std::string> out;
	std::stringstream ss(s);
	std::string part;
	while (std::getline(ss, part, sep)) {
		if (!part.empty()) out.push_back(part);
	}
	return out;
}

std::string config_home() {
	const char *x = std::getenv("XDG_CONFIG_HOME");
	if (x && *x) return x;
	const char *home = std::getenv("HOME");
	return std::string(home ? home : "") + "/.config";
}

// Exec= without the %f, %U... placeholders (nothing to open at login).
std::string strip_field_codes(const std::string &exec) {
	std::string out;
	for (size_t i = 0; i < exec.size(); i++) {
		if (exec[i] == '%' && i + 1 < exec.size()) {
			if (exec[i + 1] == '%') out += '%';
			i++;
			continue;
		}
		out += exec[i];
	}
	return out;
}

struct AutostartEntry {
	std::string exec;
	bool run = false;
};

AutostartEntry read_autostart(const std::string &path) {
	AutostartEntry e;
	std::ifstream f(path);
	std::string line;
	bool in_entry = false, hidden = false, disabled = false, wrong_desktop = false;
	std::string try_exec, type = "Application";
	while (std::getline(f, line)) {
		if (!line.empty() && line[0] == '[') {
			in_entry = line == "[Desktop Entry]";
			continue;
		}
		if (!in_entry) continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		while (!k.empty() && k.back() == ' ') k.pop_back();
		while (!v.empty() && v.front() == ' ') v.erase(0, 1);
		if (k == "Exec") e.exec = v;
		else if (k == "TryExec") try_exec = v;
		else if (k == "Type") type = v;
		else if (k == "Hidden") hidden = v == "true";
		else if (k == "X-GNOME-Autostart-enabled") disabled = v == "false";
		else if (k == "OnlyShowIn") {
			// Entries for GNOME, KDE... only run there (their helpers
			// expect that desktop's own services).
			const auto list = split(v, ';');
			wrong_desktop = std::find(list.begin(), list.end(), "Dusk") == list.end();
		} else if (k == "NotShowIn") {
			const auto list = split(v, ';');
			wrong_desktop = wrong_desktop || std::find(list.begin(), list.end(), "Dusk") != list.end();
		}
	}
	e.exec = strip_field_codes(e.exec);
	e.run = type == "Application" && !hidden && !disabled && !wrong_desktop && !e.exec.empty() &&
		(try_exec.empty() || on_path(try_exec));
	return e;
}

// Lines like "SSH_AUTH_SOCK=/run/user/1000/keyring/ssh" go into our
// environment, so apps we start find the keyring.
void export_lines(const std::string &out, std::vector<std::string> *names) {
	for (const std::string &line : split(out, '\n')) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos || eq == 0) continue;
		const std::string name = line.substr(0, eq);
		setenv(name.c_str(), line.substr(eq + 1).c_str(), 1);
		names->push_back(name);
	}
}

} // namespace

namespace {
// Does the login screen unlock this PAM module? (/etc/pam.d/gdm-password or
// what it includes.)
bool login_unlocks(const char *module) {
	for (const char *f : {"/etc/pam.d/gdm-password", "/etc/pam.d/password-auth", "/etc/pam.d/system-auth",
			"/etc/pam.d/postlogin"}) {
		std::ifstream in(f);
		std::string line;
		while (std::getline(in, line)) {
			const size_t hash = line.find('#');
			if (hash != std::string::npos) line.resize(hash);
			if (line.find(module) != std::string::npos) return true;
		}
	}
	return false;
}
} // namespace

void start_keyring(Server &server) {
	// Fedora 45 replaced GNOME Keyring with oo7: the login screen unlocks
	// oo7-daemon (pam_oo7), which also took over the old keyrings. Starting
	// gnome-keyring then put a second, locked keyring in front of it, and
	// apps (Vivaldi, Chrome, VS Code) asked for its password every login.
	const bool oo7 = executable("/usr/libexec/oo7-daemon") &&
		(login_unlocks("pam_oo7") || !login_unlocks("pam_gnome_keyring"));
	if (oo7) {
		server.spawn("systemctl --user stop gnome-keyring-daemon.service gnome-keyring-daemon.socket 2>/dev/null; "
			"systemctl --user start oo7-daemon.service 2>/dev/null || /usr/libexec/oo7-daemon 2>/dev/null");
		wlr_log(WLR_INFO, "Saved passwords: oo7 (unlocked by the login screen)");
		return;
	}
	if (!on_path("gnome-keyring-daemon")) {
		wlr_log(WLR_INFO, "gnome-keyring isn't installed: apps can't keep saved passwords");
		return;
	}
	// The login screen already started the daemon and unlocked it with
	// your password; --start connects to that one and sets it up for this
	// session (on GNOME a GNOME-only autostart entry does this). Without it
	// apps start a second, locked keyring and ask for the password.
	std::string out;
	if (FILE *pipe = popen("gnome-keyring-daemon --start --components=secrets,pkcs11 2>/dev/null", "r")) {
		char buf[512];
		while (fgets(buf, sizeof buf, pipe)) out += buf;
		pclose(pipe);
	}
	std::vector<std::string> names;
	export_lines(out, &names);
	if (!names.empty()) {
		std::string cmd = "dbus-update-activation-environment --systemd";
		for (const std::string &n : names) cmd += " " + n;
		server.spawn(cmd + " 2>/dev/null");
	}
}

void start_polkit_agent(Server &server) {
	// The window that asks for your password when an app needs admin
	// rights. Dusk uses whichever one is installed.
	static const char *kAgents[] = {
		"/usr/libexec/polkit-gnome-authentication-agent-1",
		"/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1",
		"/usr/libexec/polkit-mate-authentication-agent-1",
		"/usr/libexec/hyprpolkitagent",
		"/usr/libexec/kf6/polkit-kde-authentication-agent-1",
		"/usr/libexec/xfce-polkit",
		"/usr/bin/lxqt-policykit-agent",
		"/usr/bin/lxpolkit",
	};
	for (const char *agent : kAgents) {
		if (executable(agent)) {
			server.spawn(agent);
			return;
		}
	}
	wlr_log(WLR_INFO, "No polkit agent installed: admin password prompts won't appear "
		"(install polkit-gnome or mate-polkit)");
}

std::vector<AutostartApp> list_autostart() {
	// The user's folder wins over the system's for the same file name.
	std::vector<std::string> dirs = {config_home() + "/autostart"};
	const char *xdg = std::getenv("XDG_CONFIG_DIRS");
	for (const std::string &d : split(xdg && *xdg ? xdg : "/etc/xdg", ':')) {
		dirs.push_back(d + "/autostart");
	}
	std::set<std::string> seen;
	std::vector<AutostartApp> apps;
	for (const std::string &dir : dirs) {
		DIR *d = opendir(dir.c_str());
		if (!d) continue;
		std::vector<std::string> names;
		while (dirent *ent = readdir(d)) {
			const std::string n = ent->d_name;
			if (n.size() > 8 && n.compare(n.size() - 8, 8, ".desktop") == 0) names.push_back(n);
		}
		closedir(d);
		std::sort(names.begin(), names.end());
		for (const std::string &n : names) {
			if (!seen.insert(n).second) continue;
			const AutostartEntry e = read_autostart(dir + "/" + n);
			apps.push_back(AutostartApp{n, e.exec, e.run});
		}
	}
	return apps;
}

void run_autostart(Server &server) {
	for (const AutostartApp &app : list_autostart()) {
		if (app.run) {
			wlr_log(WLR_INFO, "Starting %s", app.file.c_str());
			server.spawn(app.exec);
		}
	}
}

} // namespace dusk
