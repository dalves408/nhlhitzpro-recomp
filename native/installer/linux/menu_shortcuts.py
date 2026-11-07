#!/usr/bin/env python3
"""Create/repair native Linux application-menu entries. Run as your desktop user."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def require_desktop_user():
    if hasattr(os, 'geteuid') and os.geteuid() == 0 and os.environ.get('SUDO_USER') not in (None, '', 'root'):
        raise ValueError('Run this program without sudo, from your normal desktop account. Menu entries belong to that user.')


def xdg_home(variable, default):
    value = os.environ.get(variable, '')
    return Path(value) if value and Path(value).is_absolute() else Path.home() / default


def data_home():
    return xdg_home('XDG_DATA_HOME', '.local/share')


def value_escape(value):
    # First the desktop-file string-value layer, then Exec's argument layer.
    return str(value).replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')


def exec_argument(value):
    value = str(value)
    for char in ('\\', '"', '`', '$'):
        value = value.replace(char, '\\' + char)
    return value_escape('"' + value.replace('%', '%%') + '"')


def refresh_menu(applications):
    results = []
    env = os.environ.copy()
    env['XDG_DATA_HOME'] = str(data_home())
    commands = []
    database = shutil.which('update-desktop-database')
    if database:
        commands.append([database, str(applications)])
    versions = ('5', '6') if os.environ.get('KDE_SESSION_VERSION') == '5' else ('6', '5')
    for version in versions:
        cache = shutil.which('kbuildsycoca' + version)
        if cache:
            commands.append([cache, '--noincremental'])
            break
    for command in commands:
        try:
            proc = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
            results.append({'command': command, 'returncode': proc.returncode,
                            'detail': (proc.stdout + proc.stderr)[-1500:]})
        except (OSError, subprocess.TimeoutExpired) as exc:
            results.append({'command': command, 'returncode': None, 'detail': str(exc)})
    return results


def install_shortcuts(game_dir, replace=False):
    require_desktop_user()
    game = Path(game_dir).expanduser().resolve(strict=True)
    if any(char in str(game) for char in '\n\r\t'):
        raise ValueError('Choose a game directory without newline or tab characters.')
    for name in ('play.sh', 'settings.sh', 'icon.png'):
        if not (game / name).is_file():
            raise ValueError('Choose the installed game folder containing play.sh, settings.sh and icon.png. Missing: ' + name)
    apps = data_home() / 'applications'
    apps.mkdir(parents=True, exist_ok=True)
    entries = [(apps / 'nhl-hitz-pro.desktop', 'NHL Hitz Pro', 'play.sh', 'Play NHL Hitz Pro'),
               (apps / 'nhl-hitz-pro-settings.desktop', 'NHL Hitz Pro Settings', 'settings.sh',
                'Configure NHL Hitz Pro')]
    if not replace and any(path.exists() or path.is_symlink() for path, *_ in entries):
        raise ValueError('Game menu entries already exist. Use the installed install-shortcuts.py repair tool, or disable shortcuts during Setup.')
    hashes, backups = {}, []
    for path, title, script, description in entries:
        content = ('[Desktop Entry]\nVersion=1.0\nType=Application\n' +
                   f'Name={title}\nComment={description}\n' +
                   f'Exec=/bin/sh {exec_argument(game / script)}\nTryExec=/bin/sh\n' +
                   f'Path={value_escape(game)}\nIcon={value_escape(game / "icon.png")}\n' +
                   'Terminal=false\nStartupNotify=false\nCategories=Game;SportsGame;\n')
        if path.is_symlink():
            raise ValueError('Refusing to overwrite a symbolic-link menu entry: ' + str(path))
        if path.exists() and path.read_text() != content:
            backup = path.with_name(path.name + f'.backup-{time.time_ns()}')
            shutil.copy2(path, backup)
            backups.append(str(backup))
        # Desktop files are executable as well as readable, for desktops that
        # apply a trust check when launching user-created entries.
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=apps, prefix='.hitz-menu-', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        try:
            temporary.chmod(0o755)
            if replace:
                temporary.replace(path)
            else:
                os.link(temporary, path)  # exclusive publication; never overwrite another entry
        finally:
            temporary.unlink(missing_ok=True)
        hashes[str(path)] = hashlib.sha256(content.encode('utf-8')).hexdigest()
    return {'shortcuts': hashes, 'applications': str(apps), 'backups': backups,
            'refresh': refresh_menu(apps), 'desktop': os.environ.get('XDG_CURRENT_DESKTOP', '')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, help='Installed directory containing play.sh')
    args = parser.parse_args()
    root = None
    try:
        require_desktop_user()
        game = args.game_dir
        if game is None:
            from tkinter import filedialog, messagebox
            import tkinter as tk
            root = tk.Tk()
            root.withdraw()
            candidate = Path(__file__).resolve().parent
            selected = filedialog.askdirectory(title='Choose the NHL Hitz Pro installation folder',
                                               initialdir=candidate if (candidate / 'play.sh').exists() else Path.home())
            if not selected:
                return
            game = Path(selected)
        result = install_shortcuts(game, replace=True)
        game = game.expanduser().resolve()
        record_path = game / '.hitz-linux-install.json'
        if record_path.exists():
            try:
                record = json.loads(record_path.read_text())
                record['shortcuts'] = result['shortcuts']
                record_path.write_text(json.dumps(record, indent=2))
            except (OSError, ValueError) as exc:
                result['uninstall_record_warning'] = str(exc)
        print(json.dumps(result, indent=2))
        if root:
            messagebox.showinfo('Menu shortcuts created',
                'Added NHL Hitz Pro and NHL Hitz Pro Settings to your application menu.\n\n' + result['applications'], parent=root)
    except (OSError, ValueError) as exc:
        if root:
            messagebox.showerror('Could not create menu shortcuts', str(exc), parent=root)
        else:
            print(str(exc), file=sys.stderr)
        raise SystemExit(1)
    finally:
        if root:
            root.destroy()


if __name__ == '__main__':
    main()
