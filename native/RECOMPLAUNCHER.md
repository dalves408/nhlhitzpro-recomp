# RecompLauncher (Level 3)

The Windows release can also be played through [RecompLauncher](https://recomplauncher.com/sdk). The launcher reads one file,
`recomplauncher.json`, from the top of a release ZIP and does the rest: it finds or asks for the player's disc, runs the
importer, writes the settings and starts the game. The normal Inno Setup installer is unchanged and stays for everyone else.

## What this build declares

| Level | What | Where |
|---|---|---|
| 1 | start `nhl_hitz_pro_recomp.exe`; the player's own USA disc (`iso_or_folder`, `default.xbe` check); saves in `%LOCALAPPDATA%\NHLHitzPro\SaveData` | `scripts/recomplauncher_manifest.py` |
| 2 | settings written to `%LOCALAPPDATA%\NHLHitzPro\settings.ini`: render scale, fullscreen, stretch, VSync, frame rate, anti-aliasing, anisotropic filtering, hi-res textures (the same keys the F1 menu and `config.exe` use) | same |
| 3 | `prepare`: `nhl_import.exe --prepare {game} {install}/game_files`; `game.dumps`: Redump entry for NHL Hitz Pro (USA), serial MW-026 | same, `native/installer/nhl_import.c` |

`nhl_import --prepare <source> <dest>` is the headless setup step: no window, `progress <0..1> <text>` lines on stdout, exit 0 only
when `<dest>` holds a complete, verified copy. It writes to `<dest>.stage-<pid>` and swaps the folder in at the end, so a failed or
interrupted run never leaves a half-written `game_files` (an existing one is kept until the new one is complete). Exit codes: 2 source
not found, 3 not an Xbox image, 4 not the supported version (`default.xbe` differs), 5 damaged or incomplete, 6 cannot write
(the launcher shows the messages in `exit_codes`). The plain `nhl_import <source> <dest>` the installer uses is unchanged.

The verified badge needs a Redump-style image: the Redump USA dump is 7,825,162,240 bytes, CRC-32 `5133da30`
(<http://redump.org/disc/54211/>). A trimmed XISO (like the 1.5 GB image used in development) imports fine but gets no badge.
The Europe dump (disc 26938) is not listed because its `default.xbe` is not the supported version.

## Build the launcher ZIP

```
cmake --build native/build-release --config Release --target nhl_hitz_pro_recomp nhl_import nhl_config
python scripts/recomplauncher_manifest.py --version <version>
```

Output: `native/installer/Output/NHLHitzPro-v<version>-Windows-x64.zip` and `.sha256` (player, importer, settings program,
`recomplauncher.json`). The name carries the system and processor as the guide asks. Publish it next to the installer on the
GitHub release; the launcher prefers a ZIP over an installer `.exe`. Every new release gets a new version number as usual.

`python scripts/recomplauncher_manifest.py --version x --json-only` prints the manifest. It validates against
`https://recomplauncher.com/schema/recomplauncher.json`.

## Not done / to check in the launcher's Developer tools

- Not opened in RecompLauncher itself yet (Settings > Developers > Add a build > the extracted ZIP): check the Level 3 badge,
  the settings rows (keys are written as `section/key`, as the NHL Hitz 2003 build does), and Play.
- `RECOMPLAUNCHER_MODE`: `bigpicture` makes the game start borderless fullscreen whatever the saved setting is (the saved setting is not changed); `desktop`, or no launcher, uses the saved fullscreen setting as before (`want_fullscreen_at_start` in `backend_d3d11.c`).
- The release build writes no log file, so the launcher's crash report has nothing to attach (the debug build does log).
