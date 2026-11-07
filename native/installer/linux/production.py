"""Real Linux installer backend for the shared Tk wizard (NHL Hitz Pro)."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.request

from disc import validate
from menu_shortcuts import data_home, xdg_home, install_shortcuts, require_desktop_user

ROOT = Path(__file__).resolve().parent


def ensure_settings():
    """Create the fresh-install defaults without replacing an existing file."""
    user_data = data_home() / 'nhlhitzpro'
    user_data.mkdir(parents=True, exist_ok=True)
    try:
        with (user_data / 'settings.ini').open('x') as output:
            output.write(
                '[display]\nresolution_scale=1\nfullscreen=1\nstretch=0\nvsync=0\n'
                'frame_rate=60\nmsaa=1\nanisotropy=1\n\n'
                '[textures]\nenabled=0\nrink=1\nlogos=1\njerseys=1\nglow=1\nother=1\n')
    except FileExistsError:
        pass


def digest(path, cancelled=lambda: False, progress=lambda done, total: None):
    h = hashlib.sha256()
    done, total = 0, path.stat().st_size
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            if cancelled():
                raise InterruptedError('Stopped. Downloaded files have been kept.')
            h.update(chunk)
            done += len(chunk)
            progress(done, total)
    return h.hexdigest()


def publish(stage, dest):
    """Overlay staged program files, moving game data only when imported."""
    if not dest.exists():
        stage.rename(dest)
        return
    if not dest.is_dir() or dest.is_symlink():
        raise ValueError('Installation folder changed during setup.')
    assets = {'game_files', 'texture_pack'}
    directories = set()
    moves = []
    for path in stage.rglob('*'):
        relative = path.relative_to(stage)
        if relative.parts[0] in assets:
            if len(relative.parts) == 1:
                moves.append((path, dest / relative))
        elif path.is_dir():
            directories.add(dest / relative)
        else:
            moves.append((path, dest / relative))
    for directory in sorted(directories):
        if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
            raise ValueError('A program folder conflicts with an existing path: ' + str(directory))
    for _, target in moves:
        if target.is_symlink() or (target.exists() and
                (not target.is_dir() if target.name in assets and target.parent == dest else not target.is_file())):
            raise ValueError('A setup file conflicts with an existing path: ' + str(target))
    backup = stage.parent / 'rollback'
    backup.mkdir()
    replaced, installed, created = [], [], []
    try:
        for directory in sorted(directories):
            if not directory.exists():
                directory.mkdir()
                created.append(directory)
        for index, (path, target) in enumerate(moves):
            if target.exists():
                saved = (target.with_name(f'game_files.backup-{time.time_ns()}')
                         if target == dest / 'game_files' else backup / str(index))
                if target == dest / 'texture_pack' and any(target.iterdir()):
                    # Replacing an older/mismatched pack (e.g. Revision 1) is intentional.
                    saved = target.with_name(f'texture_pack.backup-{time.time_ns()}')
                target.rename(saved)
                replaced.append((saved, target))
            path.rename(target)
            installed.append((target, path))
    except (OSError, ValueError):
        for target, path in reversed(installed):
            target.rename(path)
        for saved, target in reversed(replaced):
            saved.rename(target)
        for directory in reversed(created):
            directory.rmdir()
        raise


class ProductionBackend:
    is_demo = False
    default_folder = Path.home() / 'Games/NHL Hitz Pro'

    def __init__(self):
        require_desktop_user()
        self.stop = threading.Event()
        self.metadata = json.loads((ROOT / 'release.json').read_text())
        self.version = self.metadata['version']
        self.pack = self.metadata['texture']
        # Per-part caches: ~/.cache/nhlhitzpro/downloads/<part-sha256>/<filename>
        self.cache_root = xdg_home('XDG_CACHE_HOME', '.cache') / 'nhlhitzpro/downloads'
        self.downloaded = []
        self.delete_on_exit = []

    def cancel(self):
        self.stop.set()

    def cancelled(self):
        return self.stop.is_set()

    def texture_parts(self):
        parts = self.pack.get('parts')
        if parts:
            return parts
        return [dict(name=self.pack.get('file_name', 'texture.bin'),
                     bytes=self.pack['bytes'], sha256=self.pack['sha256'],
                     url=self.pack.get('url', ''))]

    def texture_summary(self):
        setup = Path(os.environ.get('HITZ_SETUP_DIR', ROOT))
        if (setup / 'texture_pack' / 'manifest.txt').is_file():
            return 'A texture_pack folder beside Setup will be verified and installed; no download is needed.'
        parts = self.texture_parts()
        beside = sum(1 for part in parts if (setup / part['name']).is_file())
        cached = sum(1 for part in parts if (self.cache_root / part['sha256'] / part['name']).is_file())
        if beside == len(parts):
            names = ' + '.join(part['name'] for part in parts)
            return f'{names} found beside Setup. They will be verified and extracted; no download is needed.'
        if beside or cached:
            return (f'Texture pack parts: {beside} beside Setup, {cached} in cache; '
                    f'missing parts will be downloaded.')
        total = sum(part['bytes'] for part in parts) / 1e9
        rev = self.pack.get('revision', '?')
        return (f'Texture Revision {rev} ({len(parts)} parts, ~{total:.2f} GB) is not beside Setup. '
                'Missing parts will be downloaded from GitHub (textures-2).')

    def validate(self, source, report):
        self.stop.clear()
        return validate(source, ROOT / 'supported-disc.json', report, cancelled=self.cancelled)

    @staticmethod
    def installed_game(destination):
        path = Path(destination) / 'game_files'
        return path if path.is_dir() else None

    def installed_textures(self, destination):
        """True only when the installed pack matches the pinned Revision manifest."""
        manifest = Path(destination) / 'texture_pack' / 'manifest.txt'
        if not manifest.is_file():
            return False
        try:
            return digest(manifest) == self.pack['manifest_sha256']
        except OSError:
            return False

    def check_destination(self, dest):
        if dest.exists() and (not dest.is_dir() or (any(dest.iterdir()) and not
                (self.installed_game(dest) or (dest / 'texture_pack').is_dir()
                 or (dest / '.hitz-linux-install.json').is_file()))):
            raise ValueError('Choose an empty folder or an existing NHL Hitz Pro folder.')

    def _verify_part(self, path, part, report, pct_lo=38, pct_hi=43):
        if path.stat().st_size != part['bytes']:
            return False
        last = [0.0]

        def update(done, total):
            now = time.monotonic()
            if now - last[0] > .15 or done == total:
                last[0] = now
                span = pct_hi - pct_lo
                report(pct_lo + span * done / total,
                       f'Verifying {part["name"]} — {done / total:.1%}')

        return digest(path, self.cancelled, update) == part['sha256']

    def _download_part(self, part, report, pct_lo, pct_hi):
        self.cache_root.mkdir(parents=True, exist_ok=True)
        folder = self.cache_root / part['sha256']
        folder.mkdir(parents=True, exist_ok=True)
        complete, partial = folder / part['name'], folder / (part['name'] + '.part')
        if complete.exists():
            if not self._verify_part(complete, part, report, pct_lo, pct_lo + 2):
                raise ValueError('Cached texture part failed verification. Remove it and retry: ' + str(complete))
            self.downloaded.append(complete)
            return complete
        report(pct_lo, f'Downloading {part["name"]}…')
        count = partial.stat().st_size if partial.exists() else 0
        if count > part['bytes']:
            raise ValueError('Partial download exceeds the expected size. Remove it and retry: ' + str(partial))
        if count < part['bytes']:
            headers = {'User-Agent': 'NHLHitzPro-Linux-Setup', 'Accept-Encoding': 'identity'}
            if count:
                headers['Range'] = f'bytes={count}-'
            response = urllib.request.urlopen(urllib.request.Request(part['url'], headers=headers), timeout=20)
            with response:
                if response.status == 200:
                    count = 0
                elif response.status != 206 or not re.fullmatch(
                        rf'bytes {count}-{part["bytes"] - 1}/{part["bytes"]}',
                        response.headers.get('Content-Range', '')):
                    raise ValueError('The server returned an invalid resume range. Partial data was kept.')
                start_count, start, last = count, time.monotonic(), 0.0
                with partial.open('ab' if count else 'wb') as output:
                    while True:
                        if self.cancelled():
                            raise InterruptedError('Download stopped. Run Setup again to resume.')
                        block = response.read(256 * 1024)
                        if not block:
                            break
                        if count + len(block) > part['bytes']:
                            raise ValueError('Server sent more data than expected.')
                        output.write(block)
                        count += len(block)
                        now = time.monotonic()
                        if now - last > .15 or count == part['bytes']:
                            speed = (count - start_count) / max(.001, now - start)
                            frac = count / part['bytes']
                            report(pct_lo + (pct_hi - pct_lo) * frac,
                                   f'Downloading {part["name"]} — {frac:.1%}\n'
                                   f'{count / 1e9:.2f} / {part["bytes"] / 1e9:.2f} GB • {speed / 1e6:.2f} MB/s')
                            last = now
        if not self._verify_part(partial, part, report, pct_hi - 2, pct_hi):
            raise ValueError('Download is incomplete or failed SHA-256 verification. Cached data was kept: ' + str(partial))
        partial.replace(complete)
        self.downloaded.append(complete)
        return complete

    def _payloads(self, report):
        if self.cancelled():
            raise InterruptedError('Download stopped. Run Setup again to resume.')
        setup = Path(os.environ.get('HITZ_SETUP_DIR', ROOT))
        folder = setup / 'texture_pack'
        if (folder / 'manifest.txt').is_file():
            report(43, 'Using texture_pack beside Setup…')
            return [folder]
        parts = self.texture_parts()
        resolved = []
        n = max(1, len(parts))
        for index, part in enumerate(parts):
            local = setup / part['name']
            pct_lo = 43 + index * (22 // n)
            pct_hi = 43 + (index + 1) * (22 // n)
            if local.is_file():
                if not self._verify_part(local, part, report, pct_lo, pct_lo + 3):
                    raise ValueError(
                        f'{part["name"]} beside Setup is not the required pack part. '
                        'Move it aside to download the current Revision 2 part.')
                resolved.append(local)
                continue
            resolved.append(self._download_part(part, report, pct_lo, pct_hi))
        return resolved

    def _extract_one_7z(self, archive, stage, report, temp, label, pct_lo, pct_hi):
        tool = shutil.which('7zz') or shutil.which('7z')
        if not tool:
            raise ValueError('Install 7-Zip to extract the texture pack: sudo dnf install 7zip / sudo apt install 7zip')
        log = Path(temp) / f'7zip-{label}.log'
        with log.open('wb') as output:
            process = subprocess.Popen(
                [tool, 'x', '-t7z', '-y', '-bsp1', '-o' + str(stage), str(archive)],
                stdout=output, stderr=subprocess.STDOUT)
            try:
                while process.poll() is None:
                    if self.cancelled():
                        raise InterruptedError('Texture extraction stopped. Downloaded data was kept.')
                    with log.open('rb') as progress_file:
                        progress_file.seek(max(0, log.stat().st_size - 4096))
                        matches = re.findall(rb'(\d+)%', progress_file.read())
                    if matches:
                        pct = min(100, int(matches[-1]))
                        report(pct_lo + (pct_hi - pct_lo) * pct / 100,
                               f'Extracting {label} — {pct}%')
                    time.sleep(.15)
                if process.returncode:
                    raise ValueError(f'Texture extraction failed for {label}. Downloaded data was kept.')
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()

    def _extract_texture_pack(self, payloads, stage, report, temp):
        dest = stage / 'texture_pack'
        if len(payloads) == 1 and payloads[0].is_dir():
            shutil.copytree(payloads[0], dest, dirs_exist_ok=True)
            return
        bins = [path for path in payloads if path.suffix.lower() in {'.bin', '.7z'}]
        exes = [path for path in payloads if path.suffix.lower() == '.exe']
        if bins:
            for index, archive in enumerate(bins):
                lo = 70 + index * (20 // max(1, len(bins)))
                hi = 70 + (index + 1) * (20 // max(1, len(bins)))
                report(lo, f'Extracting {archive.name}…')
                self._extract_one_7z(archive, stage, report, temp, archive.name, lo, hi)
            if not dest.is_dir():
                raise ValueError('Texture archives did not create texture_pack/.')
            return
        if len(exes) == 1:
            tool = shutil.which('innoextract')
            if not tool:
                raise ValueError(
                    'Place texture-01.bin and texture-02.bin (or texture_pack/) beside Setup, '
                    'or install innoextract for a legacy Windows add-on .exe.')
            extract = Path(temp) / 'inno'
            extract.mkdir()
            report(70, 'Extracting the Windows texture add-on with innoextract…')
            proc = subprocess.run([tool, '-e', '-d', str(extract), str(exes[0])],
                                  capture_output=True, text=True, timeout=600)
            if proc.returncode:
                raise ValueError('innoextract failed:\n' + (proc.stderr or proc.stdout)[-1600:])
            found = None
            for candidate in extract.rglob('manifest.txt'):
                if candidate.parent.name == 'texture_pack' or (candidate.parent / 'rink').is_dir():
                    found = candidate.parent
                    break
            if found is None:
                for candidate in extract.rglob('texture_pack'):
                    if candidate.is_dir() and (candidate / 'manifest.txt').is_file():
                        found = candidate
                        break
            if found is None:
                raise ValueError('Could not find texture_pack/manifest.txt inside the add-on installer.')
            shutil.copytree(found, dest)
            return
        raise ValueError('No usable texture payload was resolved.')

    def stage_program(self, stage):
        shutil.copytree(ROOT / 'player', stage, dirs_exist_ok=True)

    def install(self, destination, textures, shortcut, report, source, *, install_program=True):
        self.stop.clear()
        binary = ROOT / 'player/native/build-linux/nhl_hitz_pro_recomp'
        try:
            check = subprocess.run([str(binary), '--build-info'], capture_output=True, text=True, timeout=10)
        except OSError as exc:
            raise ValueError(
                'The Linux player cannot run on this system. See README-LINUX.txt for runtime requirements.\n'
                + str(exc)) from exc
        if check.returncode:
            raise ValueError(
                'A game runtime dependency is missing or incompatible. See README-LINUX.txt.\n'
                + check.stderr[-1600:])
        dest = Path(destination).expanduser().resolve()
        self.check_destination(dest)
        existing_game = self.installed_game(dest)
        reuse_game = False
        if existing_game:
            report(3, 'Checking the game files already installed…')
            try:
                self.validate(existing_game, lambda text: report(3, text))
                reuse_game = True
            except InterruptedError:
                raise
            except (OSError, ValueError) as exc:
                if not source or Path(source).expanduser().resolve() == existing_game.resolve():
                    raise ValueError(
                        'The installed game files need a supported disc image or extracted game folder.\n'
                        + str(exc)) from exc
        if not reuse_game and not source:
            raise ValueError('Select your Xbox USA disc image or extracted game folder.')
        reuse_textures = self.installed_textures(dest)
        textures = textures and not reuse_textures
        setup_dir = Path(os.environ.get('HITZ_SETUP_DIR', ROOT))
        if textures and not ((setup_dir / 'texture_pack' / 'manifest.txt').is_file()
                              or all((setup_dir / part['name']).is_file() for part in self.texture_parts())
                              or shutil.which('7zz') or shutil.which('7z') or shutil.which('innoextract')):
            raise ValueError(
                'To install textures: place texture_pack/ or texture-01.bin + texture-02.bin beside Setup, '
                'or install 7-Zip (for the .bin parts).')
        dest.parent.mkdir(parents=True, exist_ok=True)
        required = ((0 if reuse_game else self.metadata['game_bytes'])
                    + self.pack['extracted_bytes'] * bool(textures) + 200_000_000)
        if textures and not (setup_dir / 'texture_pack' / 'manifest.txt').is_file():
            self.cache_root.mkdir(parents=True, exist_ok=True)
            remaining = 0
            for part in self.texture_parts():
                if (setup_dir / part['name']).is_file():
                    continue
                complete = self.cache_root / part['sha256'] / part['name']
                partial = self.cache_root / part['sha256'] / (part['name'] + '.part')
                have = (complete.stat().st_size if complete.is_file()
                        else (partial.stat().st_size if partial.exists() else 0))
                remaining += max(0, part['bytes'] - have)
            if remaining:
                if self.cache_root.stat().st_dev == dest.parent.stat().st_dev:
                    required += remaining
                elif shutil.disk_usage(self.cache_root).free < remaining + 50_000_000:
                    raise ValueError(
                        'There is not enough free space in the texture download cache: '
                        + str(self.cache_root))
        if shutil.disk_usage(dest.parent).free < required:
            raise ValueError(f'At least {required / 1e9:.2f} GB of free installation space is needed.')
        record_path = dest / '.hitz-linux-install.json'
        old_record = (json.loads(record_path.read_text()) if install_program and
                      record_path.is_file() and not record_path.is_symlink() else {})
        with tempfile.TemporaryDirectory(prefix='.hitz-setup-', dir=dest.parent) as temp:
            stage = Path(temp) / 'install'
            stage.mkdir()
            report(5, 'Verifying your game source…')
            last = [0.0]

            def importing(done, total, name):
                now = time.monotonic()
                if now - last[0] > .15 or done == total:
                    last[0] = now
                    report(5 + done / total * 30, f'Extracting game data — {done / total:.1%}\n{name}')

            if reuse_game:
                report(35, 'Keeping the installed game files.')
            else:
                validate(source, ROOT / 'supported-disc.json', destination=stage / 'game_files',
                         copy_progress=importing, cancelled=self.cancelled)
            if install_program:
                report(36, 'Installing the Linux game and settings application…')
                self.stage_program(stage)
            if reuse_textures:
                report(70, 'Keeping the installed texture pack (matches Revision %s).'
                       % self.pack.get('revision', '?'))
            payloads = self._payloads(report) if textures else None
            if payloads:
                report(70, 'Installing the hi-res texture pack (Revision %s)…'
                       % self.pack.get('revision', '?'))
                self._extract_texture_pack(payloads, stage, report, temp)
                manifest = stage / 'texture_pack/manifest.txt'
                if not manifest.is_file():
                    raise ValueError('Texture pack is missing manifest.txt.')
                if digest(manifest) != self.pack['manifest_sha256']:
                    raise ValueError('Extracted texture manifest does not match the release.')
                rows = manifest.read_text().splitlines()
                if self.pack.get('files') is not None and len(rows) != self.pack['files']:
                    raise ValueError('Texture file count does not match the release.')
                for row in rows:
                    if self.cancelled():
                        raise InterruptedError('Installation stopped. Downloaded data was kept.')
                    _expected, size, rel = row.split(' ', 2)
                    path = stage / 'texture_pack' / rel
                    if (not path.resolve().is_relative_to(stage / 'texture_pack')
                            or path.is_symlink() or path.stat().st_size != int(size)):
                        raise ValueError('Missing or invalid extracted texture: ' + rel)
            if self.cancelled():
                raise InterruptedError('Installation stopped. Downloaded data was kept.')
            if install_program:
                owned = {
                    p.relative_to(stage).as_posix(): digest(p)
                    for p in stage.rglob('*')
                    if p.is_file() and not p.relative_to(stage).as_posix().startswith(
                        ('game_files/', 'texture_pack/'))
                }
                (stage / 'install-info.json').write_text(json.dumps(self.metadata, indent=2))
                owned['install-info.json'] = digest(stage / 'install-info.json')
                record = {'files': owned, 'shortcuts': old_record.get('shortcuts', {})}
                (stage / record_path.name).write_text(json.dumps(record, indent=2))
            publish(stage, dest)
        if shortcut:
            report(95, 'Creating application-menu shortcuts…')
            menu = install_shortcuts(dest, replace=True)
            record['shortcuts'] = menu['shortcuts']
            record['menu_refresh'] = menu['refresh']
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=dest, delete=False) as stream:
                updated_record = Path(stream.name)
                json.dump(record, stream, indent=2)
            try:
                updated_record.replace(record_path)
            finally:
                updated_record.unlink(missing_ok=True)
        ensure_settings()
        report(100, 'Installation complete.')
        return dest

    def finish(self, parent):
        if self.downloaded:
            from tkinter import messagebox
            listing = '\n'.join(str(path) for path in self.downloaded)
            keep = messagebox.askyesno(
                'Keep texture downloads?',
                'Keep the downloaded texture parts for future repairs or reinstalls?\n\n' + listing,
                parent=parent)
            if not keep:
                self.delete_on_exit = list(self.downloaded)
            self.downloaded = []

    def close(self):
        for path in self.delete_on_exit:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
        self.delete_on_exit = []

    def launch(self, destination):
        subprocess.Popen([str(Path(destination) / 'play.sh')], start_new_session=True)

    def configure(self, destination):
        subprocess.Popen([str(Path(destination) / 'settings.sh')], start_new_session=True)
