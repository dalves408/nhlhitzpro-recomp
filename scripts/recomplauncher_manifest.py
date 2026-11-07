"""Generate the RecompLauncher manifest for a platform release."""


def manifest(platform):
    linux = platform == 'linux'
    data = '{local}/' + ('nhlhitzpro' if linux else 'NHLHitzPro')

    def setting(identifier, label, key, choices, default):
        return {
            'id': identifier, 'label': label,
            'choices': [{'label': name, 'set': {key: str(value)}} for name, value in choices],
            'defaults': {key: str(default)},
        }

    toggle = [('Off', 0), ('On', 1)]
    settings = [
        setting('scale', 'Render scale', 'display/resolution_scale',
                [(f'{n}x', n) for n in range(1, 6)], 1),
        setting('fullscreen', 'Fullscreen', 'display/fullscreen', toggle, 1),
        setting('vsync', 'VSync', 'display/vsync', toggle, 0),
        setting('frame-cap', 'Frame cap', 'display/frame_rate',
                [(f'{n} FPS', n) for n in (30, 60, 120, 144, 240)] + [('Unlimited', 0)], 60),
        setting('msaa', 'MSAA', 'display/msaa', [('Off', 1)] + [(f'{n}x', n) for n in (2, 4, 8)], 1),
        setting('anisotropy', 'Anisotropic filtering', 'display/anisotropy',
                [('Off', 1)] + [(f'{n}x', n) for n in (2, 4, 8, 16)], 1),
        setting('textures', 'Hi-res textures', 'textures/enabled', toggle, 0),
    ]
    settings[-1]['help'] = 'Requires the texture pack and a render scale of 2x or higher.'
    return {
        '$schema': 'https://recomplauncher.com/schema/recomplauncher.json',
        'format': 1,
        'name': 'NHL Hitz Pro',
        'repo': 'dalves408/nhlhitzpro-recomp',
        'game': {'kind': 'iso_or_folder', 'label': 'Your NHL Hitz Pro Xbox USA disc image or extracted game folder',
                 'check': {'file': 'default.xbe', 'xdvdfs': 'default.xbe'}},
        'start': {'program': ['NHLHitzPro.AppImage' if linux else 'nhl_hitz_pro_recomp.exe'],
                  'args': ['--play'] if linux else []},
        'prepare': {'program': ['prepare-game.sh' if linux else 'nhl_import.exe'],
                    'args': ['{game}'] if linux else ['--prepare', '{game}', '{install}/game_files'],
                    'label': 'Verify and import NHL Hitz Pro game files', 'timeout': 900},
        'settings': {
            'path': (data + '/settings.ini') if linux else '{local}/NHLHitzPro/settings.ini',
            'items': settings,
        },
    }
