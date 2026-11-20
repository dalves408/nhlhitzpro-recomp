"""Standalone and RecompLauncher entry points for the Linux AppImage.

One AppImage is both Setup and the game. Opened from a folder that holds an installed game (game_files beside
it) it starts the game; opened anywhere else it opens Setup, which installs into that same folder or into any
folder you choose, and puts a copy of the AppImage there. No scripts are installed: menu entries run the
AppImage itself (--play / --settings).
"""
import argparse
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from installer import gui
from production import ProductionBackend, ROOT, data_home, data_folder, ensure_settings
from menu_shortcuts import install_shortcuts

IMAGE_NAME = 'NHLHitzPro.AppImage'


def running_image():
    image = os.environ.get('APPIMAGE')
    return Path(image).resolve() if image and Path(image).is_file() else None


def launcher_data_folder():
    """Inside a RecompLauncher install (recomplauncher.json beside the AppImage) saves and settings live in that
    folder's data/, which is where the launcher's manifest points ({install}/data). Anywhere else this is None
    and the data folder is <game folder>/data."""
    image = running_image()
    if image and (image.parent / 'recomplauncher.json').is_file():
        return image.parent / 'data'
    return None


def clean_env():
    """The environment to start another copy of the AppImage with: without this one's bundled Python, Tk and
    AppImage variables, and with the caller's own LD_LIBRARY_PATH."""
    env = dict(os.environ)
    for name in ('PYTHONHOME', 'PYTHONNOUSERSITE', 'PYTHONDONTWRITEBYTECODE', 'TCL_LIBRARY', 'TK_LIBRARY',
                 'APPIMAGE', 'APPDIR', 'ARGV0', 'OWD', 'NHLHITZPRO_DATA', 'HITZ_SETUP_DIR', 'HITZ_GAME_ROOT'):
        env.pop(name, None)
    original = env.pop('HITZ_ORIG_LD_LIBRARY_PATH', None)
    if original:
        env['LD_LIBRARY_PATH'] = original
    else:
        env.pop('LD_LIBRARY_PATH', None)
    env['PATH'] = os.pathsep.join(p for p in env.get('PATH', '').split(os.pathsep) if p and not p.startswith(str(ROOT)))
    if not Path('/dev/fuse').exists():
        env['APPIMAGE_EXTRACT_AND_RUN'] = '1'
    return env


class AppImageBackend(ProductionBackend):
    def __init__(self):
        super().__init__()
        image = running_image()
        self.image_folder = image.parent if image else None
        # Default install folder: ~/Games/NHL Hitz Pro, unless the user gives another one (the Setup window or
        # --data-dir) or this AppImage already sits in an installed game folder (see main()).
        self.data = Path(os.environ.get('XDG_RUNTIME_DIR') or tempfile.gettempdir())   # only for the setup lock

    @contextmanager
    def setup_lock(self):
        self.data.mkdir(parents=True, exist_ok=True)
        with (self.data / 'nhlhitzpro-setup.lock').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise ValueError('Another NHL Hitz Pro setup is already running.') from exc
            yield

    def program_path(self, dest):
        """The AppImage that runs the installed game: this one when it is already in the install folder,
        otherwise the copy Setup puts there."""
        image = running_image()
        if image and image.parent == Path(dest).resolve():
            return image
        return Path(dest).resolve() / IMAGE_NAME

    def stage_program(self, stage, dest):
        image = running_image()
        if not image:
            raise ValueError('Open the AppImage file itself to install the game.')
        if image.parent != Path(dest).resolve():      # installing elsewhere: the folder gets its own copy
            shutil.copy2(image, stage / IMAGE_NAME)
            (stage / IMAGE_NAME).chmod(0o755)

    def make_shortcuts(self, dest):
        return install_shortcuts(dest, replace=True, program=self.program_path(dest), icon=ROOT / 'icon.png')

    def install(self, destination, textures, shortcut, report, source):
        with self.setup_lock():
            return super().install(destination, textures, shortcut, report, source)

    def prepare(self, destination, source, textures):
        def report(percent, message):
            print(f'progress {percent / 100:.4f} {" ".join(message.splitlines())}', flush=True)
        with self.setup_lock():
            existing = self.installed_game(destination)
            if source and existing and source.resolve() != existing.resolve():
                self.validate(source, lambda message: report(0, message))
            super().install(destination, textures, False, report, source, install_program=False)
            # A silent install is finished: the downloaded texture parts are deleted (the default).
            self.delete_on_exit = list(self.downloaded)
            self.downloaded = []
            self.close()

    def _start(self, destination, option):
        program = self.program_path(destination)
        if not program.is_file():
            raise OSError('The game AppImage is missing from ' + str(destination))
        subprocess.Popen([str(program), option, '--data-dir', str(destination)], env=clean_env(),
                         cwd=str(destination), start_new_session=True)

    def launch(self, destination):
        self._start(destination, '--play')

    def configure(self, destination):
        self._start(destination, '--settings')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--setup', action='store_true', help='Open Setup (install, update or repair)')
    mode.add_argument('--prepare', action='store_true', help='Import game data without a window')
    mode.add_argument('--play', action='store_true', help='Play directly without opening Setup')
    mode.add_argument('--settings', action='store_true', help='Open the game settings')
    parser.add_argument('--game', type=Path, help='Xbox USA disc image or extracted source for --prepare')
    parser.add_argument('--data-dir', type=Path, help='The game folder (holding game_files); default: the AppImage\'s folder')
    parser.add_argument('--textures', action='store_true', help='Also install hi-res textures in --prepare mode')
    args, extra = parser.parse_known_args()
    if (args.game or args.textures) and not args.prepare:
        parser.error('--game and --textures require --prepare')
    if extra and not args.play:
        parser.error('unrecognized arguments: ' + ' '.join(extra))
    try:
        backend = AppImageBackend()
        image = running_image()
        own = image.parent if image else None
        if args.data_dir:
            destination = args.data_dir
        elif own and (backend.installed_game(own) or args.prepare and backend.usable_as_is(own)):
            destination = own              # an installed folder, or RecompLauncher's folder for --prepare
        else:
            destination = backend.default_folder
        destination = destination.expanduser().resolve()
        data = launcher_data_folder() or data_folder(destination)   # saves, settings, logs: in the install folder
        os.environ['NHLHITZPRO_DATA'] = str(data)
        if args.prepare:
            backend.prepare(destination, args.game, args.textures)
            return
        if args.settings:
            ensure_settings(destination, data)
            os.environ['XDG_DATA_HOME'] = str(data_home())
            os.execv(str(ROOT / 'player/settings.sh'), ['settings.sh'])
        source = backend.installed_game(destination)
        # Play when asked to, or when this AppImage sits in (or was pointed at) an installed game folder.
        wants_game = args.play or (not args.setup and (args.data_dir or (own and destination == own)))
        if wants_game and source:
            try:
                backend.validate(source, lambda _: None)
            except (OSError, ValueError):
                if args.play:
                    raise
            else:
                ensure_settings(destination, data)
                os.environ['XDG_DATA_HOME'] = str(data_home())
                os.environ['HITZ_GAME_ROOT'] = str(destination)
                script = ROOT / 'player/play.sh'
                os.execv(str(script), [str(script), *extra])
        if args.play:
            raise ValueError('No valid game data in ' + str(destination) +
                             '. Run Setup with your Xbox USA disc image or extracted folder.')
        backend.default_folder = destination
        gui(backend)
    except (OSError, ValueError) as exc:
        parser.exit(1, str(exc) + '\n')


if __name__ == '__main__':
    main()
