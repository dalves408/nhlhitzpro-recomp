"""Read-only Xbox disc/folder validation shared with the planned Linux setup.

Metadata comes from the Windows installer's supported-disc manifest. The demo
fixture has its own manifest and cannot validate as the supported game.
"""
import hashlib
import json
from pathlib import Path
import struct

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
BASES = (0, 0xFD90, 0x30600, 0xFD90000, 0x18300000)


def validate(source, metadata, progress=lambda text: None, destination=None,
             copy_progress=lambda done, total, name: None, cancelled=lambda: False):
    source = Path(source).expanduser().resolve(strict=True)
    manifest = json.loads(Path(metadata).read_text())
    expected = manifest['files']
    index = {}
    stream = None
    try:
        if source.is_dir():
            progress('Checking the extracted game folder…')
            # Index once, preserving case-insensitive Xbox lookup on Linux.
            for p in source.rglob('*'):
                if p.is_symlink():
                    raise ValueError('Game source folders must not contain symbolic links.')
                if p.is_file():
                    key = p.relative_to(source).as_posix().lower()
                    if key in index:
                        raise ValueError('Duplicate filenames differing only in case.')
                    index[key] = (p, p.stat().st_size)
        elif source.is_file():
            progress('Reading the Xbox disc image…')
            stream = source.open('rb')
            length = source.stat().st_size
            def read(offset, size):
                if offset < 0 or size < 0 or offset + size > length:
                    raise ValueError('The disc image is truncated or damaged.')
                stream.seek(offset)
                data = stream.read(size)
                if len(data) != size:
                    raise ValueError('Could not read the complete disc image.')
                return data
            base = next((b for b in BASES if b + 32 * SECTOR + 28 <= length and
                         read(b + 32 * SECTOR, 20) == MAGIC), None)
            if base is None:
                raise ValueError('That file is not a supported Xbox ISO/XISO image.')
            sector, size = struct.unpack('<II', read(base + 32 * SECTOR + 20, 8))
            visited_dirs = set()
            def walk(sector, size, prefix='', depth=0):
                if depth > 32 or size > 16 * 1024 * 1024 or (sector, size) in visited_dirs:
                    raise ValueError('Invalid directory structure in the disc image.')
                visited_dirs.add((sector, size))
                if not size:
                    return
                data = read(base + sector * SECTOR, size)
                pending, visited = [0], set()
                while pending:
                    pos = pending.pop()
                    if pos in visited or pos + 14 > size:
                        raise ValueError('Invalid directory entry in the disc image.')
                    visited.add(pos)
                    left, right, sec, count, attrs, nlen = struct.unpack_from('<HHIIBB', data, pos)
                    if left == 0xFFFF:
                        continue
                    if not nlen or pos + 14 + nlen > size:
                        raise ValueError('Invalid filename in the disc image.')
                    name = data[pos + 14:pos + 14 + nlen].decode('ascii')
                    if name in ('.', '..') or any(c in name for c in '/\\:\x00\r\n'):
                        raise ValueError('Unsafe filename in the disc image.')
                    rel = prefix + name
                    if attrs & 0x10:
                        walk(sec, count, rel + '/', depth + 1)
                    else:
                        offset = base + sec * SECTOR
                        if offset + count > length or rel.lower() in index:
                            raise ValueError('Invalid or duplicate file in the disc image.')
                        index[rel.lower()] = (offset, count)
                    pending.extend(link * 4 for link in (left, right) if link)
            walk(sector, size)
        else:
            raise ValueError('Select an existing disc image or extracted game folder.')
        progress('Checking the game files and supported executable…')
        for name, size in expected.items():
            if cancelled():
                raise InterruptedError('Installation stopped. Run Setup again to retry.')
            if name.lower() not in index or index[name.lower()][1] != size:
                raise ValueError('This source is incomplete or is not the supported NHL Hitz Pro Xbox USA release. Missing or incorrect: ' + name)
        location, size = index['default.xbe']
        if size > 32 * 1024 * 1024:
            raise ValueError('Invalid Xbox executable size.')
        xbe = read(location, size) if stream else location.read_bytes()
        if hashlib.sha256(xbe).hexdigest() != manifest['xbe_sha256']:
            raise ValueError('The Xbox executable does not match the supported release.')
        if destination is not None:
            destination = Path(destination).resolve()
            destination.mkdir(parents=True, exist_ok=True)
            done, total = 0, sum(expected.values())
            for name, size in expected.items():
                target = destination / name
                if not target.resolve().is_relative_to(destination):
                    raise ValueError('Unsafe game-file destination.')
                target.parent.mkdir(parents=True, exist_ok=True)
                location, _ = index[name.lower()]
                input_file = stream if stream else location.open('rb')
                try:
                    if stream:
                        stream.seek(location)
                    with target.open('xb') as output:
                        remaining = size
                        while remaining:
                            if cancelled():
                                raise InterruptedError('Installation stopped. Run Setup again to retry.')
                            block = input_file.read(min(1024 * 1024, remaining))
                            if not block:
                                raise ValueError('Source changed or was truncated while importing: ' + name)
                            output.write(block)
                            remaining -= len(block)
                            done += len(block)
                            copy_progress(done, total, name)
                finally:
                    if not stream:
                        input_file.close()
            if hashlib.sha256((destination / 'default.xbe').read_bytes()).hexdigest() != manifest['xbe_sha256']:
                raise ValueError('The source executable changed during import.')
        return dict(source=str(source), kind='ISO/XISO image' if stream else 'Extracted folder',
                    files=len(expected), bytes=sum(expected.values()), fixture=manifest.get('fixture', False))
    finally:
        if stream:
            stream.close()
