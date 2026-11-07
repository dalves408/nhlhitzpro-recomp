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
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
GAME_ROOT=${HITZ_GAME_ROOT:-$ROOT}
case ${XDG_DATA_HOME:-} in
    /*) ;;
    *) export XDG_DATA_HOME="$HOME/.local/share" ;;
esac
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/nhlhitzpro"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/nhlhitzpro"
mkdir -p "$DATA/logs" "$CACHE"
export RECOMP_GL_VERTEX_UPLOAD=stream RECOMP_GL_VERTEX_INDEXED=1
export RECOMP_GL_TEXTURE_WATCH=1 RECOMP_GL_TEXTURE_WATCH_CLEAR_EVERY=8 RECOMP_GL_TEXTURE_WATCH_FAST=1
export RECOMP_GL_TEXTURE_INDEX=1 RECOMP_PB_IDLE_WAIT_US=100
export RECOMP_GL_SHADER_PREWARM="$CACHE/hitz_prewarm.txt"
export RECOMP_TEXPACK="$GAME_ROOT/texture_pack"
cd "$ROOT/native"
exec ./build-linux/nhl_hitz_pro_recomp "$GAME_ROOT/game_files" "$@" > "$DATA/logs/play.log" 2>&1
'''

def package_appimage(root, output, version, tool, runtime, project):
    shutil.copyfile(project / 'native/installer/linux/appimage.py', root / 'appimage.py')
    app_run = root / 'AppRun'
    app_run.write_text('''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -n "${APPIMAGE:-}" ]; then
  HITZ_SETUP_DIR=$(dirname -- "$APPIMAGE")
  export HITZ_SETUP_DIR
fi
exec python3 "$root/appimage.py" "$@"
''')
    app_run.chmod(0o755)
    (root / 'nhl-hitz-pro.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=NHL Hitz Pro\n'
        'Comment=Play NHL Hitz Pro\nExec=AppRun\nIcon=nhl-hitz-pro\n'
        'Terminal=false\nCategories=Game;SportsGame;\n')
    shutil.copyfile(root / 'icon.png', root / 'nhl-hitz-pro.png')
    # The Linux-x64 SDK archive should win the launcher's asset selection.
    artifact = output / f'NHLHitzPro-{version}-Standalone.AppImage'
    subprocess.run([str(tool), '--runtime-file', str(runtime), '--no-appstream', str(root), str(artifact)],
                   env=dict(os.environ, ARCH='x86_64', APPIMAGE_EXTRACT_AND_RUN='1'), check=True)
    sdk_manifest = output / 'recomplauncher.json'
    sdk_manifest.write_text(json.dumps(manifest('linux'), indent=2) + '\n')
    prepare = output / 'prepare-game.sh'
    prepare.write_text('''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export APPIMAGE_EXTRACT_AND_RUN=1
exec "$root/NHLHitzPro.AppImage" --prepare --game "$1"
''')
    prepare.chmod(0o755)
    archive = output / f'NHLHitzPro-{version}-Linux-x64.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(artifact, arcname='NHLHitzPro.AppImage')
        tar.add(sdk_manifest, arcname=sdk_manifest.name)
        tar.add(prepare, arcname=prepare.name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / (archive.name + '.sha256')).write_text(f'{digest}  {archive.name}\n')
    return artifact


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--project', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--version', default='1.0.9-linux')
    ap.add_argument('--appimagetool', type=Path)
    ap.add_argument('--appimage-runtime', type=Path)
    args = ap.parse_args()
    source, project, output = args.source.resolve(), args.project.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version, commit = args.version, 'be8fcaa052a8731d34c5bcb7fcc192d4552972dc'
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
        binary.chmod(0o755)
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
        shutil.copyfile(project / 'native/installer/linux/uninstall.py', player / 'uninstall.py')
        shutil.copyfile(project / 'native/installer/linux/menu_shortcuts.py', player / 'menu_shortcuts.py')
        (player / 'install-shortcuts.py').write_text('#!/usr/bin/env python3\nfrom menu_shortcuts import main\nmain()\n')
        (player / 'play.sh').write_text(PLAY)
        (player / 'settings.sh').write_text('#!/bin/sh\nROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec python3 "$ROOT/nhl_config.py"\n')
        for name in ('play.sh', 'settings.sh', 'uninstall.py', 'install-shortcuts.py'):
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
Requires x86-64 Linux, glibc 2.39+, SDL2, libepoxy, libpng16, and an OpenGL 3.3 driver.
Hi-res DDS formats may require additional GPU capabilities.
Debian 13 / Ubuntu 24.04+: sudo apt install libsdl2-2.0-0 libepoxy0 libpng16-16t64 python3-tk 7zip innoextract
Fedora 41+: sudo dnf install SDL2 libepoxy libpng python3-tkinter 7zip innoextract
Settings/saves: $XDG_DATA_HOME/nhlhitzpro or ~/.local/share/nhlhitzpro.
Play log: that folder's logs/play.log. Shader prewarm: ~/.cache/nhlhitzpro.
Default fresh settings: 1x, fullscreen, 4:3, 60 FPS, enhancements off.
Enable hi-res in settings.sh or F1 with render scale 2x or greater.
python3 uninstall.py removes unchanged program files/shortcuts, keeping game data,
textures, saves, settings and modified files. Remove the game directory manually
if you also want to discard imported game data and installed textures.
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
                                        args.appimage_runtime.resolve(), project)
        else:
            artifact = output / f'NHLHitzPro-Setup-{version}-x86_64.run'
            with artifact.open('wb') as target:
                target.write(LAUNCHER.encode())
                with tarfile.open(fileobj=target, mode='w:gz') as tar:
                    for path in sorted(root.iterdir()):
                        tar.add(path, arcname=path.name)
        sha = hashlib.sha256(artifact.read_bytes()).hexdigest()
        (output / (artifact.name + '.sha256')).write_text(f'{sha}  {artifact.name}\n')
        metadata.update(installer_sha256=sha, installer_bytes=artifact.stat().st_size)
        (output / 'build-info.json').write_text(json.dumps(metadata, indent=2))
        shutil.copyfile(player / 'README-LINUX.txt', output / 'README-LINUX.txt')
        shutil.copyfile(project / 'native/installer/linux/menu_shortcuts.py', output / 'NHLHitzPro-Repair-Menu.py')
        print(artifact, artifact.stat().st_size, sha)

if __name__ == '__main__':
    main()
