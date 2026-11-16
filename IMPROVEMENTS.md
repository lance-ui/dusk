# IMPROVEMENTS

Changes applied after the code review of 0.35.13 (2026-11-16). IDs (F-xx / M-xx)
refer to that review. Nothing here was compiled or run yet — read the diff
before releasing, as with any change.

## Home

- The project's own GitHub URLs now point to `lance-ui/dusk`
  (`README.md`, `data/dusk-update` `OWNER_REPO`, `data/dusk-lyrics` user agent,
  `settings/main.cpp` updates card). `DUSK_GITHUB` still overrides it.
  Third-party URLs (scenefx, Google Fonts, Bibata, adw-gtk3, wayvnc) unchanged.
- `LICENSE` added (MIT).
- `.github/workflows/ci.yml` added: build with gcc and clang, plus a
  separate ASan + UBSan build (builds wlroots 0.20 from source when the
  distro does not ship it). The sanitizer build is the gate that catches
  exit-time use-after-free and undefined behaviour on every commit.
  A headless smoke run is the next step once verified locally.

## Compositor core (src/server.cpp, src/idle.cpp)

- **F-01** `on_virtual_keyboard`, `on_virtual_pointer`, `on_request_start_drag`
  and `on_start_drag` are now disconnected in `Server::shutdown()` before
  `wl_display_destroy`. They were left connected, so `~Listener` unlinked
  from freed signal lists on every clean exit (heap use-after-free).
- **F-02** `Output::configure()` records the applied key only after
  `wlr_output_commit_state` succeeds, on both the enable and disable paths;
  a failed commit is logged and stays retryable instead of being remembered
  as done (a screen that stayed black gets another try).
- **F-03** `Server::reload_config()` returns early when the settings file
  cannot be opened or is empty, instead of feeding defaults to
  `Config::load()` and wiping wallpaper, theme, panels and shortcuts.
- **M-03** the explicit-sync capability messages use `WLR_INFO` (they were
  `WLR_ERROR`, red in every journal on every boot).
- **M-11** `Server::focus()` refuses to give the keyboard to a not-yet-mapped
  view: it flags the view urgent instead (an X11 restore or a self-fullscreen
  request used to reach an unmapped surface).
- **F-07 (part)** `handle_capture_readable` retries on `EINTR` instead of
  treating it as end-of-output and blocking in `waitpid` while the child is
  still running.
- **M-02** `AwakeKeepers` checks the results of `sd_bus_add_filter` and
  `sd_bus_match_signal`, guards `sd_bus_get_fd` before creating the fd source,
  and guards `fd_source` in `process()`.

## Window management (src/view.cpp, src/overview.cpp, src/workspaces.cpp)

- **F-04** `move_pending_` is cleared in `View::did_unmap()` and in
  `View::cancel_animation()`, so a window that unmaps mid-configure (or whose
  animation is cancelled) no longer jumps to a stale position when it
  re-maps or the next commit arrives.
- **F-06** `Overview::open()` flashes a minimized window's disabled scene
  tree back on for the duration of the synchronous `Ghost` copy, then
  restores it. Ghosts for minimized windows render at their real positions
  again (the `overview_minimized` setting works; nothing renders in between,
  the compositor is single-threaded).
- **M-10** `refit` clamps the bounds themselves before `std::clamp`, which
  was undefined behaviour for a window wider than the usable area.
- **M-07** `Server::move_to_workspace` drops the dead ternary (both branches
  were identical) and clamps against a zero workspace count.

## Lock screen (src/lock.cpp, src/lock.hpp)

- **M-15** the password copies are wiped with `explicit_bzero` (in the auth
  thread after `pam_check`, and in the member string as it is handed off);
  `std::fill` could legally be optimized away as a dead store.
- **M-14** the logind prepare-for-sleep close timer is a member now, not a
  function-local static that could outlive the `LockScreen` and fire into
  freed memory; it is removed in the destructor.

## Rendering and helpers (src/recorder.cpp, src/notify.cpp, src/phone.cpp, src/crash.cpp)

- **M-21** `record_quality` and `record_audio` go through `shell_quote` in
  the recorder command (safe only by config whitelist before; now safe by
  construction).
- **M-22** `Recorder::stop()` escalates to `SIGKILL` after three seconds, so
  a hung `dusk-record` no longer keeps the red pill forever; the timer is
  cancelled when the recorder is reaped and removed in the destructor.
- **M-17** the notification history file is chmod 0600 before the rename
  (notification bodies can be private; the umask decided before).
- **M-16** the wayvnc config (holds the VNC password) is created with 0600
  from the start with `open()` instead of appearing 0644 until `chmod`.
- **M-18** the crash handler uses a static signal-name table instead of
  `strsignal`, which is not async-signal-safe and could lock if the crash
  happened inside malloc.

## Security at the edges (src/tray.cpp, src/xembed/main.cpp, src/config.cpp, src/main.cpp, settings/main.cpp)

- **F-12** `read_node` (dbusmenu `GetLayout` parsing) takes a depth argument
  capped at 10; deeper subtrees are consumed with `sd_bus_message_skip` and
  dropped. A hostile tray application could exhaust the stack and crash the
  whole session before.
- **F-11** the XEmbed icon grab clamps the untrusted window geometry to 512
  pixels, rejects depths other than 24 and 32, and does all size arithmetic
  in 64-bit. A huge `w * h * 4` used to overflow `int`, slip past the length
  check and overflow the heap. Icons larger than 512 are cropped.
- **M-09** `Config::write_key` chmods the temporary file to 0600 and checks
  the rename result.
- **M-08** the autostart path goes through `shell_quote` (a `HOME` containing
  quotes used to break it).
- **F-13 (part)** the update password file is only ever written under
  `XDG_RUNTIME_DIR`; when there is no runtime dir, Settings explains and
  stops instead of falling back to `/tmp`.

## Deliberately not applied (needs a test run first)

- **F-05** focus policy: gate map-time and X11-requested focus on activation
  tokens. A real behaviour change; it needs trying on a desktop.
- **F-08 / F-09 / F-10** performance reworks: persistent ink surface for the
  screenshot pen, memoized Pango layouts for the lyrics styles, off-thread
  PNG compositing for inked screenshots. All are refactors of hot paths and
  deserve before/after profiling.
- **F-14** the Wi-Fi password still travels on `nmcli`'s command line; moving
  it off argv needs a secrets-over-fd design (see the review, section 5.4).
- Remaining minors from the review table (M-01, M-04..M-06, M-12, M-13,
  M-19, M-20, M-23, M-24).

---

# UI improvements, wave 1 (2026-11-16)

Four changes from the UI improvements proposal, chosen for the first wave:
they align the interface with its two existing design signatures (the manga
line art and the OSD's LED gauge) and close two daily-use gaps, without
changing window-management semantics. Also not compiled or run yet — same
caveat as above. One build fix rides along: `main.cpp`'s autostart call now
qualifies `dusk::shell_quote` (it does not compile without the namespace).

## Shared drawing (src/draw.hpp, src/draw.cpp, src/osd.cpp)

- The OSD's slanted LED segments moved into a shared `led_segments()` in
  draw.cpp (an optional per-segment colour list, tip glow kept), so the OSD
  and the panel's new pager draw with exactly the same primitive and cannot
  drift apart.

## Panel (src/panel.cpp)

- **U3 an LED workspace pager**: the overview button has a new look,
  `overview_icon = led` (Settings › Workspaces › Overview button): one LED
  segment per workspace, the current one lit in the accent, occupied ones a
  little brighter than empty ones, stacking bottom-to-top on upright panels.
  Clicking still opens the overview and the wheel still switches workspaces
  (the button's existing behaviour, reused as-is).

## OSD (src/osd.cpp, src/osd.hpp, src/server.cpp, src/server.hpp)

- **U4 the volume and brightness bars are settable**: press and drag along
  the segments, or scroll on the OSD. The OSD joined the NodeTag scheme
  (`Kind::Osd`), accepts pointer input while a settable bar is up, and
  `hit_test` now treats it as the top layer (windows' resize margins no
  longer poke through it, so no stray resize cursors over it). server.cpp
  gained an `osd_grab` beside `cc_grab`, intercepting left-press, motion and
  wheel before the window dispatch; levels go through the existing
  `SystemStatus::set_volume()` / `set_brightness()` (dragging the volume up
  also unmutes), scroll is one step like the keyboard's.

## Notifications (src/notify.cpp, src/panel.cpp)

- **U5 the desktop quiets itself while recording**: notifications below
  critical urgency do not pop up while `Recorder::active()` (both the D-Bus
  path and Dusk's own `post()`), and the panel's bell shows bell-off for the
  duration. The user's Do-not-disturb setting is untouched — this quiets
  itself for the recording and no longer.

## Accessibility and settings (src/config.hpp, src/config.cpp, src/draw.hpp,
src/server.cpp, settings/main.cpp)

- **U14 Reduce motion**: `reduce_motion = true` collapses every animation
  through the one gate everything already asks (`Config::animate()`), so
  windows open, menus appear and the overview snaps without glides. A toggle
  on Settings › Animations.
- **High contrast**: `high_contrast = true` raises window outlines a step
  (up to 3 px, unless outlines are off), draws panel and card outlines at
  full ink strength, and pins text colours at full contrast in both themes
  (`theme::set_theme`). A toggle on Settings › Appearance.
- Both keys are parsed in config.cpp and added to Settings' groups (search
  and "Reset all settings").
