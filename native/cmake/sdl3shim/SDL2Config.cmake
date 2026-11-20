# Stand-in for the SDL2 CMake package when the Linux player is built on SDL3
# (-DHITZ_SDL3=ON). The vendored toolkit calls find_package(SDL2 REQUIRED) and
# links SDL2::SDL2; here that target carries the SDL3 library and the include
# directory whose SDL2/SDL.h maps the SDL2-style calls onto SDL3.
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR};${HITZ_SDL3_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${HITZ_SDL3_LIBRARY}")
endif()
set(SDL2_FOUND TRUE)
set(SDL2_VERSION "2.30.0")
