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
if(NHL_RELEASE AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
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
list(REMOVE_ITEM kernel_sources xbox_memory_layout.c nv2a_vsh_interp.c nv2a_combiner.c nv2a_pb_exec.c kernel_file.c kernel_path.c kernel_hal.c)
set_property(TARGET xbox_kernel PROPERTY SOURCES "${kernel_sources}")
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/xbox_memory_layout.c")
# Project-local copy of the NV2A vertex-program interpreter (predecoded slots + RECOMP_VSH_PROFILE).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/nv2a_vsh_interp.c")
# Project-local copy of the register-combiner evaluator (skips unused results; bit-identical, scripts/rc_equiv).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/nv2a_combiner.c")
# Project-local copy of the push-buffer executor (configurable raster pool threshold, RECOMP_RASTER_MT_MIN).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/nv2a_pb_exec.c")
# Project-local copy of kernel_file.c: NtReadFile failure logged with its Win32 error and retried in 64 KB pieces (disc-error screen).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/kernel_file.c")
# Project-local copy of kernel_path.c: no Partition0..5.img files are created (kernel_file.c emulates the partition devices in memory).
target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/kernel_path.c")
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
    src/backend_d3d11.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

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
target_link_libraries(nhl_config PRIVATE comctl32 user32 gdi32)
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
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
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
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
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
