# Dusk

A floating (stacking) Wayland desktop written from scratch in C++20 on top of
wlroots 0.20. (It was called Floe up to version 0.4.)

It looks like a normal modern desktop: neutral light and dark themes and an
accent colour of your choice, plus a few optional touches taken from manga
line art (bold window outlines, hard comic-style shadows) and from Audi's
cockpit displays (the segmented LED level bar in the on-screen display).

Source and updates: **https://github.com/lance-ui/dusk** — `dusk-update` reads
the newest release from there. Dusk is MIT-licensed (see `LICENSE`). Fixes
from the 2026-11 code review are listed in `IMPROVEMENTS.md`.

## What's new in 0.35.13

- **A roomier notification centre:** wider, and as tall as your notifications really are (measured, not guessed), up to most of the screen; past that the list scrolls. A single long notification was squeezed into a strip.

## What's new in 0.35.12

- **Keyboard back in games:** each tray icon a game added (through dusk-xembed, 0.35.7) took the keyboard to an invisible holder window, so the game stopped getting keys. Those holders are never shown and never get the keyboard now, and no X11 pop-up that's off every screen gets it either.

## What's new in 0.35.11

- **Super+Escape does more:** it also shows any windows that were left hidden, ends a stuck move or resize, and writes what every window was doing to the journal (for tracking down game problems).
- **X11 pop-up windows are noted in the journal too** (games under Wine sometimes use them for full screen).

## What's new in 0.35.10

- **Super+Escape gets the desktop back:** when a game leaves things stuck (keys not working, a picture that stopped moving), Super+Escape takes back the keys a game kept for itself, closes menus and the overview, gives the window in front the keyboard and redraws every screen.
- **Notes for game problems:** each X11 window (games under Wine/Proton) that opens is written to the journal with its size and state, so problems can be tracked down.

## What's new in 0.35.9

- **Media keys work everywhere:** Play/Pause, Next, Previous and Stop keys control the player the control centre shows (Spotify, a browser tab, any MPRIS player), whichever window is in front. They only reached the window in front before (and on the lock screen).

## What's new in 0.35.8

- **Any key can be a shortcut:** Settings › Keyboard › Your shortcuts takes a key on its own (F1, a laptop's extra keys like HP's Omen key, the calculator key) or with any of Super, Ctrl, Alt and Shift together (the Copilot key is Super+Shift+F23). Keys Qt has no name for are recorded by their key name. While it's listening, Dusk lets every key through to it, even ones Dusk uses itself.
- **See what keys are taken:** after you pick keys, it says if Dusk or another of your shortcuts already uses them (yours win over Dusk's own).
- **Letters stay typeable:** a letter, number or symbol on its own needs Super, Ctrl or Alt with it.

## What's new in 0.35.7

- **Tray icons from Wine and Proton games:** games and launchers under Wine (Strinova, Epic and Ubisoft launchers…) use the old XEmbed tray; with none, Wine showed their icons in a small window of its own. dusk-xembed (a small helper Dusk starts with Xwayland) is that tray now: their icons appear in the panel's tray, and clicking them (left, middle, right) goes to the game.

## What's new in 0.35.6

- **Saved passwords on Fedora 45:** Fedora 45 replaced GNOME Keyring with oo7, which the login screen unlocks. Dusk was starting GNOME Keyring anyway (a second, locked keyring), so Vivaldi, Chrome and other apps asked for a password after every login. Dusk now uses oo7 when it's there, and the installer only adds GNOME Keyring (with its login unlock) where oo7 isn't. dusk-doctor checks oo7 too.
- **Games cover the panel:** a full-screen game covers the panel whenever it's the top window on its screen, even if its launcher (Heroic, Steam) still had the keyboard. A game that goes full screen also gets the keyboard and controller.
- **Real icons for games:** windows from Wine/Proton ("rocketleague.exe") and Steam ("steam_app_…") get the icon of the matching app (by name or title, and Steam's own game icons) instead of a letter.

## What's new in 0.35.5

- **Settings opens pages at once:** the system tools pages ask (localectl, timedatectl, xdg-mime, lspci, lsblk…) are asked all together in the background as Settings starts, the installed apps are read once, and the other pages are built quietly while you look at the first. Clicking a page no longer waits.
- **Caps Lock on the lock screen right away:** the warning follows the keyboard's own state the moment Caps Lock changes (it waited for the next key, or missed the key that woke the screen).
- **Settings sits with GNOME apps:**
  - Its own header bar (the page's name in the middle, round grey window buttons), like GNOME apps, instead of Dusk's title bar on top. Drag it to move, double-click to maximise, the edges resize. Turn it off in Appearance › This Settings app.
  - libadwaita's colours (window #222226, sidebar #2e2e32 in dark; #fafafb and #ebebed in light) and shapes (9 px buttons and fields, 12 px boxed lists).
  - The page title lives in the header bar.
- **Dusk's colours match GNOME's:** panels, menus, the control centre and title bars use the same neutral greys as libadwaita apps, not a warm charcoal next to them.

## What's new in 0.35.4

- **Subtler shadows:** window and panel shadows are fainter, closer and less spread out (soft style). The ink and hard styles are unchanged.
- **The notification centre, tidied:**
  - It opens by the button you clicked: under the clock in the middle of a GNOME-style bar, by the bell at the end of a Windows-style taskbar. It used to jump to the screen's edge.
  - A clear header (Notifications, Clear all, Do not disturb), the list, then today's date along the bottom (click it for the calendar) and a settings button.
  - The list no longer has a second "Notifications" heading.

## What's new in 0.35.3

- **A notification centre of its own:** with the Windows, GNOME and macOS looks, notifications move out of the control centre into their own card, with the date, Do not disturb and the calendar at the top. Dusk's own look keeps them together. Change it in Settings › Notifications › Where they're kept.
- **The bell on the panel:** a new panel widget, put after the status icons. It shows how many notifications are waiting and opens the notification centre. Right-click it for Do not disturb and Clear all. It hides itself when notifications live in the control centre.
- **The clock can open the notifications:** a third choice for clicking the clock. The GNOME and macOS panel looks use it, like the real ones; the Windows look puts the bell after the clock.
- **A shorter control centre:** without notifications under it, the control centre is only as tall as its tiles.

## What's new in 0.35.2

- **For you's tiles fit:** "You use these most" tiles were squeezed to a sliver (a style rule overrode their height); they're full size again.
- **Rounded accent buttons:** buttons without an outline (Open, Updates, Install) were still square; every button is tall enough for its round corners now.
- **Readable text on any accent:** the text on accent buttons is white or dark, whichever stands out more, so a light grey accent from a grey wallpaper gets dark text.

## What's new in 0.35.1

- **Sharper text:** one small type scale (14 px text, nothing under 12 px at 100%), and Adwaita Sans (GNOME's screen font) or Inter instead of Google Sans, which was soft at small sizes. Only if you still had the old default.
- **Real rounded buttons in Settings:** Qt drew them square (their corners were rounder than half their height); every button is now the same height and a true pill.
- **Solid surfaces:** menus, the app menu and the control centre are nearly opaque, so a bright wallpaper no longer tints them brown or grey and text never sits on a busy picture. (Opacity is still in Appearance.)
- **GNOME-style title bar buttons:** round and grey instead of corner brackets with a red ✕, unless you chose otherwise.
- **No dead space:** the media card shrinks to one line when nothing is playing, and the app menu is as tall as what's in it.
- **Pinned apps from the start:** Files, the browser, the terminal, Software, Text Editor and Settings until you pin your own.
- **Quieter Settings:** smaller page titles, bold group headings instead of grey small caps, and a grey Reset link.

## What's new in 0.35.0

- **A calmer look:** flat, quiet colours in warm charcoal or ivory, thin hairlines and soft faint shadows instead of drawn ink lines, and softer corners everywhere. The old styles are still in Appearance (Comic, Flat, Calm).
- **Settings, redone:** a narrower centred column (no stretched cards), a grouped sidebar (Personalise, System, Windows, Devices, Apps & media) and smoother text.
- **For you:** Settings opens on a page with your picture (change it there), the pages you use most, suggestions and what you opened lately.
- **Connect your phone:** pick KDE Connect (notifications, files, clipboard) or LocalSend (files, any phone); For you opens it, or its install page if you don't have it yet.
- **Panels have names:** Panel 1, Panel 2… (or your own name), so moving one to another edge doesn't confuse it.
- **A new app menu:** pinned apps, your places (Home, Documents, Downloads, Pictures, Trash), recent files, and every app A to Z under All apps. Search finds apps, files and Settings pages, and does sums (Enter copies the answer). You, Settings, lock and power along the bottom.
- **Control centre restyled:** the date at the top, Wi-Fi and Bluetooth pills, round toggles, three tall sliders with their values, the battery with power modes, and a plain media card with an accent play button.
- **Updates you can watch:** the update in Settings shows each step ticked off and the live output (hide it if you like); the installer uses one calm colour.

## What's new in 0.34.1

- **Apps on the desktop:** right-click an app in the app menu › *Add to desktop* (and *Remove from desktop* again); the desktop's own right-click menu has *Add an app…*, which opens the app menu. Double-click it on the desktop to start it.
- **Files on the desktop:** right-click a file found in the app menu's search › *Add to desktop* puts a link to it there (the file stays where it is). Dragging files from Files onto the desktop, and anything in your Desktop folder, already showed there.

## What's new in 0.34.0

- **"Restart & Upgrade" and "Restart & Update" work** (GNOME Software, and other apps that ask the desktop to restart, shut down or log out). They ask GNOME's session manager, which isn't there under Dusk, so nothing happened; Dusk now answers in its place. Apps that ask GNOME to keep the screen on (some video players) are heard too.

## What's new in 0.33.9

- **Choosing 100% for a screen sticks.** Picking 100% in Settings › Displays cleared the setting, and since 0.33.6 a screen with none set gets the size that suits it, so a 4K TV went to 200% instead. Every choice is now written down.

## What's new in 0.33.8

- **Drag windows to another screen in the overview:** drop a window anywhere on another screen and it moves there, in the same place on that screen (as a share of it), or maximised, tiled or full screen there just as it was.

## What's new in 0.33.7

- **Play sound on a monitor or TV:** the "Play sound on" list now has HDMI and DisplayPort outputs too (by the screen's name, like "LG TV SSCR2 · HDMI / DisplayPort"). They're switched off on the sound card until picked, so they weren't listed before; picking one switches the card over and moves what's playing there.
- **Easier to find:** right-click the volume slider (or the sound tile) in the Control Centre, or the status icons on a panel (*Play sound on…*), as well as *Media output* on the media card. Mute is in the list too.

## What's new in 0.33.6

- **Big screens get the right size:** a TV or a 32"+ monitor is looked at from further away, so its scale now follows how many pixels tall it is (a 4K TV: 200%, laid out like a 1080p one; 1440p: 125%) instead of its pixels per inch, which left a 72" 4K TV at 100% with tiny text. A screen you haven't chosen a scale for now starts at the one that suits it, and Settings › Displays marks that one "recommended".

## What's new in 0.33.5

- **Smoother workspace pictures:** their numbers, the ✕ and the + no longer go missing (they were drawn see-through when the overview opened), the windows in them fade in and out with the rest, the whole row slides in gently from its edge, and the ring moves to the one you click straight away.
- **Updates know which version you have:** before you install, Settings lists what you'll get since your version (every version in between too, not just the newest one's changes), both for a zip in Downloads and for an update from GitHub; and the list after installing covers everything since the version you had.

## What's new in 0.33.4

- **Workspace pictures in the overview:** a small picture of every workspace (its wallpaper and windows, numbered, the one you're on ringed in the accent with a ✕ to remove it, and a + for a new one). Drag a window onto one to move it there, or onto + for a new workspace with it; click one to go there. Put them at the top, bottom, left or right, or turn them off, in Settings › Workspaces › Overview.
- **What's new, in a few lines:** after an update, the installer lists the headlines (in the terminal, and in Settings while updating), Settings › About has a "What's new" card, and Dusk shows a note once after you log in to the new version.

## What's new in 0.33.3

- **The password is asked for once, and checked.** Updating from Settings asked again and again (each step of the installer asks on its own when there's no terminal), and the box then said "That password wasn't right" even when it was. Now Settings asks once, before it starts, checks it straight away ("Checking…"; a wrong one says so and lets you try again), and gives it to each step itself. It's kept in a private file only you can read, wiped when the install ends.

## What's new in 0.33.2

- **A cleaner notification menu, in the style of One UI:** who it's from on top ("WhatsApp · KDE Connect"), then what you can do with it (Open first, the app's own buttons after, each with an icon that fits: Reply, Mark as read, Mute…), then Copy the text and Notification settings, and Dismiss on its own at the bottom, set apart by thin lines.

## What's new in 0.33.1

- **A nicer password box when updating:** Dusk's logo, what's being installed ("Installing Dusk 0.33.2"), a rounded field with an eye to show what you typed, a hint when Caps Lock may be on, "That password wasn't right" (with the field in red) when it asks again, and Cancel / Install buttons in Settings' style.
- **"Other versions" is gone** from Settings › About › Updates: it updates to the newest, or installs a zip you pick.

## What's new in 0.33.0

- **Settings showed the wrong version** (0.32.9 still said "Dusk 0.32.8"), so it offered an update to the version you already had, and the installer then said there was nothing to do. The version now comes from one place (meson.build), so Settings can't fall behind again.
- **"Already up to date"** instead of "Nothing to install" with a terminal command under it: it says which Dusk is installed, and offers to log out when the one running is older.

## What's new in 0.32.9

- **Update from Settings itself** (Settings › About › Updates): the steps and a progress bar as Dusk builds, your password asked for in a small dialog, *Show details* for the full output, Cancel, and *Log out now* when it's done. Three ways to get a new version:
  - **Update now** — the newest from GitHub (or *Other versions* for a particular one);
  - **a Dusk zip in your Downloads** — offered by itself when it's newer than yours;
  - **Install from a zip…** — pick any Dusk-…-source.zip.
  Prefer the terminal? Turn on *Use a terminal window instead*. (`dusk-update --zip FILE` and `dusk-update --downloads` do the same from a terminal.)
- **A new look for the installer in the terminal:** a softer heads-up instead of a block of capitals, your system's full name, numbered steps (1/3 Packages and fonts · 2/3 Building · 3/3 Installing), the compile as one line with a progress bar (the full output is kept in build/ninja.log and shown if it fails), and a small finishing card with how long it took. It asks "Update Dusk 0.32.8 to 0.32.9?" or "Install Dusk 0.32.9?", whichever it is.

## What's new in 0.32.8

- **Windows 11 look: the battery tile with the power modes**, like the other looks (the charge filling it, the percentage and the mode, and the three mode buttons), but square-cornered like Windows' buttons, in place of the Energy saver button. Reset the Windows layout (pencil › Reset) to get it.

## What's new in 0.32.7

- **Scroll over the status icons on a panel** (the Wi-Fi and speaker icons that open the Control Centre) to turn the volume up or down, with the on-screen display.
- **In the Control Centre, the wheel works over a slider's icon too** (the speaker, microphone, sun), not just the bar; clicking the speaker or microphone icon mutes it.

## What's new in 0.32.6

- **Clicking a notification in the Control Centre shows its options** instead of removing it: Open (the app's own action, or the app), the app's buttons (Reply, Mark as read…), Copy the text, Notification settings, and Dismiss. Right-click does the same; the × still dismisses straight away.

## What's new in 0.32.5

- **Every Control Centre look has the same tiles**, arranged its own way: the connections, Do not disturb, dark mode, night light, Caffeine, screenshot, **the battery with the three power modes** (a full-width tile), brightness, volume, the microphone and what's playing. (GNOME had no battery tile; Dusk and Windows had no microphone slider.)
- **The macOS look fits better:** Night light and Caffeine get room for their names, the battery tile is wide enough for its mode buttons (and has the modules' corners, not a pill's), the microphone slider is added, and "Airplane mode" shortens to "Airplane" when it doesn't fit, as macOS does.
- Layouts you've arranged yourself are kept: press **Reset** in the Control Centre's edit mode (or Settings › Control centre › Reset the layout) to get the new one.

## What's new in 0.32.4

- **The power mode selector is back** when there's no battery (a desktop, or a dead battery): the same wide tile with the three mode buttons (power saver, balanced, performance), titled *Power mode*, instead of a plain toggle.
- **Super+1…9 (and 0 for the 10th) go straight to that workspace**, if it exists; with Shift the active window goes there. The number pad works too.

## What's new in 0.32.3

- **Games in "borderless" full screen go over the panels.** Wine/Proton games (Heroic, Steam, Lutris) and older X11 games often don't ask for full screen: they make a frameless window exactly the size of the screen. Dusk now treats that as full screen, so the panel no longer sits on top. Switching the game back to windowed in its own settings works too.

## What's new in 0.32.2

- **A new Wave visualiser:** a flat line, like a heart monitor's, that spikes where there's a beat (a band of the sound suddenly jumping) and settles back flat. Steady sound leaves it calm, so it follows the music instead of just rolling along.
- **No more "Showing on the screen" message** every time lyrics are found: you can see them. If they're on the desktop behind your windows, it says so once, and the media menu has a new **Lyrics over the windows** switch.
- **One-colour (symbolic) icons in notifications** are drawn in the text colour on a soft tile, instead of dark grey on the dark card.

## What's new in 0.32.1

- **The plugged-in times are used when you're plugged in.** Dusk went by the battery's own state, and a worn-out battery can say "Not charging" or even "Discharging" on the charger, so the battery times (lock, screen off, sleep) were used instead. It now asks the charger itself.
- **A dead battery hides the battery settings** in Settings › Power too, leaving one set of times (as on a desktop).
- **"Never" for Lock was misleading:** it meant "no earlier lock", and it still locked when the screen turned off. It now reads *With the screen* (or *Never* when Settings › Lock screen › When the screen turns off is off).

## What's new in 0.32.0

A smoother, tidier release from the review of the whole shell.

- **Settings no longer closes by itself.** Changing the wallpaper (and a few other settings) could crash it: the Panels page was rebuilt on every change, leaving stale hooks behind. It now rebuilds only when the panels really changed, and its hooks clean up after themselves.
- **Settings opens faster.** Each page is built the first time you open it instead of all at once (searching still finds everything).
- **A dead battery is handled.** A battery stuck at 0% with the charger in is treated as no battery after a couple of minutes, so you don't get a permanent red 0%.
- **Watches instead of polling.** Sound, network, Bluetooth, brightness and battery changes come in as they happen (pactl, nmcli, udev, D-Bus), so the panel reacts at once and the slow poll runs a third as often.
- **Less work while you use it.** Panel and Control Centre redraws are bundled together, text measurements are cached, album art loads in the background, the app menu only rereads apps when they change, and lyrics pause while a maximised or full-screen window covers them.
- **One set of status colours.** Reds, ambers and greens (low battery, recording, charging, close buttons, errors) come from the theme, and are a touch deeper in the light theme so they still read.
- **Light versions** of the window overview and the GNOME/macOS app menus: a pale frosted backdrop with dark text when you use the light theme.
- **Corners follow Roundness** everywhere, including the Windows-look buttons, the lyrics backdrop, the media card's buttons and the OSD.
- **The OSD shares the menus' blur and shadow**, so it matches the rest of the desktop.
- **macOS dock magnification** (Settings › Panels › Look › Magnify the dock): icons grow as the pointer passes over them.
- **Files show up sooner in the Dusk app menu:** the best few apps, then the files and folders, then the rest of the apps.
- **Windows-look Control Centre:** the battery button is *Energy saver* (power saver on and off), like Windows 11.

## What's new in 0.31.9

- **Smooth sliders in the control centre**: dragging volume, brightness or
  the microphone redrew every panel, the media card and the control centre
  for each tiny pointer move. Now only the control centre is redrawn, at
  most once a frame and only when the value changes; the panels catch up
  when you let go.

## What's new in 0.31.8

- **The GNOME and macOS app menus look like the overview**: the wallpaper
  behind them is blurred as strongly and tinted the same dark grey (it was
  lighter and only a little blurred, a washed-out look).
- **Minimised windows show in the overview** (a little faded) with the
  others of the workspace; click one to bring it back. Settings ›
  Workspaces › Overview › Show minimised windows.

## What's new in 0.31.7

- **The GNOME and macOS app menus' background is the wallpaper itself**,
  softened and darkened, instead of a live blur of the screen. The live
  blur also took in the panels, their shadows and the screen's edges, and
  showed them as dark bands and corners whatever was tried; a softened
  copy of the wallpaper has nothing else in it. (Made once per wallpaper.)

## What's new in 0.31.6

- The panels' shadows step aside too while the GNOME / macOS app menu is
  open: they lie under the windows, and the blur behind the menu showed
  them as dark bands along the panels (a hidden dock's along the bottom).

## What's new in 0.31.5

- **The GNOME and macOS app menus' background is built like the
  overview's**: its own blur over the whole screen (past the edges), under
  the panels, with a dark tint in front of it that hides what's left of
  the blur's edges; the apps are drawn on top without a blur or shadow of
  their own. (Before, the menu's own card blur showed dark bands and
  edges.)

## What's new in 0.31.4

- **No dark edges round the full-screen app menus and the overview**: the
  blur behind them ended at the screen's edges and darkened them (most at
  the bottom and the corners); it now reaches a little past every edge
  with no other screen beyond it, so the whole screen is evenly frosted.

## What's new in 0.31.3

- **No dark band along the panel over the GNOME and macOS app menus**: the
  full-screen app grid now covers the whole screen under the panels (like
  the overview), so its blur has no edge where you can see it; ending at
  the panel, the blur pulled the panel's dark colour in (the halo). The
  panels stay sharp on top, and the apps keep to the space between them.

## What's new in 0.31.2

- **No more dark halos behind the GNOME and macOS app menus**: the open
  windows step aside while the full-screen app grid is open (as in the
  overview), so the blur behind it is of the wallpaper alone instead of
  showing the windows as dark smudges. They're back as it closes.

## What's new in 0.31.1

- **The GNOME and macOS app menus no longer glitch** (as the overview did
  before its fix): the full-screen grid is drawn once per frame however
  often something asks (hover, scrolling, typing, files arriving), and it
  only fades in and out, without the zoom and the shadow that made the
  blur behind it jump.

## What's new in 0.31.0

- **Favourites in the app menu** (every look): right-click an app › Add to
  favourites. They sit above the rest — a row under the search (Dusk),
  Pinned over All apps (Windows 11), a row like a dock (GNOME, macOS) — and
  the section disappears when there are none. Right-click also has Open and
  Pin to panel.
- **Search files and folders from the app menu**: type two letters or more
  and your files and folders whose names match show after the apps (from
  the file index — plocate — plus a quick look through your folders, so new
  files show too; hidden ones are left out). Enter or click opens one with
  its app (a folder in the file manager); right-click: Show in its folder,
  Copy where it is.
- **Sharper text** in the app menu, control centre, panels and menus:
  lightly hinted to the pixel grid (it was soft, most at 100% scale).
- **HD screenshots stay sharp**: they're enlarged with whole pixels made
  bigger instead of smoothed (the smoothing made text look low-res).

## What's new in 0.30.8

- **Screenshots are HD for sharing**: a small screenshot (a region, or a
  laptop screen like 1366×768) is enlarged so its long side is at least
  1920 px. WhatsApp only offers HD for pictures above ~1600 px and turns
  smaller ones into a blurry JPEG; now text stays sharp after sending (also
  through KDE Connect). Settings › Screenshots & recording › HD for sharing
  (on by default). Screenshots already that big are untouched.

## What's new in 0.30.7

- **Long lyric lines fit, in every style** (on the screen and in the media
  card): they get a little smaller, then go onto two lines instead of
  running off the edge (Comic, Karaoke — whose fill now goes along both
  lines — Glow, Bounce, Subtitles, Typewriter, Classic).
- **Scrolling no longer squashes lines together**: each line takes the room
  it needs when it wraps, so they don't overlap.

## What's new in 0.30.6

- **Refresh the lyrics**: a refresh button in the panel media card's lyrics
  (and "Search for lyrics again" in the media right-click menus, now there
  even when lyrics were found) forgets what was found for this song (and
  its timing) and looks again; "Listen for the song" asks SongRec first.
  For lyrics of the wrong song, or badly timed ones.

## What's new in 0.30.5

- The lyrics in the panel's media card are bigger and readable in every
  style: the Scrolling style (and other tall ones) were shrunk to fit and
  came out tiny; now they're laid out in the room there is, showing fewer
  of the lines around the one being sung. The lyrics area is taller too.

## What's new in 0.30.4

- **The panel's media card shows the lyrics like the screen does**: in the
  lyric style you picked, with the translation under the line being sung,
  shrunk to fit (on a dark backdrop, so they read in light mode too).
- **Lyrics colour** (Settings › Media & lyrics): the accent colour (the
  default), the album cover's colour, or white. Highlights are in it
  (karaoke's fill, comic's shadow, glow) and the words are touched with it,
  translations too.

## What's new in 0.30.3

- **SongRec is found wherever it came from**: lyrics by ear only looked
  for one Flathub id; now it also asks Flatpak for any SongRec, checks a
  snap, and runs what SongRec's menu entry runs (an AppImage or a build of
  your own). If it still can't, the lyrics panel says so.

## What's new in 0.30.2

- **Lyrics by ear actually work now**: SongRec from Flathub couldn't open
  the clip Dusk recorded for it (it was kept where Flatpak apps can't
  read); it's now kept in ~/.cache/dusk.
- **Compilations and mixes**: a song heard in the middle of a long video
  lines up with it (SongRec says how far into the song the clip was, so
  Dusk knows where the song started), and when that song ends Dusk
  listens again for the next one.
- The lyrics panel says what listening did (what SongRec heard, or why it
  couldn't), not only "not found".

## What's new in 0.30.1

- **The microphone indicator is only for microphones**: apps recording
  what's playing (SongRec listening to a song, visualizers, a screen
  recorder's system sound) no longer light it; only recording from a
  microphone does.

## What's new in 0.30.0

- **Language & region** in Settings: pick the language of menus and apps
  (type to find it; ~75 to choose from, plus any already on the computer),
  and separately how dates, times, numbers and money are written, with a
  preview. A language that isn't on the computer yet is downloaded and
  installed first (Fedora's langpacks, or locale-gen on Debian/Ubuntu/Arch),
  then set with localectl — one password prompt. Log out and back in to see
  it everywhere (there's a button for that). Lyrics translation follows it.

## What's new in 0.29.3

- New defaults for lyrics on the screen: the **Comic** style, on the desktop
  **under the windows**, and **translated into your system's language** when
  a song is in another one. (Anything you've already chosen stays as it is.)

## What's new in 0.29.2

- The panel's media widget has the media card's menu too when you
  right-click it: lyrics on the screen, search again / listen for the song,
  the lyric style, the beat visualizer and Media & lyrics settings.

## What's new in 0.29.1

- **The visualizers are in stereo**: the music is now heard in both
  channels, the left one drawn on the left half of the card and the right
  one on the right, the bass meeting in the middle (Bars, Mirror, Dots,
  Wave and Hills).
- **Hills show the music**: a smooth landscape of the spectrum that rises
  where the music is loud (the bass in the middle, the treble at the
  sides), a slower layer behind it, instead of fixed hills that all rose
  and fell together.
- **The wave is gentler**: more, smaller ripples near the controls instead
  of tall spikes across the whole card.

## What's new in 0.29.0

- **Translated lyrics, into any language**: Settings › Media & lyrics ›
  Translation. Lyrics in another language are translated line by line
  (Google Translate's free web service; no account) into your system's
  language or one you pick, and shown in italics under the line being sung,
  or instead of the lyrics. Translations are kept, so a song is only
  translated once; lyrics already in your language are left alone.

## What's new in 0.28.3

- **The on-screen display and the lyrics go wherever you click**, like
  notifications: a little screen of nine places in Settings (corners,
  edges or the middle). At a side, the lyrics keep to that side's part of
  the screen.

## What's new in 0.28.2

- **The camera indicator goes off when the camera does**: it used the
  webcam driver's count of users, which stayed up after an app let the
  camera go (and counted PipeWire holding it for nobody). It now looks at
  which of your programs have a camera open, and for PipeWire only while
  one of its camera streams is actually running.

## What's new in 0.28.1

- **No more clicks in headphones**: the sound card was put to sleep after a
  few quiet seconds and clicked when it woke for the next sound (a
  notification, a video, the media wave starting to listen). Dusk now tells
  WirePlumber to keep it awake (Settings › Media & lyrics › Sound; on by
  default, uses a little more battery). `dusk-doctor` also checks the
  kernel's own power saving for the sound chip and shows how to turn it off
  if clicks remain.

## What's new in 0.28.0

- **Panels and the app menu have looks too**, each chosen on its own
  (Settings › Panels › Look), like the control centre's:
  - **Panels**: Dusk, **Windows 11** (a taskbar with Start and the apps in
    the middle, nearly square buttons, the time over the date at the
    right), **GNOME** (a black top bar: Activities, the date and time in the
    middle, the status icons in a pill) or **macOS** (a menu bar along the
    top and a dock with a dot under open apps). Picking one also sets the
    panels up that way; change anything after. Your pinned apps stay.
  - **App menu**: Dusk's list, **Windows 11's Start** (search, the apps six
    to a row, you, Settings and the power button along the bottom, in the
    middle over the taskbar), **GNOME's app grid** or **macOS's Launchpad**
    (the whole screen, blurred, big icons; click anywhere else to close).
    Arrow keys move through the grid; type to search.

## What's new in 0.27.1

- **Lyrics when lrclib.net is down**: a busy or broken answer (like "503
  Service Unavailable") is asked again after a moment, and songs are also
  looked up on **NetEase Cloud Music** (free, synced lyrics for a great many
  songs) when LRCLIB doesn't have them or can't be reached.
- If both sites are unreachable it no longer says "No lyrics for this song":
  it says the sites are unreachable, and tries again by itself (after 45 s,
  then 1½, 3 and 6 minutes) while the song plays.
- Builds again: the Num Lock / Caps Lock start-up setting needed a wlroots
  header that wasn't included.

## What's new in 0.27.0

- **Beat visualizer like Arqen's** behind the media card: 24 bars from the
  music's spectrum (bass to treble), quick up and slow down, that never move
  sideways. Styles: Bars, Mirror, Dots, Wave (one line) or the old Hills;
  coloured from the album art, the accent, white or a rainbow. Without
  anything to listen to it plays Arqen's made-up beat. (Settings › Media &
  lyrics, or right-click the card › Beat visualizer…)
- **The media card's right-click menu works**: an open menu now gets the
  click before the control centre under it. The lyric styles and the
  visualizer are menus of their own (it was taller than a laptop screen).
- **Lyrics say why they're missing**: the lyrics panel (and a notification
  when you turn lyrics on) shows what the search found — not on lrclib.net,
  SongRec not installed, what SongRec heard, offline. *Search again* and
  *Listen for it* (ask SongRec first) buttons. Lyrics now show over the
  windows by default. SongRec's recording is written out properly (a
  timed-out recorder could leave a broken file), and silence is reported.
- **Windows look**: shorter rows, so the toggles are Windows 11's wide
  buttons with their names under them, three to a row with no gaps; the
  battery / power mode fills the last place. (An arrangement you made for
  the Windows look starts over once.)
- **Num Lock and Caps Lock at start-up** (Settings › Keyboard): on, off or
  as the keyboard comes up; a keyboard plugged in later matches; changing
  the layout keeps them as they were.
- **Separate see-through and blur settings** (Settings › Appearance): menus
  and the control centre, the panels (blur; each panel keeps its own
  opacity), and the Settings app itself, which can be see-through with
  Dusk's blur behind it.
- **Lock, screen off and sleep each have their own time** (Settings ›
  Power): lock after a while on its own, without the screen going off.
- The battery percentage ignores a mouse's or headset's battery listed
  before the laptop's, and batteries without a "now" amount.

## What's new in 0.26.11

- The media card's sound hills stay in place and rise and fall with the
  music: up at once on a beat, settling back between beats (they no longer
  flow sideways).

## What's new in 0.26.10

- Builds without warnings from Dusk's own code (a too-small buffer for the
  recording time, and a few unused functions).

## What's new in 0.26.9

- The installer no longer stops silently when an optional package (like the
  Bibata cursor or android-tools) isn't in your repositories: trying a
  package can't end the install any more, and a check is then made for what
  actually arrived.
- Things already installed by package are recognised even where the check by
  file doesn't find them (so Qt SVG / Qt Wayland aren't "installed" again
  every time).

## What's new in 0.26.8

- The Windows, GNOME and macOS looks of the control centre now use the same
  grid as Dusk's: in edit mode the free cells show, drag a tile anywhere,
  pull its corner to any size, hide it and add it back (the bars, battery
  and media card too). Each tile is drawn the look's way at whatever size you
  give it (Windows buttons with the name under them, GNOME pills, macOS
  modules), and each look keeps its own arrangement. (The 0.26.4 "buttons
  list" editing is replaced by this.)

## What's new in 0.26.7

- The installer checks for what Dusk needs by what it is (a command, a file,
  a library), not by package name: copies from another repository, Flathub,
  a source build in /usr/local or ~/.local all count, and nothing is
  installed over them.
- Only what's needed to build Dusk can stop the install. Tools for features
  (screen recording, Bluetooth, the media wave, lyrics...) are installed one
  by one; one that isn't in your enabled repositories is skipped with a note
  saying which feature won't work, and the install carries on. Optional
  extras are each tried once (new ones in later versions too).
- SongRec: used from wherever it is (system, Flathub for you or for everyone,
  cargo). Otherwise from Flathub, added for you only if it isn't set up (a
  system-wide or filtered Flathub, like Fedora's, is left alone); without
  Flatpak, the system's package if there is one. If none works, lyrics still
  work for songs found by name.
- Fedora Silverblue / Kinoite and other image-based systems: the installer
  lists what's missing (for rpm-ostree or a toolbox) instead of failing.

## What's new in 0.26.6

- The media card's sound waves moved off the progress bar into the card's
  background: soft see-through hills behind the playback buttons, in the
  album art's colour, flowing in from the right and rising on each beat. The
  progress bar is a plain bar with a round knob.

## What's new in 0.26.5

- Control centre tiles look right at any size: toggles one cell wide and
  taller stand as pills (round icon on top, the name underneath when it
  fits); the battery stands as a gauge filling from the bottom with the power
  modes in a column (and in a row only when there's room); the media card
  shrinks to the art with play / pause (and next / previous when tall).

## What's new in 0.26.4

- The Sound tile is gone (the volume bar does the same).
- The Windows, GNOME and macOS looks of the control centre can be edited
  too: the pencil (Windows: bottom strip; GNOME: top row; macOS: "Edit
  Controls…"), or right-click › Edit buttons. Drag a button onto another to
  swap them, the red badge removes one, the corner makes it wide (or narrow
  again), hidden ones are added back underneath, Reset brings the look's own
  layout back. Each look keeps its own arrangement.

## What's new in 0.26.3

- The media wave really moves to the beat now: it listens to the bass (kick
  drums) and the hills are the music's last few seconds flowing away from the
  knob, rising on every hit. If it can't hear the music (no parec or
  pw-record) it sways gently, and Dusk's log says why.
- **Media source**: the app playing is a chip at the top left of the media
  card, with its icon. With several things playing, click it to pick which
  one the card shows and controls (or open its window).
- Lyrics are found far more often: "Artist - Title" video names are split
  first, other lengths of a song (music videos with intros) are accepted, and
  songs not found earlier are looked up again (after a few hours, not a day).
- SongRec is installed by ./build.sh install (from Flathub when the system
  has no package), so songs with odd names are recognised by ear.
- Moving and resizing control centre tiles is smooth (drawn at most once a
  frame while dragging).
- The screenshot notification says its size in pixels.

## What's new in 0.26.2

- The media card's progress bar is now like Samsung's: a thick bar in a
  light colour taken from the album art, with soft see-through hills of sound
  on it that rise and fall slowly with the music, and a round knob (it moved
  far too fast before).
- Two things playing at once: the cards now stay on one of them (the one
  shown while it plays, else the one that started playing last) instead of
  flicking between them, and their buttons act on that one.
- Control centre: changing its look (or back) while editing tiles, or with a
  list or a drag open, starts it clean instead of leaving it blank.

## What's new in 0.26.1

- Builds again (the new parts' headers weren't seen where Dusk shuts down).

## What's new in 0.26.0

- **Screen recording**, in the screenshot tool like GNOME's: a photo / video
  switch on its bar (or C and V), then an area, a window or a screen, with
  the delay, and sound: none, what you hear, or the microphone (the speaker
  button). Shift+Ctrl+Alt+R records the screen at once. While it records, a
  red pill with the time sits on the panel; click it (or press the keys
  again) to stop. Saved in Videos/Screencasts, with a notification showing a
  frame of it (Play / Show in folder). Uses wf-recorder with the graphics
  chip's own encoder when it can (else x264, OpenH264 or VP9). Settings ›
  Screenshots & recording: sound, 30 / 60 fps, quality.
- **Sharper screenshots**: always taken at the full pixel density of the
  sharpest screen in them, and their notification shows the picture big
  (any app's notification with a picture does too) instead of a tiny icon.
- **Samsung-style media card**, in the control centre and the panel's media
  popover: the album art fills the card, the app and a **Media output**
  button (pick the speakers / headphones) on top, the title (it **scrolls
  sideways when it's too long**) and artist, and a wavy progress line that
  **moves with the music's beat** (it listens to what's playing; drag it to
  seek). Album art from streaming apps and browsers is fetched too.
- **Synced lyrics**: looked up on LRCLIB when a song starts (and, with
  SongRec installed, recognised by ear when its name finds nothing), shown on
  the screen in time with the music in **eight styles**: Classic, Karaoke,
  Subtitles, Glow (pulses with the beat), Scrolling, Bounce, Typewriter and
  Comic. The microphone button on the media card turns them on; under the
  panel's media card, Earlier / Later and Slower / Faster fix one song's
  timing (sped-up and slowed versions), and Settings › Media & lyrics sets
  the delay and speed for all songs, the style, place and size.
- **Control centre looks**: Dusk's own (now Samsung-like: round toggles, wide
  pill bars for brightness and volume), or like **Windows 11**, **GNOME** or
  **macOS** quick settings (Settings › Control centre › Look, or right-click
  the control centre).
- **Bluetooth on other computers**: the adapter is found from the kernel, so
  a computer whose Bluetooth service is switched off (common on Arch and
  others) shows "Service off" instead of "No adapter", and switching
  Bluetooth on starts the service (it asks for your password). The installer
  turns the service on when there's an adapter, and installs bluez.
  dusk-doctor says what's missing.

## What's new in 0.25.8

- Settings › About › Updates: a check now ends with a clear coloured line
  (✓ up to date, with the time; ★ a new version is out; ✕ couldn't reach
  GitHub). **Other versions** opens a list in the card: every version on
  GitHub with a Newer / Installed / Older badge and an Install or
  "Go back to this" button.

## What's new in 0.25.7

- `dusk-update` never installs an older Dusk than the one you have by
  itself: when GitHub's newest is older or the same, it says so and stops.
- Going back (or to any version): Settings › About › Updates ›
  **Other versions…** lists every version on GitHub (newer / older / the one
  you have) and installs the one you pick. In a terminal:
  `dusk-update --list` and `dusk-update --version 0.25.4`.

## What's new in 0.25.6

- `dusk-uninstall` (or `./build.sh uninstall`) removes Dusk completely and
  puts back what it changed for other desktops: GNOME's appearance settings
  (theme, icons, cursor, fonts, light/dark, accent, window buttons), Dusk's
  blocks in GTK's files, the qt6ct setup, kitty / foot fonts, the browser
  dark-mode flag, the cursor files, Flatpak permissions, and Dusk's own
  settings, data, sources and downloaded fonts and themes
  (`--keep-themes` keeps those). `-y` doesn't ask.

## What's new in 0.25.5

- Overview: no more stale, blocky "hollow" patches in the blurred
  background. The blur was redrawn only where something under it changed,
  which left squares of old picture round the edges; while the overview is
  up (and while a window is being dragged) the whole screen is redrawn
  instead. Not during games.

## What's new in 0.25.4

- Pinned apps on the panel can be dragged into a new order: press one and
  slide it along; the others make way, and the order is kept when you let
  go. A plain click still opens or brings up the app.

## What's new in 0.25.3

- Lock screen: the password dots have room between them.
- Lock screen: notifications waiting show under the password as the app,
  its icon and how many ("Discord · 3 new"), never what they say or their
  pictures. More than three apps: "N more from other apps". Off in
  Settings › Lock screen › Waiting notifications.

## What's new in 0.25.2

- Settings › Appearance › Fonts: the preview now changes as you change the
  sizes (it didn't before), shows the interface font as a heading and as
  text, and while a font list is open it shows the font under the pointer,
  so you can see a font before choosing it.

## What's new in 0.25.1

- The interface font now changes in Settings too, at once, like everywhere
  else (Qt's style sheet kept the old family on every widget).

## What's new in 0.25.0

- **Back to the classic Dusk Settings.** The new Qt Quick (QML) Settings
  from 0.24 is removed; `dusk-settings` is the Qt Widgets one again, with
  every page and all the fixes made to it so far (no phone & tablet page,
  crash log, smoother sliders and steppers). Qt Quick is no longer needed
  to build Dusk.

## What's new in 0.24.5

- Settings: the list of pages no longer stretches across the whole window
  and hides the page; it keeps its width at the side.

## What's new in 0.24.4

- Colour pickers in Settings start on the colour already chosen.

## What's new in 0.24.3

- Settings pages really show their settings now: a page setting was named
  `top`, a name Qt keeps for itself, so every page made from the settings
  list failed to load (found thanks to `settings-qml.log`).

## What's new in 0.24.2

- Settings pages were blank: the page wasn't being told which settings to
  show. Fixed. A page that ever fails to load now shows the reason in the
  window instead of staying empty.
- "Desktop & clipboard" and "Mouse & touchpad" show their "&" again.

## What's new in 0.24.1

- **Settings opens again.** A typing mistake in the new Settings' main page
  stopped the whole window from loading. Fixed, and now if the new Settings
  ever can't start, the classic one opens in its place, so Settings always
  opens. Anything Qt complains about is written to
  `~/.cache/dusk/settings-qml.log` (and shown by `dusk-doctor`).
- A Settings stuck while starting no longer blocks the next one from opening.
- The installer looks for packages again on every build, and if Qt Quick is
  missing, `dusk-settings` runs the classic Settings instead of nothing.

## What's new in 0.24.0 (smoothness, pass two: a new Settings)

- **Dusk Settings is rebuilt with Qt Quick (QML)**: drawn on the GPU, so it
  scrolls and animates smoothly and stays sharp at any scale; pages are made
  only when you open them; every change is one quick write (sliders when you
  let go, − / + a moment after the last press). Settings are described once
  (settings-qml/schema.json, made from the old app's code by
  tools/settings-schema.py) and every page is drawn from that.
  - Pages with their own parts: Appearance (accent colours, fonts, wallpaper
    picture, icon and cursor themes), Panels (place, look, what's on it and in
    which order, pinned apps, clock), Keyboard (layouts), Lock screen (styles,
    your picture), Default apps, Startup apps, Date & time, About (updates,
    report, reset).
  - Search goes over every setting.
  - Not moved over yet: the screens' arrangement (Displays), a second panel,
    adding startup apps, and the lock screen preview. Those pages have a
    button that opens the **classic Settings** (still installed, as
    dusk-settings-classic).
- **Phone & tablet** is gone from Settings.
- Needs Qt Quick: build.sh installs it (Fedora: qt6-qtdeclarative-devel).

## What's new in 0.23.1

- On-screen keyboard: arrow keys on its bar (held: they repeat); a handle to
  move it anywhere (the size button puts it back); and the clipboard opens
  inside it: tap an entry to put it in (short text is typed, pictures, files
  and long text are pasted).
- The clipboard history is kept after logging out or restarting, by default
  (pictures too now: the newest eight).

## What's new in 0.23.0 (smoothness, pass one)

- **Games**: while a game (or video) is full screen and focused, Dusk gets
  out of its way: nothing of Dusk's is drawn over it (the rounded screen
  corners go), so the GPU can show it directly, and Dusk's background checks
  (Wi-Fi, Bluetooth, sound, camera and microphone) pause. Games that ask to
  skip vsync may (less input lag, as on KDE): Settings › Windows › "Games can
  skip vsync". Explicit sync (smooth frames on NVIDIA) was already on when
  the driver supports it; dusk-doctor now says whether it is.
- **No more ghosts when moving windows**: dragging a window out of maximised or
  into a tile no longer leaves a copy of it behind where it was; the copy that
  fades over a window after it changes size is cut short as soon as you grab
  the window again; and a slow app no longer keeps a copy on screen for half
  a second. Faded copies are switched off entirely, and their shadows shrink
  with them (no dark smudges round small windows in the overview).
- **The overview makes way for new windows**: an app that opens while the
  overview is up closes it and shows, as in GNOME.
- **On-screen keyboard**: compact in the middle by default. The new button on
  its bar (and Settings › Keyboard › Size) goes round normal, split in two,
  and the whole width.
- Settings: pressing + / − several times is one change for Dusk (saved a moment
  after the last press), and changes are saved in bursts.

## What's new in 0.22.1

- AppImages (and other apps that bring their own libraries) follow the cursor
  theme: Dusk sets XCURSOR_PATH (your icons, then the system's) and writes the
  cursor into GTK's settings.ini for GTK apps that can't read GNOME's
  settings.

## What's new in 0.22.0

- **An on-screen keyboard** (Super+K, or the new Keyboard button for the
  panel): along the bottom of the screen above the windows, typing into the
  app you're in without taking the focus. Letters, a symbols page (!#1), a
  shift that's one letter or, tapped twice, Caps Lock, Backspace that
  repeats while held, and emoji in six categories. On a wide screen it splits
  into two halves at the sides, like Samsung's. Its bar has emoji, the
  clipboard, Settings and hide. Keys use your keyboard layout; anything the
  layout can't type (emoji) is typed through a keymap made for it.
- **Bigger scales fit**: windows that no longer fit the screen after the scale
  goes up are made smaller and brought back on screen, and Dusk Settings
  opens no bigger than the screen (and can be made much smaller).
- **Settings crashes are recorded** in ~/.cache/dusk/settings-crash.log
  (dusk-doctor shows them), so they can be found and fixed.

## What's new in 0.21.9

- Lock screen: every style shows your picture now (Stacked, Clock face and
  Minimal had it off). The background can be "Custom picture" or "Custom
  picture, blurred" (with the Blur slider), besides your wallpaper (plain or
  blurred) and a colour.

## What's new in 0.21.8

- New panel defaults: app menu, apps, space, workspaces, space, now playing,
  clipboard, tray, status, clock (the trash is on the desktop); the battery
  as its icon and a coloured percentage; calendar weeks start on Sunday.

## What's new in 0.21.7

- New defaults for window title bars: the Drawn buttons, and no maximise
  button (double-click the title bar or drag it to the top to maximise).

## What's new in 0.21.6

- **Apps on the panel, like a dock**: pinned apps and open windows are one
  widget now. Pinned apps come first, in their order, with a dot when open
  (and their windows next to them); other open windows follow. A pinned app
  that isn't open starts it. Right-click a window to pin or unpin its app.
  Panels with the old separate "Pinned apps" widget are moved over.
- **Text size works everywhere**: Dusk's own panels, menus and cards and the
  Settings app follow Settings › Appearance › Fonts › Text size.
- **Settings is quick again**: sliders save when you let go (not at every
  step, each of which made Dusk restyle everything), and nothing is restyled
  when the look didn't change.
- Maximised (and full-screen) windows have no outline.
- A panel with its background at 0% casts no shadow (and the full-width
  panel's edge line follows Lines: None).
- Show desktop is a proper button with an icon.
- Window titles on the panel are off by default.
- Settings › Panels: the flexible space chips aren't black any more.

## What's new in 0.21.5

- After installing or updating while Dusk is running, the installer asks
  whether to log out now to start the new version (or sends a notification
  saying to log out and back in).

## What's new in 0.21.4

- The camera indicator goes off again: the webcam driver's count isn't zero
  when the camera is idle (other parts of the system hold it), so "in use"
  now means above its idle count.
- The installer starts with a plain text header.

## What's new in 0.21.3

- First install straight from GitHub: `install.sh` at the top of the
  repository downloads the newest zip and installs it.
- Updates check the version *inside* the newest zip (a renamed zip can't make
  Settings offer the same version again and again).
- The installer shows Dusk's real logo.

## What's new in 0.21.2

- **Updates from GitHub**: upload each new zip as it is
  (Dusk-0.21.2-source.zip...) to github.com/lance-ui/dusk,
  into the repository or onto a release. Settings › About › Updates checks
  for a newer one and "Update to …" downloads it, unpacks it into
  ~/.local/src/dusk (hidden) and installs it in a terminal window. Also from
  a terminal: `dusk-update`. The version comes from the zip's name, or from
  inside it when the name has none. Installing from ~/src hides that folder
  in Files (~/.hidden).
- **The installer** shows Dusk's logo, a clear warning that Dusk is written by
  an AI and may have bugs, and asks before going on (`-y` skips the
  question). It works on Fedora (tested) and, untested, on Debian / Ubuntu
  and Arch (their package names; Dusk needs wlroots 0.20).
- **Fonts**: Google Sans (not Flex) for the interface, Google Sans Code for
  terminals; apps' text is 11 pt by default.
- **Settings no longer freezes** when picking from a list: font lists no longer
  load every font to draw each name, changes apply after the list closes,
  and the whole app is only restyled when its look really changed.
- Appearance: the pure black (OLED) dark style is gone; Comic is the look.
- About: storage free counts every partition of the computer's drives;
  only "Reset all settings" is left at the bottom.
- **Steam and other X11 apps follow the cursor theme** (the "default" theme in
  ~/.icons, the X resources and XCURSOR_THEME all say Dusk's cursor).
- Less work in the background while gaming: the camera / microphone check is
  cheap now (the webcam driver's own count) and pauses while something is
  full screen.

## What's new in 0.21.1

- **Dusk Settings opens again.** An old Settings could hang while closing
  (its picture threads), keep holding the "one Settings at a time" lock with
  no window, and every later one stepped aside for it. Settings now quits at
  once when closed, says when its window is really up, and a new one ends a
  stuck one instead of doing nothing.
- **Notifications stay until you clear them, also after a restart.** No more
  History tab: the control centre's list itself is kept
  (~/.local/state/dusk/notifications-open) and comes back when you log in,
  with the bell on. Settings › Notifications › "Keep them after a restart".
- **Overview**: switching workspaces slides the windows out one way and the
  next ones in from the other; workspace changes are drawn once instead of
  several times in a row (the flicker); the card's shadow, which showed as a
  dark smudge down one side, is gone.

## What's new in 0.21.0

- **Dusk's login screen is gone; GNOME's (GDM) is back.** `./build.sh install`
  puts GDM back by itself if Dusk's was in use (greetd off, GDM on, greetd's
  old settings restored) and removes everything it had installed
  (dusk-greeter, dusk-login-sync, /etc/dusk, the copies in
  /usr/local/share/dusk-login). Restart once afterwards.
- **Lock screen styles** (Settings › Lock screen): a live preview at the top,
  and six presets drawn as little lock screens: Classic, Stacked (big hours
  over the minutes, the minutes in your accent), Clock face (hands), Corner
  (a big clock bottom left, you on a card at the side), Outline and Minimal.
  Then change anything: where the clock goes (top middle, top left, bottom
  left), its style (digital, stacked, outline, clock face), its own font,
  weight and size; the password field in the middle, lower down or on the
  right, on a frosted card or not; music and the Wi-Fi / battery corner on or
  off. Everything is laid out in one place so nothing overlaps (the field
  moves down while music is showing).
- **Fonts**: Google Sans Flex for the interface and Google Sans Code for
  terminals and code, by default (free, OFL; `./build.sh` downloads them into
  ~/.local/share/fonts/dusk, `./build.sh fonts` gets them again). Settings ›
  Appearance › Fonts picks both (each listed in its own lettering, with a
  preview) and their sizes; GNOME/GTK apps, Qt apps, kitty and foot follow.
  If they're missing, a button there downloads them.
- **Sliders** are drawn like the rest of Dusk now: a rounded track filled with
  the accent, a white knob with a shadow that grows when you point at or drag
  it, click anywhere to jump, and the value in a little pill.
- **Panel widgets, reworked**: the panel's widgets as chips that wrap instead
  of a scrolling strip. Drag one to move it (the others make room), × takes
  it off, Ctrl+← → moves the picked one; underneath, everything you can add,
  one click each (added after the picked chip).
- Lists in Settings (keyboard layouts, pinned apps) are only as tall as what's
  in them; scroll bars match the rest of the app.
- **The notification bell** is in the accent's opposite colour (across the
  colour wheel), so it stands out from everything accent-coloured.

## What's new in 0.20.8

- **Copying works again in every app**: putting the newest clipboard entry
  back when the clipboard emptied used a newer serial than the app's next
  copy, and wlroots turned that copy down (apps that clear the clipboard
  just before copying, like Chromium-based ones, couldn't copy at all). Now
  it waits a moment, only puts it back if it's still empty, and never
  outranks an app's copy.
- **The notification bell is its own small icon**, just before the network
  icon, sitting high, in a vivid accent colour.
- **Tray icons in colour, found from the app behind them**: the program that
  owns a tray icon (a Flatpak's id, or the program's name) picks the
  installed app, so Heroic and other Electron apps get their colour icon.

## What's new in 0.20.7

- **One Settings window**: opening Settings again (the app menu, a panel's
  "Panel settings…", the control centre…) brings the open one forward, from
  another workspace too, and shows the page asked for.
- **Tray icons**: Electron apps (Heroic, Discord…) are matched to their app by
  name, so their colour icon is found; an icon with light and dark parts
  (Heroic's shield with its "!") is no longer turned into one flat shape.

## What's new in 0.20.6

- **App status icons in colour**: when an app gives the tray a grey "mono"
  icon (Steam's steam_tray_mono, for one), its own colour icon is shown
  instead. Only when there's none is the grey one kept (in the panel's text
  colour if it would vanish against the panel).

## What's new in 0.20.5

- **Arrow keys on the lock and login screens**: Left / Right move the cursor
  through the password (Ctrl: to the start or end), Home / End, Delete and
  Backspace where the cursor is, typing goes in at the cursor. On the login
  screen Up / Down pick who's logging in and Ctrl+Up / Down the desktop.
- **The music on the lock screen**: every button lights up when pointed at,
  a progress bar shows how far in it is (click it to jump there), it moves on
  every second, and the music keys work while it's locked.
- **The notification bell** sits small on the top left of the Wi-Fi icon, in
  a vivid version of the accent colour.
- **Login screen wallpaper**: if its settings don't name a picture it can
  read, it uses the copy dusk-login-sync made. It also says what it read
  (journalctl -b -u greetd, and dusk-doctor shows it).

## What's new in 0.20.4

- **The lock and login screens react to you**: the password field lights up
  with an accent ring when picked and has a blinking cursor; the eye shows
  or hides what you've typed; "Caps Lock is on" shows under it; what's
  playing is on the lock screen (cover, title, previous / play-pause / next);
  Wi-Fi, volume and the battery are top right.
- **The login screen remembers** who logged in last and each person's last
  desktop (Dusk, GNOME...), kept in more than one place in case one can't be
  written.
- **App status icons show in light mode**: grey or white ones (Steam's, for
  one) are drawn in the panel's text colour when they'd vanish against it.
- **Apps started at login show in the tray**: they start a moment after Dusk,
  once the tray is there, and the tray says it's here a few more times and
  picks up any that are waiting.
- **The notification bell is small**, with a deeper, more vivid accent dot.
- **Notifications aren't grouped** in the control centre: one card each
  (Settings › Notifications › Group by app turns it back on).

## What's new in 0.20.3

- **Scale goes up to 200%** (Settings › Displays), which is what Dusk's own
  panels, menus and Settings are made for; anything set higher becomes 200%.
- **Settings fits narrow screens** (a phone or tablet screen, or a big
  scale): lists and fields give way down to half their width, and titles
  wrap, instead of running off the right edge.
- **A bell on the panel** (with an accent dot) when notifications have come
  in since you last opened the control centre; it goes when you open it.
- **Microphone and camera in use**: an amber microphone or camera on the
  panel's status icons while an app is recording from the microphone or has
  the camera open.

## What's new in 0.20.2

- **More for the lock and login screens** (Settings › Lock & login screen):
  the clock's size and colour (white or your accent), where the password
  field sits, your picture's shape (round or a rounded square), your name on
  or off, "Just the clock at first" (the field shows when you press a key or
  click, and goes again after 20 s with nothing typed), and the restart and
  shut down buttons on the login screen.
- **Your picture is round** (the top of the circle was cut flat), and you can
  **change it in Settings** (You › Your picture: Choose… / Remove). It's saved
  as ~/.face and given to the system too, so other login screens show it.
- **Settings tidied**: "Show more settings" only on pages that have more;
  switches and lists line up in one column (rows without a reset button were
  further right); the Power page's menu card says what it is; no more talk
  of other screen lockers being needed.
- **The power menu opens by its button** in the app menu, wherever the app
  menu is (it went to the bottom of the screen).

## What's new in 0.20.1

- **The lock and login screens are smooth now.** Every key and every bit of
  pointer movement used to redraw whole-screen pictures for every screen,
  with a full-screen blur running on top. Now the background is made once
  (blurred and darkened already), and the clock, the password form and the
  login screen's bar are small separate pictures: typing redraws just the
  form. Changing a setting no longer rebuilds the whole thing.
- **The password form is on your main screen** (the laptop's, or the one
  picked in Displays), not wherever the pointer was; at the login screen the
  pointer starts there too.
- **The login screen gets your wallpaper**: pictures are copied to
  /usr/local/share/dusk-login (where the login screen may read them), .jxl
  and other pictures `file` doesn't know are accepted, and what was copied is
  logged in /var/log/dusk-login-sync.log (dusk-doctor shows it).
  `./build.sh install` copies your look again when Dusk's login screen is on.
- The login screen never has panels or desktop icons under it.

## What's new in 0.20

- **A lock screen and a login screen that look the same**, set up together in
  Settings › Lock & login screen: the background (your wallpaper blurred or
  as it is, a picture of its own, or a colour), how blurred and how dark,
  the clock (big in the middle, top left, or none; light or bold), the date,
  a line of your own, and your picture.
  - The lock screen is Dusk's own now: Super+L, Lock in the power menu, when
    the screen turns off by itself, before the computer sleeps (lid too), and
    `loginctl lock-session`. Your password is checked like at login (PAM);
    apps carry on underneath but get no keys or clicks.
  - The login screen is Dusk too (through greetd), opt-in:
    `./build.sh login-screen`, then restart. It shows who's logging in (arrows
    when there are several of you), which desktop to start (Dusk, GNOME...),
    and restart / shut down. If it can't start, a plain text login appears so
    you can still log in. `./build.sh login-screen-off` brings GNOME's back.
    Changing the look in Settings copies it there by itself.
- **The logo is round**, and drawn the same everywhere (Qt didn't draw the
  rounded corners).
- **About › Storage** shows the whole drive (and any other drives), not just
  the system's partition.
- **Phone & tablet is a few plain steps**: install (one button), your
  phones (each with Show screen), and Add a phone: "With a USB cable" or
  "Over Wi-Fi, no cable", each with numbered steps. Pairing tells you when
  something's wrong instead of hanging.

## What's new in 0.19

- **A new logo**: the sun half set on the horizon, two bands of evening
  through it, and the first star. It's the app menu button now (in your
  accent colour), the login screen's icon, and Dusk Settings has its own
  icon (a cog round the setting sun) instead of a borrowed one.
- **Phone & tablet works by itself**: phones plugged in (or paired) are
  found and listed by name, each with "Show screen", and "Use without
  cable" switches one to Wi-Fi with no address to type. Pairing over Wi-Fi
  with a code (Android 11+) needs no cable at all. A button installs scrcpy
  and adb; the steps to turn on USB debugging are written out.
- **About is simpler**, like a phone's "About phone": the computer's name
  (Rename), then what it is (processor, graphics, memory, storage, system),
  then Keyboard shortcuts, Software information and Made with, each opened
  when you want them. "Copy a report" puts dusk-doctor's report on the
  clipboard.
- **Panel widgets are a strip like the panel**: drag them along, or pick one
  and use the ◀ ▶ arrows; Delete takes it off; new ones go after the one
  picked. The default panel: app menu and open apps at the start, the
  workspaces ("2 / 3") in the middle, the clipboard, trash, tray, status
  icons and clock at the end.
- **Papirus icons and the Bibata Modern Classic cursor** by default
  (./build.sh installs them, or downloads them if Fedora doesn't have them).
  Papirus follows light and dark.
- **Wallpaper transitions**: the wipe is diagonal now, and there are three
  circles: from the middle, from the pointer, and an iris (the old picture
  closes into the middle).
- **The pointer pauses longer between screens**, so it doesn't slip across
  by accident (carrying a window across stays easy).
- **Shadows in Settings are even** all round the cards (they were cut off
  in a hard line on the right), and drawn without Qt's slow shadow effect.

## What's new in 0.18.9

- **Settings opens quickly and doesn't slow everything down.** It used to
  read every wallpaper picture at once, one thread each (GNOME's .jxl
  wallpapers are slow to read), which made the whole computer stutter. Now
  two workers do it in the background at low priority, and the small copies
  are kept in ~/.cache/dusk/thumbnails, so it's instant after the first time.
- **Icon theme** (Settings › Appearance › Icons and cursor): for the panels,
  menus and apps (GTK and Qt), like the cursor theme.
- **Overview button**: the grid, squares, circle and "Activities" looks are
  gone (they become dots). The rest use the accent colour for the
  workspace you're on, like Dusk's other controls.
- **The control centre closes smoothly** next to another screen (it stopped
  at a quarter visible, then vanished).
- The overview's search field and labels no longer show a dark box round
  their rounded shape.

## What's new in 0.18.8

- **Clipboard: the newest thing is picked first.** When the clipboard opens,
  the newest entry (text, picture or files) is picked: Enter puts it in the
  clipboard, Up/Down pick another, Delete forgets one. And the newest entry
  is always what you paste: after logging in, and when the app it was copied
  from closes (which used to leave the clipboard empty).
- **Overview button styles that show your workspaces**, like GNOME's, in
  several kinds: dots, numbers (1 2 3), Roman numerals (I II III), letters
  (A B C), short lines, rings, small squares, or "2 / 4". The one you're on
  is the wide, filled one, and it glides along when you switch.

## What's new in 0.18.7

- **GNOME logs in after Dusk** (for real this time). Two things were still
  left behind: graphical-session.target, kept up by the desktop portals after
  Dusk's own target stopped (GNOME won't start while it's up), and apps
  started from Dusk that kept the old login "closing". `dusk-session` now
  stops the first and ends the login with Dusk, which also hands the sound
  card back (PipeWire showing only "Dummy Output").
- **No more crash when logging out**: each screen's part of the scene was
  freed twice on the way out.

## What's new in 0.18.6

- **Other desktops (GNOME, KDE) log in again after Dusk.** While it runs,
  Dusk tells your user session it's the desktop, and it never took that
  back (a crash couldn't). The session outlives logging out, so the next
  desktop found a graphical session "already running" and gave up. The login
  screen now starts Dusk through `dusk-session`, which takes it all back
  however Dusk ends, and also first thing next time.
- **The control centre stays on its screen**: with another screen on the
  side it slides from, it fades in with a short slide instead of passing
  through the other screen.
- `dusk-doctor` checks sound (PipeWire, WirePlumber, the output device) and
  what's left in your user session that would stop other desktops.

## What's new in 0.18.5

- **Dragging windows to another screen works again** (especially on a
  touchpad). "Pause between screens" used to need a long push into the edge
  in one go while carrying a window. Now a short push gets it across, or
  hold it against the edge for a moment and nudge it on.

## What's new in 0.18.4

- **Snap layouts only at the top centre**: dragging a window to the top of the
  screen shows the layouts bar only around the middle. The rest of the top
  edge maximises as before.
- **Corner tiling**: drag a window into a corner (or along an edge close to
  one) and it takes that quarter of the screen.
- **Screenshot tool, window mode**: the open windows are listed as chips
  under the buttons. Point at one to bring it forward and see what will be
  taken; click it to take it.
- **Displays › Arrangement draws screens at their real size** (from what
  each screen reports), so a 21.5" monitor looks bigger than a 16" laptop.
  Where they touch and how they line up still sets the layout.

## What's new in 0.18.3

- **A nicer battery and power tile** in the control centre: the charge fills
  the tile like a gauge, in the power mode's colour (made from your accent:
  warmer for Performance, greener for Power saver, the accent itself for
  Balanced, red when the battery is low). The three modes are buttons at its
  right end: click one to switch straight to it.
- Batteries that are plugged in but not taking charge say so ("Plugged in,
  not charging") instead of "charging", and a battery that only reports its
  percentage isn't shown as 0%.

## What's new in 0.18.2

- **Fixed the wallpaper crash.** The "zoom" transition (also picked by
  "random") worked out a crop that was a hair below zero at its last step,
  from rounding, and wlroots stops the whole desktop on that. Every crop is
  now kept inside the picture.
- **The on-screen display follows its orientation for everything**: with
  Upright, Microphone, Caps Lock, Num Lock and layout changes are a short
  upright pill too, not a wide one.
- Overview: window copies have their title bars again, and the "New" card
  stays clear of the screen's edge.

## What's new in 0.18.1

- **Window screenshots**: the window you point at in the screenshot tool
  comes to the front, so nothing covers it.
- **Wallpapers to choose from** (Settings › Appearance › Wallpaper) come from
  ~/Pictures/Wallpapers, the system's wallpaper folders
  (/usr/share/backgrounds, /usr/share/wallpapers and the same under
  ~/.local/share) and any folder you add there ("Add folder…"). Up to 160
  are shown, read in the background.

## What's new in 0.18

- **Crash reports**: if Dusk crashes, `~/.local/state/dusk/crash.log` says
  where and what it was doing just before, and `dusk.log.old` has what it
  printed in that session. `dusk-doctor` shows both. (Changing wallpapers
  still crashes for some; this is to find out exactly why.)
- Only the wallpaper keeps its picture after it's on the GPU now (0.15 did
  it for everything Dusk draws), in case that's what goes wrong.
- **Battery and power mode are one control centre tile** on laptops: the
  charge on top, the power mode under it; click to change the mode.
- Arqen shell is no longer in the credits.

## What was new in 0.17

- **Draw on a screenshot before taking it**: in the screenshot tool, the pen
  on the bar (or D) draws inside the area; the colour dot changes colour,
  undo (or Z / Backspace) takes the last line away. The lines are in the
  saved and copied picture.
- **Title bars**: shorter (36 px), in the window colour (the grey of Dusk
  Settings' page and GNOME apps' windows), and without the window's name
  (Settings › Windows › Title bar › Show the window's name brings it back).
- **Standing panels show the same as lying ones**: the day and date under
  the time, and the battery percentage under the battery when it's on.
- **Battery level**: several batteries are added up and peripherals left
  out (a stray 0% came from one of those).
- **Accent from wallpaper is instant from Settings**: Settings picks it from
  the thumbnail and writes it with the picture; other apps restyle at once.
- **Quick wallpaper changes** no longer stack transitions: one still coming
  in finishes and the newest replaces it.

## What was new in 0.16

- **Touchpad gestures** (Settings › Mouse & touchpad › Gestures): three
  fingers up opens the overview, down closes it, left/right switch
  workspaces. Every direction for 3, 4 and 5 fingers can do one of 18
  things (or nothing). Two fingers scroll. Gestures you haven't set go to
  the app (pinch to zoom in a browser).
- **Super+Tab** opens the overview.
- **Open apps** (the window buttons, renamed) shows the apps on every
  workspace; clicking one on another workspace takes you there (they're a
  little faded).
- **Removing workspaces**: the ✕ next to "Workspace 2 of 3" in the overview
  (its windows go to the workspace before), besides the panel menu.
- **Battery**: the battery icon is the default again, optionally with the
  percentage coloured green / amber / red (Settings › Panels). The setting
  was never read before.
- **Music controls react at once**: Dusk follows the player
  (`playerctl --follow`) instead of asking now and then.
- **The app menu's scroll bar** can be dragged (or clicked to jump).
- **Accent from wallpaper** changes as soon as the new wallpaper arrives;
  other apps restyle after the transition.
- **Changing wallpapers quickly**: picture decoding is now one at a time
  (the wallpaper thread and Dusk could decode at once), which is the most
  likely cause of the crash. If Dusk still quits, `dusk-doctor` now shows
  where it crashed.
- **The overview** redraws less (titles and ✕ buttons are only drawn when
  they change).

## What was new in 0.15

- **Wallpaper transitions work now.** The scene let go of each picture once
  it was on the GPU, so Dusk never saw an old wallpaper to fade from (and
  window animations lost their title bars the same way). Pictures are now
  kept while they're shown. The end of a transition no longer re-uploads
  the picture, and "Accent from wallpaper" is worked out on the wallpaper
  thread and applied after the transition (apps restyling mid-transition
  made it stutter).
- **GNOME apps get your accent again.** The colour was passed to the theme
  script unquoted, and the shell read "#…" as a comment: GNOME apps got an
  empty accent (grey/white switches). `dusk-doctor` now flags a broken one.
- **Workspaces you make**: "New" at the right of the overview (or drop a
  window on it), New/Remove workspace when you right-click the overview
  button, or the number in Settings › Workspaces. GNOME's automatic mode is
  still there.
- **The overview** has no row of workspace pictures any more: the other
  workspaces peek in at the sides, with "Workspace 2 of 3" under the search.
  Search finds open windows only.
- **Super opens the app menu again**; Super+S opens the overview (or choose
  in Settings › Workspaces).
- **Notification history** moved from the clipboard to the control centre
  (History, next to Settings).
- Copying: Dusk reads a copy into the clipboard history a moment later, so
  it doesn't compete with an immediate paste.

## What was new in 0.14

- **Workspaces, like GNOME's**: as many as you use plus an empty one at the
  end (or a fixed number). Super+Page Up/Down or Ctrl+Alt+Left/Right switch,
  with Shift the window comes along; windows slide sideways.
- **The overview** (tap Super, Super+S, the top-left hot corner or the
  overview button on a panel): the workspace's windows side by side on a card
  of the wallpaper, the workspaces in a row above, the next ones peeking in
  at the sides. Type to search windows (on every workspace) and apps. Click a
  window to go to it, middle-click or ✕ to close it, drag it onto a workspace
  to move it there; scroll to switch workspaces.
- **Overview button** for panels, in seven looks: GNOME's workspace dots,
  grid, squares, circle, "Activities", your distribution's logo, or a picture
  of your own. Scrolling over it switches workspaces.
- **Trash** on the panel and on the desktop (both can be removed): opens the
  trash in whatever file manager you have, shows when something is in it,
  empties it (after asking). Drop desktop icons on it to throw them away. A
  Home folder icon can go on the desktop too.
- **Screenshots include what's open**: the control centre, menus, the app
  menu and the overview stay on screen and are in the picture.
- **Sharper icons**: the best size of an icon is picked (scalable first),
  instead of the first one found, so logos aren't blown up from small
  pictures.
- **App menu scrolls** a row per wheel notch (and smoothly on a touchpad).
- **Browsers and games**: explicit sync (linux-drm-syncobj), presentation
  timing, xdg-activation, pointer lock/confine and raw mouse movement,
  keyboard-shortcut inhibit, xdg-foreign and more. Explicit sync is the usual
  cure for Chrome/Vivaldi/XWayland flicker (above all on NVIDIA).
  `dusk-doctor` now checks the graphics driver and how your browser runs.
- **Dusk Settings is back to its own design** (Qt), with only the title bar
  following GNOME's look. Everything 0.13 added to Settings is still there,
  plus a Workspaces page.

## What was new in 0.13

- Dusk Settings was briefly rewritten in GTK 4 (undone in 0.14). Changing
  the wallpaper no longer makes Settings lag: pictures are loaded and shrunk
  in the background and kept for next time.
- **Title bars look like GNOME's**: bold centred title, round buttons, 46 px
  tall by default. The old styles are still in Windows › Title bar.
- **Drag files from the desktop into apps** (Files, a browser upload box,
  an editor), not just from apps onto the desktop.
- **Accent colour in GNOME apps**: libadwaita apps now use your exact accent
  (switches, buttons, selections), not the nearest of GNOME's nine colours.
  `dusk-doctor` checks it.
- **Opacity and blur for every part of Dusk** (panel, menus, control centre,
  notifications, OSD, cards). 75% opacity and full blur by default. Panel
  background opacity now goes all the way down to 0%.
- **Phone & tablet** (new Settings page):
  - Use a phone or tablet as a second screen over Wi-Fi or a USB cable. Dusk
    adds a virtual screen and shares it with wayvnc; connect with any VNC app.
    Touch controls that screen.
  - See and control your Android phone on the computer with scrcpy, over USB
    or Wi-Fi.
- **Wallpaper transitions**: fade, slide, push, zoom, wipe and grow, or random,
  or none, with an adjustable length. Pictures are decoded off the main thread
  so changing the wallpaper never stutters.
- **Lucide icons** everywhere (panel, control centre, OSD, Settings).
  Settings › About lists every project and source Dusk uses, with links.
- **App status indicators (system tray)** between the clipboard and the
  control centre. Apps such as Steam, Discord and Nextcloud show their own
  icons with their menus, instead of a window. Left click opens, right click
  shows the menu.
- **App icons in notifications**, and the pictures apps send (album art,
  avatars).
- **Notification history**: the clipboard card has a Notifications tab with
  past notifications (kept across restarts, configurable). Clipboard history
  can also be kept across restarts (off by default).
- **No more sleeping while you play**: games and video players that ask the
  screen to stay on are respected (ScreenSaver and PowerManagement inhibit,
  plus logind inhibitors), full-screen windows keep the screen on, and
  gamepad input counts as activity.

## What was new in 0.12

- **A screenshot tool.** Print now dims the screen and shows a bar at the
  bottom: drag out an area (drag it again to move it), click a window, or
  click a screen; show the pointer or not, wait 3, 5 or 10 seconds first,
  then the shutter button (or Enter). Keys: 1/2/3 switch mode, P the pointer,
  Esc or right-click closes. Shift+Print and Alt+Print still take every
  screen and the active window at once. The new Settings → Screenshots page
  has what Print does, the default mode, pointer, delay, copy, save (or
  clipboard only), folder, file name, notification, sound, and drawing on it
  first in Satty or Swappy.
- **A power button in the app menu** instead of Log out. It opens a menu
  with Lock, Sleep, Log out, Restart and Shut down; in Settings → Power you
  pick which (Hibernate and Restart to firmware setup are there too), whether
  it asks first, and can turn the button off. The control centre's Power
  uses the same menu. Lock uses hyprlock, gtklock, swaylock or waylock,
  whichever is installed (or your own command).
- **The app menu button's icon** can be dots, squares, a moon, a magnifier,
  your distribution's logo or a picture of your own (Settings → Panels).
- **Accent from wallpaper stays on.** "From wallpaper" is now a switch:
  pick another wallpaper and the accent changes with it.
- **The on-screen display goes anywhere**: bottom, top, left, right or the
  centre, horizontal or as an upright pill (Settings → On-screen display).
- **Simpler icons**, drawn like the window buttons: one even line weight,
  round ends, outlines (Material Symbols Rounded and One UI were the models).
- **New defaults**: notifications at the bottom right, and a full-width panel
  (both still in Settings).
- **Control centre tidied up**: in edit mode, free cells are soft filled
  squares, the remove badge sits inside the tile's corner and the resize
  handle follows the corner's curve; sliders only show their number while you
  drag them; the edit buttons and "Add a tile" chips match the tiles.
- **Fixes**
  - Bluetooth pairing: it now runs in one bluetoothctl session that keeps
    searching while it pairs (BlueZ forgets a device as soon as a search
    stops), confirms pairing codes and waits for each step.
  - Power modes now switch over D-Bus (power-profiles-daemon and tuned-ppd,
    old and new names), with powerprofilesctl as a fallback; a status check
    that was running at the same time no longer flips the tile back.
  - The battery tile ignored mice, keyboards and headsets that report a
    battery (they showed as "0% · charging" on desktops).
  - Middle-click paste: dusk-doctor now checks every piece of it (Dusk's
    setting, GTK's setting, whether there's selected text) and
    `DUSK_DEBUG=1` logs each selection. dusk-doctor also checks Bluetooth
    and the power-mode service.

## What was new in 0.11

- **GNOME's dark.** Dark mode now uses the same greys as GNOME apps, so
  Dusk's title bars, panels, menus and Settings match them. Pure black for
  OLED screens is still there: Settings → Appearance → Dark style.
- **Switches and buttons in GNOME apps always show your accent.** With some
  GTK versions the accent didn't reach them, so a switch that was on, or a
  "Rename"/"Open" button, looked blank. Dusk now colours them directly (and
  uses dark text on light accents).
- **Dusk's own icons** for the panel, control centre, menus and Settings'
  sidebar, drawn in one consistent style (app and file icons still come
  from your icon theme). Settings → Appearance → Icons switches back to the
  icon theme's.
- **Files and folders on the desktop.** What's in your Desktop folder shows
  on the main screen. Double-click to open; drag icons to arrange them (or
  onto a folder to move them in); drag files onto the desktop from Files to
  put them there. Right-click the wallpaper for New folder and Paste, or an
  icon for Open, Copy, Cut, Rename and Move to the trash. After clicking the
  desktop, Delete, F2, Enter and Ctrl+C / X / V / A work too.
- **Clipboard history.** A clipboard on the panel (and Super+V) lists what
  you copied: text, pictures and files. Pick one and it's on the clipboard
  again, so right-click → Paste (or Ctrl+V) pastes it. Kept in memory only;
  passwords from password managers are skipped. Middle-click paste can be
  turned off (Settings → Desktop & clipboard).
- **Flexible spaces that centre.** Put a flexible space on each side of
  something and it sits in the middle of the panel, whatever is on either
  side (before, the spaces only shared the room equally).
- **Right-click menus of their own.** Right-clicking a panel widget (clock,
  status icons, app menu, window button, clipboard...) shows that widget's
  options, plus "Remove from panel"; the panel's options are on empty bar,
  flexible spaces and separators (and one click away in every widget menu).
- **Notifications show how long they stay**: a small line at the bottom
  right runs down. Pointing at a notification pauses it.
- **GNOME and KDE apps get your title bar buttons**: the same buttons
  (minimise / maximise shown or not) on the same side. Settings → Windows →
  Title bar.
- **Reset all settings** (Settings → About), with switches for what to keep
  (display and keyboard settings are kept unless you say otherwise).
- **Unplugging a screen works properly.** Windows that were on it are moved
  for real now (before, only Dusk's idea of where they were changed, so they
  were drawn, and took clicks, where the screen used to be and couldn't be
  used). The pointer comes back to the main screen and any drag in progress
  ends.
- Tidier: two-cell control centre tiles no longer cut their titles short,
  the panel widget list in Settings has icons and a calmer selection, and
  every reset button uses the same icon.

## What was new in 0.10

- **A new control centre, like Arqen shell's.**
  - **At the top:** your picture, your name and how long the computer
    has been on. It opens your account settings. Next to it are round
    buttons to edit the tiles, open Settings and power off. Sleep happens
    straight away; log out, restart and shut down ask first.
  - **Tiles on a 6-column grid, arranged by you.** Brightness, volume and
    the microphone stand upright as sliders, like a phone's. Toggles show
    a round icon you click to switch them; the rest of the tile opens its
    list or options. There are also tiles for screenshots, the battery
    (click for power saver) and what's playing.
  - **Edit mode** (the pencil): drag a tile to move it (onto another to
    swap), pull its corner to resize, hide it with the red button, and add
    hidden ones back from underneath.
  - **Notifications** are stacked by app: click the number to open a
    stack, and ✕ to clear one or a whole stack. There are Settings and
    Clear buttons too.
- **Notifications, like Arqen's**:
  - pick where pop-ups appear on a little 3×3 screen (any corner, edge or
    the middle).
  - choose whether they pop up over full-screen games and videos.
  - group by app, and set the pop-up duration in seconds.
- **Settings, the Arqen way (same look)**:
  - **Search box:** type, pick a result, and the page opens with that
    setting glowing.
  - **Reset** on a card's title puts everything in it back to the
    defaults.
  - **"Show more settings"** at the bottom of a page shows the
    fine-tuning options; they're hidden otherwise, so pages stay short.
- **Drag and drop works**: files, folders, text and pictures between apps.
- **Windows have shadows again** in apps that draw their own title bar
  (Chrome, Vivaldi, Electron apps). GTK apps keep their own. The default
  shadow is also a little stronger.
- Volume and brightness keys always step by 5%. The panel's battery is
  always the ring with the number inside.

## What was new in 0.9.1

- **Unplugging a screen works.** Dusk looked at its windows before the screen
  had left the layout, so they stayed where the screen used to be, out of
  reach. They now move to a screen that's on, and go back when it returns.
- **Super+Shift+Left/Right** moves the active window to the next screen. It
  keeps its place, or its tiling.
- **No more squashed windows while tiling.** The window's snapshot now zooms
  to its new spot instead of stretching. Apps are also told they're tiled, so
  GNOME/GTK apps drop their shadow and round corners and fill the spot
  exactly.

## What was new in 0.9

- **Snap layouts, like Windows 11.** Three ways to use them:
  - Drag a window to the top of the screen: a bar of layouts drops down.
    Let go over a zone and the window fills it.
  - Rest the pointer on a window's maximise button to get the same
    layouts as a menu.
  - Press Super+Z.

  While you drag, a tinted preview shows where the window will land,
  including at the screen edges.
- **Your own layouts** in Settings → Windows → Tiling:
  - pick the columns (halves, thirds, 60/40, a wide middle...) and how
    many rows each has, then add.
  - drag to reorder, remove the ones you don't use.
  - a gap between tiled windows, and the layouts bar can be turned off.
- **Tiling on the right screen.** Snapping used the screen the middle of the
  window was on, so a window dragged to the second screen could tile on the
  first. It now goes where the pointer is, and keeps that screen when
  things change.
- **Panels' shadows go under windows**, so tiled and maximised windows sit
  cleanly against the panel.
- **Title bars stay clear of panels**: a window can't be dragged (or resized)
  up under the top panel or down under the bottom one.

## What was new in 0.8.1

- **Several screens**:
  - The pointer pauses briefly at an edge shared with another screen, so you
    can drop a window there to snap it. Settings → Displays → Pause between
    screens: light, strong or off.
  - Unplug a screen and its windows move to a screen that's still on. Plug it
    back in and they go back where they were. The same happens when you turn
    a screen off in Settings.
- **Bluetooth pairing in the control centre**:
  - The device list now searches for nearby devices in pairing mode, and you
    click one to pair, trust and connect it.
  - Right-click a paired device to connect, disconnect or forget it.
- **Snappier clicks**:
  - Toggling things in the control centre no longer reloads every setting
    twice.
  - GNOME's settings aren't asked for every time (that meant waiting for a
    program).
  - The keyboard layout, window frames and wallpaper are only redone when
    their own settings change.
- **The desktop portal starts.** Dusk now tells systemd a graphical session
  is running (dusk-session.target), which the portal waits for. File pickers
  and Flatpak apps' light/dark depend on it.
- **Cooler when idle.** Status checks are less frequent and much cheaper,
  and panels only redraw when something they show changes.

## What was new in 0.8

Fixes:

- **Outlines around GTK apps sit on the window.** Apps that draw their own
  shadow (GNOME/GTK apps) were drawn up and to the left of where Dusk
  thought they were. That put the outline in the wrong place, and menus
  could also open slightly off.
- **Saved passwords unlock at login.** Dusk now connects to the keyring the
  login screen unlocked, instead of apps starting a second, locked one. The
  "Unlock Login Keyring" prompt should no longer appear.
- **Dark mode reaches GNOME apps.** They now read it straight from GNOME's
  settings instead of relying on the desktop portal.
- The round marks in right-click menus no longer have stray lines.

New, mostly ideas from Arqen shell:

- **Style other apps** (Settings → Appearance, on by default):
  - GTK 3 apps use adw-gtk3 (or Adwaita) in light or dark.
  - GTK apps get your exact accent colour.
  - Qt apps get Dusk's colours through qt6ct.
  - Chrome-type browsers follow dark mode.
- **Accent from the wallpaper**: one click when your wallpaper is a picture.
- **Startup apps** page: apps that open at login (the standard autostart
  folder), with switches. You can add any installed app.
- **Admin password prompts** work: Dusk starts the installed polkit agent
  (polkit-gnome or mate-polkit).
- **Power** page:
  - turn the screen off after a while.
  - sleep after a while (separately on battery and plugged in).
  - power mode.
  - low battery warnings at 20%, 10% and 5%.

  Videos, games and calls keep the screen on. So does Caffeine.
- **Screenshots**:
  - Print takes this screen, Shift+Print all screens, Alt+Print the active
    window.
  - They're saved to Pictures/Screenshots and copied to the clipboard.
  - A notification lets you open the screenshot or its folder.

  Tools like grim and clipboard managers now work in Dusk too.
- **Your own keyboard shortcuts** (Settings → Keyboard): record the keys and
  pick an app, or type a command. Super+E opens your files.
- **Default apps**: web browser, files, email, music, videos, pictures, text
  and PDF.
- **Date & time** page: time zone (type a city) and automatic time.
- **dusk-doctor**: run it in a terminal to see why dark mode, saved passwords
  or admin prompts aren't working.

## What was new in 0.7.1

- **Right-click menus** instead of jumping into Settings:
  - **On a panel**: auto-hide, window titles, position and style, add or
    remove a panel. On a window button you also get minimise, maximise,
    pin to panel and close. On a pinned app: new window and unpin.
  - **On a control centre tile**: that tile's options. For example, the
    night light's warmth, the power mode, light or dark, and clearing
    notifications. You can also make a tile wide or remove it.
    Right-click empty space in the control centre to choose what it
    shows.
  - **On the wallpaper**: terminal, show desktop, light/dark, wallpaper and
    display settings.
- **Outlines on every window**, including apps that draw their own title
  bar (GTK, GNOME and Electron apps).
- **Light and dark reach your apps**: GNOME/libadwaita apps, Firefox and
  Electron apps (through the desktop portal), GTK 3 apps (Adwaita ↔
  Adwaita-dark) and Qt apps (through GTK's settings). GNOME apps also get
  the closest match to your accent colour.
- **Settings looks tidier**:
  - Drop-downs shorten long choices with "…", have an arrow, and open
    below as a rounded list.
  - Numbers have − and + buttons instead of tiny spin arrows.
  - The sidebar has icons, and shrinks to just icons when the window is
    narrow.
  - Windows can't be resized smaller than the app supports.
  - Monitors with junk maker names ("@@@") show just their model.

## What was new in 0.7

- **Two looks: Comic and Flat.** Ink and Soft are gone. Comic is calmer
  now: thinner, lighter ink lines, shorter and fainter shadows, and no
  screentone dots. If you used the old Comic look, it switches to the new
  one by itself.
- **Drawn window buttons** (the new default): minimise is two corners
  pointing in, maximise two corners pointing out, close a red hand-drawn
  cross, each on a dark rounded patch when you point at it. GTK-style
  and plain buttons are still in Settings → Windows.
- **No more cursor lag over panels and menus.** Hover and selection
  highlights are now drawn by the graphics card and glide on their own, so
  moving the mouse no longer redraws the whole panel, menu or control
  centre.
- **Displays settings**:
  - drag your screens into place.
  - pick the resolution (the screen's own one is marked recommended) and
    the refresh rate.
  - scale, with a recommendation worked out from the screen's size.
  - rotation, variable refresh rate (FreeSync / Adaptive-Sync) where the
    screen supports it, turning screens on and off, and the main display.
  - a change you don't confirm goes back by itself after 15 seconds.
- **Fewer things to type in Settings**:
  - Keyboard layouts are picked from a searchable list, and drag to
    reorder.
  - Switching between layouts, what Caps Lock does, the Compose key and
    more are menus.
  - Terminal: a list of the installed ones.
  - Wallpaper: thumbnails of the system's backgrounds and your pictures.
- **More mouse and touchpad options**: acceleration on or off,
  left-handed, ignore the touchpad while typing, how right-click works,
  and middle-click with both buttons.
- Settings rows line up: every control on the right is the same width.

## What was new in 0.6

- **A new look, in variations.** Panels, menus, the control centre and
  popups are drawn in one of four looks, picked in Settings → Appearance →
  Look, and each part can also be changed on its own:
  - **Ink** (the default): soft dark outlines like line art, a thin light
    rim along top edges, and soft floating shadows.
  - **Comic**: bold lines, short drawn shadows and screentone dots.
  - **Soft**: no lines, just floating shadows.
  - **Flat**: thin lines and no shadows.

  You can also set how round corners are and pick a font. Shadows are now
  drawn by the graphics card. Nested corners follow the rule "outer radius =
  inner radius + padding", so curves run parallel.
- **Smoother everything.**
  - Animations follow your choice of motion: smooth, snappy or bouncy.
  - Hover highlights fade in and out.
  - Window buttons in the panel glide over when windows come and go.
  - In the app menu, the selection glides and the list scrolls smoothly.
  - Control centre tiles cross-fade on and off and give a little press
    when clicked. Sliders glide, switches stretch as they flip, and Wi-Fi
    and Bluetooth pages slide in.
- **Window animations, with choices.**
  - Windows open and close with a zoom, slide or fade.
  - Minimising shrinks a window into its button on the panel, and
    restoring grows it back out.
  - Maximising, snapping and fullscreen glide the window to its new size.
- **Now playing**: a chip on the panel shows what's playing. Click it for a
  card with the album art behind, a wavy progress line you can drag, and
  the controls.
- **Calendar**: clicking the clock opens a month view (or the control
  centre, if you prefer).
- **Battery** in the panel as an icon, an icon with a percentage, or a ring
  with the number inside.

## What was new in 0.5

- **Renamed to Dusk.** Your Floe settings are copied over the first time
  Dusk starts, and installing removes the old Floe session.
- **GTK-style title bar buttons**: round buttons with the icon theme's own
  close / maximise / minimise icons, the same ones GTK apps use. The old plain
  style is still there, and the minimise and maximise buttons can be hidden.
- **Panels you set up yourself**: as many as you like, on any edge (top,
  bottom, left, right), floating, full-width or dock-style, any size,
  see-through, auto-hiding, on every screen or just one. Choose and order
  what's on each: app menu, window buttons, pinned apps, clock, status icons,
  separators, flexible space, show-desktop. Right-click a panel to change it.
- **Control centre**: a sidebar (like One UI) that opens from the panel's
  status icons, on the side of the screen that button is on (or Super+A):
  - quick-setting tiles: Wi-Fi and Bluetooth (with network and device
    lists, and a password box for new networks), dark mode, night light,
    do not disturb, airplane mode, caffeine (stay awake), power mode and
    microphone. You can pick the tiles, their order and their size.
  - brightness and volume sliders, and media controls for whatever is
    playing.
  - recent notifications.
  - suspend, log out, restart and shut down.
- **Notifications**: Dusk shows app notifications itself, as popups that
  stack in a corner and also go into the control centre list.
- **Status icons** in the panel: network, Bluetooth, do not disturb, volume
  and battery (optionally with a percentage).
- **Smoother animations**: they now advance in step with the screen's
  refresh instead of on a separate timer. Reversing half way (e.g. restoring a
  window while it's still minimising) carries on from where it is instead of
  jumping. Closing windows fade where they are in the stack, not on top.
  Maximising and snapping wait for the app to redraw at its new size, so the
  window no longer jumps across first.
- **Dusk Settings** is a Qt app again, styled like Dusk itself (light/dark,
  accent, your border and shadow style), and it uses Dusk's own title bar.
  - Settings that don't apply right now are hidden. For example, the picture
    options only show when the wallpaper is a picture, the per-animation
    switches only show while animations are on, and the touchpad section
    only shows on computers that have a touchpad.
  - Anything you've changed gets a reset button (↺) next to it.
  - New pages for Panels, Control centre and Notifications.

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

## Not done yet (next steps)

- Layer-shell (lets external panels, docks and lock screens run on Dusk;
  needed for picking a screenshot area with slurp)
- A lock screen (and locking when idle)
- Screen sharing to apps (xdg-desktop-portal screencast)
