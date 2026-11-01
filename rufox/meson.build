project(
  'rufus-linux',
  'c',
  version: '4.15.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.59',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
    'buildtype=debug',
  ],
)

cc = meson.get_compiler('c')

add_project_arguments(
  '-D_GNU_SOURCE',
  '-DRUFUS_LINUX_VERSION="@0@"'.format(meson.project_version()),
  language: 'c',
)

# ---------------------------------------------------------------------------
# Mandatory dependencies
# ---------------------------------------------------------------------------
glib_dep      = dependency('glib-2.0',       version: '>= 2.70')
gio_dep       = dependency('gio-2.0',        version: '>= 2.70')
gio_unix_dep  = dependency('gio-unix-2.0',   version: '>= 2.70')
gobject_dep   = dependency('gobject-2.0')
blkid_dep     = dependency('blkid')
libcdio_dep   = dependency('libcdio')
libiso9660_dep= dependency('libiso9660', required: false)
libudf_dep    = dependency('libudf',     required: false)
if not libiso9660_dep.found()
  libiso9660_dep = cc.find_library('iso9660', required: true)
endif
if not libudf_dep.found()
  libudf_dep = cc.find_library('udf', required: true)
endif
parted_dep    = cc.find_library('parted',    required: true)
m_dep         = cc.find_library('m')

# ---------------------------------------------------------------------------
# Optional dependencies (feature-gated)
# ---------------------------------------------------------------------------
gtk_dep   = dependency('gtk4',         version: '>= 4.6', required: get_option('gui'))
adw_dep   = dependency('libadwaita-1', version: '>= 1.0', required: get_option('gui'))
gudev_dep = dependency('gudev-1.0',    required: get_option('hotplug'))
udisks_dep= dependency('udisks2',      required: get_option('udisks'))
zlib_dep  = dependency('zlib',         required: false)
lzma_dep  = cc.find_library('lzma',    required: false)

have_gtk    = gtk_dep.found()
have_adw    = adw_dep.found()
have_gudev  = gudev_dep.found()
have_udisks = udisks_dep.found()

conf = configuration_data()
conf.set('HAVE_GTK',    have_gtk)
conf.set('HAVE_ADW',    have_adw)
conf.set('HAVE_GUDEV',  have_gudev)
conf.set('HAVE_UDISKS', have_udisks)
conf.set_quoted('RUFOX_DATADIR',  get_option('prefix') / get_option('datadir') / 'rufox')
conf.set_quoted('RUFOX_LIBEXECDIR', get_option('prefix') / get_option('libexecdir'))
conf.set_quoted('RUFOX_LOCALEDIR', get_option('localedir'))
configure_file(output: 'rufox-config.h', configuration: conf)

# 'src/ui' only exists when the GTK front-end is being built; referencing it
# unconditionally makes `meson setup` fail for headless/CLI builds.
rufox_inc = include_directories('.', 'src')
if have_gtk and have_adw
  rufox_inc = include_directories('.', 'src', 'src/ui')
endif

# ---------------------------------------------------------------------------
# Core library: everything that is not UI
# ---------------------------------------------------------------------------
core_src = files(
  'src/cancel.c',
  'src/cliopts.c',
  'src/rlog.c',
  'src/rutil.c',
  'src/rproc.c',
  'src/dev.c',
  'src/diskio.c',
  'src/rtarget.c',
  'src/partition.c',
  'src/format.c',
  'src/iso_scan.c',
  'src/iso_extract.c',
  'src/bootloaders.c',
  'src/hash.c',
  'src/badblocks.c',
  'src/persistence.c',
  'src/wintogo_stub.c',
  'src/job.c',
)

core_deps = [
  glib_dep, gio_dep, gio_unix_dep, gobject_dep,
  blkid_dep, libcdio_dep, libiso9660_dep, libudf_dep, parted_dep, m_dep,
]
if zlib_dep.found()
  core_deps += zlib_dep
  add_project_arguments('-DHAVE_ZLIB=1', language: 'c')
endif
if lzma_dep.found()
  core_deps += lzma_dep
  add_project_arguments('-DHAVE_LZMA=1', language: 'c')
endif
if have_udisks
  core_deps += udisks_dep
endif
if have_gudev
  core_deps += gudev_dep
endif

librufox_core = static_library(
  'rufox-core',
  core_src,
  dependencies: core_deps,
  include_directories: rufox_inc,
  install: false,
)

rufox_core_dep = declare_dependency(
  link_with: librufox_core,
  dependencies: core_deps,
  include_directories: rufox_inc,
)

# ---------------------------------------------------------------------------
# CLI front-end (also our headless test harness)
# ---------------------------------------------------------------------------
executable(
  'rufox',
  'src/main_cli.c',
  dependencies: [rufox_core_dep],
  install: true,
)

# ---------------------------------------------------------------------------
# GUI front-end
# ---------------------------------------------------------------------------
if have_gtk and have_adw
  executable(
    'rufox-gui',
    ['src/main_gui.c', 'src/ui/engine.c', 'src/ui/app.c', 'src/ui/window.c'],
    dependencies: [rufox_core_dep, gtk_dep, adw_dep],
    c_args: ['-DHAVE_GTK_UI=1'],
    install: true,
  )

  install_data('data/rufox-gui.desktop',
               install_dir: get_option('datadir') / 'applications')
  install_data('data/rufox.gschema.xml',
               install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
endif

# ---------------------------------------------------------------------------
# Privileged helper (runs under pkexec for distros without udisks2)
# ---------------------------------------------------------------------------
executable(
  'rufox-helper',
  'tools/rufox-helper.c',
  dependencies: [rufox_core_dep],
  install: true,
  install_dir: get_option('libexecdir'),
)

install_data('data/rufox-helper.policy',
             install_dir: get_option('datadir') / 'polkit-1' / 'actions')

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
test(
  'unit',
  executable('rufox-tests', 'tests/test_core.c',
             dependencies: [rufox_core_dep], install: false),
  protocol: 'exitcode',
)
