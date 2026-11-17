# Dusk

A floating (stacking) Wayland desktop written from scratch in C++20 on top of
wlroots 0.20. (It was called Floe up to version 0.4.)

It looks like a normal modern desktop: neutral light and dark themes and an
accent colour of your choice, plus a few optional touches taken from manga
line art (bold window outlines, hard comic-style shadows) and from Audi's
cockpit displays (the segmented LED level bar in the on-screen display).


## Build (Fedora 44)

```sh
./build.sh
```

This installs any missing build packages with `dnf` (it will ask for your
password), then compiles to `build/dusk`. The first build also downloads
[scenefx](https://github.com/wlrfx/scenefx) (the library SwayFX uses for
rounded corners and shadows) from GitHub and builds it in.

## Try it

**In a window, inside your current desktop** (safest way to test):

```sh
./build.sh run
```

Dusk opens as a window. Anything you launch from its app menu opens inside
Dusk. To also start a terminal in it: `DUSK_TERMINAL=kitty ./build.sh run`. (Night light and
notifications only work when Dusk is your real session.)

**As your real desktop:**

```sh
./build.sh install
```

Log out, click the gear on the login screen, and choose **Dusk**.
`./build.sh uninstall` removes it again.

If something goes wrong, **Ctrl+Alt+Backspace** quits Dusk and returns you to
the login screen.

## Shortcuts

| Keys | Action |
|---|---|
| Super (tap) or Alt+F1 | Open/close the app menu |
| Super+A or Super+N | Control centre |
| Super+Enter | Terminal (set in Settings, else the first installed one: ptyxis, konsole, kitty…) |
| Super+I | Dusk Settings |
| Super+E | Your files |
| Print | Screenshot tool (area, window or screen) |
| Shift+Print / Alt+Print | Screenshot of all screens / the active window, at once |
| Super+D | Show the desktop (again to bring the windows back) |
| Super+V | Clipboard history |
| Super+F | Fullscreen on/off |
| Alt+Tab / Alt+Shift+Tab | Switch windows |
| Alt+F4 or Super+Q | Close window |
| Super+Up | Maximise |
| Super+Down | Restore, or minimise if already normal |
| Super+Left / Super+Right | Snap to left/right half (again to restore) |
| Super+Z | Snap layouts for the active window |
| Super+Shift+Left / Right | Move the window to the next screen |
| Super + left-drag | Move a window from anywhere in it |
| Super + right-drag | Resize a window from anywhere in it |
| Volume up / down / mute keys | Change volume (with on-screen display) |
| Mic mute key | Mute/unmute the microphone |
| Brightness keys | Change screen brightness |
| Ctrl+Alt+F1…F12 | Switch virtual terminal |
| Ctrl+Alt+Backspace | Quit Dusk |

In the panel: middle-click a window button to close the window; right-click
anywhere on a panel for its options. In the control centre: right-click a
tile for its list or options; scroll over a slider to change it.
Right-click the wallpaper for a desktop menu.

## Settings

Open **Dusk Settings** from the gear in the app menu or the control centre,
or with Super+I. Changes apply instantly. They are stored in
`~/.config/dusk/dusk.conf`, a plain `key = value` file you can also edit by
hand; Dusk notices when it is saved. Panels look like this in the file:

```
panels = main, panel2
panel.main.edge = bottom
panel.main.widgets = menu,pins,tasks,spacer,status,clock
panel.main.pins = firefox.desktop,org.gnome.Nautilus.desktop
panel.panel2.edge = left
panel.panel2.style = dock
panel.panel2.autohide = true
```

Screens are set by their connector name (Settings → Displays shows it):

```
output.HDMI-A-1.mode = 2560x1440@143.912
output.HDMI-A-1.position = 1920,0
output.HDMI-A-1.rotation = 90
output.HDMI-A-1.vrr = true
output.eDP-1.enabled = false
scale.eDP-1 = 1.25
primary_output = HDMI-A-1
```

## What the control centre uses

All of these come with Fedora Workstation; anything missing just greys out
its tile or slider:

- `nmcli` (NetworkManager) for Wi-Fi.
- `bluetoothctl` for Bluetooth.
- `rfkill` for airplane mode.
- `wpctl` (PipeWire) for volume and the microphone.
- `brightnessctl` for brightness.
- `powerprofilesctl` for the power mode.
- `systemd-inhibit` for caffeine.
- `playerctl` for media controls.

## Startup

- `dusk -s "command"` runs a command once Dusk has started.
- `~/.config/dusk/autostart`, if it exists and is executable, runs at startup.
- `dusk -d` turns on verbose logging.

## How the code is organised

| File | What it does |
|---|---|
| `src/server.cpp` | Startup, monitors, mouse/keyboard input, focus, shortcuts, night light |
| `src/view.cpp` | Shared window code: title bar, maximise/snap/minimise/fullscreen, animations |
| `src/xdg_view.cpp` | Wayland app windows and their popups |
| `src/xwayland.cpp` | X11 app windows, menus and tooltips (XWayland) |
| `src/panel.cpp` | Panels and their widgets |
| `src/ccentre.cpp` | The control centre |
| `src/notify.cpp` | Notification server (D-Bus) and popups |
| `src/system.cpp` | Network, sound, brightness, Bluetooth, battery, media status |
| `src/glyphs.cpp` | Small icons (theme symbolic icons, with drawn fallbacks) |
| `src/ui.cpp` | Shared UI pieces: GPU shadows, fade/zoom, easing hover highlights |
| `src/ghost.cpp` | Copies of windows used for open/close/minimise/resize animations |
| `src/popover.cpp` | The calendar and the media card |
| `src/launcher.cpp` | The app menu (search, launching) |
| `src/osd.cpp` | On-screen display for volume, brightness and lock keys |
| `src/clipboard.cpp` | Clipboard history and its card |
| `src/desktop.cpp` | Files and folders on the desktop |
| `src/anim.cpp` | Animation clock and easing |
| `src/apps.cpp` | Installed-app list and icon theme lookup |
| `src/config.cpp` | Reads the settings file |
| `src/session.cpp` | Keyring, polkit agent, apps that start at login |
| `src/idle.cpp` | Screen off and sleep when idle, low battery warnings |
| `src/screenshot.cpp` | Taking, saving and copying screenshots |
| `src/shot.cpp` | The screenshot tool (Print) |
| `src/tiling.cpp` | Where dragged windows land, snap layouts menu |
| `data/dusk-apply-theme` | Makes GTK, Qt and browser apps match Dusk |
| `data/dusk-doctor` | Checks dark mode, keyring and polkit set-up |
| `src/draw.cpp` | Drawing helpers (cairo + pango → wlroots buffers), colours |
| `settings/main.cpp` | Dusk Settings (Qt 6) |
| `src/wallpaper.cpp` | Wallpaper loading (background thread) and transitions |
| `src/tray.cpp` | System tray (StatusNotifierItem host + menus) |
| `src/phone.cpp` | Phone/tablet as a second screen (virtual screen + wayvnc) |
| `src/lucide.hpp`, `src/lucide_icons.hpp` | Lucide icons as cairo drawing ops |
| `tools/lucide2cpp.py` | Converts Lucide SVGs into `src/lucide_icons.hpp` |
| `settings/settings_file.hpp` | Reads and writes the settings file for Settings |
| `src/overview.cpp` | The overview (Super): windows, workspaces, search |
| `src/workspaces.cpp` | Workspaces: switching, moving windows, hot corner |
| `src/trash.cpp` | The trash: count, open, empty |
| `src/crash.cpp` | Crash log (crash.log, dusk.log) |
| `src/gestures.cpp` | Touchpad gestures |
| `src/protocols.cpp` | xdg-activation, pointer lock for games, shortcut inhibit |

Colours and fonts live in `theme` in `src/draw.hpp` (`set_theme`).