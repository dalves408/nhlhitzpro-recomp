"""Texture pack release helper.

  python gen_pack_manifest.py manifest <pack folder>
      writes <pack folder>\\manifest.txt: one line per file, "<sha256> <size> <path relative to the pack folder>"
  python gen_pack_manifest.py release <pack folder> <add-on installer exe> <download url>
      writes pack_release.iss (beside this script) with the pack's version data for the game installer:
      the add-on file name, size and SHA-256, the SHA-256 of manifest.txt, and the download URL

The game installer compares the manifest of an installed pack with PackManifestSha256 (is it the current
release?) and then checks every listed file's size and SHA-256 (is it intact?).
"""
import hashlib, os, sys

PACK_VERSION = '1'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
    return h.hexdigest()


def manifest(pack):
    lines = []
    for root, _, files in os.walk(pack):
        for n in files:
            if n.lower() == 'manifest.txt' and root == pack:
                continue
            p = os.path.join(root, n)
            rel = os.path.relpath(p, pack).replace('\\', '/')
            lines.append((rel, sha256(p), os.path.getsize(p)))
    lines.sort()
    with open(os.path.join(pack, 'manifest.txt'), 'w', newline='\n') as f:
        for rel, h, sz in lines:
            f.write(f'{h} {sz} {rel}\n')
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, 'pack_version.iss'), 'w', newline='\n') as f:
        f.write('; written by gen_pack_manifest.py manifest; do not edit\n')
        f.write('#define PackVersion "%s"\n' % PACK_VERSION)
    print(len(lines), 'files,', sum(x[2] for x in lines) // (1 << 20), 'MB ->', os.path.join(pack, 'manifest.txt'))


def release(pack, exe, url):
    here = os.path.dirname(os.path.abspath(__file__))
    mf = os.path.join(pack, 'manifest.txt')
    total = sum(int(l.split(' ', 2)[1]) for l in open(mf))
    with open(os.path.join(here, 'pack_release.iss'), 'w', newline='\n') as f:
        f.write('; written by gen_pack_manifest.py release; do not edit\n')
        f.write(f'#define PackManifestSha256 "{sha256(mf)}"\n')
        f.write(f'#define PackFileName "{os.path.basename(exe)}"\n')
        f.write(f'#define PackFileSize "{os.path.getsize(exe)}"\n')
        f.write(f'#define PackFileSha256 "{sha256(exe)}"\n')
        f.write(f'#define PackInstalledBytes "{total}"\n')
        f.write(f'#define PackUrl "{url}"\n')
    print('wrote pack_release.iss for', os.path.basename(exe))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'manifest':
        manifest(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == 'release':
        release(*sys.argv[2:])
    else:
        raise SystemExit(__doc__)
