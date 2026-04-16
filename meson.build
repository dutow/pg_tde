project('pg_tde',
  ['c'],
  version: '2.1.2',
  meson_version: '>=0.56',
)

fs = import('fs')

pg_config = find_program(get_option('pg_config'))

bindir = run_command(pg_config, '--bindir', check: true).stdout().strip()
includedir = run_command(pg_config, '--includedir', check: true).stdout().strip()
includedir_server = run_command(pg_config, '--includedir-server', check: true).stdout().strip()
libdir = run_command(pg_config, '--libdir', check: true).stdout().strip()
pkgincludedir = run_command(pg_config, '--pkgincludedir', check: true).stdout().strip()
pkglibdir = run_command(pg_config, '--pkglibdir', check: true).stdout().strip()
sharedir = run_command(pg_config, '--sharedir', check: true).stdout().strip()
version = run_command(pg_config, '--version', check: true).stdout().strip()

cc = meson.get_compiler('c')

# To make sure we compile code taken from PostgreSQL safely we need to use the
# same functional flags.
#
# These are copied from common_functional_flags in meson.build in PostgreSQL 18.
add_project_arguments(cc.get_supported_arguments([
  '-fno-strict-aliasing',
  '-fwrapv',
  '-fexcess-precision=standard',
]), language: ['c'])

# Use MSVC's standard-conforming preprocessor. Without it, variadic macro
# expansion with an empty __VA_ARGS__ leaves a stray trailing comma, which
# breaks every elog(ERROR, "msg") call in the frontend include pg_tde_fe.h.
if cc.get_id() == 'msvc'
  add_project_arguments('/Zc:preprocessor', language: 'c')
endif

# TODO: Why does it seems like they sometimes are in pkglibdir and other times in libdir?
pgport = cc.find_library('pgport', dirs: [pkglibdir, libdir], static: true)
pgcommon = cc.find_library('pgcommon', dirs: [pkglibdir, libdir], static: true)
pgfeutils = cc.find_library('pgfeutils', dirs: [pkglibdir, libdir], static: true)

pq = cc.find_library('pq', dirs: [libdir])
crypto = dependency('libcrypto')
ssl = dependency('libssl')
curl = dependency('libcurl')
lz4 = dependency('liblz4', required: false)
z = dependency('zlib')
zstd = dependency('libzstd', required: false)

major_version = version.split(' ')[1].split('.')[0].to_int()

fetools = 'fetools/pg@0@'.format(major_version)

incdir_list = [
  'src/include',
  'src/libkmip/libkmip/include',
  fetools / 'include',
  includedir_server,
  includedir,
  pkgincludedir / 'internal',
]

# On Windows, PostgreSQL ships POSIX/Winsock shims under server/port/{win32,win32_msvc}
# (e.g. netinet/in.h, dirent.h, unistd.h). PG's own build adds these to the include
# path automatically; out-of-tree extensions must do the same.
if host_machine.system() == 'windows'
  incdir_list += [
    includedir_server / 'port/win32',
    includedir_server / 'port/win32_msvc',
  ]
endif

incdirs = include_directories(incdir_list)

backend_link_args = []
backend_deps = [crypto, ssl, curl]

if host_machine.system() == 'darwin'
  backend_link_args += ['-bundle_loader', bindir / 'postgres']
endif

# On Windows, loadable modules must resolve backend symbols at link time via
# postgres.lib (the import library for postgres.exe).
if host_machine.system() == 'windows'
  backend_deps += cc.find_library('postgres', dirs: [libdir])
endif

# libkmip is not portable to MSVC (statement expressions, Windows SDK name
# collisions, CP-1252 source). Skip the KMIP keyring provider on Windows.
build_kmip = host_machine.system() != 'windows'

kmip_c_args = []
if build_kmip
  kmip_c_args += '-DHAVE_LIBKMIP'
endif

install_data(
  'pg_tde.control',
  'pg_tde--1.0.sql',
  'pg_tde--1.0--2.0.sql',
  'pg_tde--2.0--2.1.sql',
  install_dir: sharedir / 'extension',
)

backend_sources = [
  'src/access/pg_tde_tdemap.c',
  'src/access/pg_tde_xlog.c',
  'src/access/pg_tde_xlog_keys.c',
  'src/access/pg_tde_xlog_smgr.c',
  'src/catalog/tde_keyring.c',
  'src/catalog/tde_keyring_parse_opts.c',
  'src/catalog/tde_principal_key.c',
  'src/common/pg_tde_utils.c',
  'src/encryption/enc_aes.c',
  'src/encryption/enc_tde.c',
  'src/keyring/keyring_api.c',
  'src/keyring/keyring_curl.c',
  'src/keyring/keyring_file.c',
  'src/keyring/keyring_vault.c',
  'src/pg_tde.c',
  'src/pg_tde_event_capture.c',
  'src/pg_tde_guc.c',
  'src/smgr/pg_tde_smgr.c',
]

frontend_sources = [
  'src/access/pg_tde_tdemap.c',
  'src/access/pg_tde_xlog_keys.c',
  'src/catalog/tde_keyring.c',
  'src/catalog/tde_keyring_parse_opts.c',
  'src/catalog/tde_principal_key.c',
  'src/common/pg_tde_utils.c',
  'src/encryption/enc_aes.c',
  'src/encryption/enc_tde.c',
  'src/keyring/keyring_api.c',
  'src/keyring/keyring_curl.c',
  'src/keyring/keyring_file.c',
  'src/keyring/keyring_vault.c',
]

kmip_link = []

if build_kmip
  backend_sources += [
    'src/keyring/keyring_kmip.c',
    'src/keyring/keyring_kmip_impl.c',
  ]
  frontend_sources += [
    'src/keyring/keyring_kmip.c',
    'src/keyring/keyring_kmip_impl.c',
  ]

  kmip = static_library('kmip',
    files(
      'src/libkmip/libkmip/src/kmip.c',
      'src/libkmip/libkmip/src/kmip_bio.c',
      'src/libkmip/libkmip/src/kmip_locate.c',
      'src/libkmip/libkmip/src/kmip_memset.c',
    ),
    c_args: ['-w'], # This is a 3rd party, disable warnings completely
    include_directories: incdirs,
  )
  kmip_link += kmip
else
  backend_sources += 'src/keyring/keyring_kmip_stub.c'
  frontend_sources += 'src/keyring/keyring_kmip_stub.c'
endif

shared_module('pg_tde',
  files(backend_sources),
  c_args: kmip_c_args,
  include_directories: incdirs,
  install: true,
  install_dir: pkglibdir,
  dependencies: backend_deps,
  name_prefix: '',
  link_with: kmip_link,
  link_args: backend_link_args,
)

pg_tde_frontend = static_library('pg_tde_frontend',
  files(frontend_sources),
  c_args: ['-DFRONTEND'] + kmip_c_args,
  dependencies: [pgfeutils, pgcommon, pgport, curl, crypto, ssl],
  include_directories: incdirs,
  link_with: kmip_link,
)

pg_tde_frontend_xlog = static_library('pg_tde_frontend_xlog',
  files(
    'src/access/pg_tde_xlog_smgr.c',
    fetools / 'xlogreader.c',
  ),
  dependencies: [lz4, zstd],
  c_args: ['-DFRONTEND'],
  include_directories: incdirs,
  link_with: kmip_link,
)

executable('pg_tde_change_key_provider',
  files('src/bin/pg_tde_change_key_provider.c'),
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  link_with: [pg_tde_frontend],
)

executable('pg_tde_archive_decrypt',
  files('src/bin/pg_tde_archive_decrypt.c'),
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  link_with: [pg_tde_frontend, pg_tde_frontend_xlog],
)

executable('pg_tde_restore_encrypt',
  files('src/bin/pg_tde_restore_encrypt.c'),
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  link_with: [pg_tde_frontend, pg_tde_frontend_xlog],
)

executable('pg_tde_upgrade',
  files('src/bin/pg_tde_upgrade.c'),
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  dependencies: [pgcommon, pgport],
)

if major_version == 18
  bb_sources = files(
    fetools / 'pg_basebackup/receivelog.c',
    fetools / 'pg_basebackup/streamutil.c',
    fetools / 'pg_basebackup/walmethods.c',
    fetools / 'pg_basebackup/pg_basebackup.c',
    fetools / 'pg_basebackup/astreamer_inject.c',
  )
else
  bb_sources = files(
     fetools / 'pg_basebackup/receivelog.c',
     fetools / 'pg_basebackup/streamutil.c',
     fetools / 'pg_basebackup/walmethods.c',
     fetools / 'pg_basebackup/pg_basebackup.c',
     fetools / 'pg_basebackup/bbstreamer_file.c',
     fetools / 'pg_basebackup/bbstreamer_gzip.c',
     fetools / 'pg_basebackup/bbstreamer_inject.c',
     fetools / 'pg_basebackup/bbstreamer_lz4.c',
     fetools / 'pg_basebackup/bbstreamer_tar.c',
     fetools / 'pg_basebackup/bbstreamer_zstd.c',
  )
endif

basebackup_deps = [pq, z]

# pg_basebackup uses select() which requires Winsock on Windows.
if host_machine.system() == 'windows'
  basebackup_deps += cc.find_library('ws2_32')
endif

executable('pg_tde_basebackup',
  bb_sources,
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  dependencies: basebackup_deps,
  link_with: [pg_tde_frontend, pg_tde_frontend_xlog],
)

executable('pg_tde_checksums',
  files(
    fetools / 'pg_checksums/pg_checksums.c',
  ),
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  link_with: [pg_tde_frontend],
)

executable('pg_tde_resetwal',
  files(
    fetools / 'pg_resetwal/pg_resetwal.c',
  ),
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  link_with: [pg_tde_frontend, pg_tde_frontend_xlog],
)

executable('pg_tde_rewind',
  files(
    fetools / 'pg_rewind/datapagemap.c',
    fetools / 'pg_rewind/file_ops.c',
    fetools / 'pg_rewind/filemap.c',
    fetools / 'pg_rewind/libpq_source.c',
    fetools / 'pg_rewind/local_source.c',
    fetools / 'pg_rewind/parsexlog.c',
    fetools / 'pg_rewind/pg_rewind.c',
    fetools / 'pg_rewind/timeline.c',
  ),
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  dependencies: [pq],
  link_with: [pg_tde_frontend, pg_tde_frontend_xlog],
)

executable('pg_tde_waldump',
  files(
    fetools / 'pg_waldump/compat.c',
    fetools / 'pg_waldump/pg_waldump.c',
    fetools / 'pg_waldump/rmgrdesc.c',
    fetools / 'rmgrdesc/brindesc.c',
    fetools / 'rmgrdesc/clogdesc.c',
    fetools / 'rmgrdesc/committsdesc.c',
    fetools / 'rmgrdesc/dbasedesc.c',
    fetools / 'rmgrdesc/genericdesc.c',
    fetools / 'rmgrdesc/gindesc.c',
    fetools / 'rmgrdesc/gistdesc.c',
    fetools / 'rmgrdesc/hashdesc.c',
    fetools / 'rmgrdesc/heapdesc.c',
    fetools / 'rmgrdesc/logicalmsgdesc.c',
    fetools / 'rmgrdesc/mxactdesc.c',
    fetools / 'rmgrdesc/nbtdesc.c',
    fetools / 'rmgrdesc/relmapdesc.c',
    fetools / 'rmgrdesc/replorigindesc.c',
    fetools / 'rmgrdesc/rmgrdesc_utils.c',
    fetools / 'rmgrdesc/seqdesc.c',
    fetools / 'rmgrdesc/smgrdesc.c',
    fetools / 'rmgrdesc/spgdesc.c',
    fetools / 'rmgrdesc/standbydesc.c',
    fetools / 'rmgrdesc/tblspcdesc.c',
    fetools / 'rmgrdesc/xactdesc.c',
    fetools / 'rmgrdesc/xlogdesc.c',
    fetools / 'xlogstats.c',
  ),
  include_directories: incdirs,
  install: true,
  install_dir: bindir,
  c_args: ['-DFRONTEND'],
  link_with: [pg_tde_frontend, pg_tde_frontend_xlog],
)

tap_tests = [
  't/2pc_replication.pl',
  't/basic.pl',
  't/change_key_provider.pl',
  't/crash_recovery.pl',
  't/key_rotate_tablespace.pl',
  't/keys_update.pl',
  't/key_validation.pl',
  't/multiple_extensions.pl',
  't/pg_basebackup.pl',
  't/pg_resetwal_basic.pl',
  't/pg_resetwal_corrupted.pl',
  't/pg_rewind_basic.pl',
  't/pg_rewind_databases.pl',
  't/pg_rewind_extrafiles.pl',
  't/pg_rewind_growing_files.pl',
  't/pg_rewind_keep_recycled_wals.pl',
  't/pg_rewind_min_recovery_point.pl',
  't/pg_rewind_options.pl',
  't/pg_rewind_pg_xlog_symlink.pl',
  't/pg_rewind_same_timeline.pl',
  't/pg_tde_change_key_provider.pl',
  't/pg_tde_upgrade.pl',
  't/pg_waldump_basic.pl',
  't/pg_waldump_fullpage.pl',
  't/regress.pl',
  't/replication.pl',
  't/reuse_relfilenode_in_cache.pl',
  't/rotate_key.pl',
  't/stream_rep.pl',
  't/tde_heap_aes_256.pl',
  't/tde_heap.pl',
  't/unlogged_tables.pl',
  't/wal_archiving.pl',
  't/wal_encrypt.pl',
  't/wal_key_tli.pl',
]

testwrap = files('testwrap')
perl = find_program('perl')
pg_regress = find_program('pg_regress', dirs: [pkglibdir / 'pgxs/src/test/regress'])

env = environment()
env.prepend('PATH', bindir)
env.set('PG_REGRESS', pg_regress.full_path())
env.set('top_builddir', meson.project_build_root())

foreach test : tap_tests
  test(test,
    testwrap,
    protocol: 'tap',
    env: env,
    args: [
      fs.stem(fs.name(test)),
      meson.project_source_root(),
      perl.full_path(),
      '-I', meson.project_source_root() / 't',
      '-I', pkglibdir / 'pgxs/src/test/perl',
      files(test),
    ],
  )
endforeach
