cmake_minimum_required(VERSION 3.19)
project(ghostling C)

# Generate compile_commands.json for clangd/IDE support.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
option(GMUX_BUILD_CLIENT "Build the GTK client" ON)

# Ghostty's CMake wrapper caches ZIG_EXECUTABLE. When the flake changes the Zig
# package, an existing build tree can otherwise keep using the old store path.
# Resolve Zig from the active environment before adding Ghostty so builds follow
# the flake/dev-shell selection.
find_program(GHOSTLING_ZIG_EXECUTABLE zig REQUIRED NO_CACHE)
set(ZIG_EXECUTABLE "${GHOSTLING_ZIG_EXECUTABLE}" CACHE FILEPATH "Path to the Zig compiler" FORCE)

# --- GTK client --------------------------------------------------------------
if (GMUX_BUILD_CLIENT)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
endif()

# --- libghostty-vt -----------------------------------------------------------
# Virtual terminal emulator library from Ghostty. Requires `zig` on PATH.
# The Ghostty CMakeLists.txt delegates to `zig build lib-vt` to produce the
# shared library. See https://ghostty.org/docs/install/build for the
# required Zig version.
FetchContent_Declare(ghostty
  GIT_REPOSITORY https://github.com/ghostty-org/ghostty.git
  GIT_TAG f64f4aca2c29b554d111b36c3d946a9bddd159ff
)
FetchContent_MakeAvailable(ghostty)

# --- MessagePack -------------------------------------------------------------
# Vendored as a Git submodule so the exact C implementation is visible and
# pinned with the rest of the project.
set(MSGPACK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MSGPACK_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(MSGPACK_ENABLE_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/msgpack-c)

# --- Embed font as a C header ------------------------------------------------
# Convert the TTF font into a C byte-array header so we don't rely on C23
# #embed (which GCC < 15 doesn't support).
if (GMUX_BUILD_CLIENT)
set(FONT_INPUT "${CMAKE_SOURCE_DIR}/fonts/MonaspaceArgonFrozen-Regular.ttf")
set(FONT_HEADER "${CMAKE_BINARY_DIR}/font_monaspace_argon.h")
add_custom_command(
  OUTPUT "${FONT_HEADER}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=${FONT_INPUT}
    -DOUTPUT=${FONT_HEADER}
    -DARRAY_NAME=font_monaspace_argon
    -P ${CMAKE_SOURCE_DIR}/bin2header.cmake
  DEPENDS "${FONT_INPUT}" "${CMAKE_SOURCE_DIR}/bin2header.cmake"
  COMMENT "Generating font_monaspace_argon.h"
)

set(ITALIC_FONT_INPUT "${CMAKE_SOURCE_DIR}/fonts/MonaspaceArgonFrozen-Italic.ttf")
set(ITALIC_FONT_HEADER "${CMAKE_BINARY_DIR}/font_monaspace_argon_italic.h")
add_custom_command(
  OUTPUT "${ITALIC_FONT_HEADER}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=${ITALIC_FONT_INPUT}
    -DOUTPUT=${ITALIC_FONT_HEADER}
    -DARRAY_NAME=font_monaspace_argon_italic
    -P ${CMAKE_SOURCE_DIR}/bin2header.cmake
  DEPENDS "${ITALIC_FONT_INPUT}" "${CMAKE_SOURCE_DIR}/bin2header.cmake"
  COMMENT "Generating font_monaspace_argon_italic.h"
)

# --- Our Project -------------------------------------------------------------
add_executable(gmux gmux_client.c "${FONT_HEADER}" "${ITALIC_FONT_HEADER}")
target_compile_features(gmux PRIVATE c_std_11)
target_compile_definitions(gmux PRIVATE GMUX_CLIENT)
target_include_directories(gmux PRIVATE
  "${CMAKE_BINARY_DIR}"
  "${ghostty_SOURCE_DIR}/include"
)
target_link_libraries(gmux PkgConfig::GTK4 PkgConfig::FONTCONFIG msgpack-c)
endif()

add_executable(gmux-server gmux_server.c server_image.c)
target_compile_features(gmux-server PRIVATE c_std_11)
target_compile_definitions(gmux-server PRIVATE GMUX_SERVER)
target_include_directories(gmux-server PRIVATE
  "${ghostty_SOURCE_DIR}/src/stb"
)
target_link_libraries(gmux-server ghostty-vt-static msgpack-c)

# The headless server uses forkpty().
if (UNIX AND NOT APPLE)
  target_link_libraries(gmux-server util m)
endif()
