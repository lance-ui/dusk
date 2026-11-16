#include "crash.hpp"

#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace dusk {

namespace {

constexpr int kCrumbs = 24, kCrumbLen = 160;
char crumbs[kCrumbs][kCrumbLen];
std::atomic<int> next_crumb{0};
int crash_fd = -1;

void put(const char *s) {
	if (crash_fd >= 0) {
		ssize_t ignored = write(crash_fd, s, std::strlen(s));
		(void)ignored;
	}
}

// Only async-signal-safe work from here on: strsignal can lock, so the name
// comes from a static table instead.
const char *signal_name(int sig) {
	switch (sig) {
	case SIGINT: return "SIGINT";
	case SIGILL: return "SIGILL";
	case SIGABRT: return "SIGABRT";
	case SIGFPE: return "SIGFPE";
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS: return "SIGBUS";
	case SIGTERM: return "SIGTERM";
	case SIGTRAP: return "SIGTRAP";
	default: return "signal";
	}
}

void on_crash(int sig) {
	char line[160];
	std::snprintf(line, sizeof line, "\n=== Dusk crashed (signal %d: %s) at %ld\n", sig, signal_name(sig),
		static_cast<long>(std::time(nullptr)));
	put(line);
	put("--- what it was doing (oldest first)\n");
	const int n = next_crumb.load();
	for (int i = std::max(0, n - kCrumbs); i < n; i++) {
		put("  ");
		put(crumbs[i % kCrumbs]);
		put("\n");
	}
	put("--- where (run: addr2line -Cfe $(command -v dusk) <address> for a line number)\n");
	void *frames[64];
	const int count = backtrace(frames, 64);
	if (crash_fd >= 0) backtrace_symbols_fd(frames, count, crash_fd);
	fsync(crash_fd);
	signal(sig, SIG_DFL);
	raise(sig);
}

std::string state_dir() {
	const char *xdg = std::getenv("XDG_STATE_HOME");
	const char *home = std::getenv("HOME");
	const std::string dir = xdg && *xdg ? std::string(xdg) + "/dusk" : std::string(home ? home : "/tmp") + "/.local/state/dusk";
	for (size_t i = 1; i <= dir.size(); i++) {
		if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0700);
	}
	return dir;
}

} // namespace

void crumb(const char *fmt, ...) {
	const int i = next_crumb.fetch_add(1) % kCrumbs;
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	const int head = static_cast<int>(std::strftime(crumbs[i], kCrumbLen, "%H:%M:%S ", &local));
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(crumbs[i] + head, kCrumbLen - head, fmt, ap);
	va_end(ap);
}

void install_crash_log() {
	const std::string dir = state_dir();
	// Everything Dusk (and wlroots) prints: this session's, and the last one's.
	const std::string log = dir + "/dusk.log";
	std::rename(log.c_str(), (log + ".old").c_str());
	const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd >= 0) {
		// Still to the journal too, through a tee.
		int pipefd[2];
		if (pipe(pipefd) == 0) {
			const pid_t pid = fork();
			if (pid == 0) {
				close(pipefd[1]);
				char buf[4096];
				ssize_t n;
				while ((n = read(pipefd[0], buf, sizeof buf)) > 0) {
					ssize_t a = write(fd, buf, static_cast<size_t>(n));
					ssize_t b = write(STDERR_FILENO, buf, static_cast<size_t>(n));
					(void)a;
					(void)b;
				}
				_exit(0);
			}
			close(pipefd[0]);
			if (pid > 0) {
				dup2(pipefd[1], STDERR_FILENO);
			}
			close(pipefd[1]);
		}
		close(fd);
	}
	crash_fd = open((dir + "/crash.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	static char alt_stack[64 * 1024];
	stack_t ss{};
	ss.ss_sp = alt_stack;
	ss.ss_size = sizeof alt_stack;
	sigaltstack(&ss, nullptr);
	struct sigaction sa{};
	sa.sa_handler = on_crash;
	sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
		sigaction(sig, &sa, nullptr);
	}
	crumb("Dusk started");
}

} // namespace dusk
