#!/usr/bin/env python3
"""Disposable Linux setup prototype. Contains no game or real textures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import time
from disc import validate as validate_disc

ROOT = Path(__file__).resolve().parent


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def check_source(source, report=lambda text: None):
    source = Path(source).expanduser().resolve()
    metadata = 'sample-disc.json' if source == (ROOT / 'sample-disc.iso').resolve() else 'supported-disc.json'
    return validate_disc(source, ROOT / metadata, report)


def install(destination, textures=True, shortcut=True, report=lambda *a: None, source=None):
    source_info = check_source(source or ROOT / 'sample-disc.iso', lambda text: report(2, text))
    dest = Path(destination).expanduser().resolve()
    if dest.exists() and any(dest.iterdir()):
        raise ValueError('Please choose an empty folder for this demo. Existing installations are left alone.')
    if shortcut:
        apps = Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'applications'
        if (apps / 'nhlhitzpro-installer-demo.desktop').exists():
            raise ValueError('A demo menu shortcut already exists. Remove the earlier demo first, or disable shortcuts.')
    # Extract only our pinned, bundled demo archive; never a user's texture.bin.
    meta = json.loads((ROOT / 'payload.json').read_text())
    archive = ROOT / 'demo-texture.bin'
    report(5, 'Verifying the bundled demo payload…')
    if sha(archive) != meta['sha256']:
        raise ValueError('Demo payload verification failed.')
    tool = shutil.which('7zz') or shutil.which('7z')
    if not tool:
        raise ValueError('7-Zip is missing. See the included README.')
    with tempfile.TemporaryDirectory(prefix='hitz-demo-stage-') as tmp:
        stage = Path(tmp)
        report(10, 'Extracting game data… (demo placeholders)')
        game = stage / 'game_files'
        game.mkdir()
        for name in ('default.xbe', 'Anims.pck', 'NIS/nis.pck', 'Sound/BaSpeech.dat'):
            target = game / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'')
            time.sleep(0.15)
        (game / 'DEMO-NOT-GAME-DATA.txt').write_text('These are placeholders, not extracted game data.\n')
        report(20, 'Extracting demo files (7z / LZMA2)…')
        result = subprocess.run([tool, 'x', '-t7z', '-y', '-o' + str(stage), str(archive)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise ValueError('Extraction failed:\n' + result.stdout[-2000:])
        actual = {p.relative_to(stage).as_posix(): sha(p) for p in stage.rglob('*') if p.is_file()}
        archive_files = {name: value for name, value in actual.items() if not name.startswith('game_files/')}
        if archive_files != meta['files'] or any(p.is_symlink() for p in stage.rglob('*')):
            raise ValueError('Extracted demo files do not match their manifest.')
        if not textures:
            for p in (stage / 'texture_pack').rglob('*'):
                if p.is_file():
                    p.unlink()
            for p in sorted((stage / 'texture_pack').rglob('*'), reverse=True):
                if p.is_dir():
                    p.rmdir()
            (stage / 'texture_pack').rmdir()
        shutil.copyfile(ROOT / 'icon.png', stage / 'icon.png')
        shutil.copyfile(ROOT / 'remove-demo.py', stage / 'remove-demo.py')
        files = sorted(p for p in stage.rglob('*') if p.is_file())
        dest.mkdir(parents=True, exist_ok=True)
        # Record only the files this demo owns. Cleanup preserves added/modified files.
        owned = {}
        for index, p in enumerate(files):
            rel = p.relative_to(stage)
            target = dest / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, target)
            if rel.as_posix() in ('launch-demo.sh', 'remove-demo.py'):
                target.chmod(0o755)
            owned[rel.as_posix()] = sha(target)
            report(35 + 55 * (index + 1) / len(files), 'Installing ' + rel.as_posix())
            time.sleep(0.12)
        desktop = None
        if shortcut:
            apps = Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'applications'
            apps.mkdir(parents=True, exist_ok=True)
            desktop = apps / 'nhlhitzpro-installer-demo.desktop'
            if desktop.exists():
                raise ValueError('A demo menu shortcut already exists. Remove the earlier demo first, or disable shortcuts.')
            # Freedesktop quoted Exec arguments: escape reserved characters.
            exe = str(dest / 'launch-demo.sh')
            for ch in ('\\', '"', '`', '$'):
                exe = exe.replace(ch, '\\' + ch)
            exe = exe.replace('%', '%%')
            desktop.write_text('[Desktop Entry]\nType=Application\nName=NHL Hitz Pro — Installer Demo\n'
                               f'Exec="{exe}"\nIcon={dest / "icon.png"}\nTerminal=false\nCategories=Game;\n')
        record = {'files': owned, 'shortcut': str(desktop) if desktop else None,
                  'shortcut_sha256': sha(desktop) if desktop else None}
        (dest / '.hitz-installer-demo.json').write_text(json.dumps(record, indent=2))
        (dest / 'installation.log').write_text('Installer demo completed. No game files or real textures installed.\n'
                                              f'Destination: {dest}\nDemo textures: {textures}\nSource: {source_info}\n')
    report(100, 'Installation complete.')
    return dest


class DemoBackend:
    """Backend contract: the wizard does not need to know how files are imported.

    Production supplies the same properties and validate/install/launch methods.
    All backend calls except launch run off the Tk thread. install reports
    (percentage, message), returns the installation folder, and raises on error.
    """
    is_demo = True
    default_folder = Path.home() / 'Games/NHL Hitz Pro Demo'

    def validate(self, source, report):
        return check_source(source, report)

    def install(self, destination, textures, shortcut, report, source):
        return install(destination, textures, shortcut, report, source)

    def launch(self, destination):
        subprocess.Popen([str(Path(destination) / 'launch-demo.sh')])


def gui(backend=None):
    """Shared screen flow. The demo-specific operations live in DemoBackend."""
    backend = backend or DemoBackend()
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    root = tk.Tk()
    root.title('NHL Hitz Pro • Linux Setup' + (' Demo' if backend.is_demo else ' — ' + getattr(backend, 'version', 'Test')))
    root.geometry('880x600')
    root.minsize(880, 600)
    style = ttk.Style()
    style.theme_use('clam')
    style.configure('TButton', padding=(16, 9))
    style.configure('TCheckbutton', padding=8)
    style.configure('TProgressbar', background='#f37a27')
    sidebar = tk.Frame(root, bg='#101923', width=225)
    sidebar.pack(side='left', fill='y')
    sidebar.pack_propagate(False)
    image = tk.PhotoImage(file=str(ROOT / 'icon.png'))
    image = image.subsample(max(1, (image.width() + 143) // 144))
    root.iconphoto(True, image)
    tk.Label(sidebar, image=image, bg='#101923').pack(pady=(38, 20))
    tk.Label(sidebar, text='NHL HITZ\nPRO', fg='white', bg='#101923',
             font=('Sans', 25, 'bold'), justify='left').pack(anchor='w', padx=25)
    tk.Label(sidebar, text='LINUX SETUP' + ('\n\nINSTALLER DEMO' if backend.is_demo else ''), fg='#ff9b51', bg='#101923',
             font=('Sans', 11), justify='left').pack(anchor='w', padx=25, pady=25)
    shell = tk.Frame(root, bg='#f5f6f8')
    shell.pack(fill='both', expand=True)
    body = tk.Frame(shell, bg='#f5f6f8', padx=28, pady=28)
    body.pack(fill='both', expand=True)
    bar = tk.Frame(shell, bg='#e7e9ed', padx=18, pady=14)
    bar.pack(fill='x', side='bottom')
    step = 0
    busy = False
    destination = tk.StringVar(value=str(backend.default_folder))
    source = tk.StringVar()
    verified_source = None
    reusing_game = False
    reusing_textures = False
    textures = tk.BooleanVar(value=True)
    shortcut = tk.BooleanVar(value=True)
    launch_after = tk.BooleanVar(value=False)
    events = queue.Queue()
    progress = tk.DoubleVar()
    status = tk.StringVar()

    def label(text, size=11, bold=False):
        tk.Label(body, text=text, bg='#f5f6f8', fg='#172537', justify='left',
                 anchor='w', wraplength=535, font=('Sans', size, 'bold' if bold else 'normal')).pack(fill='x', pady=(0, 16))

    def close():
        if busy:
            if hasattr(backend, 'cancel'):
                if messagebox.askyesno('Stop installation?', 'Stop the current operation? Downloaded data is kept so you can resume later.', parent=root):
                    backend.cancel()
            else:
                messagebox.showinfo('Setup is busy', 'Please let the current verification or extraction finish.', parent=root)
        else:
            if hasattr(backend, 'close'):
                backend.close()
            root.destroy()

    def render():
        nonlocal step
        for child in body.winfo_children():
            child.destroy()
        back.configure(state='normal' if step in (1, 2, 4, 5) else 'disabled')
        next_button.configure(text='Next →', state='normal')
        if step == 0:
            label('Welcome to NHL Hitz Pro', 22, True)
            label('Setup will guide you through the installation.', 14)
            label('Setup can use an NHL Hitz Pro Xbox USA disc image (.iso or .xiso) or an extracted game folder containing default.xbe.')
            label('Choose where to install the game, select your game source, and optionally add the 4x hi-res texture pack. Administrator access is not required.')
            if backend.is_demo:
                label('Demo mode: use the sample disc or verify your own source. Only placeholders are installed; no downloads occur.', 10)
        elif step == 1:
            label('Choose an installation folder', 21, True)
            label('Choose an empty folder for the demo.' if backend.is_demo else
                  'Choose a new folder or your existing NHL Hitz Pro folder. Setup can reuse the game files and textures already there.')
            ttk.Entry(body, textvariable=destination).pack(fill='x', pady=10)
            def browse():
                selected = filedialog.askdirectory(parent=root)
                if selected:
                    folder = Path(selected)
                    if backend.is_demo or not (backend.installed_game(folder) or
                            (folder / 'texture_pack').is_dir() or (folder / '.hitz-linux-install.json').is_file()):
                        folder /= Path(backend.default_folder).name
                    destination.set(str(folder))
            ttk.Button(body, text='Browse…', command=browse).pack(anchor='e')
        elif step == 2:
            label('Your copy of the game', 22, True)
            label('Select an Xbox disc image (.iso, .xiso) or an extracted folder containing default.xbe. Setup will verify the supported USA release before installing.')
            ttk.Entry(body, textvariable=source).pack(fill='x', pady=(0, 10))
            choices = tk.Frame(body, bg='#f5f6f8')
            choices.pack(fill='x', pady=(0, 16))
            def browse_iso():
                selected = filedialog.askopenfilename(parent=root, title='Select NHL Hitz Pro Xbox disc image',
                    filetypes=[('Xbox disc images', '*.iso *.xiso *.ISO *.XISO'), ('All files', '*')])
                if selected:
                    source.set(selected)
            def browse_game_folder():
                selected = filedialog.askdirectory(parent=root, title='Select folder containing default.xbe')
                if selected:
                    source.set(selected)
            ttk.Button(choices, text='Disc image…', command=browse_iso).pack(side='left', padx=(0, 8))
            ttk.Button(choices, text='Game folder…', command=browse_game_folder).pack(side='left')
            if backend.is_demo:
                ttk.Button(body, text='Use sample disc (demo only)',
                           command=lambda: source.set(str(ROOT / 'sample-disc.iso'))).pack(anchor='w', pady=(0, 14))
                label('Your original disc image and game folder are read only. This demo checks real sources but installs placeholder game data.', 10)
        elif step == 3:
            label('Verifying your game', 22, True)
            label('Checking the disc structure, required files, and supported game executable…')
            ttk.Progressbar(body, mode='indeterminate').pack(fill='x', pady=20)
            bar_widget = body.winfo_children()[-1]
            bar_widget.start(12)
            tk.Label(body, textvariable=status, bg='#f5f6f8', wraplength=535, anchor='w', justify='left').pack(fill='x')
            next_button.configure(state='disabled')
        elif step == 4:
            label('Installation options', 22, True)
            if reusing_textures:
                label('Existing textures detected. Setup will keep them.')
            else:
                ttk.Checkbutton(body, text='Install the 4x hi-res texture pack', variable=textures).pack(anchor='w')
                label('Uses NHLHitzPro-HiRes-Textures-1.exe or texture_pack/ beside Setup if available. Otherwise the pack is downloaded. Interrupted downloads can be resumed by running Setup again.')
                label('Download: 1.85 GB  •  Installed textures: 2.95 GB', 10)
                if hasattr(backend, 'texture_summary'):
                    label(backend.texture_summary(), 10)
            ttk.Checkbutton(body, text='Create an application-menu shortcut', variable=shortcut).pack(anchor='w')
            if backend.is_demo:
                label('Demo mode uses a tiny bundled texture archive. No real hi-res pack will be downloaded or installed.', 10)
        elif step == 5:
            label('Ready to install', 22, True)
            label('Folder:\n' + destination.get())
            label('Game source:\n' + source.get())
            label('Game source verified: ' + verified_source['kind'] +
                  '\nHi-res textures: ' + ('Keep existing' if reusing_textures else 'Yes' if textures.get() else 'No') +
                  '\nApplication-menu shortcut: ' + ('Yes' if shortcut.get() else 'No'))
            if textures.get() and not reusing_textures and hasattr(backend, 'texture_summary'):
                label(backend.texture_summary(), 10)
            if backend.is_demo:
                label('Demo: installs tiny placeholders. Your source files remain unchanged.', 10)
            next_button.configure(text='Install')
        elif step == 6:
            label('Installing…', 22, True)
            label('Import game data → Install program files → Add textures → Create shortcuts', 11)
            ttk.Progressbar(body, variable=progress, maximum=100).pack(fill='x', pady=20)
            tk.Label(body, textvariable=status, bg='#f5f6f8', wraplength=535, anchor='w', justify='left').pack(fill='x')
            next_button.configure(state='disabled')
        else:
            label('You’re all set', 24, True)
            label('NHL Hitz Pro setup has finished.\n\nInstallation folder:\n' + destination.get())
            if textures.get() or reusing_textures:
                label('Enable hi-res textures in the configuration tool or F1 settings, with render scale at 2x or higher.')
            if backend.is_demo:
                label('Demo mode installed placeholders. Launch opens a confirmation window.', 10)
            ttk.Checkbutton(body, text='Launch NHL Hitz Pro', variable=launch_after).pack(anchor='w', pady=(0, 14))
            ttk.Button(body, text='Open installation folder', command=lambda: subprocess.Popen(['xdg-open', destination.get()])).pack(anchor='w')
            if hasattr(backend, 'configure'):
                ttk.Button(body, text='Open settings', command=lambda: backend.configure(destination.get())).pack(anchor='w', pady=8)
            next_button.configure(text='Finish')

    def verify_worker(value, installed=False):
        try:
            info = backend.validate(value, lambda text: events.put(('status', text)))
            events.put(('verified', info, installed))
        except Exception as exc:
            events.put(('verify-error', str(exc), installed))

    def start_verify(value, installed=False):
        nonlocal step, busy
        step = 3
        busy = True
        status.set('Checking existing game files…' if installed else 'Opening your selected source…')
        render()
        threading.Thread(target=verify_worker, args=(value, installed), daemon=True).start()

    def worker():
        try:
            backend.install(destination_value, textures_value, shortcut_value, lambda p, s: events.put(('progress', p, s)), source_value)
            events.put(('done',))
        except Exception as exc:
            events.put(('error', str(exc)))

    def advance():
        nonlocal step, busy, destination_value, textures_value, shortcut_value, source_value, reusing_game, reusing_textures
        if step == 7:
            if launch_after.get():
                try:
                    backend.launch(destination.get())
                except OSError as exc:
                    messagebox.showerror('Could not launch the game', str(exc), parent=root)
                    return
            if hasattr(backend, 'close'):
                backend.close()
            root.destroy()
            return
        if step == 1:
            p = Path(destination.get()).expanduser()
            if not p.is_absolute() or '\n' in str(p) or '\r' in str(p):
                messagebox.showerror('Folder', 'Choose an absolute folder path.', parent=root)
                return
            try:
                p = p.resolve()
                if backend.is_demo:
                    if p.exists() and (not p.is_dir() or any(p.iterdir())):
                        raise ValueError('Please choose an empty folder.')
                else:
                    backend.check_destination(p)
                    reusing_textures = backend.installed_textures(p)
            except (OSError, ValueError) as exc:
                messagebox.showerror('Folder', str(exc), parent=root)
                return
            destination.set(str(p))
            reusing_game = False
            if not backend.is_demo:
                source.set('')
                installed = backend.installed_game(p)
                if installed:
                    source.set(str(installed))
                    start_verify(str(installed), installed=True)
                    return
        if step == 2:
            if not source.get().strip():
                messagebox.showerror('Game source', 'Select your disc image or game folder.' +
                                     (' You can also use the sample disc for this demo.' if backend.is_demo else ''), parent=root)
                return
            start_verify(source.get())
            return
        step += 1
        render()
        if step == 6:
            busy = True
            destination_value, textures_value, shortcut_value = destination.get(), textures.get(), shortcut.get()
            source_value = source.get()
            threading.Thread(target=worker, daemon=True).start()

    def poll():
        nonlocal step, busy, verified_source, reusing_game
        try:
            while True:
                event = events.get_nowait()
                if event[0] == 'progress':
                    progress.set(event[1])
                    status.set(f'{event[1]:.0f}% — {event[2]}')
                elif event[0] == 'status':
                    status.set(event[1])
                elif event[0] == 'verified':
                    verified_source = event[1]
                    reusing_game = event[2]
                    busy = False
                    step = 4
                    render()
                elif event[0] == 'verify-error':
                    busy = False
                    step = 2
                    if event[2]:
                        source.set('')
                    render()
                    messagebox.showerror('Game source could not be verified', event[1], parent=root)
                elif event[0] == 'done':
                    busy = False
                    if hasattr(backend, 'finish'):
                        backend.finish(root)
                    step = 7
                    render()
                else:
                    busy = False
                    step = 5
                    render()
                    messagebox.showerror('Installation stopped', event[1], parent=root)
        except queue.Empty:
            pass
        root.after(100, poll)

    def previous():
        nonlocal step
        step = (1 if reusing_game else 2) if step == 4 else step - 1
        render()
    destination_value, textures_value, shortcut_value = '', False, False
    source_value = ''
    ttk.Button(bar, text='Close', command=close).pack(side='left')
    next_button = ttk.Button(bar, text='Next →', command=advance)
    next_button.pack(side='right')
    back = ttk.Button(bar, text='← Back', command=previous)
    back.pack(side='right', padx=8)
    root.protocol('WM_DELETE_WINDOW', close)
    render()
    poll()
    root.mainloop()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--test-install', type=Path, help='Headless extraction smoke test')
    parser.add_argument('--no-textures', action='store_true')
    args = parser.parse_args()
    if args.test_install:
        install(args.test_install, not args.no_textures, False, lambda p, s: print(f'{p:.0f}% {s}'))
    else:
        gui()
