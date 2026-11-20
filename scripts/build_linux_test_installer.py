"""Package the isolated current-branch Linux build. Run with Linux Python."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
from recomplauncher_manifest import manifest

LAUNCHER = '''#!/bin/sh
set -eu
if [ "$(uname -m)" != x86_64 ]; then echo 'This test installer requires x86-64 Linux.' >&2; exit 1; fi
if ! command -v python3 >/dev/null 2>&1 || ! python3 -c 'import tkinter' >/dev/null 2>&1; then
  printf '%s\\n' 'Setup needs Python 3 and Tk.' 'Debian/Ubuntu: sudo apt install python3 python3-tk' 'Fedora: sudo dnf install python3 python3-tkinter' >&2
  exit 1
fi
HITZ_SETUP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export HITZ_SETUP_DIR
setup_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nhlhitz-setup.XXXXXXXX")
trap 'rm -rf -- "$setup_tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
setup_line=$(awk '/^__HITZ_LINUX_ARCHIVE__$/ {print NR + 1; exit}' "$0")
tail -n +"$setup_line" "$0" | tar -xz -C "$setup_tmp"
python3 "$setup_tmp/setup.py" "$@"
exit 0
__HITZ_LINUX_ARCHIVE__
'''

PLAY = '''#!/bin/sh
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
GAME_ROOT=${HITZ_GAME_ROOT:-$ROOT}
case ${XDG_DATA_HOME:-} in
    /*) ;;
    *) export XDG_DATA_HOME="$HOME/.local/share" ;;
esac
# Portable layout: saves, settings, logs and caches live in <install folder>/data. Nothing goes to
# ~/.local/share or ~/.cache any more.
DATA="${NHLHITZPRO_DATA:-$GAME_ROOT/data}"
export NHLHITZPRO_DATA="$DATA"
CACHE="$DATA/cache"
mkdir -p "$DATA" "$CACHE"
# Files that older versions kept in ~/.local/share/nhlhitzpro are moved here once. A file that exists in
# both places keeps the newer copy; the older copy goes to data/old-folder-backup, nothing is deleted.
OLD="${XDG_DATA_HOME:-$HOME/.local/share}/nhlhitzpro"
if [ -d "$OLD" ] && [ "$OLD" != "$DATA" ] && [ ! -e "$DATA/.moved-from-old-folder" ]; then
    BACKUP="$DATA/old-folder-backup"
    for item in SaveData settings.ini keymap.ini logs texture_pack; do
        from="$OLD/$item"; to="$DATA/$item"
        [ -e "$from" ] || continue
        if [ -d "$from" ]; then
            find "$from" -type f | while IFS= read -r f; do
                rel=${f#"$from"/}
                if [ ! -e "$to/$rel" ]; then
                    mkdir -p "$(dirname "$to/$rel")" && mv "$f" "$to/$rel"
                elif [ "$f" -nt "$to/$rel" ]; then
                    mkdir -p "$(dirname "$BACKUP/$item/$rel")" && mv "$to/$rel" "$BACKUP/$item/$rel" && mv "$f" "$to/$rel"
                else
                    mkdir -p "$(dirname "$BACKUP/$item/$rel")" && mv "$f" "$BACKUP/$item/$rel"
                fi
            done
            find "$from" -depth -type d -empty -delete 2>/dev/null
        elif [ ! -e "$to" ]; then
            mv "$from" "$to"
        elif [ "$from" -nt "$to" ]; then
            mkdir -p "$BACKUP" && mv "$to" "$BACKUP/$item" && mv "$from" "$to"
        else
            mkdir -p "$BACKUP" && mv "$from" "$BACKUP/$item"
        fi
    done
    rmdir "$OLD" 2>/dev/null
    echo "Moved here from $OLD" > "$DATA/.moved-from-old-folder"
fi
mkdir -p "$DATA/logs"
LOG="$DATA/logs/play.log"
# keep the last run's log: a second try must not erase the first failure
if [ -f "$LOG" ]; then mv -f "$LOG" "$DATA/logs/play.previous.log"; fi
VERSION=$(cat "$ROOT/version.txt" 2>/dev/null || echo unknown)
{
    echo "=== NHL Hitz Pro $VERSION - play log ==="
    echo "started    : $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "system     : $(uname -srm)"
    echo "distro     : $( . /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}" )"
    echo "glibc      : $(ldd --version 2>/dev/null | head -n 1)"
    echo "session    : XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-} DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-} SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-}"
    echo "game root  : $GAME_ROOT"
    echo "game files : $(ls "$GAME_ROOT/game_files/default.xbe" 2>&1)"
    echo "---"
} > "$LOG" 2>&1
export RECOMP_GL_VERTEX_UPLOAD=stream RECOMP_GL_VERTEX_INDEXED=1
export RECOMP_GL_TEXTURE_WATCH=1 RECOMP_GL_TEXTURE_WATCH_CLEAR_EVERY=8 RECOMP_GL_TEXTURE_WATCH_FAST=1
export RECOMP_GL_TEXTURE_INDEX=1 RECOMP_PB_IDLE_WAIT_US=100
export RECOMP_GL_SHADER_PREWARM="$CACHE/hitz_prewarm.txt"
export RECOMP_TEXPACK="$GAME_ROOT/texture_pack"
# The Python bundled for Setup and Settings puts its own libraries on LD_LIBRARY_PATH; the game needs none of
# them, so it gets the caller's original value back.
if [ -n "${HITZ_ORIG_LD_LIBRARY_PATH+x}" ]; then
    if [ -n "$HITZ_ORIG_LD_LIBRARY_PATH" ]; then export LD_LIBRARY_PATH="$HITZ_ORIG_LD_LIBRARY_PATH"; else unset LD_LIBRARY_PATH; fi
fi
cd "$ROOT/native" || exit 1
START=$(date +%s)
./build-linux/nhl_hitz_pro_recomp "$GAME_ROOT/game_files" "$@" >> "$LOG" 2>&1
STATUS=$?
SECONDS_RUN=$(( $(date +%s) - START ))
echo "--- exited with code $STATUS after $SECONDS_RUN s ---" >> "$LOG"
if [ "$STATUS" -ne 0 ] || [ "$SECONDS_RUN" -lt 10 ]; then
    {
        echo "NHL Hitz Pro stopped (exit code $STATUS, after $SECONDS_RUN s)."
        echo "The log is: $LOG"
        echo "Please attach that file (and play.previous.log, if there is one) when reporting a problem."
        echo "--- last lines of the log ---"
        tail -n 15 "$LOG"
    } >&2
    if command -v notify-send >/dev/null 2>&1; then
        notify-send "NHL Hitz Pro stopped" "Exit code $STATUS. Log: $LOG" >/dev/null 2>&1 || true
    fi
fi
exit "$STATUS"
'''

# Libraries that every Linux desktop has (or that must come from the system: glibc, the X server's and the
# GPU's). Everything else that Setup, Settings or the game load is shipped inside the AppImage.
SYSTEM_LIBS = re.compile(r'^(linux-vdso|ld-linux|libc|libm|libdl|libpthread|librt|libresolv|libz|libstdc\+\+|libgcc_s|'
                         r'libX11|libxcb|libXau|libXdmcp|libXext|libXrender|libfontconfig|libfreetype|libexpat|'
                         r'libpng16|libbz2|libbrotli\w*|libbsd|libmd|libssl|libcrypto|libGL\w*|libEGL)\.')

PYTHON_WRAPPER = '''#!/bin/sh
# Bundled Python 3 with Tk for Setup and Settings, so the system needs neither.
here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -z "${HITZ_ORIG_LD_LIBRARY_PATH+x}" ]; then
  HITZ_ORIG_LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}
  export HITZ_ORIG_LD_LIBRARY_PATH
fi
LD_LIBRARY_PATH="$here/lib${HITZ_ORIG_LD_LIBRARY_PATH:+:$HITZ_ORIG_LD_LIBRARY_PATH}"
PYTHONHOME=$here PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1
TCL_LIBRARY=$here/tcltk/tcl8.6 TK_LIBRARY=$here/tcltk/tk8.6
export LD_LIBRARY_PATH PYTHONHOME PYTHONNOUSERSITE PYTHONDONTWRITEBYTECODE TCL_LIBRARY TK_LIBRARY
exec "$here/bin/python3.12" -s "$@"
'''


def shared_deps(path):
    """name -> resolved path for each library `path` loads that is not a system one."""
    out = subprocess.check_output(['ldd', str(path)], text=True)
    deps = {}
    for name, target in re.findall(r'^\s*(\S+) => (/\S+)', out, re.M):
        if not SYSTEM_LIBS.match(name):
            deps[name] = Path(target).resolve()
    missing = re.findall(r'^\s*(\S+) => not found', out, re.M)
    if missing:
        raise SystemExit(f'{path}: missing {missing}')
    return deps


def bundle_python(root):
    """Python 3.12 (the build machine's), its standard library and Tcl/Tk, under root/python."""
    py = root / 'python'
    stdlib = Path('/usr/lib/python3.12')
    shutil.copytree(stdlib, py / 'lib/python3.12', symlinks=False, ignore=shutil.ignore_patterns(
        '__pycache__', 'test', 'tests', 'idlelib', 'turtledemo', 'ensurepip', 'lib2to3', 'pydoc_data',
        'config-3.12-*', 'dist-packages', 'site-packages', 'EXTERNALLY-MANAGED'))
    (py / 'bin').mkdir(parents=True)
    shutil.copy2(Path('/usr/bin/python3.12').resolve(), py / 'bin/python3.12')
    wrapper = py / 'bin/python3'
    wrapper.write_text(PYTHON_WRAPPER)
    wrapper.chmod(0o755)
    libs = {}
    for module in (py / 'lib/python3.12/lib-dynload').glob('_tkinter*.so'):
        libs.update(shared_deps(module))
    for path in list(libs.values()):
        libs.update(shared_deps(path))
    if not any(name.startswith('libtk8') for name in libs):
        raise SystemExit('Tk not found: install python3-tk on the build machine')
    (py / 'lib').mkdir(exist_ok=True)
    for name, path in libs.items():
        shutil.copy2(path, py / 'lib' / name)
    shutil.copytree('/usr/share/tcltk', py / 'tcltk', symlinks=False)
    for license_file in ('/usr/share/doc/python3.12/copyright', '/usr/share/doc/tcl8.6/copyright', '/usr/share/doc/tk8.6/copyright'):
        if Path(license_file).is_file():
            (root / 'player/LICENSES').mkdir(parents=True, exist_ok=True)
            shutil.copyfile(license_file, root / 'player/LICENSES' / (Path(license_file).parent.name + '-copyright.txt'))
    return sorted(libs)


def package_appimage(root, output, version, tool, runtime, project, args):
    """The AppImage carries everything it needs: its own Python with Tk, 7-Zip and the libraries the player loads.
    The only files released are the AppImage and the RecompLauncher tar.gz."""
    shutil.copyfile(project / 'native/installer/linux/appimage.py', root / 'appimage.py')
    # Python 3 with Tk 8.6 (the build machine's, which draws text through fontconfig) and 7-Zip travel inside: nothing
    # from the system but the graphics driver.
    bundle_python(root)
    (root / 'bin').mkdir()
    if not args.sevenzip or not args.sevenzip.is_file():
        raise SystemExit('pass --sevenzip (the 7zz binary)')
    shutil.copy2(args.sevenzip, root / 'bin/7zz')
    (root / 'bin/7zz').chmod(0o755)
    app_run = root / 'AppRun'
    app_run.write_text('''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -n "${APPIMAGE:-}" ]; then
  HITZ_SETUP_DIR=$(dirname -- "$APPIMAGE")
  export HITZ_SETUP_DIR
fi
export PATH="$root/python/bin:$root/bin:$PATH"
export PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1
exec "$root/python/bin/python3" "$root/appimage.py" "$@"
''')
    app_run.chmod(0o755)
    (root / 'nhl-hitz-pro.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=NHL Hitz Pro\n'
        'Comment=Play NHL Hitz Pro\nExec=AppRun\nIcon=nhl-hitz-pro\n'
        'Terminal=false\nCategories=Game;SportsGame;\n')
    shutil.copyfile(root / 'icon.png', root / 'nhl-hitz-pro.png')
    # RecompLauncher finds the manifest at the root of the AppImage.
    manifest_text = json.dumps(manifest('linux'), indent=2) + '\n'
    (root / 'recomplauncher.json').write_text(manifest_text)
    (root / 'THIRD-PARTY.txt').write_text(
        'Bundled in this AppImage:\n'
        '- Python 3.12 with Tcl/Tk 8.6 (Ubuntu): Python Software Foundation License; Tcl/Tk license.\n'
        '- 7-Zip 23.01 (7zz): GNU LGPL with the unRAR restriction; Igor Pavlov.\n'
        '- SDL3: zlib license.  libepoxy: MIT.  libpng: libpng license.  OpenSSL (libcrypto): Apache 2.0.\n'
        'Their license texts are in LICENSES/ and python/ and the project repository.\n')
    artifact = output / f'NHLHitzPro-{version}-Standalone.AppImage'
    subprocess.run([str(tool), '--runtime-file', str(runtime), '--no-appstream', str(root), str(artifact)],
                   env=dict(os.environ, ARCH='x86_64', APPIMAGE_EXTRACT_AND_RUN='1'), check=True)
    # The Linux-x64 archive is the RecompLauncher asset: the AppImage and the manifest, nothing else.
    archive = output / f'NHLHitzPro-{version}-Linux-x64.tar.gz'
    with tempfile.TemporaryDirectory() as temp:
        sdk_manifest = Path(temp) / 'recomplauncher.json'
        sdk_manifest.write_text(manifest_text)
        with tarfile.open(archive, 'w:gz') as tar:
            tar.add(artifact, arcname='NHLHitzPro.AppImage')
            tar.add(sdk_manifest, arcname='recomplauncher.json')
    return artifact


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--project', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--version', default='1.0.9-linux')
    ap.add_argument('--sdl3-lib', type=Path, help='libSDL3.so.0 to ship beside the player (required when the player is built on SDL3)')
    ap.add_argument('--sevenzip', type=Path, help='the 7zz binary to bundle')
    ap.add_argument('--bundle-lib', type=Path, action='append', default=[], help='a system library the player loads, copied beside it (repeat)')
    ap.add_argument('--appimagetool', type=Path)
    ap.add_argument('--appimage-runtime', type=Path)
    args = ap.parse_args()
    source, project, output = args.source.resolve(), args.project.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = args.version
    try:  # the commit the package is built from (the project checkout)
        commit = subprocess.check_output(['git', '-C', str(project), 'rev-parse', 'HEAD'], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        commit = 'unknown'
    if not re.fullmatch(r'[0-9A-Za-z.-]+', version):
        ap.error('Invalid version label')
    if bool(args.appimagetool) != bool(args.appimage_runtime):
        ap.error('Provide both --appimagetool and --appimage-runtime for an AppImage build')
    with tempfile.TemporaryDirectory(prefix='hitz-linux-package-') as temp:
        root = Path(temp) / 'AppDir'
        root.mkdir(mode=0o755)
        for name in ('installer.py', 'disc.py'):
            shutil.copyfile(project / 'native/installer/linux-demo' / name, root / name)
        shutil.copyfile(project / 'native/installer/linux/production.py', root / 'production.py')
        shutil.copyfile(project / 'native/installer/linux/menu_shortcuts.py', root / 'menu_shortcuts.py')
        icon = project / 'native/resources/nhl_hitz_pro.png'
        shutil.copyfile(icon, root / 'icon.png')
        header = (source / 'native/installer/manifest.h').read_text()
        files = {json.loads(name): int(size) for name, size in re.findall(r'\{ ("[^"]+"), (\d+)ULL \}', header)}
        xbe_hash = bytes(map(int, re.search(r'XBE_SHA256\[32\] = \{([^}]+)', header)[1].split(','))).hex()
        (root / 'supported-disc.json').write_text(json.dumps(dict(files=files, xbe_sha256=xbe_hash)))
        texture = dict(
            revision=2,
            files=6287,
            extracted_bytes=4970707932,
            manifest_sha256='073915019f2be011a79ccd238608e3d232e157d1b89459d6b602ac010a66b0bf',
            parts=[
                dict(name='texture-01.bin', bytes=1046022933,
                     sha256='b5a91ce04fc23bec07e334ed23f19b556887e6630fa8b515542d2d251fad4c1f',
                     url='https://github.com/dalves408/nhlhitzpro-recomp/releases/download/textures-2/texture-01.bin'),
                dict(name='texture-02.bin', bytes=1717588748,
                     sha256='6da010784eb9b44bd8d4e973fca3db35b0da494cd17416fe31ec779b583047fb',
                     url='https://github.com/dalves408/nhlhitzpro-recomp/releases/download/textures-2/texture-02.bin'),
            ],
            # Convenience totals for space estimates / summaries
            bytes=1046022933 + 1717588748,
            sha256='073915019f2be011a79ccd238608e3d232e157d1b89459d6b602ac010a66b0bf',
            file_name='texture-01.bin+texture-02.bin',
            url='https://github.com/dalves408/nhlhitzpro-recomp/releases/download/textures-2/',
            standalone_setup='NHLHitzPro-HiRes-Textures-2-Setup.exe',
            standalone_setup_url='https://github.com/dalves408/nhlhitzpro-recomp/releases/download/textures-2/NHLHitzPro-HiRes-Textures-2-Setup.exe',
            draft_base='https://github.com/dalves408/nhlhitzpro-recomp/releases/download/untagged-717f2cc93031f4eadea2/')

        metadata = dict(version=version, source_commit=commit, toolkit_commit='f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b',
            platform='Linux x86-64', texture=texture,
            packaging_patch='HITZ_SHADER_DIR=src/shaders (relative to launcher working directory)',
            game_files=len(files), game_bytes=sum(files.values()))
        (root / 'release.json').write_text(json.dumps(metadata, indent=2))
        (root / 'setup.py').write_text('''from pathlib import Path
import argparse
from installer import gui
from production import ProductionBackend
p = argparse.ArgumentParser()
p.add_argument('--install', type=Path, help='Noninteractive test installation')
p.add_argument('--source', type=Path)
p.add_argument('--no-textures', action='store_true')
p.add_argument('--no-shortcuts', action='store_true')
a = p.parse_args()
try:
    b = ProductionBackend()
except ValueError as exc:
    if not a.install:
        try:
            from tkinter import messagebox
            messagebox.showerror('Run Setup as your desktop user', str(exc))
        except Exception:
            pass
    p.error(str(exc))
if a.install:
    try:
        b.install(a.install, not a.no_textures, not a.no_shortcuts, lambda n, s: print(f'{n:.0f}% {s}', flush=True), a.source)
    except (OSError, ValueError) as exc:
        p.exit(1, str(exc) + '\\n')
    if b.downloaded: print('Noninteractive mode keeps the verified texture download:', b.downloaded)
else: gui(b)
''')
        player = root / 'player'
        binary = player / 'native/build-linux/nhl_hitz_pro_recomp'
        binary.parent.mkdir(parents=True)
        shutil.copy2(source / 'native/build-linux/nhl_hitz_pro_recomp', binary)
        subprocess.run(['strip', '--strip-debug', str(binary)], check=True)
        needed = subprocess.check_output(['readelf', '-d', str(binary)], text=True)
        if 'libSDL3.so' in needed:
            if not args.sdl3_lib or not args.sdl3_lib.is_file():
                raise SystemExit('the player is built on SDL3: pass --sdl3-lib path/to/libSDL3.so.0')
            shutil.copy2(args.sdl3_lib, binary.parent / 'libSDL3.so.0')
            metadata['sdl3_sha256'] = hashlib.sha256(args.sdl3_lib.read_bytes()).hexdigest()
        elif 'libSDL2' in needed:
            metadata['sdl2'] = 'system library'
        binary.chmod(0o755)
        for lib in args.bundle_lib:
            shutil.copyfile(lib.resolve(), binary.parent / lib.name)
        metadata['player_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        versions = re.findall(r'GLIBC_(\d+)\.(\d+)', subprocess.check_output(['readelf', '--version-info', str(binary)], text=True))
        metadata['elf_glibc_required'] = '.'.join(map(str, max(tuple(map(int, value)) for value in versions)))
        generated = source / 'native/src/recomp/gen'
        generated_hash = hashlib.sha256()
        for path in sorted(generated.glob('*')):
            if path.is_file():
                generated_hash.update(path.name.encode() + b'\0' + hashlib.sha256(path.read_bytes()).digest())
        metadata['generated_sources_sha256'] = generated_hash.hexdigest()
        (root / 'release.json').write_text(json.dumps(metadata, indent=2))
        shutil.copytree(source / 'native/src/shaders', player / 'native/src/shaders')
        shutil.copyfile(project / 'linux/nhl_config.py', player / 'nhl_config.py')
        shutil.copyfile(icon, player / 'icon.png')
        shutil.copyfile(icon, binary.parent / 'icon.png')  # window icon beside ELF
        shutil.copyfile(project / 'native/installer/linux/menu_shortcuts.py', player / 'menu_shortcuts.py')
        (player / 'install-shortcuts.py').write_text('#!/usr/bin/env python3\nfrom menu_shortcuts import main\nmain()\n')
        (player / 'play.sh').write_text(PLAY)
        (player / 'version.txt').write_text(version + '\n')
        (player / 'settings.sh').write_text('#!/bin/sh\nROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec python3 "$ROOT/nhl_config.py"\n')
        for name in ('play.sh', 'settings.sh', 'install-shortcuts.py'):
            (player / name).chmod(0o755)
        for base, name in [(source, 'LICENSE'), (source / 'tools/xboxrecomp', 'LICENSE')]:
            if (base / name).is_file():
                target = player / ('LICENSE-toolkit.txt' if base.name == 'xboxrecomp' else 'LICENSE.txt')
                shutil.copyfile(base / name, target)
        toolkit = source / 'tools/xboxrecomp'
        if (toolkit / 'NOTICE').is_file():
            shutil.copyfile(toolkit / 'NOTICE', player / 'NOTICE-toolkit.txt')
        if (toolkit / 'LICENSES').is_dir():
            shutil.copytree(toolkit / 'LICENSES', player / 'LICENSES')
        sdl3_license = project / 'native/third_party/SDL3-3.4.20/LICENSE.txt'
        if (binary.parent / 'libSDL3.so.0').is_file() and sdl3_license.is_file():
            (player / 'LICENSES').mkdir(exist_ok=True)
            shutil.copyfile(sdl3_license, player / 'LICENSES' / 'SDL3-LICENSE.txt')
        if (source / 'native/third_party/xxhash.h').is_file():
            licenses = player / 'LICENSES'
            licenses.mkdir(exist_ok=True)
            shutil.copyfile(source / 'native/third_party/xxhash.h', licenses / 'xxhash.h')
        (player / 'README-LINUX.txt').write_text(f'''NHL Hitz Pro — Linux test build
Install: sh NHLHitzPro-Setup-{version}-x86_64.run
Choose a new or existing NHL Hitz Pro installation folder. Setup reuses valid
game_files and keeps existing texture_pack contents. Provide an Xbox USA
ISO/XISO or extracted game folder if valid game data is missing.
Optional hi-res (Revision 2): place texture-01.bin + texture-02.bin or an extracted
texture_pack/ folder beside the AppImage to avoid downloading. Otherwise Setup
downloads both parts from the textures-2 GitHub release (7-Zip required to extract).

Run play.sh to play, settings.sh to configure. F1 opens in-game settings.
Requires x86-64 Linux, glibc 2.39+, libepoxy, libpng16, and an OpenGL 3.3 driver. SDL3 is included.
Controllers: Xbox, PlayStation, Switch and most other pads work (SDL3). If Steam is running with Steam Input
on, it can take the pad away from the game: turn Steam Input off for that controller, or close Steam.
Hi-res DDS formats may require additional GPU capabilities.
Debian 13 / Ubuntu 24.04+: sudo apt install libepoxy0 libpng16-16t64 python3-tk 7zip innoextract
Fedora 41+: sudo dnf install libepoxy libpng python3-tkinter 7zip innoextract
Portable: everything the game writes (saves, settings, keymap, logs, caches, the download cache) is in the
"data" folder inside the install folder. Copy or delete the install folder as one piece. Files that
older versions kept in ~/.local/share/nhlhitzpro are moved there on the first start (the newer copy of a
file wins; an older duplicate goes to data/old-folder-backup).
Play log: data/logs/play.log (the previous run is kept as play.previous.log).
If the game stops or does not start, run play.sh from a terminal: it prints the log path and the
last lines of the log. Attach play.log when reporting a problem. Shader prewarm: data/cache.
Default fresh settings: 1x, fullscreen, 4:3, 60 FPS, enhancements off.
Enable hi-res in settings.sh or F1 with render scale 2x or greater.
There is no uninstaller: delete the install folder to remove the game. If you made menu entries, delete the
two NHL Hitz Pro .desktop files in ~/.local/share/applications as well.
Setup can update or repair an existing installation in place.
Menu repair (no reinstall): python3 install-shortcuts.py
Run Setup and menu repair as your normal desktop user, without sudo.
''')
        if args.appimagetool:
            readme = player / 'README-LINUX.txt'
            readme.write_text(readme.read_text().replace(
                f'Install: sh NHLHitzPro-Setup-{version}-x86_64.run',
                f'Run: chmod +x NHLHitzPro-{version}-Standalone.AppImage\n'
                f'     ./NHLHitzPro-{version}-Standalone.AppImage\n'
                'Use --setup to repair or update the standalone installation.\n'
                'RecompLauncher users use the .tar.gz release asset.'))
            artifact = package_appimage(root, output, version, args.appimagetool.resolve(),
                                        args.appimage_runtime.resolve(), project, args)
        else:
            artifact = output / f'NHLHitzPro-Setup-{version}-x86_64.run'
            with artifact.open('wb') as target:
                target.write(LAUNCHER.encode())
                with tarfile.open(fileobj=target, mode='w:gz') as tar:
                    for path in sorted(root.iterdir()):
                        tar.add(path, arcname=path.name)
        sha = hashlib.sha256(artifact.read_bytes()).hexdigest()
        print(artifact, artifact.stat().st_size, sha)
        archive = next(output.glob('*-Linux-x64.tar.gz'), None)
        if archive:
            print(archive, archive.stat().st_size, hashlib.sha256(archive.read_bytes()).hexdigest())

if __name__ == '__main__':
    main()
