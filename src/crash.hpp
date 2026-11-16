// When Dusk crashes, what it was doing ends up in ~/.local/state/dusk:
// crash.log (where it crashed and the last few things it did) and
// dusk.log.old (everything it printed in that session). dusk-doctor shows
// both.
#pragma once

namespace dusk {

void install_crash_log();
// A short note of what's happening (kept in memory, written out on a crash).
void crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace dusk
