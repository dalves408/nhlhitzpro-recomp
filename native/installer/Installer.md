# Installer and RecompLauncher changes: handover notes

Status as of 2026-11-05. **All of this is uncommitted on `main`.** Nothing has been pushed or released. A release needs a new
version number (the last release is 1.0.8) and a test on other PCs first. Do not commit or publish without David's say-so.

## What changed (files)

| File | Change |
|---|---|
| `native/installer/nhl_hitz_pro.iss` | Default install folder is now `C:\Games\NHL Hitz Pro`. `DisableDirPage=no`: the folder page is always shown. `UsePreviousAppDir=yes` is kept, so an existing install (found by AppId) is pre-filled instead of the default. The player can change the folder either way. |
| `native/installer/nhl_import.c` | New `--prepare <source> <dest>` mode for RecompLauncher (see below). Plain `nhl_import <source> <dest>` (used by the installer) is unchanged. |
| `native/src/backend_d3d11.c` | `RECOMPLAUNCHER_MODE` handling in `want_fullscreen_at_start()` (see below). |
| `scripts/recomplauncher_manifest.py` (new) | Builds `recomplauncher.json` (Level 3) and the launcher ZIP. |
| `native/RECOMPLAUNCHER.md` (new) | Levels, prepare, Redump data, build command, open items. Read it first. |
| `native/src/debug_report.c`, `native/CMakeLists.txt`, `native/DEBUG-BUILD.md` | Earlier debug-kit fixes, also uncommitted (log opened in append mode, symbols loaded at start, `NHL_DEBUG_STDERR_BUFFER`). Not part of a release. |

## Installer default folder

- Fresh install: folder page opens at `C:\Games\NHL Hitz Pro`.
- Update or reinstall: folder page opens at the folder of the existing install.
- The `[Code]` section only uses `WizardDirValue`, so it needs no change. `ShouldSkipPage` only skips the disc page.
- Checked: the script compiles with ISCC. **Not yet checked:** the wizard itself (fresh install, update over an existing install, changing the folder), because the real installer must not be run on David's PC. Test on another PC or a VM.
- The old default was `{autopf}\NHL Hitz Pro`. An old install under Program Files is still found and pre-filled.

## nhl_import `--prepare`

`nhl_import --prepare <source> <dest>`: headless setup step for the launcher.

- No window. Prints `progress <0..1> <text>` lines on stdout. Exit 0 only when `<dest>` holds a complete, verified copy.
- Writes to `<dest>.stage-<pid>`, then swaps it in. An existing `<dest>` is moved to `<dest>.old-<pid>` and deleted only after the swap succeeds, so a failed run never leaves half-written game files.
- Exit codes: 2 source not found, 3 not an Xbox image, 4 unsupported version (`default.xbe` differs), 5 damaged or incomplete, 6 cannot write. The launcher messages for these are in `exit_codes` in the manifest.
- Tested locally: schema-valid manifest, prepare runs, ZIP game start. Not tested in RecompLauncher itself.

## RECOMPLAUNCHER_MODE

The launcher sets this environment variable when it starts the game.

- `bigpicture`: the game starts borderless fullscreen whatever the saved setting is. The forced value is **not** written to `settings.ini`.
- `desktop`, unset, or any other value: the saved fullscreen setting is used, as before.
- Implementation: `want_fullscreen_at_start()` in `backend_d3d11.c`, a `getenv` + `_stricmp` check at the top of the release/F11 path.
- Not used on purpose: `RECOMPLAUNCHER_VERSION` (David said it is not needed). `RECOMPLAUNCHER_PAD` is not handled.
- Not tested with a real launcher: only reasoned from the code and a local run with the variable set.

## Build commands

Release (when a version is agreed):

```
cmake --build native/build-release --config Release --target nhl_hitz_pro_recomp nhl_import nhl_config
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" /DAppVersion=<version> /DBuildDir=..\build-release\Release native\installer\nhl_hitz_pro.iss
python scripts\recomplauncher_manifest.py --version <version>
```

Debug flavour (never part of a release): see `native/DEBUG-BUILD.md`. The last one built was 1.0.8.1-debug, in `C:\Recomp\local-build\1.0.8.1-debug\`. It was built **before** the default-folder change, so rebuild it to test that change.

## To do for the next worker

1. Rebuild the debug installer (version 1.0.8.1 or a new one) and test the folder page on another PC: fresh install, update over an existing install, changing the folder, uninstall.
2. Open the launcher ZIP in RecompLauncher (Settings > Developers > Add a build > the extracted ZIP). Check the Level 3 badge, the settings rows, Play, and `bigpicture` vs `desktop`.
3. Check the `section/key` setting keys (for example `display/fullscreen`) are written correctly into `%LOCALAPPDATA%\NHLHitzPro\settings.ini`. This form is unverified and follows the NHL Hitz 2003 build.
4. The Redump verified badge needs a full 7,825,162,240-byte Redump-style image (CRC-32 `5133da30`). David's 1.5 GB trimmed XISO imports fine but gets no badge.
5. The release build writes no log, so the launcher's crash report has nothing to attach. Decide whether to change that.
6. For a release: new version number, tests on other PCs, then David publishes. Installers are unsigned, so SmartScreen warns on other PCs (More info, then Run anyway). Remind him on each installer build.

## Don't

- Don't run the real game installer on David's PC, or touch his real settings or registry. Tests use an isolated `LOCALAPPDATA`.
- Don't put the debug build, the upscaler or frame generation, or the Windows XP work in a release. The XP work is on hold on the `winxp` branch.
