project(
	'dusk',
	'cpp',
	version: '0.35.13',
	meson_version: '>=1.3.0',
	default_options: [
		'cpp_std=c++20',
		'warning_level=2',
		'buildtype=debugoptimized',
	],
)

add_project_arguments(
	'-DWLR_USE_UNSTABLE',
	'-DDUSK_VERSION="' + meson.project_version() + '"',   # (the version lives only above)
	'-Wno-unused-parameter',
	'-Wno-missing-field-initializers',
	'-Wno-pedantic',
	language: 'cpp',
)

wlroots = dependency('wlroots-0.20', version: ['>=0.20.0', '<0.21.0'])
wayland_server = dependency('wayland-server')
wayland_protocols = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
libdrm = dependency('libdrm')
libinput = dependency('libinput')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
gdk_pixbuf = dependency('gdk-pixbuf-2.0')
libsystemd = dependency('libsystemd')   # sd-bus, for notifications
pam = meson.get_compiler('cpp').find_library('pam')   # the lock screen checks your password

# scenefx: rounded corners and shadows. Uses a system copy if there is one,
# otherwise downloads and builds it (see subprojects/scenefx.wrap).
scenefx = dependency('scenefx-0.5',
	fallback: ['scenefx', 'scenefx'],
	default_options: [
		'default_library=static',
		'examples=false',
		'color-management=disabled',
		'werror=false',
	],
)

dusk_deps = [
	scenefx, wlroots, wayland_server, xkbcommon, pixman, libdrm, libinput, cairo,
	pangocairo, gdk_pixbuf, libsystemd, pam,
]

# X11 apps through XWayland (needs wlroots built with it, plus xcb).
xcb = dependency('xcb', required: false)
xcb_icccm = dependency('xcb-icccm', required: false)
have_xwayland = wlroots.get_variable(pkgconfig: 'have_xwayland', default_value: 'false') == 'true'
if have_xwayland and xcb.found() and xcb_icccm.found()
	add_project_arguments('-DDUSK_XWAYLAND', language: 'cpp')
	dusk_deps += [xcb, xcb_icccm]
	message('XWayland support: yes')
else
	warning('XWayland support: no (X11 apps will not run)')
endif

# wlroots headers include protocol headers that each compositor generates.
scanner = find_program(
	dependency('wayland-scanner', native: true).get_variable('wayland_scanner'),
	native: true,
)
protocol_dir = wayland_protocols.get_variable('pkgdatadir')
protocols = [
	protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	protocol_dir / 'stable/viewporter/viewporter.xml',
	protocol_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
	protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
	protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
	protocol_dir / 'staging/tearing-control/tearing-control-v1.xml',
	protocol_dir / 'stable/tablet/tablet-v2.xml',
	protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
	protocol_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
	protocol_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml',
	protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
	protocol_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
	protocol_dir / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml',
]
protocol_headers = []
foreach xml : protocols
	protocol_headers += custom_target(
		xml.underscorify() + '_server_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

executable(
	'dusk',
	[
		'src/main.cpp',
		'src/server.cpp',
		'src/session.cpp',
		'src/idle.cpp',
		'src/screenshot.cpp',
		'src/tiling.cpp',
		'src/anim.cpp',
		'src/apps.cpp',
		'src/ccentre.cpp',
		'src/audiolevel.cpp',
		'src/mediaview.cpp',
		'src/lyrics.cpp',
		'src/recorder.cpp',
		'src/clipboard.cpp',
		'src/desktop.cpp',
		'src/shot.cpp',
		'src/wallpaper.cpp',
		'src/tray.cpp',
		'src/phone.cpp',
		'src/overview.cpp',
		'src/workspaces.cpp',
		'src/trash.cpp',
		'src/protocols.cpp',
		'src/gestures.cpp',
		'src/crash.cpp',
		'src/ghost.cpp',
		'src/glyphs.cpp',
		'src/notify.cpp',
		'src/lock.cpp',
		'src/osd.cpp',
		'src/osk.cpp',
		'src/popover.cpp',
		'src/ui.cpp',
		'src/system.cpp',
		'src/view.cpp',
		'src/xdg_view.cpp',
		'src/xwayland.cpp',
		'src/panel.cpp',
		'src/launcher.cpp',
		'src/config.cpp',
		'src/draw.cpp',
		'src/imageload.cpp',
		protocol_headers,
	],
	dependencies: dusk_deps,
	install: true,
)

# dusk-xembed: old-style (XEmbed) tray icons (Wine, Proton games) in Dusk's
# tray. Its own process; Dusk starts it with Xwayland.
xcb_composite = dependency('xcb-composite', required: false)
xcb_damage = dependency('xcb-damage', required: false)
if xcb.found() and xcb_composite.found() and xcb_damage.found()
	executable(
		'dusk-xembed',
		'src/xembed/main.cpp',
		dependencies: [xcb, xcb_composite, xcb_damage, libsystemd],
		install: true,
	)
endif

install_data(
	'data/dusk.desktop',
	install_dir: get_option('datadir') / 'wayland-sessions',
)

# Dusk Settings (Qt 6, Qt Widgets): settings/. Built into the same folder as
# dusk so the app menu can find it before it is installed. Qt draws no title
# bar of its own, so Dusk's window frame is used, like every app.
qt6 = dependency('qt6', modules: ['Core', 'Gui', 'Widgets'], required: false)
if qt6.found()
	executable(
		'dusk-settings',
		'settings/main.cpp',
		dependencies: [qt6, xkbcommon],
		link_args: ['-rdynamic'],
		pie: true,
		install: true,
	)
	install_data(
		'data/org.dusk.Settings.desktop',
		install_dir: get_option('datadir') / 'applications',
	)
endif
