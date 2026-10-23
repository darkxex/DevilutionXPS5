#!/usr/bin/env python3
"""Build DevilutionX for PS5 (native application, title PPSA98514).

  sdl       build SDL2 for the console, with its Vulkan render driver (.deps/native/sdl2)
  configure cmake configure (cross build of the unmodified core)
  build     compile and link
  package   sign the application folder dist/PPSA98514 and zip it
  deploy    upload dist/PPSA98514 to a console over FTP (--ip)

The core (Source/, CMake/, 3rdParty/ and the top-level CMakeLists.txt) is not touched.
This port is independent of Packaging/ps5 (the payload-ELF port): other title ID,
other data folder (/data/DevilutionX) and its own dependencies in ps5port/.deps.
"""
import argparse
import ftplib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BUILD = HERE / 'build'
DEPS = HERE / '.deps'
SDK = DEPS / 'native' / 'ps5-payload-sdk'
RADV = DEPS / 'native' / 'radv-release'  # RADV (Mesa's AMD Vulkan driver for the console) and the Vulkan headers
STUBS = BUILD / 'stubs'  # link stubs of the system modules the SDK has no stubs for (AGC)
SDL = DEPS / 'native' / 'sdl2'
SRC = DEPS / 'src'
CMAKE_DIR = BUILD / 'cmake'
RT = BUILD / 'rt'
TITLE_ID = 'PPSA98514'
# Loader/container constants of the native app format (firmware 6.02 to 12.70)
MODULE_SDK = '0x02000009'
COMPANION_SDK = '0x08050001'
FSELF_MAGIC = '0x1D3D154F'
CLANG = os.environ.get('CLANG', 'clang-18')



def run(*cmd, **kw):
    cmd = [str(c) for c in cmd]
    print('+', ' '.join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def toolchain_env():
    env = os.environ.copy()
    env.update(PS5_PAYLOAD_SDK=str(SDK), PS5_CLANG=shutil.which(CLANG) or CLANG, USE_CCACHE='0',
               PS5_RT_OBJECTS=' '.join(str(rt_object(src)) for src, _ in RT_SOURCES),
               PS5_RADV_ARCHIVE=str(RADV / 'lib' / 'libvulkan_radeon.ps5.a'), PS5_AGC_STUBS=str(STUBS),
               PKG_CONFIG_LIBDIR=str(BUILD / 'pc'))
    return env


SDL_URL = 'https://github.com/ps5-payload-dev/SDL'
SDL_COMMIT = '8c56053f13ca13a0c050de613706ff69eb615836'


def patch_sdl(snapshot):
    """Static build switch, joystick fixes (SDL2/*.patch) and the registration of the PS5 render driver."""
    run('git', 'init', '--quiet', snapshot)
    for name in ('static-ps5.patch', 'ps5-joystick.patch'):
        run('git', 'apply', HERE / 'sdl2' / name, cwd=snapshot)
    # the sandbox keeps the process in "/" (chdir is refused) and argv[0] has no path: the app tells SDL where it is
    filesystem = snapshot / 'src' / 'filesystem' / 'ps5' / 'SDL_sysfilesystem.c'
    text = filesystem.read_text()
    anchor = '    const char** argv = getargv();\n    char dirname[PATH_MAX];\n'
    if text.count(anchor) != 1:
        raise SystemExit('SDL_sysfilesystem.c changed: the base path anchor was not found')
    text = text.replace(anchor, anchor + '    const char *base = getenv("SDL_PS5_BASE_PATH");\n\n'
                                         '    if (base && *base) {\n        return strdup(base);\n    }\n')
    filesystem.write_text(text.replace('#include <string.h>\n', '#include <string.h>\n#include <stdlib.h>\n', 1))
    render = snapshot / 'src' / 'render' / 'SDL_render.c'
    text = render.read_text()
    anchor = '#if SDL_VIDEO_RENDER_SW\n    &SW_RenderDriver\n'
    if text.count(anchor) != 1:
        raise SystemExit('SDL_render.c changed: the PS5 render driver anchor was not found')
    render.write_text(text.replace(anchor, '#if SDL_VIDEO_RENDER_PS5VK\n    &PS5VK_RenderDriver,\n#endif\n' + anchor))
    header = snapshot / 'src' / 'render' / 'SDL_sysrender.h'
    text = header.read_text()
    anchor = 'extern SDL_RenderDriver SW_RenderDriver;'
    if text.count(anchor) != 1:
        raise SystemExit('SDL_sysrender.h changed: the PS5 render driver anchor was not found')
    header.write_text(text.replace(anchor, anchor + '\nextern SDL_RenderDriver PS5VK_RenderDriver;'))


def cmd_sdl(args):
    """Build SDL2 (static) for the console, with the Vulkan render driver; installs into .deps/native/sdl2."""
    source = SRC / 'SDL'
    if not (source / '.git').exists():
        run('git', 'clone', '-q', SDL_URL, source)
    run('git', '-C', source, 'cat-file', '-e', SDL_COMMIT + '^{commit}')
    work = BUILD / 'sdl2'
    shutil.rmtree(work, ignore_errors=True)
    snapshot = work / 'SDL'
    snapshot.mkdir(parents=True)
    archive = work / 'sdl-source.tar'
    with archive.open('wb') as stream:
        run('git', '-C', source, 'archive', SDL_COMMIT, stdout=stream)
    with tarfile.open(archive) as tar:
        tar.extractall(snapshot, filter='data')
    patch_sdl(snapshot)
    prefix = DEPS / 'native' / 'sdl2'
    shutil.rmtree(prefix, ignore_errors=True)
    env = toolchain_env()
    run('cmake', '-S', HERE / 'sdl2', '-B', work / 'cmake', '-G', 'Ninja', f'-DSDL_SOURCE={snapshot}',
        f'-DPS5_VULKAN_PREFIX={RADV}', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SYSTEM_NAME=Generic',
        '-DCMAKE_SYSTEM_PROCESSOR=x86_64', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
        f'-DCMAKE_C_COMPILER={HERE / "tooling" / "prospero-clang18"}',
        f'-DCMAKE_CXX_COMPILER={HERE / "tooling" / "prospero-clang18"}',
        f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DCMAKE_INSTALL_LIBDIR=lib',
        '-DCMAKE_C_FLAGS=-D__PROSPERO__ -fPIC -ffunction-sections -fdata-sections', env=env)
    run('cmake', '--build', work / 'cmake', '--parallel', env=env)
    run('cmake', '--install', work / 'cmake', env=env)


# The startup objects every link gets (tooling/ps5-cc): C runtime, heap, the entry and the sandbox exit.
RT_SOURCES = (
    ('tooling/native/app_crt.cpp', ['-std=c++20', '-fno-exceptions', '-fno-rtti']),
    ('tooling/native/app_cpp_runtime.cpp', ['-std=c++20', '-fno-exceptions', '-fno-rtti']),
    ('tooling/native/app_heap.c', ['-std=c11']),
    ('app/ps5_entry.c', ['-std=c11']),
    ('app/ps5_unlock.c', ['-std=c11']),
    ('app/elevation_shim.cpp', ['-std=c++20', '-fno-exceptions', '-fno-rtti']),
    ('app/elevation/elevation.cpp', ['-std=c++20', '-fno-exceptions', '-fno-rtti']),
)


def rt_object(src):
    path = Path(src)
    return RT / ((path.parent.name + '_' if path.parent.name == 'elevation' else '') + path.stem + '.o')


def build_runtime():
    RT.mkdir(parents=True, exist_ok=True)
    env = toolchain_env()
    cc = ['sh', str(HERE / 'tooling' / 'prospero-clang18'), '-O2', '-ffunction-sections', '-fdata-sections',
          '-D__PROSPERO__', f'-I{HERE / "app"}']
    for src, flags in RT_SOURCES:
        run(*cc, *flags, '-c', HERE / src, '-o', rt_object(src), env=env)
    # RADV calls AGC, which the SDK has no stubs for: link-only libraries that name its imports
    STUBS.mkdir(parents=True, exist_ok=True)
    for library, source in (('libSceAgc', 'agc_link_stub'), ('libSceAgcDriver', 'agc_driver_link_stub')):
        obj = STUBS / f'{source}.o'
        run(*cc, '-std=c11', '-fPIC', '-c', HERE / 'tooling' / 'ps5-stubs' / f'{source}.c', '-o', obj, env=env)
        run(SDK / 'bin' / 'prospero-lld', '--shared', '-soname', f'{library}.prx', '-o', STUBS / f'{library}.so', obj,
            env=env)


def write_toolchain():
    tc = BUILD / 'ps5-toolchain.cmake'
    tc.parent.mkdir(parents=True, exist_ok=True)
    flags = '-ffunction-sections -fdata-sections -fno-strict-aliasing -D_GNU_SOURCE -DDEVILUTIONX_FIXED_DISPLAY'
    # the console's libc has the math functions inside libc: empty archives for what cmake projects look for
    stubs = BUILD / 'stublibs'
    (stubs / 'lib').mkdir(parents=True, exist_ok=True)
    for name in ('libm.a',):
        archive = stubs / 'lib' / name
        archive.unlink(missing_ok=True)
        run(shutil.which('llvm-ar-18') or 'llvm-ar', 'rc', archive)
    tc.write_text(f"""# Generated by build_ps5.py: the core is built as the PC version (the console is FreeBSD) with the PS5 compiler driver.
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER {HERE}/tooling/ps5-cc)
set(CMAKE_CXX_COMPILER {HERE}/tooling/ps5-c++)
set(CMAKE_AR {shutil.which('llvm-ar-18') or 'llvm-ar'} CACHE FILEPATH "")
set(CMAKE_RANLIB {shutil.which('llvm-ranlib-18') or 'llvm-ranlib'} CACHE FILEPATH "")
set(CMAKE_STRIP {shutil.which('llvm-strip-18') or 'llvm-strip'} CACHE FILEPATH "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "{flags} -include {HERE}/stubs/ps5_compat.h")
set(CMAKE_CXX_FLAGS_INIT "{flags} -fexceptions -frtti")
set(CMAKE_FIND_ROOT_PATH {stubs} {SDL} {DEPS}/native/zlib/root/usr {SDK}/target)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH {SDL})
""")
    return tc


def cmd_configure(args):
    build_runtime()
    tc = write_toolchain()
    if CMAKE_DIR.exists():
        shutil.rmtree(CMAKE_DIR)
    run('cmake', '-S', ROOT, '-B', CMAKE_DIR, '-G', 'Ninja', f'-DCMAKE_TOOLCHAIN_FILE={tc}',
        '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF', '-DDISCORD_INTEGRATION=OFF', '-DDISABLE_LTO=ON',
        '-DASAN=OFF', '-DUBSAN=OFF', '-DBUILD_ASSETS_MPQ=ON', '-DBUILD_MOD_MPQS=ON', '-DNONET=OFF',
        '-DDEVILUTIONX_SYSTEM_SDL2=ON', '-DDEVILUTIONX_STATIC_SDL2=ON', f'-DSDL2_DIR={SDL}/lib/cmake/SDL2',
        '-DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF',
        # 720p internal resolution by default (the console output is always full screen, 4K)
        '-DDEFAULT_WIDTH=1280', '-DDEFAULT_HEIGHT=720',
        # third-party libraries are built from source, statically: nothing of the build machine's gets linked
        '-DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF', '-DDEVILUTIONX_STATIC_LIBSODIUM=ON', '-DDEVILUTIONX_SYSTEM_BZIP2=OFF',
        '-DDEVILUTIONX_STATIC_BZIP2=ON', '-DDEVILUTIONX_SYSTEM_SHEENBIDI=OFF', '-DDEVILUTIONX_SYSTEM_UNORDERED_DENSE=OFF',
        '-DDEVILUTIONX_SYSTEM_MAGIC_ENUM=OFF', '-DDEVILUTIONX_SYSTEM_LUA=OFF',
        # the console's render driver takes ARGB8888/ABGR8888 textures
        '-DDEVILUTIONX_DISPLAY_TEXTURE_FORMAT=SDL_PIXELFORMAT_ARGB8888',
        *args.cmake_args, env=toolchain_env())


def cmd_build(args):
    build_runtime()
    exe = CMAKE_DIR / 'devilutionx'
    # the startup objects are not cmake inputs: relink when they are newer than the last link
    if exe.exists() and any(o.stat().st_mtime > exe.stat().st_mtime for o in RT.glob('*.o')):
        exe.unlink()
    run('cmake', '--build', CMAKE_DIR, '--parallel', env=toolchain_env())


def cmd_package(args):
    dist = HERE / 'dist'
    app = dist / TITLE_ID
    tool = BUILD / 'host' / 'ps5-native-tool'
    if not tool.exists():
        run('bash', HERE / 'tools' / 'build-host-tools.sh', env=toolchain_env())
    pie = CMAKE_DIR / 'devilutionx'
    eboot = BUILD / 'eboot.elf'
    run(tool, 'link', '--in', pie, '--out', eboot, '--stub-dir', SDK / 'target' / 'lib',
        '--stub', STUBS / 'libSceAgc.so', '--stub', STUBS / 'libSceAgcDriver.so', '--module-sdk', MODULE_SDK, '--companion-sdk', COMPANION_SDK, '--file-name', 'eboot.elf')
    shutil.rmtree(app, ignore_errors=True)
    (app / 'sce_sys').mkdir(parents=True)
    (app / 'sce_module').mkdir()
    run(tool, 'self', '--sign', '--in', eboot, '--out', app / 'eboot.bin', '--magic', FSELF_MAGIC)

    param = json.loads((HERE / 'sce_sys' / 'param.json').read_text())
    (app / 'sce_sys' / 'param.json').write_text(json.dumps(param, indent=2) + '\n')
    shutil.copy2(HERE / 'sce_sys' / 'icon0.png', app / 'sce_sys' / 'icon0.png')
    # the home-screen backgrounds are made from sce_sys/back.png
    if (HERE / 'sce_sys' / 'back.png').exists() and not all(
            (HERE / 'sce_sys' / name).exists() for name in ('pic0.dds', 'pic1.dds')):
        run(sys.executable, HERE / 'tools' / 'make_dds.py')
    for extra in ('pic0.dds', 'pic1.dds', 'snd0.at9'):
        if (HERE / 'sce_sys' / extra).exists():
            shutil.copy2(HERE / 'sce_sys' / extra, app / 'sce_sys' / extra)

    # what the game ships itself: the assets and the Hellfire mod (the user's own files go to /data/DevilutionX)
    shutil.copy2(CMAKE_DIR / 'devilutionx.mpq', app / 'devilutionx.mpq')
    (app / 'mods').mkdir()
    shutil.copy2(CMAKE_DIR / 'mods' / 'hf.mpq', app / 'mods' / 'hf.mpq')
    shutil.copy2(HERE / 'README.md', app / 'README.md')
    shutil.copy2(ROOT / 'LICENSE.md', app / 'LICENSE.md')
    (app / 'licenses').mkdir()
    for notice in (HERE / 'licenses').glob('*'):
        shutil.copy2(notice, app / 'licenses' / notice.name)
    # the helper the sandbox exit sends to the ELF loader (/app0/lapy.elf)
    helper = BUILD / 'lapy-helper' / TITLE_ID
    run(sys.executable, HERE / 'tools' / 'build-lapy-helper.py', TITLE_ID, helper)
    shutil.copy2(helper / 'lapy.elf', app / 'lapy.elf')
    shutil.copy2(helper / 'lapy-manifest.json', app / 'lapy-manifest.json')
    shutil.copy2(helper / 'Lapy-MIT.txt', app / 'licenses' / 'Lapy-MIT.txt')
    shutil.copy2(HERE / 'runtime' / 'libc.prx', app / 'sce_module' / 'libc.prx')
    run(tool, 'self', '--sign', '--in', HERE / 'runtime' / 'libc.prx', '--out', app / 'sce_module' / 'libc.prx') \
        if not is_signed(HERE / 'runtime' / 'libc.prx') else None
    run(tool, 'self', '--inspect', '--file', app / 'eboot.bin')

    archive = dist / f'{TITLE_ID}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(app.rglob('*')):
            if path.is_file():
                zf.write(path, path.relative_to(dist))
    print(f'App folder: {app}\nZIP: {archive}')


def cmd_deploy(args):
    """Upload dist/PPSA98514 to /data/homebrew/PPSA98514 over FTP (the console's homebrew FTP server)."""
    app = HERE / 'dist' / TITLE_ID
    if not (app / 'eboot.bin').exists():
        raise SystemExit('nothing to deploy: run "package" first')
    ports = [args.port] if args.port else [2121, 1337, 21]
    ftp = None
    for port in ports:
        try:
            ftp = ftplib.FTP()
            ftp.connect(args.ip, port, timeout=15)
            ftp.login()
            print(f'connected to {args.ip}:{port}')
            break
        except OSError as error:
            print(f'{args.ip}:{port}: {error}')
            ftp = None
    if ftp is None:
        raise SystemExit(f'could not reach the console FTP server at {args.ip}')
    ftp.set_pasv(True)
    target = f'{args.dest}/{TITLE_ID}'
    made = set()

    def mkdirs(path):
        current = ''
        for part in path.strip('/').split('/'):
            current += '/' + part
            if current in made:
                continue
            try:
                ftp.mkd(current)
            except ftplib.error_perm:
                pass  # exists
            made.add(current)

    def remove_if_present(path):
        try:
            # ftpsrv answers a successful DELE with 226, which ftplib.FTP.delete would reject
            ftp.sendcmd(f'DELE {path}')
        except ftplib.error_perm as error:
            text = str(error).lower()
            if not (str(error).startswith('550') and ('no such' in text or 'not found' in text)):
                raise

    def upload_atomic(local, remote):
        # to a temporary name beside it and then renamed over the target, so a copy that
        # is running (and holds the file open) does not make the upload fail
        directory, name = remote.rsplit('/', 1)
        temporary = f'{directory}/.{name}.upload'
        remove_if_present(temporary)
        with local.open('rb') as stream:
            ftp.storbinary(f'STOR {temporary}', stream, blocksize=256 * 1024)
        remove_if_present(remote)
        ftp.rename(temporary, remote)

    files = sorted(p for p in app.rglob('*') if p.is_file())
    total = sum(p.stat().st_size for p in files)
    sent = 0
    for path in files:
        remote = f'{target}/{path.relative_to(app).as_posix()}'
        mkdirs(remote.rsplit('/', 1)[0])
        upload_atomic(path, remote)
        sent += path.stat().st_size
        print(f'[{sent * 100 // max(total, 1):3d}%] {remote}', flush=True)
    mkdirs('/data/DevilutionX')  # where diabdat.mpq and the other game files go
    ftp.quit()
    print(f'deployed {len(files)} files ({total // 1024} KiB) to {target}')


def is_signed(path):
    return path.read_bytes()[:4].hex() in ('4f153d1d', '5414f5ee')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    configure = sub.add_parser('configure')
    configure.add_argument('cmake_args', nargs='*', help='extra -D options for cmake')
    configure.set_defaults(fn=cmd_configure)
    for name, fn in (('sdl', cmd_sdl), ('build', cmd_build), ('package', cmd_package)):
        sub.add_parser(name).set_defaults(fn=fn)
    deploy = sub.add_parser('deploy')
    deploy.add_argument('--ip', required=True)
    deploy.add_argument('--port', type=int)
    deploy.add_argument('--dest', default='/data/homebrew')
    deploy.set_defaults(fn=cmd_deploy)
    args = parser.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
