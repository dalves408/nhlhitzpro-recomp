"""Standalone and RecompLauncher entry points for the Linux AppImage."""
import argparse
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import shutil
import tempfile

from installer import gui
from production import ProductionBackend, ROOT, data_home, ensure_settings

IMAGE_NAME = 'NHLHitzPro.AppImage'


class AppImageBackend(ProductionBackend):
    def __init__(self):
        super().__init__()
        self.data = data_home() / 'nhlhitzpro'
        self.location = self.data / 'appimage-game-folder'
        if self.location.is_file():
            folder = Path(self.location.read_text().rstrip('\n'))
            if folder.is_absolute():
                self.default_folder = folder

    @contextmanager
    def setup_lock(self):
        self.data.mkdir(parents=True, exist_ok=True)
        with (self.data / 'appimage-setup.lock').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise ValueError('Another NHL Hitz Pro setup is already running.') from exc
            yield

    def stage_program(self, stage):
        image = os.environ.get('APPIMAGE')
        if not image or not Path(image).is_file():
            raise ValueError('Open the AppImage file itself to install the desktop launchers.')
        shutil.copy2(image, stage / IMAGE_NAME)
        (stage / IMAGE_NAME).chmod(0o755)
        for name in ('icon.png', 'uninstall.py', 'menu_shortcuts.py', 'install-shortcuts.py', 'README-LINUX.txt'):
            shutil.copy2(ROOT / 'player' / name, stage / name)
        for script, option in (('play.sh', '--play'), ('settings.sh', '--settings')):
            launcher = stage / script
            launcher.write_text(
                '#!/bin/sh\nset -eu\n'
                'dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                'if [ ! -c /dev/fuse ]; then export APPIMAGE_EXTRACT_AND_RUN=1; fi\n'
                f'exec "$dir/{IMAGE_NAME}" {option} --data-dir "$dir" "$@"\n')
            launcher.chmod(0o755)

    def install(self, destination, textures, shortcut, report, source):
        with self.setup_lock():
            dest = super().install(destination, textures, shortcut, report, source)
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=self.data, delete=False) as stream:
                pending = Path(stream.name)
                stream.write(str(dest) + '\n')
            try:
                pending.replace(self.location)
            finally:
                pending.unlink(missing_ok=True)
            return dest

    def prepare(self, destination, source, textures):
        def report(percent, message):
            print(f'progress {percent / 100:.4f} {" ".join(message.splitlines())}', flush=True)
        with self.setup_lock():
            existing = self.installed_game(destination)
            if source and existing and source.resolve() != existing.resolve():
                self.validate(source, lambda message: report(0, message))
            super().install(destination, textures, False, report, source, install_program=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--setup', action='store_true', help='Open the standalone setup wizard')
    mode.add_argument('--prepare', action='store_true', help='Import game data without a window')
    mode.add_argument('--play', action='store_true', help='Play directly without opening Setup')
    mode.add_argument('--settings', action='store_true', help='Open the game settings')
    parser.add_argument('--game', type=Path, help='Xbox USA disc image or extracted source for --prepare')
    parser.add_argument('--data-dir', type=Path, help='Directory holding game_files and texture_pack')
    parser.add_argument('--textures', action='store_true', help='Also install hi-res textures in --prepare mode')
    args, extra = parser.parse_known_args()
    if (args.game or args.textures) and not args.prepare:
        parser.error('--game and --textures require --prepare')
    if extra and not args.play:
        parser.error('unrecognized arguments: ' + ' '.join(extra))
    try:
        backend = AppImageBackend()
        destination = (args.data_dir or backend.default_folder).expanduser().resolve()
        if args.prepare:
            backend.prepare(destination, args.game, args.textures)
            return
        if args.settings:
            ensure_settings()
            os.environ['XDG_DATA_HOME'] = str(data_home())
            os.execv(str(ROOT / 'player/settings.sh'), ['settings.sh'])
        source = backend.installed_game(destination)
        if not args.setup and source:
            try:
                backend.validate(source, lambda _: None)
            except (OSError, ValueError):
                if args.play:
                    raise
            else:
                ensure_settings()
                os.environ['XDG_DATA_HOME'] = str(data_home())
                os.environ['HITZ_GAME_ROOT'] = str(destination)
                script = ROOT / 'player/play.sh'
                os.execv(str(script), [str(script), *extra])
        if args.play:
            raise ValueError('No valid game data. Run Setup with your Xbox USA disc image or extracted folder.')
        backend.default_folder = destination
        gui(backend)
    except (OSError, ValueError) as exc:
        parser.exit(1, str(exc) + '\n')


if __name__ == '__main__':
    main()
