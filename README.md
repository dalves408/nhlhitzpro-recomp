# NHL Hitz Pro for PC

A native Windows build of **NHL Hitz Pro** (original Xbox, 2003, USA release), made by statically recompiling the game with the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit. It plays the intro movies, menus, team select and full matches at 60 fps on a current PC.

This project contains **no game content**. You need your own copy of the Xbox disc.

## Playing

**Requirements**

- Windows 10 or 11, 64-bit
- A GPU with Direct3D 11 support (any from the last decade)
- A multi-core CPU (the game's vertex work and audio run on the CPU)
- About 2 GB of free disk space
- Your own **NHL Hitz Pro (USA)** disc, as an `.iso` / `.xiso` image or as a folder with the game's files (the folder that holds `default.xbe`)

**Install**

1. Run `NHLHitzPro-Setup-1.0.0.exe` and choose where to install.
2. When asked, pick your disc image or folder. Setup checks that it is the supported version and copies the game files it needs (about 1.5 GB).
3. Start **NHL Hitz Pro** from the Start menu.

Saves and the shader cache are kept in `%LOCALAPPDATA%\NHLHitzPro`. Uninstalling removes the game files but leaves your saves.

**Controls**

| | Xbox controller | Keyboard |
|---|---|---|
| Start | Start | Enter |
| A / B / X / Y | A / B / X / Y | Z / X / C / V |
| Move, D-pad | Left stick, D-pad | W A S D, arrow keys |
| Fullscreen / window | | F11 or Alt+Enter (starts fullscreen; the last mode is remembered) |
| Pause and quit | | Esc pauses the game and asks to quit: Yes (Y or Enter) or No (N or Esc, resumes) |

Esc pauses the game (clocks, timers and sound stop) and asks "Do you want to quit?"; choosing No resumes exactly where you left off. The game starts in borderless fullscreen and remembers the last mode you used.

XInput controllers (Xbox 360, Xbox One, most PC pads) work without setup, up to four for the four-player game: they can be plugged in or removed while the game runs, and controller N is player N. The keyboard is an extra input for player 1. The window keeps the game's 4:3 shape; in fullscreen it is centred with black bars.

**Changing the keyboard keys:** run **NHL Hitz Pro Settings** (`config.exe`, in the install folder and the Start menu). Pick an action, click Change key and press the key you want; each action also has an alternate key. Save applies the keys the next time the game starts, and Reset to defaults brings the original layout back. Esc, F11 and Alt+Enter are used by the game itself and can't be assigned. The choices are stored in `%LOCALAPPDATA%\NHLHitzPro\keymap.ini`.

**Known issues**

- The first time through a new part of the game, a short stutter can appear while shaders are built. It does not return once they are cached.
- Some visual details differ from the original console: the entrance tunnel haze, the ice and jersey look, and skate-wear textures on the ice.
- On rare starts the screen stays black after the intro; close the game and start it again.
- Only the USA release is supported. The disc check will refuse any other region or revision.

## Building from source

The recompiled game code is generated from your own `default.xbe`, so it is not in this repository.

**Requirements:** Windows 10/11 x64, Visual Studio 2022 (Desktop C++ workload, Windows SDK 10.0.26100), CMake 3.20 or newer, Python 3.12 with `pip install -r requirements.txt`, and a fast multi-core CPU (generating and building the code takes a long time).

```powershell
git clone --recurse-submodules https://github.com/dalves408/nhlhitzpro-recomp.git
cd nhlhitzpro-recomp
pip install -r requirements.txt
```

Put your game files in `game_files/` (so that `game_files/default.xbe` exists). `native/build/nhl_import.exe <disc image or folder> game_files` does this from an ISO. Then generate the code and build:

```powershell
python scripts/prepare.py --full
cd native
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo --parallel
```

Run `native\build\RelWithDebInfo\nhl_hitz_pro_recomp.exe` from the `native` folder. For the player build (no logging, data in `%LOCALAPPDATA%`, static runtime):

```powershell
cmake -S native -B native/build-release -DNHL_RELEASE=ON
cmake --build native/build-release --config Release --target nhl_hitz_pro_recomp nhl_import
```

The installer script is `native/installer/nhl_hitz_pro.iss` (Inno Setup). `native/installer/gen_manifest.py` regenerates the list of disc files and the `default.xbe` hash it checks.

## How it works

1. `scripts/prepare.py` drives the xboxrecomp pipeline over `default.xbe`: parse, disassemble, identify functions, recover calling conventions, lift to C.
2. `scripts/tool_runner.py` wraps each stage and injects this game's hooks while the code is lifted.
3. `native/` is the Windows CMake project. It links the generated C with the toolkit's runtime (kernel, emulated NV2A GPU, audio, input, file system) and adds a Direct3D 11 back end (`native/src/backend_d3d11.c`).

| Path | Contents |
|---|---|
| `scripts/` | Pipeline driver and lift-time hooks |
| `native/` | CMake project, startup code, the D3D11 back end, project overrides of toolkit files (`native/overrides/`) |
| `native/installer/` | Disc import tool and installer script |
| `native/config/` | `config.exe`, the settings program (keyboard remapper) |
| `tools/xboxrecomp/` | The toolkit, a git submodule pinned to the tested commit |

## Credits and legal

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and contributors (MIT; some runtime parts LGPL-2.1, see its NOTICE). Some files in `native/overrides/` are modified copies of its files (xemu-derived parts are LGPL-2.1).
- NHL Hitz Pro is © Midway. This project is not affiliated with or endorsed by Midway, the NHL or the NHLPA, and distributes none of their content.
