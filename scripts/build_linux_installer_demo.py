"""Build a small self-extracting Linux demo; no game or real textures included."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'native/installer/linux-demo'
OUT = ROOT / 'native/installer/Output/linux-demo'

LAUNCHER = '''#!/bin/sh
set -eu
if ! command -v python3 >/dev/null 2>&1 || ! python3 -c 'import tkinter' >/dev/null 2>&1 ||
   { ! command -v 7zz >/dev/null 2>&1 && ! command -v 7z >/dev/null 2>&1; }; then
    printf '%s\\n' 'This demo needs Python 3, Tk, and 7-Zip.' \
      'Debian/Ubuntu: sudo apt install python3 python3-tk 7zip' \
      'Older Debian: sudo apt install python3 python3-tk p7zip-full' \
      'Fedora: sudo dnf install python3 python3-tkinter 7zip' \
      'Then run this setup again. No packages were installed automatically.' >&2
    exit 1
fi
demo_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nhlhitz-demo.XXXXXXXX")
trap 'rm -rf -- "$demo_tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
demo_line=$(awk '/^__HITZ_DEMO_ARCHIVE__$/ {print NR + 1; exit}' "$0")
tail -n +"$demo_line" "$0" | tar -xz -C "$demo_tmp"
python3 "$demo_tmp/installer.py" "$@"
exit 0
__HITZ_DEMO_ARCHIVE__
'''

DEMO_LAUNCH = '''#!/bin/sh
exec python3 -c 'import tkinter as t; from tkinter import messagebox; r=t.Tk(); r.withdraw(); messagebox.showinfo("NHL Hitz Pro — Demo", "Installation worked!\\n\\nThis is a placeholder, not the game."); r.destroy()'
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='hitz-linux-demo-') as tmp:
        stage = Path(tmp)
        payload = stage / 'payload'
        (payload / 'texture_pack/players').mkdir(parents=True)
        (payload / 'texture_pack/arenas').mkdir(parents=True)
        (payload / 'launch-demo.sh').write_text(DEMO_LAUNCH, newline='\n')
        (payload / 'GAME-DEMO.txt').write_text('NHL Hitz Pro Linux installer demo. No game content.\n')
        for name in ('players/sample-player.dds', 'arenas/sample-ice.dds'):
            (payload / 'texture_pack' / name).write_bytes(b'')
        (payload / 'texture_pack/DEMO.txt').write_text('Empty placeholder DDS files. Not usable textures.\n')
        archive = stage / 'demo-texture pack'
        subprocess.run(['C:/Program Files/7-Zip/7z.exe', 'a', '-t7z', '-m0=LZMA2', '-mx=7',
                        str(archive), './*'], cwd=payload, check=True, stdout=subprocess.DEVNULL)
        files = {p.relative_to(payload).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in payload.rglob('*') if p.is_file()}
        meta = dict(sha256=hashlib.sha256(archive.read_bytes()).hexdigest(), files=files)
        # A tiny synthetic XDVDFS disc, containing only a clearly marked stub.
        stub = b'NHL HITZ INSTALLER DEMO - NOT A GAME EXECUTABLE\n'
        sample = bytearray(35 * 2048)
        sample[32 * 2048:32 * 2048 + 20] = b'MICROSOFT*XBOX*MEDIA'
        struct.pack_into('<II', sample, 32 * 2048 + 20, 33, 2048)
        name = b'default.xbe'
        struct.pack_into('<HHIIBB', sample, 33 * 2048, 0, 0, 34, len(stub), 0, len(name))
        sample[33 * 2048 + 14:33 * 2048 + 14 + len(name)] = name
        sample[34 * 2048:34 * 2048 + len(stub)] = stub
        (stage / 'sample-disc.iso').write_bytes(sample)
        fixture = dict(files={'default.xbe': len(stub)}, xbe_sha256=hashlib.sha256(stub).hexdigest(), fixture=True)
        header = (ROOT / 'native/installer/manifest.h').read_text()
        xbe_hash = bytes(map(int, re.search(r'XBE_SHA256\[32\] = \{([^}]+)', header)[1].split(','))).hex()
        supported = dict(files={json.loads(name): int(size) for name, size in
                               re.findall(r'\{ ("[^"]+"), (\d+)ULL \}', header)}, xbe_sha256=xbe_hash)
        for name, data in [('sample-disc.json', fixture), ('supported-disc.json', supported)]:
            (stage / name).write_text(json.dumps(data))
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode='w:gz') as tar:
            for name in ('installer.py', 'remove-demo.py', 'disc.py'):
                tar.add(SOURCE / name, arcname=name)
            for name in ('sample-disc.iso', 'sample-disc.json', 'supported-disc.json'):
                tar.add(stage / name, arcname=name)
            tar.add(ROOT / 'native/resources/nhl_hitz_pro.png', arcname='icon.png')
            tar.add(archive, arcname='demo-texture pack')
            data = json.dumps(meta).encode()
            entry = tarfile.TarInfo('payload.json')
            entry.size = len(data)
            tar.addfile(entry, io.BytesIO(data))
        artifact = OUT / 'NHLHitzPro-Linux-Setup-DEMO.run'
        artifact.write_bytes(LAUNCHER.encode() + buffer.getvalue())
        sha = hashlib.sha256(artifact.read_bytes()).hexdigest()
        (OUT / (artifact.name + '.sha256')).write_text(f'{sha}  {artifact.name}\n')
        (OUT / 'README.txt').write_text('''NHL Hitz Pro — Linux installer demo

One .run for Debian/Ubuntu and Fedora (Python 3.9+).
No game, real textures, or downloads. Uses the existing game icon.

1. Copy NHLHitzPro-Linux-Setup-DEMO.run onto your Linux computer.
2. In its folder, run:
   sh NHLHitzPro-Linux-Setup-DEMO.run
   Or mark executable and launch from a terminal.
3. Follow Welcome > Folder > Your copy of the game > Verification >
   Options > Ready > Installing > Finish.
   Choose "Use sample disc" to test without a real ISO. Or select your
   own ISO/XISO or extracted folder for real, read-only verification.

Dependencies if missing:
  Debian/Ubuntu: sudo apt install python3 python3-tk 7zip
  Older Debian: sudo apt install python3 python3-tk p7zip-full
  Fedora: sudo dnf install python3 python3-tkinter 7zip

No sudo needed to run the installer. Use a normal desktop session.
Default: ~/Games/NHL Hitz Pro Demo. An empty folder is required.
The sample ISO has a real XDVDFS directory with a non-game stub file.
Real sources are checked using the Windows supported-file sizes and XBE hash.
The demo never copies real game data. It creates game_files placeholders.
The small sample .bin is really extracted with 7-Zip and verified.
Texture and menu-shortcut checkboxes are functional.
The finish screen can open the folder. Its launch-demo.sh opens a confirmation.
Remove it with: python3 "~/Games/NHL Hitz Pro Demo/remove-demo.py"
(Use the full path to your home directory in place of ~ inside quotes.)
Removal preserves added or modified files. No system package registration.

This is a visual/extraction prototype, not the final bundled runtime.
The real texture pack, resumable network transfers, existing game detection,
game settings, full game-data extraction, and upgrade/repair flows are not included.
The final Linux and Windows installers will share the existing texture pack.
''', encoding='utf-8')
        print(f'{artifact}\n{artifact.stat().st_size:,} bytes\nSHA256 {sha}')


if __name__ == '__main__':
    main()
