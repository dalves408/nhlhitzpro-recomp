# Debug build for a player's PC

A normal release has no log and no diagnostics. When a player hits a bug that needs more information, build the
debug flavour: the same game code plus self-monitoring (`src/debug_report.c`). It is never part of a release.

## What it records (all inside the game, nothing to install on the PC)
Files go to `%LOCALAPPDATA%\NHLHitzPro\debug`:
- `session_<date>.log` (and `.log.1`): everything the game prints, a header (build tag, OS, CPU, RAM, displays, settings),
  a heartbeat every 5 s (frames, memory, handles), every controller change (`[PADLOG]`), key presses (`[KEYLOG]`), audio
  voice stats (`[APU-VSTAT]`), first-chance exceptions (`[EXC]`) and the reason for every exit (`[EXIT]`). Last 6 sessions kept,
  at most ~190 MB per session.
- `crash_<date>.dmp`: written on an unhandled exception, `abort()`/failed assert or invalid CRT parameter (about 200 MB: the
  usual minidump plus the guest's 64 MB RAM and 64 MB contiguous window). A message box names the folder.
- `hang_<date>.dmp`: no frame presented for 15 s (at most 2 per run).
- `running.lock`: present while running; seen at the next start it logs `[PREV]` (the last session did not end cleanly).

**DEL marks a graphical problem.** Press DEL in the game window when something looks wrong (the game never sees the key):
the log gets a `[MARK]` line with the time and frame count, three screenshots of the game image are saved (now, +1.5 s, +4 s:
`mark<n>_<date>_<k>.bmp`) and the first two marks also get a dump (`mark<n>_<date>.dmp`, with the guest RAM and GPU window). The
game keeps running and the crash monitoring stays on. A session that ends normally with at least one mark is zipped to the
Desktop on exit (`NHLHitzPro-debug-marks-<date>.zip`, waited for, then a message says where it is); a session with no mark and no
crash ends silently. Crashes zip as before.

The installer adds a Start menu shortcut "Collect debug files" (`collect_debug.ps1`) that zips the folder to the Desktop.

## Build (own build directory, never build-release)
```
cmake -S native -B native/build-debuglog -DNHL_RELEASE=ON -DNHL_DEBUGLOG=ON "-DNHL_DEBUG_TAG=1.0.8.1-debug"
cmake --build native/build-debuglog --config Release --target nhl_hitz_pro_recomp nhl_import nhl_config
ISCC /DAppVersion=1.0.8.1 /DBuildDir=..\build-debuglog\Release /DOutSuffix=-debug /DDebugBuild=1 native\installer\nhl_hitz_pro.iss
```
Give the debug installer a four-part version above the release it is based on (the installer then offers an update).
Keep `nhl_hitz_pro_recomp.exe` and `.pdb` from the build directory: the dumps can only be read with that exact pair.
`NHL_DEBUG_SELFTEST=crash` makes the game fault 20 s in, to check the dump path.

## Why it cannot leak into a release
`NHL_DEBUGLOG` defaults to OFF, needs `NHL_RELEASE`, and the installer script only adds the debug files when
`/DDebugBuild=1` is passed (default `BuildDir` stays `build-release`, output name gets `-debug`). A release build is
configured without the flag, so `debug_report.c` is not even compiled.
