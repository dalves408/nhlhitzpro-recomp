#!/usr/bin/env python3
"""Remove this install's unchanged program files and shortcuts; retain game data."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent
record_path = root / '.hitz-linux-install.json'
record = json.loads(record_path.read_text())
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
for name, expected in record['files'].items():
    path = root / name
    if path.resolve().is_relative_to(root) and not path.is_symlink() and path.is_file() and digest(path) == expected:
        path.unlink()
for name, expected in record['shortcuts'].items():
    path = Path(name)
    if path.is_file() and not path.is_symlink() and digest(path) == expected:
        path.unlink()
record_path.unlink()
print('Program files and shortcuts removed. Game data, texture pack, saves, settings, and modified files were kept.')
