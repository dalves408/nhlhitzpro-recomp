# Linux installer (NHL Hitz Pro)

Ports the NHL Hitz 2003 Linux Setup wizard (`gui` + `ProductionBackend`) to Pro
with Pro branding, the Pro app icon, and Pro texture-pack handling.


## Primary artifact: Standalone AppImage

Same deliverable style as NHL Hitz 2003 (`NHLHitz2003-*-Standalone.AppImage`):

```sh
python3 scripts/build_linux_test_installer.py \
  --source "$PWD" --project "$PWD" \
  --output native/installer/Output/linux --version 1.0.8-linux \
  --appimagetool /path/to/appimagetool-x86_64.AppImage \
  --appimage-runtime /path/to/runtime-x86_64
```

Produces `NHLHitzPro-<version>-Standalone.AppImage` (and a RecompLauncher `.tar.gz`).
Without `--appimagetool` / `--appimage-runtime`, the builder falls back to a `.run` self-extractor (optional).

## Options (parity with 2003)

| Option | Behavior |
|---|---|
| Destination folder | New empty folder or existing NHL Hitz Pro install |
| Disc import | Xbox USA ISO/XISO or extracted folder with `default.xbe` (manifest-checked) |
| Reuse `game_files` | Skips import when installed data still validates |
| Hi-res textures | Optional; reuses nonempty `texture_pack`, else local folder / Windows add-on `.exe` / download |
| Menu shortcuts | Game + **Settings** entries (selected by default); Pro icon |
| Launch / Configure | Finish page can start `play.sh` or `settings.sh` (`nhl_config.py`) |
| Noninteractive | `--install DIR [--source PATH] [--no-textures] [--no-shortcuts]` |
| Uninstall | Removes unchanged program files + recorded shortcuts; keeps game data/textures/settings |
| Menu repair | `install-shortcuts.py` / `NHLHitzPro-Repair-Menu.py` |
| AppImage / tar.gz | Optional via `--appimagetool` + `--appimage-runtime` |

## Branding / paths

- Binary: `nhl_hitz_pro_recomp`
- XDG data/cache: `nhlhitzpro`
- Default install: `~/Games/NHL Hitz Pro`
- Icon: `native/resources/nhl_hitz_pro.png` (from Pro `.ico`) → packaged `icon.png`
- Settings app: `linux/nhl_config.py` (Video / Controls / Textures), desktop shortcut beside the game

## Texture pack (Pro-specific)

Windows ships `NHLHitzPro-HiRes-Textures-1.exe` (Inno). Linux Setup accepts, in order:

1. `texture_pack/` beside Setup (with `manifest.txt`)
2. The add-on `.exe` beside Setup (extracted with **innoextract**)
3. Download from the `textures-1` GitHub release (resume + SHA-256), then innoextract
4. Optional `texture_pack.7z` / `.bin` via 7-Zip

## Build

```sh
# After a Release Linux player build exists at native/build-linux/nhl_hitz_pro_recomp:
python3 scripts/build_linux_test_installer.py \
  --source "$PWD" \
  --project "$PWD" \
  --output native/installer/Output/linux \
  --version 1.0.8-linux
```

Artifact: `NHLHitzPro-Setup-<version>-x86_64.run`

```sh
sh NHLHitzPro-Setup-1.0.8-linux-x86_64.run \
  --install "$HOME/Games/Hitz Pro Test" --source /path/to/game.iso \
  --no-textures --no-shortcuts
```

Runtime deps: Python/Tk, SDL2, libepoxy, libpng, optional 7-Zip / innoextract for textures.
