# Linux installer demo

Visual/extraction prototype for the Debian/Ubuntu and Fedora families. No game
code, real texture pack, network requests, or changes to the Linux game checkout.

Build from the Windows project root:

```powershell
python scripts/build_linux_installer_demo.py
```

Output: `native/installer/Output/linux-demo/NHLHitzPro-Linux-Setup-DEMO.run`.
The adjacent README contains Linux launch and package prerequisite commands.
The script requires Python/Tk plus either `7zz` or `7z`. It runs as the user.

The self-extracting shell wrapper unpacks a temporary UI and tiny pinned demo
7z/LZMA2 archive. The wizard actually extracts and verifies that archive, installs
placeholder files to an empty user-selected directory, optionally creates an
application-menu shortcut, and writes a removal manifest. Removal preserves
added or modified payload files. The temporary setup directory is cleaned on exit.

Steps: Welcome, Folder, Your copy of the game (ISO/XISO or folder), background
verification, Options, Ready, Installing, Finish. Application-menu shortcut is
checked by default. Finish offers optional launch and opening the installed
folder. Uses the existing game icon. Demo default is
`~/Games/NHL Hitz Pro Demo`.

`gui(backend)` is the shared wizard; `DemoBackend` owns demo operations.
The real test installer now uses `../linux/production.py` with the same
validate/install/launch contract, its real default folder, and `is_demo=False`.
Demo-only explanatory text and the sample-disc button are conditional. The
standalone DEMO.run still uses only the demo backend; see `../linux/README.md`
for the separate real test installer and its remaining limitations.

`disc.py` already performs real read-only verification of ISO/XISO and extracted
folders using generated metadata from the Windows manifest: all required file
sizes and exact default.xbe SHA-256. It handles bare XISO and known full-disc
partition offsets, bounded directory traversal, invalid paths, cycles, truncated
images, and case-insensitive source lookup. Linux testing recognized the local
USA ISO: 484 files, 2,064,188,924 bytes, exact supported executable hash.

The included synthetic sample ISO contains a stub and has a separate fixture
manifest. It does not pass supported-game validation. Selecting a real source
only verifies it. Demo installation creates empty game_files placeholders and
actually extracts the tiny bundled demo texture archive; it never imports the
user's real game data or downloads the real texture pack.

Validated on Ubuntu 24.04 WSL with Xvfb: packaged shell launch, full GUI flow,
extraction/hash verification, paths with spaces, menu shortcut, textures on/off,
nonempty destination rejection, removal and preservation of user-added files.
Screenshots and smoke results: `analysis/linux-installer-demo`. The revised
smoke also checks source selection/verification, wrong-game rejection and real
ISO read-only validation. The headless smoke entry point remains available.
Fedora execution has not been tested; do not claim cross-distro certification.

Real game-data import, Pro hi-res texture pack download/cache/resume with pinned
SHA-256 and progress, settings-app integration and shortcuts are connected in
the separate real test installer. Further work: bundle a GUI runtime/extractor
if desired, validate target distros, and add upgrade/repair handling. Do not
replace or republish the existing shared texture payload. DEMO.run itself only
exercises its archive format with empty files.
