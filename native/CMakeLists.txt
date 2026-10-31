cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(nhl_hitz_pro_recomp C RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Player release build: cmake -S native -B native/build-release -DNHL_RELEASE=ON
# then build --config Release. No logging, no overlay stats, no RECOMP_* switches,
# data under %LOCALAPPDATA%\NHLHitzPro, no debug info, and the C runtime linked
# statically so a player needs no Visual C++ redistributable.
option(NHL_RELEASE "Player release build (no diagnostics, data in %LOCALAPPDATA%)" OFF)
# 32-bit build (native 32-bit Windows, XP included): cmake -S native -B native/build-x86 -A Win32 -DNHL_X86=ON.
# A 32-bit process cannot hold the guest's 4 GB address space, so guest addresses are translated through a table
# (overrides/xbox_host.h, xbox_host_x86.c). Implies the Direct3D 9 back end.
option(NHL_X86 "32-bit build: guest address translation" OFF)
if(NHL_X86)
    set(NHL_DX9 ON)
endif()
# Windows 7 build: cmake -S native -B native/build-dx9 -DNHL_DX9=ON [-DNHL_RELEASE=ON]. Direct3D 9 back end
# (src/backend_d3d9.c, 1x native), XInput and shell dependencies resolved at run time, exe nhl_hitz_pro_recomp_dx9.
# See DX9-PLAN.md. The default (Direct3D 11) build is unchanged.
option(NHL_DX9 "Windows 7 build: Direct3D 9 back end, 1x native" OFF)
if((NHL_RELEASE OR NHL_X86) AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Windows XP: the 32-bit build links VC-LTL (the C runtime of the OS's own msvcrt.dll instead of the UCRT, which XP does not
# have) and YY-Thunks (stand-ins for the Vista/7/8/10 functions XP lacks, looked up at run time). Both are third-party
# (VC-LTL5: EPL-2.0, YY-Thunks: MIT), downloaded outside the repository: NHL_XP_DIR holds vc-ltl/x (VC-LTL-Binary.7z unpacked)
# and yy-thunks/x (YY-Thunks-Objs.zip unpacked). Set -DNHL_XP=OFF to build the 32-bit exe with the ordinary runtime.
set(NHL_XP_DIR "C:/Recomp/xp-toolchain" CACHE PATH "folder with vc-ltl/x and yy-thunks/x (XP support for the 32-bit build)")
option(NHL_XP "32-bit build that starts on Windows XP (VC-LTL + YY-Thunks)" ON)
set(NHL_XP_ACTIVE OFF)
if(NHL_X86 AND NHL_XP AND EXISTS "${NHL_XP_DIR}/vc-ltl/x/_msvcrt.h" AND EXISTS "${NHL_XP_DIR}/yy-thunks/x/objs/x86/YY_Thunks_for_WinXP.obj")
    set(WindowsTargetPlatformMinVersion "5.1.2600.0")
    include("${NHL_XP_DIR}/vc-ltl/x/VC-LTL helper for cmake.cmake")
    set(NHL_XP_ACTIVE ON)
    message(STATUS "XP support: VC-LTL + YY-Thunks from ${NHL_XP_DIR}")
elseif(NHL_X86 AND NHL_XP)
    message(WARNING "NHL_XP is on but ${NHL_XP_DIR} has no vc-ltl/x or yy-thunks/x: the 32-bit exe will not start on Windows XP")
endif()

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# â”€â”€ xboxrecomp toolkit â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# Project-local runtime diagnostics; preserve the pinned toolkit checkout.
get_target_property(kernel_sources xbox_kernel SOURCES)
list(REMOVE_ITEM kernel_sources xbox_memory_layout.c nv2a_vsh_interp.c nv2a_combiner.c nv2a_pb_exec.c nv2a_pb_scan.c kernel_file.c kernel_path.c kernel_hal.c kernel_bridge.c)
set_property(TARGET xbox_kernel PROPERTY SOURCES "${kernel_sources}")
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/xbox_memory_layout.c")
# Project-local copy of the NV2A vertex-program interpreter (predecoded slots + RECOMP_VSH_PROFILE).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/nv2a_vsh_interp.c")
# Project-local copy of the register-combiner evaluator (skips unused results; bit-identical, scripts/rc_equiv).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/nv2a_combiner.c")
# Project-local copy of the push-buffer executor (configurable raster pool threshold, RECOMP_RASTER_MT_MIN).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/nv2a_pb_exec.c")
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/nv2a_pb_scan.c")
# Project-local copy of kernel_file.c: NtReadFile failure logged with its Win32 error and retried in 64 KB pieces (disc-error screen).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/kernel_file.c")
# Project-local copy of kernel_path.c: no Partition0..5.img files are created (kernel_file.c emulates the partition devices in memory).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/kernel_path.c")
# Project-local copy of kernel_bridge.c: the emulated display rate (vblank) is adjustable (frame rate option).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/kernel_bridge.c")
# Project-local copy of kernel_hal.c: guest clocks that can be paused (ESC quit overlay).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/kernel_hal.c")

get_target_property(apu_sources xbox_apu SOURCES)
list(REMOVE_ITEM apu_sources apu_vp.c apu_xaudio2.c)
set_property(TARGET xbox_apu PROPERTY SOURCES "${apu_sources}")
target_sources(xbox_apu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/apu_vp.c")
# Project-local copies of ohci.c and usb_gamepad.c: controllers plug in and out while the game runs (hot-plug).
get_target_property(usb_sources xbox_usb SOURCES)
list(REMOVE_ITEM usb_sources ohci.c usb_gamepad.c)
set_property(TARGET xbox_usb PROPERTY SOURCES "${usb_sources}")
target_sources(xbox_usb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/ohci.c" "${CMAKE_CURRENT_SOURCE_DIR}/overrides/usb_gamepad.c")
# Project-local copy of keyboard.c: the key map can be changed by the player (config.exe, keymap.ini).
get_target_property(input_sources xbox_input SOURCES)
list(REMOVE_ITEM input_sources keyboard.c)
set_property(TARGET xbox_input PROPERTY SOURCES "${input_sources}")
target_sources(xbox_input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/keyboard.c")
target_include_directories(xbox_input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides")
# Project-local copy of host_time.c: a pausable host clock and a timer thread that holds while paused.
get_target_property(host_time_sources host_time SOURCES)
list(REMOVE_ITEM host_time_sources host_time.c)
set_property(TARGET host_time PROPERTY SOURCES "${host_time_sources}")
target_sources(host_time PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/host_time.c")
# Project-local copy of apu_xaudio2.c: xa2_set_muted, used while paused.
target_sources(xbox_apu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/apu_xaudio2.c")


if(NHL_X86)
    foreach(t IN ITEMS xbox_kernel xbox_nv2a xbox_usb xbox_apu xbox_video xbox_d3d8 xbox_input xbox_dsound host_time platform)
        get_target_property(NHL_T_TYPE ${t} TYPE)
        if(TARGET ${t} AND NOT NHL_T_TYPE STREQUAL "INTERFACE_LIBRARY")
            target_compile_definitions(${t} PRIVATE NHL_X86=1)
            target_include_directories(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides")
        endif()
    endforeach()
    target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/xbox_host_x86.c")
    # the MMIO instruction decoder against the 32-bit CONTEXT (same API; defines MMIO_DECODE_H so the toolkit header is skipped)
    foreach(t IN ITEMS xbox_apu xbox_usb)
        target_compile_options(${t} PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/overrides/mmio_decode_x86.h")
    endforeach()
    # apu_core.c with the contiguous-window lookup going through the translation table
    get_target_property(apu_x86_sources xbox_apu SOURCES)
    list(REMOVE_ITEM apu_x86_sources apu_core.c)
    set_property(TARGET xbox_apu PROPERTY SOURCES "${apu_x86_sources}")
    target_sources(xbox_apu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/apu_core.c")
    target_include_directories(xbox_apu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/xboxrecomp/src/apu" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/xboxrecomp/src/nv2a" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/xboxrecomp/src/kernel")
    get_target_property(nv2a_sources xbox_nv2a SOURCES)
    list(REMOVE_ITEM nv2a_sources nv2a_mmio_hook.c)
    set_property(TARGET xbox_nv2a PROPERTY SOURCES "${nv2a_sources}")
    target_sources(xbox_nv2a PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/nv2a_mmio_hook_x86.c")
    # The generated header turns a guest address into a host address with one constant. Replace that by the table lookup
    # (idempotent; the 64-bit definition stays under #else, so every build still compiles it).
    set(NHL_GEN_TYPES "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/recomp_types.h")
    if(EXISTS "${NHL_GEN_TYPES}")
        file(READ "${NHL_GEN_TYPES}" NHL_TYPES_TXT)
        string(FIND "${NHL_TYPES_TXT}" "NHL_X86" NHL_TYPES_DONE)
        if(NHL_TYPES_DONE EQUAL -1)
            string(REPLACE "#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)"
"#ifdef NHL_X86
extern uintptr_t g_xbox_off_tbl[256];
static inline uintptr_t nhl_xbox_ptr(uint32_t a) { return (uintptr_t)a + g_xbox_off_tbl[a >> 24]; }
#define XBOX_PTR(addr) nhl_xbox_ptr((uint32_t)(addr))
#else
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#endif" NHL_TYPES_TXT "${NHL_TYPES_TXT}")
            file(WRITE "${NHL_GEN_TYPES}" "${NHL_TYPES_TXT}")
            message(STATUS "patched generated recomp_types.h for the 32-bit address table")
        endif()
    endif()
endif()

# Upstream runtime targets force /O2 even in Debug, conflicting with /RTC1.
# Keep their optimization in release configurations and permit Debug builds.
if(MSVC)
    foreach(runtime_target IN ITEMS xbox_input xbox_dsound xbox_d3d8 xbox_usb xbox_kernel)
        get_target_property(runtime_options ${runtime_target} COMPILE_OPTIONS)
        list(REMOVE_ITEM runtime_options /O2)
        list(APPEND runtime_options "$<$<NOT:$<CONFIG:Debug>>:/O2>")
        set_property(TARGET ${runtime_target} PROPERTY COMPILE_OPTIONS ${runtime_options})
    endforeach()
endif()


# â”€â”€ Recompiled game executable â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

if(NHL_DX9)
    list(APPEND GAME_SOURCES src/backend_d3d9.c src/dx9_compat.c)
else()
    list(APPEND GAME_SOURCES src/backend_d3d11.c)
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
    resources/nhl_hitz_pro.rc      # exe icon and version info
)

# Disc import tool used by the installer (ISO / XISO / folder -> game_files).
add_executable(nhl_import installer/nhl_import.c)
target_link_libraries(nhl_import PRIVATE bcrypt shell32)
set_property(TARGET nhl_import PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# config.exe: the player's settings program, installed next to the game (keyboard remapper for now).
add_executable(nhl_config WIN32 config/config.c resources/config.rc)
target_link_libraries(nhl_config PRIVATE comctl32 user32 gdi32 shell32)
set_target_properties(nhl_config PROPERTIES OUTPUT_NAME config)
set_property(TARGET nhl_config PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src/kernel          # nv2a_backend.h (D3D11 back end)
)

# Windows SDK libraries needed at link time
if(NHL_DX9)
    # d3dcompiler and XInput are loaded at run time (dx9_compat.c, backend_d3d9.c): Windows 7 has no
    # d3dcompiler_47.dll or xinput1_4.dll by default.
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d9 ole32 uuid dxguid winmm)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NHL_DX9=1 _WIN32_WINNT=0x0601 WINVER=0x0601)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides")
    if(NHL_X86)
        if(NHL_XP_ACTIVE)
            target_link_libraries(${PROJECT_NAME} PRIVATE "${NHL_XP_DIR}/yy-thunks/x/objs/x86/YY_Thunks_for_WinXP.obj")
            target_link_options(${PROJECT_NAME} PRIVATE "/SUBSYSTEM:WINDOWS,5.01")
            # Linked in by toolkit code this project never runs (the D3D11 framebuffer presenter, the D3D8 shader compiler,
            # the host movie/WMA decoders that need RECOMP_FMV_HOST): none of these DLLs exist on XP, so load them on first call.
            target_link_libraries(${PROJECT_NAME} PRIVATE delayimp)
            target_link_options(${PROJECT_NAME} PRIVATE "/DELAYLOAD:d3d11.dll" "/DELAYLOAD:d3dcompiler_47.dll"
                                "/DELAYLOAD:mfreadwrite.dll" "/DELAYLOAD:mfplat.dll" "/DELAYLOAD:dxgi.dll")
        endif()
        target_compile_definitions(${PROJECT_NAME} PRIVATE NHL_X86=1)
        target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides")
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME nhl_hitz_pro_recomp_dx9)
else()
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    windowscodecs
    ole32
    uuid
    d3dcompiler
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

if(NHL_RELEASE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NHL_RELEASE=1)
    # kernel_thunks.c override: no xbox_kernel.log file (the stock one opens it in the cwd).
    get_target_property(kernel_sources2 xbox_kernel SOURCES)
    list(REMOVE_ITEM kernel_sources2 kernel_thunks.c)
    set_property(TARGET xbox_kernel PROPERTY SOURCES "${kernel_sources2}")
    target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/kernel_thunks.c")
    target_compile_definitions(xbox_kernel PRIVATE NHL_RELEASE=1)
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:NONE)
    endif()
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    if(NOT NHL_X86)
        target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    endif()
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP4)
endif()

# â”€â”€ Game-specific additions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
