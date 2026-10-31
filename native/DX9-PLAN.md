# DX9 build for Windows 7 and newer (plan, not part of any release yet)

Goal: a separate exe (`nhl_hitz_pro_recomp_dx9.exe`) that runs on Windows 7 SP1 and newer with a Direct3D 9 renderer.
Scope (David, 2026-10-30): keep it simple, **1x native only**. No Render Scale, Upscaling, Frame Generation, MSAA,
anisotropic filtering or hi-res texture pack. Not in the next release; David says when it ships.

## What the D3D11 build uses that Windows 7 does not have
| Piece | Today | DX9 build |
|---|---|---|
| Renderer | `src/backend_d3d11.c` (4,300 lines), SM4/5 HLSL | new `src/backend_d3d9.c`, SM3 HLSL |
| Shader compiler | `D3DCOMPILER_47.dll` (not in Windows 7 without an update) | ship `d3dcompiler_47.dll` next to the exe (redistributable) |
| Audio | `XAudio2Create` (inline loader finds xaudio2_9/2_8; Windows 7 has neither) | new override `apu_win7.c` on `waveOut` or WASAPI |
| Controller | `XINPUT1_4.dll` (Windows 8+) | link `xinput9_1_0` (same API, Windows 7) |
| Video (XMV intro) | Media Foundation (`MFPlat`, `MFReadWrite`): present on Windows 7 | check the MF calls used exist on 7 |
| CRT | static (`MultiThreaded`) | same; set `_WIN32_WINNT=0x0601` |
| Installer | `MinVersion=10.0` | a DX9 installer variant with `MinVersion=6.1sp1`; the pack page's download needs TLS 1.2 (Windows 7 needs an update), so the DX9 installer should not download |

Windows 7 compatibility of the rest of `main.c` and the kernel/runtime (newer-only APIs such as `GetDpiForWindow`,
`SetThreadDescription`, `GetSystemTimePreciseAsFileTime`) has to be checked with a dumpbin import scan and runtime loading
fallbacks. The current exe's direct imports contain none of those three (checked 2026-10-30), only the DLLs above.

## The renderer is the big job
`nv2a_backend.h` (clear, draw, flip + blit + the batch extension in `nv2a_pbx.h`) is small. `backend_d3d11.c` is large because
it also does: register-combiner pixel shaders per program (`d3d11_rc.hlsl`), GPU vertex programs (`d3d11_gvs.hlsl`, opt-in),
render targets that are also sampled as textures, texture decode + cache, MSAA/scale/upscale/frame generation, the F1 and Esc
overlays, window/fullscreen/aspect handling, pause, shader cache. A DX9 renderer needs only the 1x subset:

1. window, device, back buffer, 4:3 letterbox, fullscreen toggle, vsync, F1 (Stretch, Fullscreen, VSync) and Esc overlays (reuse the GDI overlay code)
2. render target at 640x480, clear, flip, render-to-texture blit
3. state mapping (blend, depth, stencil, alpha test, cull, colour mask) to `SetRenderState`
4. textures: decode to BGRA on the CPU (`nv2a_backend_decode_texture`), upload to `D3DPOOL_MANAGED`, cache by hash as today
5. draw: pre-transformed vertices (`D3DFVF_XYZRHW`), then the register-combiner path
6. register combiners: **SM3 has no bit operations**. fxc rejects `>>`/`&` on `ps_3_0` even on constants (error X3535,
   checked). The current specialised shader decodes the combiner words in HLSL. The DX9 build must decode them in C
   and emit plain float HLSL per program (a code generator), one `ps_3_0` per distinct combiner program (about 20 per match).
7. GPU vertex programs (`vs_3_0`: 256 constants, 512 instructions, 32 temps: enough for NV2A programs): optional; without them
   vertex programs run on the CPU, which costs frame rate. Plan: first CPU vertex programs, then GPU vertex shaders as a second step.

Audio and input are small by comparison; the register-combiner generator and the draw/state/RT code are most of the work.

## Build layout
Share the 66 MB of generated game code between the two exes (an OBJECT library) so the second exe adds only the backend,
audio and link settings. `backend_d3d9.c` exports the same `backend_d3d11_register()` entry that `main.c` calls, so
`main.c` stays shared. The D3D11 target must not change behaviour.

## Milestones
- M0 platform: dumpbin import check, XInput 9.1.0, waveOut/WASAPI audio, d3dcompiler bundling, second exe target that builds.
- M1 skeleton: window + D3D9 device + clear + flip, game runs to the title with a black or flat image (no crash).
- M2 fixed-function path: textures, state, pre-transformed triangles, render-to-texture; menus readable.
- M3 register combiners (generator) and GPU/CPU vertex programs; matches look right (compare against the D3D11 build frames).
- M4 overlays, fullscreen, controller, installer variant; test on a real Windows 7 PC or VM (none available here: needs David's help).

Testing note: this PC is Windows 11. D3D9 runs here, so rendering can be compared against D3D11 frames by the same
scripted runs. Real Windows 7 behaviour (audio, XInput, d3dcompiler, MF video) must be tested on a Windows 7 machine.

## Status (2026-10-30)
Build: `cmake -S native -B native/build-dx9 -DNHL_DX9=ON` (add `-DNHL_RELEASE=ON` for the static-CRT player build), target
`nhl_hitz_pro_recomp` -> `nhl_hitz_pro_recomp_dx9.exe`. Test run: `analysis\dx9test.ps1 -Seconds 300 -Every 450 -Fixed`
(frames in `analysis\slots\dx9rames`, contact sheet `analysis\sheet.py`).

Done and checked on this Windows 11 PC (dev build): window, D3D9 device, 1x render targets, render-to-texture, blit, depth
(shared per zeta), blend/depth/stencil state, texture cache, register-combiner pixel shaders generated per program
(`gen_rc_ps`, 23 programs in a match, 0 failures), stage-0 fallback shader, alpha test in the shader, F1 and Esc overlays
(Fullscreen, Stretch, VSync, Frame Rate), 4:3 window. Intro, menus, loading screens, arena entrance and a played match all
render correctly. Draws must sit between BeginScene/EndScene (clears do not).
Platform: XInput resolved at run time (`dx9_compat.c`), audio falls back to waveOut when XAudio2 is missing
(`RECOMP_AUDIO_WAVEOUT=1` forces it), shader compiler loaded at run time (d3dcompiler_47.dll beside the exe, else _43).
Import scan of the exe: nothing newer than Windows 7 (d3d11.dll and D3DCOMPILER_47.dll still imported by a toolkit library:
bundle d3dcompiler_47.dll; d3d11.dll exists on Windows 7 SP1). The dev build uses the dynamic CRT: the player build must
use `-DNHL_RELEASE=ON` (static) so Windows 7 needs no Visual C++ redistributable.

Open: vertex programs run on the CPU (no GPU vertex shader port yet): slow in a match. No depth-as-texture (entrance
depth of field). Not yet tested on a real Windows 7 PC. No installer variant, config.exe still shows the D3D11 options.

## Update 2026-10-30 (GPU vertex programs)
src/d3d9_gvs.inc: vertex programs run on the GPU as generated vs_3_0 (the body is nv2a_vsh_gen_hlsl's, unchanged). SM3 has no byte buffers or
integer ops, so the CPU decodes each draw's distinct vertices into float4 streams (per attribute, into cacheable memory) and draws indexed
(16-bit). Programs compile on a worker thread and get their shader object on the graphics thread (gvs_pump). On this PC (dev build, scripted
match): 21 programs, 0 failures, picture identical to the CPU path, match about 37 fps (CPU vertex programs: about 12). What is left is mostly
Direct3D 9 call overhead per draw (about 12 us, 2,000-3,000 draws a frame). Pitfall: the vertex-constant cache must be invalidated by every
other writer of c0 (the CPU path writes c0).
Tool for XP PCs: native/tools/dx9debug (no C runtime, runs on XP SP2 and newer; package in C:\Recomp\local-build\dx9debug).

## x86 (32-bit) experiment, 2026-10-30
Configured native/build-x86 (-A Win32 -DNHL_DX9=ON); the first build stops in the toolkit, and reading why shows the real size of the job:
1. Hardware register access is trapped by faults and decoded from the 64-bit CONTEXT (Rip/Rax..R15 in mmio_decode.h, nv2a_mmio_hook.c,
   apu_mmio_hook.c, overrides/ohci.c): needs an x86 CONTEXT version (Eip/Eax..Edi) - mechanical.
2. The memory model needs 4 GB of address space: guest address = host address + offset (generated XBOX_PTR in recomp_types.h), RAM plus
   28 mirrors of 64 MB fill 0x00000000-0x7FFFFFFF, plus a tiled alias at 0x80000000 (render targets live there, e.g. 0x809AC000),
   NV2A registers at 0xFD000000, APU/USB at 0xFE800000/0xFED00000, ROM at 0xFF000000. A 32-bit process on 64-bit Windows with
   /LARGEADDRESSAWARE could hold that; a native 32-bit Windows (XP/7 32-bit, user space 2 or 3 GB) cannot.
   Native 32-bit needs address translation: XBOX_PTR via a table indexed by the top byte (RAM mirrors reduce to a mask), MMIO and the
   alias as low-address no-access/aliased regions, the fault handler mapping a host address back to the guest address, and the ~100 sites
   that add xbox_GetMemoryOffset() to a guest address (nv2a_pb_exec, ohci, fb_present, backends) moved to a translate helper.
Cost estimate: an extra load and add on every guest memory access (CPU-bound game: measure before committing to it).

## x86 milestone, 2026-10-30 (later)
`cmake -S native -B native/build-x86 -A Win32 -DNHL_X86=ON` builds `nhl_hitz_pro_recomp_dx9.exe` as a 32-bit program WITHOUT large-address-aware (2 GB).
Done: per-16-MB-piece guest address table (overrides/xbox_host.h, xbox_host_x86.c; the generated recomp_types.h XBOX_PTR is patched by CMake
at configure time, idempotent), layout init x86 branches (contiguous window anywhere then registered, tiled = alias, NV2A/MCPX/flash as reserved
pieces, RAM mirrors as table entries), 32-bit MMIO decoder (overrides/mmio_decode_x86.h, forced into xbox_apu/xbox_usb), NV2A hook stub,
apu_core.c override, guest->host sweep (nv2a_pb_exec, pb_scan, gvs_exec.inc, ohci, kernel_bridge, thunks, recomp_manual, main, backend_d3d9) using XBOX_HOST.
Result (analysis\dx9test.ps1 -X86 -Fixed): boots, USB and audio traps work, intro to a played match render correctly, no crash in 150 s;
about 38 fps average (64-bit D3D9 build: 44) on this PC under WOW64.
Not done: the exe uses the dynamic CRT (api-ms-win-crt-*, FlsAlloc: 32-bit XP has neither); Vista+ APIs it imports (SRW locks, condition
variables, InitOnce, GetTickCount64, CreateWaitableTimerEx, GetFinalPathNameByHandleW, SetFileInformationByHandle, TrySubmitThreadpoolCallback)
need shims; Media Foundation movies; XInput 9.1.0/1_3 fallback is in; testing on a real 32-bit Windows.

## XP toolchain, 2026-10-30 (late)
32-bit exe for Windows XP: `cmake -S native -B native/build-x86xp -A Win32 -DNHL_X86=ON` (add -DNHL_RELEASE=ON for a player build). CMake pulls in
VC-LTL5 5.3.1 (EPL-2.0; the C runtime is the OS's msvcrt.dll) and YY-Thunks 1.2.3 (MIT; run-time stand-ins for Vista+ functions), both unpacked
under C:/Recomp/xp-toolchain (NHL_XP_DIR; vc-ltl/x and yy-thunks/x; downloads from github.com/Chuyu-Team, not in the repo).
Result: subsystem 5.01, /MT; analysis\xp_imports.py checks every static import against YY-Thunks' list of XP exports: all exist on XP;
d3d11, d3dcompiler_47 and mfreadwrite are delay-loaded (toolkit code this project never calls). LOCALAPPDATA is set from the shell when
missing (XP has none); d3dcompiler_43 is preferred on XP and load errors are silent. Dev builds started without a console log to
native\hitz_log.txt. Test package: C:\Recomp\local-build\xp-test (README inside). Not yet run on a real XP machine.
If EPL-2.0 code is distributed in a release, mention VC-LTL in the credits/notices.
