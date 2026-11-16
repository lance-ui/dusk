#!/usr/bin/env bash
# Dusk's installer and build script.
#
#   ./build.sh            install missing build packages, then build
#   ./build.sh run        build, then run Dusk in a window on your current desktop
#   ./build.sh install    build, then install Dusk as a login session
#   ./build.sh update     get the newest Dusk from GitHub and install it (dusk-update)
#   ./build.sh uninstall  remove the installed session
#   ./build.sh fonts      (re)download Dusk's fonts: Google Sans and Google Sans Code
#
# Fedora is what Dusk is made on. Debian / Ubuntu and Arch are supported on a
# best-effort basis: UNTESTED. Add -y (or DUSK_YES=1) to skip the question.
set -euo pipefail
cd "$(dirname "$0")"

# ---- Looks ----------------------------------------------------------------------------
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
	B=$'\e[1m'; D=$'\e[2m'; R=$'\e[0m'
	PURPLE=$'\e[38;5;141m'; DEEP=$'\e[38;5;97m'; SUN=$'\e[38;5;215m'; CORAL=$'\e[38;5;209m'
	STAR=$'\e[38;5;229m'; CYAN=$'\e[38;5;116m'; GREEN=$'\e[38;5;114m'; YELLOW=$'\e[38;5;221m'; RED=$'\e[38;5;203m'
	GREY=$'\e[38;5;244m'
	# 0.35: one calm accent (the rainbow was loud); the steps and bar use it.
	SKY=($'\e[38;5;209m' $'\e[38;5;209m' $'\e[38;5;209m' $'\e[38;5;209m' $'\e[38;5;209m')
else
	B=; D=; R=; PURPLE=; DEEP=; SUN=; CORAL=; STAR=; CYAN=; GREEN=; YELLOW=; RED=; GREY=
	SKY=("" "" "" "" "")
fi
say()  { printf '   %s›%s %s\n' "$CYAN" "$R" "$*"; }
good() { printf '   %s✓%s %s\n' "$GREEN" "$R" "$*"; }
warn() { printf '   %s!%s %s\n' "$YELLOW" "$R" "$*" >&2; }
fail() {
	printf '\n  %s✗ %s%s\n' "$RED$B" "$*" "$R" >&2
	[[ -n "${DUSK_PROGRESS:-}" ]] && printf '@@failed %s\n' "$*"
	exit 1
}

DUSK_VERSION=$(sed -n "s/^[[:space:]]*version: '\([^']*\)'.*/\1/p" meson.build | head -1)
STARTED=$SECONDS

# The steps, numbered ("2/4 · Building"); Settings follows them too (@@ lines).
STEP=0 STEPS=3
step() {
	STEP=$((STEP + 1))
	printf '\n   %s%s%s/%s %s %s%s%s\n' "$PURPLE" "$B" "$STEP" "$STEPS" "$R$GREY·$R" "$B" "$*" "$R"
	[[ -n "${DUSK_PROGRESS:-}" ]] && printf '@@step %s %s %s\n' "$STEP" "$STEPS" "$*"
	return 0
}

banner() {
	local title="D U S K" i out=""
	for ((i = 0; i < ${#title}; i++)); do
		local ch="${title:i:1}"
		out+="${SKY[$(( (i / 2) % 5 ))]}${B}${ch}${R}"
	done
	printf '\n'
	printf '   %s✦%s  %s   %s%s%s\n' "$STAR" "$R" "$out" "$GREY" "$DUSK_VERSION" "$R"
	printf '   %s' ""
	for ((i = 0; i < 30; i++)); do printf '%s─' "${SKY[$(( i * 5 / 30 ))]}"; done
	printf '%s\n' "$R"
	printf '   %sa desktop for the evening%s\n\n' "$D" "$R"
}

warning() {
	printf '   %s%s⚠  Heads up%s\n' "$YELLOW" "$B" "$R"
	printf '   %s│%s  Dusk is written by an AI (Claude, by Anthropic). It may have bugs,\n' "$YELLOW" "$R"
	printf '   %s│%s  crash, or break things on your computer. Use it at your own risk,\n' "$YELLOW" "$R"
	printf '   %s│%s  and keep another desktop installed to fall back on.\n' "$YELLOW" "$R"
	printf '\n'
	case "$DISTRO" in
	fedora) printf '   %s●%s %s%s%s %s· the system Dusk is made and tested on%s\n' "$GREEN" "$R" "$B" "${PRETTY_NAME:-Fedora}" "$R" "$GREY" "$R" ;;
	*) printf '   %s●%s %s%s%s %s· untested:%s package names are a best guess, and Dusk needs\n     wlroots 0.20, which your system may not have yet. Fedora is the tested one.\n' \
		"$RED" "$R" "$B" "$DISTRO_NAME" "$R" "$RED$B" "$R" ;;
	esac
	printf '\n'
}

confirm() {
	[[ "${DUSK_YES:-}" == 1 ]] && return 0
	[[ -t 0 ]] || return 0
	local answer
	read -r -p "   ${B}${1:-Carry on?}${R} ${GREY}[y/N]${R} " answer
	[[ "$answer" == [yY]* ]] || { printf '\n'; say "Nothing was changed."; exit 0; }
}

# How long it took ("1 min 12 s").
took() {
	local t=$((SECONDS - STARTED))
	if ((t >= 60)); then printf '%d min %d s' $((t / 60)) $((t % 60)); else printf '%d s' "$t"; fi
}

# The finish: a small card.
finished() {
	local msg="$1" w
	w=$((${#msg} + 7))
	printf '\n   %s╭%s╮%s\n' "$GREEN" "$(printf '─%.0s' $(seq 1 "$w"))" "$R"
	printf '   %s│%s  %s✓%s %s%s%s   %s│%s\n' "$GREEN" "$R" "$GREEN$B" "$R" "$B" "$msg" "$R" "$GREEN" "$R"
	printf '   %s╰%s╯%s\n' "$GREEN" "$(printf '─%.0s' $(seq 1 "$w"))" "$R"
	printf '   %sin %s%s\n\n' "$GREY" "$(took)" "$R"
}

# What's new: the headline of each change (the bold start of each "- **…**"
# line in README.md's "What's new in X" sections), after version $1 (left
# out: just $2's) up to $2, newest first, as "version<TAB>headline".
newer_v() { [[ "$1" != "$2" && "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" == "$1" ]]; }
whats_new() {
	local from="$1" to="$2" v
	[[ -f README.md ]] || return 0
	while IFS= read -r v; do
		if [[ -n "$from" ]]; then newer_v "$v" "$from" || continue
		else [[ "$v" == "$to" ]] || continue; fi
		newer_v "$v" "$to" && continue
		sed -n "/^## What's new in ${v//./\\.}\$/,/^## /p" README.md | grep '^- \*\*' |
			sed -E 's/^- \*\*([^*]*)\*\*.*/\1/; s/[:.]+$//' |
			while IFS= read -r t; do printf '%s\t%s\n' "$v" "$t"; done
	done < <(sed -n "s/^## What's new in \([0-9.]*\)$/\1/p" README.md)
}
# Shown in the terminal at the end: a few of them.
show_whats_new() {
	local list="$1" n=0 v t
	[[ -n "$list" ]] || return 0
	printf '   %s%sWhat'"'"'s new%s\n' "$PURPLE" "$B" "$R"
	while IFS=$'\t' read -r v t; do
		n=$((n + 1))
		if ((n > 6)); then
			printf '   %s… and %s more (README.md)%s\n' "$GREY" "$(($(wc -l <<<"$list") - 6))" "$R"
			break
		fi
		printf '   %s•%s %s\n' "${SKY[$(( (n - 1) % 5 ))]}" "$R" "$t"
	done <<<"$list"
	printf '\n'
}

# Asked for the password by Settings (no terminal): sudo asks through it.
if [[ -n "${SUDO_ASKPASS:-}" && ! -t 0 ]]; then
	sudo() { command sudo -A "$@"; }
fi

# ---- Which system ---------------------------------------------------------------------
DISTRO=unknown
DISTRO_NAME="This system"
if [[ -r /etc/os-release ]]; then
	# shellcheck disable=SC1091
	. /etc/os-release
	DISTRO_NAME="${NAME:-$DISTRO_NAME}"
	case " ${ID:-} ${ID_LIKE:-} " in
	*" fedora "*|*" rhel "*) DISTRO=fedora ;;
	*" arch "*) DISTRO=arch ;;
	*" debian "*|*" ubuntu "*) DISTRO=debian ;;
	esac
fi

# Three kinds of things Dusk needs, each checked by what it *is* (a command,
# a file, a library pkg-config knows), not by which package it came from: a
# copy from another repository, Flathub, a source build in /usr/local or
# ~/.local all count, and nothing is installed over it.
#   BUILD     to compile Dusk: must be there (installed if missing, or it stops)
#   TOOLS     what features use at run time: installed if missing; if that
#             fails (no such package, repository off), Dusk still installs and
#             says which feature won't work
#   OPTIONAL  nice to have: tried once each
# Entries are "package=probe"; a probe is a command, an absolute path (several
# with |), or pc:<pkg-config module>.
case "$DISTRO" in
fedora)
	BUILD=(gcc-c++=g++ meson=meson ninja-build=ninja pkgconf-pkg-config=pkg-config git=git curl=curl file=file
		wlroots-devel=pc:wlroots-0.20 wayland-devel=pc:wayland-server wayland-protocols-devel=pc:wayland-protocols
		libxkbcommon-devel=pc:xkbcommon pixman-devel=pc:pixman-1 libdrm-devel=pc:libdrm libinput-devel=pc:libinput
		cairo-devel=pc:cairo pango-devel=pc:pangocairo gdk-pixbuf2-devel=pc:gdk-pixbuf-2.0
		pam-devel=/usr/include/security/pam_appl.h
		libglvnd-devel=pc:egl mesa-libEGL-devel=pc:egl mesa-libgbm-devel=pc:gbm
		libxcb-devel=pc:xcb xcb-util-wm-devel=pc:xcb-icccm systemd-devel=pc:libsystemd
		qt6-qtbase-devel=pc:Qt6Widgets qt6-qtsvg=/usr/lib64/libQt6Svg.so.6)
	TOOLS=(xorg-x11-server-Xwayland=Xwayland qt6-qtwayland=/usr/lib64/qt6/plugins/platforms/libqwayland-generic.so
		brightnessctl=brightnessctl wireplumber=wpctl playerctl=playerctl bluez=bluetoothctl
		pulseaudio-utils=parec python3=python3 wf-recorder=wf-recorder
		xdg-desktop-portal-gtk=/usr/libexec/xdg-desktop-portal-gtk
		qt6-qtbase-gui=/usr/lib64/qt6/plugins/platformthemes/libqgtk3.so flatpak=flatpak)
	# Saved passwords: Fedora 45 has oo7 (unlocked at login) in place of GNOME
	# Keyring; GNOME Keyring only where oo7 isn't.
	if [ -x /usr/libexec/oo7-daemon ]; then KEYRING=(); else KEYRING=(gnome-keyring=gnome-keyring-daemon gnome-keyring-pam=/usr/lib64/security/pam_gnome_keyring.so); fi
	OPTIONAL=("${KEYRING[@]}" polkit-gnome=/usr/libexec/polkit-gnome-authentication-agent-1
		adw-gtk3-theme=/usr/share/themes/adw-gtk3 qt6ct=qt6ct grim=grim wl-clipboard=wl-copy libnotify=notify-send
		wayvnc=wayvnc android-tools=adb scrcpy=scrcpy papirus-icon-theme=/usr/share/icons/Papirus
		bibata-cursor-themes=/usr/share/icons/Bibata-Modern-Classic)
	;;
arch)
	BUILD=(base-devel=g++ meson=meson ninja=ninja pkgconf=pkg-config git=git curl=curl file=file
		wlroots0.20=pc:wlroots-0.20 wayland=pc:wayland-server wayland-protocols=pc:wayland-protocols
		libxkbcommon=pc:xkbcommon pixman=pc:pixman-1 libdrm=pc:libdrm libinput=pc:libinput cairo=pc:cairo
		pango=pc:pangocairo gdk-pixbuf2=pc:gdk-pixbuf-2.0 pam=/usr/include/security/pam_appl.h
		libglvnd=pc:egl mesa=pc:gbm libxcb=pc:xcb xcb-util-wm=pc:xcb-icccm systemd-libs=pc:libsystemd
		qt6-base=pc:Qt6Widgets qt6-svg=/usr/lib/libQt6Svg.so.6)
	TOOLS=(xorg-xwayland=Xwayland qt6-wayland=/usr/lib/qt6/plugins/platforms/libqwayland-generic.so
		brightnessctl=brightnessctl wireplumber=wpctl playerctl=playerctl bluez=bluetoothd bluez-utils=bluetoothctl
		libpulse=parec python=python3 wf-recorder=wf-recorder
		xdg-desktop-portal-gtk=/usr/lib/xdg-desktop-portal-gtk flatpak=flatpak)
	OPTIONAL=(gnome-keyring=gnome-keyring-daemon polkit-gnome=/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1
		qt6ct=qt6ct grim=grim wl-clipboard=wl-copy libnotify=notify-send wayvnc=wayvnc android-tools=adb
		scrcpy=scrcpy papirus-icon-theme=/usr/share/icons/Papirus)
	;;
debian)
	BUILD=(build-essential=g++ meson=meson ninja-build=ninja pkg-config=pkg-config git=git curl=curl file=file
		libwlroots-0.20-dev=pc:wlroots-0.20 libwayland-dev=pc:wayland-server wayland-protocols=pc:wayland-protocols
		libxkbcommon-dev=pc:xkbcommon libpixman-1-dev=pc:pixman-1 libdrm-dev=pc:libdrm libinput-dev=pc:libinput
		libcairo2-dev=pc:cairo libpango1.0-dev=pc:pangocairo libgdk-pixbuf-2.0-dev=pc:gdk-pixbuf-2.0
		libpam0g-dev=/usr/include/security/pam_appl.h libegl-dev=pc:egl libgbm-dev=pc:gbm libgles-dev=pc:glesv2
		libxcb1-dev=pc:xcb libxcb-icccm4-dev=pc:xcb-icccm libxcb-composite0-dev=pc:xcb-composite libxcb-damage0-dev=pc:xcb-damage libsystemd-dev=pc:libsystemd
		qt6-base-dev=pc:Qt6Widgets libqt6svg6-dev=pc:Qt6Svg)
	TOOLS=(xwayland=Xwayland qt6-wayland="/usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqwayland-generic.so|/usr/lib/aarch64-linux-gnu/qt6/plugins/platforms/libqwayland-generic.so"
		brightnessctl=brightnessctl wireplumber=wpctl playerctl=playerctl bluez=bluetoothctl
		pulseaudio-utils=parec python3=python3 wf-recorder=wf-recorder
		xdg-desktop-portal-gtk=/usr/libexec/xdg-desktop-portal-gtk flatpak=flatpak)
	OPTIONAL=(gnome-keyring=gnome-keyring-daemon policykit-1-gnome=/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1
		mate-polkit=/usr/libexec/polkit-mate-authentication-agent-1 qt6ct=qt6ct grim=grim wl-clipboard=wl-copy
		libnotify-bin=notify-send wayvnc=wayvnc adb=adb scrcpy=scrcpy papirus-icon-theme=/usr/share/icons/Papirus)
	;;
*)
	BUILD=(); TOOLS=(); OPTIONAL=()
	;;
esac

# Is it there, from wherever? ("package=probe": the probe, or else the package itself)
present() {
	local probe="${1#*=}" p
	case "$probe" in
	pc:*) pkg-config --exists "${probe#pc:}" 2>/dev/null && return 0 ;;
	/*) IFS='|' read -ra paths <<< "$probe"
		for p in "${paths[@]}"; do [[ -e "$p" ]] && return 0; done ;;
	*) { command -v "$probe" >/dev/null 2>&1 || [[ -x "$HOME/.local/bin/$probe" || -x "/usr/local/bin/$probe" ]]; } && return 0 ;;
	esac
	have_pkg "$(pkgname "$1")"
}
have_pkg() {
	case "$DISTRO" in
	fedora) rpm -q "$1" >/dev/null 2>&1 ;;
	arch) pacman -Q "$1" >/dev/null 2>&1 ;;
	debian) dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed" ;;
	*) return 1 ;;
	esac
}
pkgname() { printf '%s' "${1%%=*}"; }

pkg_install() {   # must-haves: stop if they can't be installed
	case "$DISTRO" in
	fedora) sudo dnf install -y "$@" ;;
	arch) sudo pacman -S --needed --noconfirm "$@" ;;
	debian) sudo apt-get update -qq && sudo apt-get install -y "$@" ;;
	esac
}
pkg_try() {   # one at a time, so one that can't be found doesn't stop the others; never fails
	local p
	for p in "$@"; do
		case "$DISTRO" in
		fedora) sudo dnf install -y "$p" >/dev/null 2>&1 || true ;;
		arch) sudo pacman -S --needed --noconfirm "$p" >/dev/null 2>&1 || true ;;
		debian) sudo apt-get install -y "$p" >/dev/null 2>&1 || true ;;
		esac
	done
	return 0
}

install_deps() {
	# Fedora Silverblue / Kinoite and other image-based systems: packages are
	# layered with rpm-ostree (and a restart), not installed here.
	local ostree=0
	[[ -e /run/ostree-booted ]] && ostree=1
	if [[ "$DISTRO" == unknown || $ostree == 1 ]]; then
		local need=()
		for e in "${BUILD[@]+"${BUILD[@]}"}" "${TOOLS[@]+"${TOOLS[@]}"}"; do present "$e" || need+=("$(pkgname "$e")"); done
		if [[ $ostree == 1 ]]; then
			((${#need[@]})) && warn "This system's packages are layered with rpm-ostree; missing: ${need[*]}" \
				"(e.g. build inside a toolbox, or: rpm-ostree install ${need[*]})"
		else
			warn "Don't know how to install packages on $DISTRO_NAME: make sure Dusk's build packages are installed (see build.sh)."
		fi
	else
		local missing=() e
		for e in "${BUILD[@]}"; do present "$e" || missing+=("$(pkgname "$e")"); done
		if ((${#missing[@]})); then
			say "Installing build packages: ${D}${missing[*]}${R}"
			if ! pkg_install "${missing[@]}"; then
				# Some may have come from elsewhere meanwhile, or under another
				# name: only stop if something really needed is still missing.
				local still=()
				for e in "${BUILD[@]}"; do present "$e" || still+=("$(pkgname "$e")"); done
				((${#still[@]})) && fail "Couldn't install: ${still[*]}. Install them (or what provides them) and run this again."
			fi
		fi
		[[ "$DISTRO" == debian ]] && sudo apt-get update -qq >/dev/null 2>&1
		local tools=()
		for e in "${TOOLS[@]}"; do present "$e" || tools+=("$e"); done
		if ((${#tools[@]})); then
			say "Installing what Dusk uses: ${D}$(for e in "${tools[@]}"; do printf '%s ' "$(pkgname "$e")"; done)${R}"
			for e in "${tools[@]}"; do
				pkg_try "$(pkgname "$e")"
				present "$e" || \
					warn "Couldn't install $(pkgname "$e") (not in your enabled repositories?): $(feature_of "$(pkgname "$e")")"
			done
		fi
		# Nice to have: each one tried once (remembered), whatever's there already skipped.
		local asked="$HOME/.cache/dusk-optional-tried" want=()
		mkdir -p "$HOME/.cache"
		touch "$asked"
		for e in "${OPTIONAL[@]}"; do
			present "$e" && continue
			grep -qxF "$(pkgname "$e")" "$asked" && continue
			want+=("$(pkgname "$e")")
		done
		# One polkit agent is enough.
		if ls /usr/libexec/polkit-*-authentication-agent-1 /usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1 \
				/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1 >/dev/null 2>&1; then
			local keep=() p
			for p in "${want[@]+"${want[@]}"}"; do
				[[ "$p" == polkit-gnome || "$p" == mate-polkit || "$p" == policykit-1-gnome ]] || keep+=("$p")
			done
			want=("${keep[@]+"${keep[@]}"}")
		fi
		if ((${#want[@]})); then
			say "Installing optional packages (skipped if not available): ${D}${want[*]}${R}"
			for p in "${want[@]}"; do pkg_try "$p"; echo "$p" >> "$asked"; done
		fi
	fi
	install_themes
	install_fonts
	install_songrec
}

# What stops working without a tool (said when it can't be installed).
feature_of() {
	case "$1" in
	wf-recorder) echo "screen recording won't work" ;;
	pulseaudio-utils|libpulse) echo "the media wave won't follow the music unless pw-record is there" ;;
	bluez|bluez-utils) echo "Bluetooth won't work" ;;
	playerctl) echo "the media card and lyrics won't see what's playing" ;;
	python3|python) echo "no lyrics" ;;
	brightnessctl) echo "the brightness slider won't work" ;;
	wireplumber) echo "the volume controls won't work" ;;
	xdg-desktop-portal-gtk) echo "apps may not follow dark mode" ;;
	flatpak) echo "SongRec can't come from Flathub (install it another way to recognise songs)" ;;
	*) echo "some features may not work" ;;
	esac
}

# SongRec (a Shazam client): lyrics for songs whose name finds nothing are
# found by listening. From the system's packages if it has it, else Flathub.
install_songrec() {
	command -v songrec >/dev/null 2>&1 && return            # from the system, a source build, cargo...
	[[ -x "$HOME/.cargo/bin/songrec" || -x "$HOME/.local/bin/songrec" ]] && return
	if command -v flatpak >/dev/null 2>&1; then
		# Already there from Flathub, for you or for everyone?
		flatpak info com.github.marinm.songrec >/dev/null 2>&1 && return
		say "Installing SongRec (recognises songs for lyrics)"
		# Your own Flathub, added if it isn't there (a system-wide one, possibly
		# filtered like Fedora's, is left alone).
		if ! flatpak remotes --user --columns=name 2>/dev/null | grep -qx flathub; then
			flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo >/dev/null 2>&1 || {
				warn "Couldn't add Flathub (offline?): no SongRec, lyrics still work for songs found by name."
				return
			}
		fi
		flatpak install --user -y --noninteractive flathub com.github.marinm.songrec >/dev/null 2>&1 && return
		warn "Couldn't install SongRec from Flathub: lyrics still work for songs found by name."
		return
	fi
	# No Flatpak: the system's package if it has one (Debian/Ubuntu may; Arch has it in the AUR).
	case "$DISTRO" in
	debian|fedora) pkg_try songrec; command -v songrec >/dev/null 2>&1 && return ;;
	esac
	warn "SongRec isn't installed (no Flatpak here): install it to recognise songs by ear (Arch: from the AUR)."
}

# Dusk's default icons and cursor: Papirus and Bibata Modern Classic. From
# the system's packages when it has them; otherwise downloaded into your own
# icons folder.
install_themes() {
	local icons="$HOME/.local/share/icons"
	if ! ls -d /usr/share/icons/Papirus "$icons/Papirus" >/dev/null 2>&1; then
		pkg_try papirus-icon-theme
		if ! ls -d /usr/share/icons/Papirus >/dev/null 2>&1; then
			say "Downloading the Papirus icon theme"
			curl -fsSL https://raw.githubusercontent.com/PapirusDevelopmentTeam/papirus-icon-theme/master/install.sh |
				DESTDIR="$icons" sh >/dev/null 2>&1 || warn "Couldn't get Papirus (Dusk uses GNOME's icons meanwhile)"
		fi
	fi
	if ! ls -d /usr/share/icons/Bibata-Modern-Classic "$icons/Bibata-Modern-Classic" >/dev/null 2>&1; then
		if [[ "$DISTRO" == fedora ]]; then pkg_try bibata-cursor-themes; fi
		if ! ls -d /usr/share/icons/Bibata-Modern-Classic >/dev/null 2>&1; then
			say "Downloading the Bibata Modern Classic cursor"
			mkdir -p "$icons"
			curl -fsSL https://github.com/ful1e5/Bibata_Cursor/releases/latest/download/Bibata-Modern-Classic.tar.xz |
				tar -xJ -C "$icons" || warn "Couldn't get Bibata (Dusk uses GNOME's cursor meanwhile)"
		fi
	fi
	# X11 apps with older libraries (Steam) only look in ~/.icons.
	if [[ -d "$icons/Bibata-Modern-Classic" && ! -e "$HOME/.icons/Bibata-Modern-Classic" ]]; then
		mkdir -p "$HOME/.icons" && ln -s "$icons/Bibata-Modern-Classic" "$HOME/.icons/Bibata-Modern-Classic" || true
	fi
}

# Dusk's default fonts: Google Sans for the interface and Google Sans Code
# for terminals (both free, OFL, from Google Fonts), into your own fonts folder.
# Without them everything uses Noto Sans / the system monospace instead.
install_fonts() {
	local dir="$HOME/.local/share/fonts/dusk" base="https://github.com/google/fonts/raw/main/ofl" got=0
	local want=("googlesans/GoogleSans%5BGRAD%2Copsz%2Cwght%5D.ttf:GoogleSans.ttf"
		"googlesans/GoogleSans-Italic%5BGRAD%2Copsz%2Cwght%5D.ttf:GoogleSans-Italic.ttf"
		"googlesanscode/GoogleSansCode%5Bwght%5D.ttf:GoogleSansCode.ttf"
		"googlesanscode/GoogleSansCode-Italic%5Bwght%5D.ttf:GoogleSansCode-Italic.ttf")
	local fams; fams=$(fc-list : family 2>/dev/null || true)
	mkdir -p "$dir"
	rm -f "$dir/GoogleSansFlex.ttf"   # Dusk 0.21.0-0.21.1 used Google Sans Flex
	for w in "${want[@]}"; do
		local src="${w%%:*}" name="${w##*:}"
		if [[ "${1:-}" != again ]]; then
			[[ -s "$dir/$name" ]] && continue
			case "$name" in
				GoogleSans.ttf|GoogleSans-Italic.ttf) grep -qxF "Google Sans" <<<"$fams" && continue ;;
				GoogleSansCode*) grep -qxF "Google Sans Code" <<<"$fams" && continue ;;
			esac
		fi
		say "Downloading the font $name"
		if curl -fsSL --retry 2 -o "$dir/$name.part" "$base/$src"; then
			mv "$dir/$name.part" "$dir/$name"; got=1
		else
			rm -f "$dir/$name.part"; warn "Couldn't download $name (Dusk uses Noto Sans / monospace meanwhile)"
		fi
	done
	if ((got)); then fc-cache -f "$dir" >/dev/null 2>&1 || true; fi
}

# Dusk once had its own login screen (greetd). It's gone: put GNOME's (GDM)
# back if it was in use, and remove what it installed.
remove_login_screen() {
	if grep -q dusk-greeter /etc/greetd/config.toml 2>/dev/null; then
		say "Putting GNOME's login screen (GDM) back"
		sudo systemctl disable greetd.service 2>/dev/null || true
		sudo systemctl enable gdm.service 2>/dev/null || sudo systemctl enable gdm3.service 2>/dev/null || true
		if [[ -f /etc/greetd/config.toml.before-dusk ]]; then
			sudo mv /etc/greetd/config.toml.before-dusk /etc/greetd/config.toml
		else
			sudo rm -f /etc/greetd/config.toml
		fi
		if [[ -f /etc/pam.d/greetd.before-dusk ]]; then sudo mv /etc/pam.d/greetd.before-dusk /etc/pam.d/greetd; fi
		good "GDM is back after a restart."
	fi
	sudo rm -rf /usr/local/bin/dusk-greeter /usr/local/bin/dusk-login-sync \
		/usr/share/polkit-1/actions/org.dusk.login-sync.policy /etc/dusk/greeter.conf \
		/usr/local/share/dusk-login /var/cache/dusk-greeter /var/lib/dusk-greeter \
		/var/log/dusk-login-sync.log
	sudo rmdir /etc/dusk 2>/dev/null || true
}

# The compile: in a terminal, one line with a bar (the full output kept in
# build/ninja.log, and shown if it fails); otherwise ninja's own lines.
run_ninja() {
	if [[ ! -t 1 || -n "${DUSK_PROGRESS:-}" ]]; then
		ninja -C build
		return
	fi
	local log=build/ninja.log line n=0 m=0 pct fill i bar
	set +e
	NINJA_STATUS='[%f/%t] ' ninja -C build 2>&1 | tee "$log" | while IFS= read -r line; do
		if [[ "$line" =~ ^\[([0-9]+)/([0-9]+)\]\ (.*)$ ]]; then
			n=${BASH_REMATCH[1]} m=${BASH_REMATCH[2]}
			pct=$((m > 0 ? n * 100 / m : 0)) fill=$((pct * 28 / 100)) bar=""
			for ((i = 0; i < 28; i++)); do
				if ((i < fill)); then bar+="${SKY[$(( i * 5 / 28 ))]}━"; else bar+="${GREY}─"; fi
			done
			local what="${BASH_REMATCH[3]##* }"; what="${what##*/}"
			printf '\r\e[K     %s%s %s%3d%%%s %s%.40s%s' "$bar" "$R" "$B" "$pct" "$R" "$GREY" "$what" "$R"
		fi
	done
	local status=${PIPESTATUS[0]}
	set -e
	printf '\r\e[K'
	if ((status != 0)); then
		printf '  %sThe compiler said:%s\n' "$RED" "$R"
		grep -v '^\[[0-9]*/[0-9]*\]' "$log" | tail -n 40 | sed 's/^/    /'
		printf '  %s(all of it: %s)%s\n' "$GREY" "$(pwd)/$log" "$R"
		return 1
	fi
	return 0
}

build() {
	step "Packages and fonts"
	install_deps
	step "Building Dusk $DUSK_VERSION"
	if [[ ! -d build ]]; then
		if [[ -t 1 && -z "${DUSK_PROGRESS:-}" ]]; then
			meson setup build > build-setup.log 2>&1 || { cat build-setup.log; fail "meson couldn't set up the build (a missing package? see above)."; }
			rm -f build-setup.log
		else
			meson setup build || fail "meson couldn't set up the build (a missing package? see above)."
		fi
	else
		# Look for packages again (one installed since, like Qt Quick, is found).
		meson setup --reconfigure build >/dev/null || fail "meson couldn't set up the build (a missing package? see above)."
	fi
	run_ninja || fail "The build failed (see above)."
	good "Built ${GREY}$(pwd)/build/dusk and dusk-settings${R}"
}

ARGS=()
for a in "$@"; do
	if [[ "$a" == -y || "$a" == --yes ]]; then DUSK_YES=1; else ARGS+=("$a"); fi
done
set -- "${ARGS[@]+"${ARGS[@]}"}"

case "${1:-build}" in
build)
	STEPS=2
	banner
	warning
	confirm "Build Dusk $DUSK_VERSION?"
	build
	finished "Dusk $DUSK_VERSION is built"
	;;
run)
	build
	# Inside an existing Wayland desktop, Dusk opens as a window.
	# DUSK_TERMINAL=kitty ./build.sh run  also starts a terminal in it.
	if [[ -n "${DUSK_TERMINAL:-}" ]]; then
		exec ./build/dusk -s "$DUSK_TERMINAL" "${@:2}"
	fi
	exec ./build/dusk "${@:2}"
	;;
install)
	STEPS=3
	if [[ -z "${DUSK_PROGRESS:-}" ]]; then
		banner
		warning
		if [[ -f "$HOME/.local/state/dusk/installed" ]]; then
			confirm "Update Dusk $(head -1 "$HOME/.local/state/dusk/installed") to $DUSK_VERSION?"
		else
			confirm "Install Dusk $DUSK_VERSION?"
		fi
	fi
	build
	step "Installing"
	PREVIOUS=""
	# The version you had (noted at each install; else the one in /usr/local),
	# so the list covers everything since then, however many versions it skipped.
	[[ -f "$HOME/.local/state/dusk/installed" ]] && PREVIOUS=$(head -1 "$HOME/.local/state/dusk/installed" | tr -d '[:space:]')
	[[ -z "$PREVIOUS" && -f /usr/local/share/dusk/version ]] && PREVIOUS=$(head -1 /usr/local/share/dusk/version | tr -d '[:space:]')
	[[ "$PREVIOUS" == "$DUSK_VERSION" ]] && PREVIOUS=""
	NEWS=$(whats_new "$PREVIOUS" "$DUSK_VERSION" || true)
	say "Copying Dusk into place ${GREY}(your password may be asked for)${R}"
	# What changed, for Settings › About and the "Dusk was updated" note.
	printf '%s\n' "$NEWS" | sudo install -Dm644 /dev/stdin /usr/local/share/dusk/whatsnew.txt
	printf '%s\n' "$DUSK_VERSION" | sudo install -Dm644 /dev/stdin /usr/local/share/dusk/version
	sudo install -Dm755 build/dusk /usr/local/bin/dusk
	sudo install -Dm755 build/dusk-settings /usr/local/bin/dusk-settings
	# Old-style tray icons (Wine and Proton games) in Dusk's tray.
	[ -x build/dusk-xembed ] && sudo install -Dm755 build/dusk-xembed /usr/local/bin/dusk-xembed
	sudo rm -f /usr/local/bin/dusk-settings-classic   # from 0.24 (the QML Settings, now gone)
	sudo install -Dm755 data/dusk-session /usr/local/bin/dusk-session
	sudo install -Dm755 data/dusk-update /usr/local/bin/dusk-update
	sudo install -Dm755 data/dusk-uninstall /usr/local/bin/dusk-uninstall
	# The lock screen's password check.
	sudo install -Dm644 data/pam-dusk-lock /etc/pam.d/dusk-lock
	remove_login_screen
	sudo install -Dm644 data/icons/dusk.svg /usr/local/share/icons/hicolor/scalable/apps/dusk.svg
	sudo install -Dm644 data/icons/org.dusk.Settings.svg /usr/local/share/icons/hicolor/scalable/apps/org.dusk.Settings.svg
	sudo gtk-update-icon-cache -qtf /usr/local/share/icons/hicolor 2>/dev/null || true
	sed 's|^Exec=.*|Exec=/usr/local/bin/dusk-session|' data/dusk.desktop |
		sudo install -Dm644 /dev/stdin /usr/share/wayland-sessions/dusk.desktop
	sudo install -Dm755 data/dusk-apply-theme /usr/local/bin/dusk-apply-theme
	sudo install -Dm755 data/dusk-doctor /usr/local/bin/dusk-doctor
	sudo install -Dm755 data/dusk-lyrics /usr/local/bin/dusk-lyrics
	sudo install -Dm755 data/dusk-record /usr/local/bin/dusk-record
	sudo install -Dm755 data/dusk-audio-outputs /usr/local/bin/dusk-audio-outputs
	# Bluetooth: many systems install its service switched off. With an
	# adapter there, start it now and at every start-up.
	if ls /sys/class/bluetooth 2>/dev/null | grep -q '^hci' && ! systemctl is-active --quiet bluetooth.service 2>/dev/null; then
		sudo systemctl enable --now bluetooth.service 2>/dev/null || warn "Couldn't start the Bluetooth service."
	fi
	sudo install -Dm644 data/dusk-session.target /usr/lib/systemd/user/dusk-session.target
	# Lets apps (libadwaita, Firefox, Electron...) see Dusk's light/dark setting.
	sudo install -Dm644 data/dusk-portals.conf /usr/share/xdg-desktop-portal/dusk-portals.conf
	sed 's|^Exec=.*|Exec=/usr/local/bin/dusk-settings|' data/org.dusk.Settings.desktop |
		sudo install -Dm644 /dev/stdin /usr/local/share/applications/org.dusk.Settings.desktop
	# Floe was this desktop's old name: take the old session off the login screen.
	sudo rm -f /usr/local/bin/floe /usr/local/bin/floe-settings \
		/usr/share/wayland-sessions/floe.desktop \
		/usr/local/share/applications/floe-settings.desktop \
		/usr/local/share/applications/org.floe.Settings.desktop
	# Built from ~/src (the zips): keep that folder out of sight in Files
	# (the file manager hides what's listed in a folder's .hidden file).
	if [[ "$(pwd)" == "$HOME/src/"* ]] && ! grep -qxF src "$HOME/.hidden" 2>/dev/null; then
		echo src >> "$HOME/.hidden"
	fi
	# Which version, from where (Settings › About › Updates).
	mkdir -p "$HOME/.local/state/dusk" && printf '%s\n%s\n' "$DUSK_VERSION" "$(pwd)" > "$HOME/.local/state/dusk/installed"
	finished "Dusk $DUSK_VERSION is installed"
	[[ -z "${DUSK_PROGRESS:-}" ]] && show_whats_new "$NEWS"
	[[ -n "${DUSK_PROGRESS:-}" ]] && printf '@@installed %s\n' "$DUSK_VERSION"
	if [[ -n "${DUSK_PROGRESS:-}" ]]; then
		:   # (Settings offers to log out itself)
	elif pgrep -u "$(id -u)" -x dusk >/dev/null 2>&1; then
		# Running Dusk now: the new version starts when you log in again.
		printf '\n'
		answer=n
		if [[ -t 0 && "${DUSK_YES:-}" != 1 ]]; then
			read -r -p "${B}Log out now to start the new version?${R} Save your work first. [y/N] " answer
		fi
		if [[ "$answer" == [yY]* ]]; then
			say "Logging out…"
			sleep 1
			pkill -TERM -u "$(id -u)" -x dusk || true
		else
			say "The new version starts the next time you log in."
			command -v notify-send >/dev/null &&
				notify-send -a Dusk -i dusk "Dusk $DUSK_VERSION is installed" "Log out and back in to start it." 2>/dev/null || true
		fi
	else
		say "Log out, then pick \"Dusk\" from the gear menu on the login screen."
	fi
	;;
update)
	exec bash data/dusk-update "${@:2}"
	;;
uninstall)
	# Everything, and what Dusk changed for other desktops put back.
	exec bash data/dusk-uninstall "${@:2}"
	;;
fonts)
	install_fonts again
	good "Fonts ready. Pick them in Settings › Appearance › Fonts."
	;;
*)
	echo "Usage: $0 [build|run|install|update|uninstall|fonts] [-y]"
	exit 1
	;;
esac
