#!/usr/bin/env python3
"""Remove only unchanged files recorded by this demo; keep user-added files."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent
record_path = root / '.hitz-installer-demo.json'
record = json.loads(record_path.read_text())

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

for rel, expected in record['files'].items():
    path = root / rel
    if path.is_symlink() or not path.resolve().is_relative_to(root):
        continue
    if path.is_file() and digest(path) == expected:
        path.unlink()
shortcut = Path(record['shortcut']) if record['shortcut'] else None
if shortcut and shortcut.is_file() and not shortcut.is_symlink() and digest(shortcut) == record['shortcut_sha256']:
    shortcut.unlink()
(root / 'installation.log').unlink(missing_ok=True)
record_path.unlink()
for path in sorted(root.rglob('*'), reverse=True):
    if path.is_dir() and not path.is_symlink():
        try:
            path.rmdir()
        except OSError:
            pass
try:
    root.rmdir()
except OSError:
    pass
print('Demo removed. Any added or modified files have been kept.')
