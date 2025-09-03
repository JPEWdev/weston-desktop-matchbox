project('weston-xdg-shell', 'c')
conf_data = configuration_data()
conf_data.set('version', '0.0.1')
conf_data.set('datadir', get_option('datadir'))
conf_data.set('prefix', get_option('prefix'))

compiler = meson.get_compiler('c')

wl_client = dependency('wayland-client')
wl_scanner = dependency('wayland-scanner')
wl_protocols = dependency('wayland-protocols')
wl_cursor = dependency('wayland-cursor')
cairo = dependency('cairo')
gio_2_0 = dependency('gio-2.0')
libm = compiler.find_library('m')
wayland_scanner = wl_scanner.get_variable('wayland_scanner')
wl_protocols_path = wl_protocols.get_variable('pkgdatadir')

desktop_shell_header = custom_target(
  'weston-desktop-shell-protocol-header',
  output: 'weston-desktop-shell.h',
  input: 'protocol/weston-desktop-shell.xml',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
)

desktop_shell_code = custom_target(
  'weston-desktop-shell-protocol-code',
  output: 'weston-desktop-shell.c',
  input: 'protocol/weston-desktop-shell.xml',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
)

xdg_shell_header = custom_target(
  'xdg-shell-protocol-header',
  output: 'xdg-shell.h',
  input: wl_protocols_path + '/stable/xdg-shell/xdg-shell.xml',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
)

xdg_shell_code = custom_target(
  'xdg-shell-protocol-code',
  output: 'xdg-shell.c',
  input: wl_protocols_path + '/stable/xdg-shell/xdg-shell.xml',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
)

viewporter_header = custom_target(
  'viewporter-protocol-header',
  output: 'viewporter.h',
  input: wl_protocols_path + '/stable/viewporter/viewporter.xml',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
)

viewporter_code = custom_target(
  'viewporter-protocol-code',
  output: 'viewporter.c',
  input: wl_protocols_path + '/stable/viewporter/viewporter.xml',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
)

weston_desktop_matchbox = executable(
  'weston-desktop-matchbox',
  'src/main.c',
  desktop_shell_code,
  desktop_shell_header,
  xdg_shell_code,
  xdg_shell_header,
  viewporter_code,
  viewporter_header,
  dependencies: [wl_client, wl_cursor, cairo, gio_2_0, libm],
  install: true,
  install_dir: get_option('libexecdir')
)

## Run the shell under weston
#run_target('weston',
#  depends: [weston_desktop_matchbox],
#  command: ['weston', '-c', meson.project_source_root() / 'weston.ini'],
#)
