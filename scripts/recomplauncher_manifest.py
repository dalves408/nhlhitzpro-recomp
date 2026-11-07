"""RecompLauncher manifest helpers for NHL Hitz Pro.

Linux packaging imports ``manifest('linux')`` (see ``build_linux_test_installer.py``).

Windows Level 3 ZIP (Champ handoff):

    python scripts/recomplauncher_manifest.py --version 1.0.9 \\
        [--build native/build-release/Release] [--out native/installer/Output]

writes ``NHLHitzPro-v<version>-Windows-x64.zip`` (player, importer, settings program,
``recomplauncher.json``) and its ``.sha256``. The normal Inno Setup installer is unchanged
and stays the choice for players who do not use the launcher.
"""
import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

# Redump, Microsoft Xbox, "NHL Hitz Pro" USA, serial MW-026 (http://redump.org/disc/54211/).
# The Europe dump (disc 26938, CRC-32 eb3dea0c) is not listed: its default.xbe is unsupported.
DUMPS = [{'name': 'NHL Hitz Pro (USA)', 'size': 7825162240, 'crc32': '5133da30'}]

DATA_WIN = '{local}/NHLHitzPro'  # %LOCALAPPDATA%\\NHLHitzPro


def _setting(identifier, label, key, choices, default, help_text=None):
    row = {
        'id': identifier,
        'label': label,
        'choices': [{'label': name, 'set': {key: str(value)}} for name, value in choices],
        'defaults': {key: str(default)},
    }
    if help_text:
        row['help'] = help_text
    return row


def _linux_manifest():
    data = '{local}/nhlhitzpro'
    toggle = [('Off', 0), ('On', 1)]
    settings = [
        _setting('scale', 'Render scale', 'display/resolution_scale',
                 [(f'{n}x', n) for n in range(1, 6)], 1),
        _setting('fullscreen', 'Fullscreen', 'display/fullscreen', toggle, 1),
        _setting('vsync', 'VSync', 'display/vsync', toggle, 0),
        _setting('frame-cap', 'Frame cap', 'display/frame_rate',
                 [(f'{n} FPS', n) for n in (30, 60, 120, 144, 240)] + [('Unlimited', 0)], 60),
        _setting('msaa', 'MSAA', 'display/msaa', [('Off', 1)] + [(f'{n}x', n) for n in (2, 4, 8)], 1),
        _setting('anisotropy', 'Anisotropic filtering', 'display/anisotropy',
                 [('Off', 1)] + [(f'{n}x', n) for n in (2, 4, 8, 16)], 1),
        _setting('textures', 'Hi-res textures', 'textures/enabled', toggle, 0),
    ]
    settings[-1]['help'] = 'Requires the texture pack and a render scale of 2x or higher.'
    return {
        '$schema': 'https://recomplauncher.com/schema/recomplauncher.json',
        'format': 1,
        'name': 'NHL Hitz Pro',
        'repo': 'dalves408/nhlhitzpro-recomp',
        'game': {
            'kind': 'iso_or_folder',
            'label': 'Your NHL Hitz Pro Xbox USA disc image or extracted game folder',
            'check': {'file': 'default.xbe', 'xdvdfs': 'default.xbe'},
        },
        'start': {'program': ['NHLHitzPro.AppImage'], 'args': ['--play']},
        'prepare': {
            'program': ['prepare-game.sh'],
            'args': ['{game}'],
            'label': 'Verify and import NHL Hitz Pro game files',
            'timeout': 900,
        },
        'settings': {
            'path': data + '/settings.ini',
            'items': settings,
        },
    }


def _windows_manifest():
    toggle = [('Off', 0), ('On', 1)]
    settings = [
        _setting('scale', 'Render scale', 'display/resolution_scale', [(f'{n}x', n) for n in range(1, 6)], 1,
                 '1x is the original 640x480; higher draws the game sharper but needs a faster graphics card.'),
        _setting('fullscreen', 'Fullscreen', 'display/fullscreen', toggle, 1),
        _setting('stretch', 'Stretch image', 'display/stretch', [('Off (4:3)', 0), ('On (fills window)', 1)], 0),
        _setting('vsync', 'VSync', 'display/vsync', toggle, 0),
        _setting('frame-rate', 'Frame rate', 'display/frame_rate',
                 [(f'{n} FPS', n) for n in (30, 60, 120, 144, 240, 360)], 60,
                 '60 is the original. Higher rates run the game smoother; the intro movies always play at 60.'),
        _setting('msaa', 'Anti-aliasing', 'display/msaa', [('Off', 1)] + [(f'{n}x MSAA', n) for n in (2, 4, 8)], 1),
        _setting('anisotropy', 'Anisotropic filtering', 'display/anisotropy',
                 [('Off', 1)] + [(f'{n}x', n) for n in (2, 4, 8, 16)], 1),
        _setting('textures', 'Hi-res textures', 'textures/enabled', toggle, 1,
                 'Only has an effect when the separate hi-res texture pack is installed.'),
    ]
    return {
        '$schema': 'https://recomplauncher.com/schema/recomplauncher.json',
        'format': 1,
        'name': 'NHL Hitz Pro',
        'repo': 'dalves408/nhlhitzpro-recomp',
        'game': {
            'kind': 'iso_or_folder',
            'label': (
                'Your NHL Hitz Pro Xbox disc image (USA, serial MW-026) or the folder '
                'with its files (the one that holds default.xbe).'
            ),
            'check': {'file': 'default.xbe', 'xdvdfs': 'default.xbe'},
            'dumps': DUMPS,
        },
        'start': {'program': ['nhl_hitz_pro_recomp.exe']},
        'prepare': {
            'program': ['nhl_import.exe'],
            'args': ['--prepare', '{game}', '{install}/game_files'],
            'label': 'Game files from your disc',
            'timeout': 900,
        },
        'config': [{
            'format': 'ini',
            'file': [{'path': DATA_WIN + '/settings.ini'}],
            'new': {key: value for row in settings for key, value in row['defaults'].items()},
        }],
        'settings': settings,
        'saves': [DATA_WIN + '/SaveData'],
        'exit_codes': {
            '2': 'The disc image or folder could not be found.',
            '3': 'That file is not an Xbox disc image.',
            '4': 'That is not the supported version of NHL Hitz Pro (USA).',
            '5': 'The disc image is damaged or incomplete.',
            '6': 'The game files could not be written to the install folder.',
        },
        'looks': {'graphics': 'd3d11', 'lines': 480},
    }


def manifest(platform='windows'):
    """Return the RecompLauncher document for ``linux`` or ``windows``."""
    if platform == 'linux':
        return _linux_manifest()
    if platform in ('windows', 'win32', 'win'):
        return _windows_manifest()
    raise ValueError(f'unsupported platform: {platform!r}')


def main():
    here = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True, help='release version, e.g. 1.0.9')
    parser.add_argument('--build', type=Path, default=here / 'native' / 'build-release' / 'Release')
    parser.add_argument('--out', type=Path, default=here / 'native' / 'installer' / 'Output')
    parser.add_argument('--json-only', action='store_true', help='print recomplauncher.json and exit')
    args = parser.parse_args()
    text = json.dumps(manifest('windows'), indent=2) + '\n'
    if args.json_only:
        sys.stdout.write(text)
        return
    args.out.mkdir(parents=True, exist_ok=True)
    archive = args.out / f'NHLHitzPro-v{args.version}-Windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as package:
        for name in ('nhl_hitz_pro_recomp.exe', 'nhl_import.exe', 'config.exe'):
            package.write(args.build / name, name)
        package.writestr('recomplauncher.json', text)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'RecompLauncher ZIP: {archive}\nSHA-256: {digest}')


if __name__ == '__main__':
    main()
