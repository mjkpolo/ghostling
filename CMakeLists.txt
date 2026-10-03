cmake_minimum_required(VERSION 3.19)
project(ghostling C)

# Generate compile_commands.json for clangd/IDE support.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
option(GMUX_BUILD_CLIENT "Build the GTK client" ON)
option(GMUX_BUILD_SERVER "Build the headless libghostty server" ON)
set(GMUX_GHOSTTY_SOURCE_DIR "" CACHE PATH
    "Existing Ghostty source tree for a client-only build")

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
if (GMUX_BUILD_SERVER)
  # Ghostty's CMake wrapper caches ZIG_EXECUTABLE. Resolve Zig from the active
  # environment so builds follow build.sh's pinned toolchain.
  find_program(GHOSTLING_ZIG_EXECUTABLE zig REQUIRED NO_CACHE)
  set(ZIG_EXECUTABLE "${GHOSTLING_ZIG_EXECUTABLE}"
      CACHE FILEPATH "Path to the Zig compiler" FORCE)
  FetchContent_Declare(ghostty
    GIT_REPOSITORY https://github.com/ghostty-org/ghostty.git
    GIT_TAG befcdfd2c3a1cb24d9ec886e93c95b2b5daa7028
  )
  FetchContent_MakeAvailable(ghostty)
elseif (GMUX_BUILD_CLIENT)
  if (NOT GMUX_GHOSTTY_SOURCE_DIR)
    message(FATAL_ERROR
      "A client-only build requires -DGMUX_GHOSTTY_SOURCE_DIR=/path/to/ghostty")
  endif()
  set(ghostty_SOURCE_DIR "${GMUX_GHOSTTY_SOURCE_DIR}")
endif()

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
function(embed_font output filename array_name)
  add_custom_command(
    OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND}
      -DINPUT=${CMAKE_SOURCE_DIR}/fonts/${filename}
      -DOUTPUT=${output}
      -DARRAY_NAME=${array_name}
      -P ${CMAKE_SOURCE_DIR}/bin2header.cmake
    DEPENDS "${CMAKE_SOURCE_DIR}/fonts/${filename}"
            "${CMAKE_SOURCE_DIR}/bin2header.cmake"
    COMMENT "Generating ${filename} header"
  )
endfunction()

set(FONT_HEADER "${CMAKE_BINARY_DIR}/font_monaspace_argon.h")
set(ITALIC_FONT_HEADER "${CMAKE_BINARY_DIR}/font_monaspace_argon_italic.h")
set(BOLD_FONT_HEADER "${CMAKE_BINARY_DIR}/font_monaspace_argon_bold.h")
set(BOLD_ITALIC_FONT_HEADER
    "${CMAKE_BINARY_DIR}/font_monaspace_argon_bold_italic.h")
embed_font("${FONT_HEADER}" "MonaspaceArgonFrozen-Regular.ttf"
           font_monaspace_argon)
embed_font("${ITALIC_FONT_HEADER}" "MonaspaceArgonFrozen-Italic.ttf"
           font_monaspace_argon_italic)
embed_font("${BOLD_FONT_HEADER}" "MonaspaceArgonFrozen-Bold.ttf"
           font_monaspace_argon_bold)
embed_font("${BOLD_ITALIC_FONT_HEADER}" "MonaspaceArgonFrozen-BoldItalic.ttf"
           font_monaspace_argon_bold_italic)

# --- Our Project -------------------------------------------------------------
add_executable(gmux gmux_client.c "${FONT_HEADER}" "${ITALIC_FONT_HEADER}"
                    "${BOLD_FONT_HEADER}" "${BOLD_ITALIC_FONT_HEADER}")
target_compile_features(gmux PRIVATE c_std_11)
target_compile_definitions(gmux PRIVATE GMUX_CLIENT)
target_include_directories(gmux PRIVATE
  "${CMAKE_BINARY_DIR}"
  "${ghostty_SOURCE_DIR}/include"
)
target_link_libraries(gmux PkgConfig::GTK4 PkgConfig::FONTCONFIG msgpack-c m)
endif()

if (GMUX_BUILD_SERVER)
  add_executable(gmux-server gmux_server.c server_image.c)
  target_compile_features(gmux-server PRIVATE c_std_11)
  target_compile_definitions(gmux-server PRIVATE GMUX_SERVER)
  target_include_directories(gmux-server PRIVATE
    "${ghostty_SOURCE_DIR}/src/stb"
  )
  target_link_libraries(gmux-server ghostty-vt-static msgpack-c)
  add_custom_command(TARGET gmux-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/vendor/iterm2-color-schemes/ghostty"
      "$<TARGET_FILE_DIR:gmux-server>/themes"
    COMMAND ${CMAKE_COMMAND} -E copy
      "${CMAKE_SOURCE_DIR}/vendor/iterm2-color-schemes/LICENSE"
      "$<TARGET_FILE_DIR:gmux-server>/themes/LICENSE"
    COMMENT "Copying Ghostty color themes"
  )
endif()

# The headless server uses forkpty().
if (GMUX_BUILD_SERVER AND UNIX AND NOT APPLE)
  target_link_libraries(gmux-server util m)
endif()

if (GMUX_BUILD_CLIENT)
  install(TARGETS gmux RUNTIME DESTINATION bin)
  install(PROGRAMS gmuxctl DESTINATION bin)
endif()
if (GMUX_BUILD_SERVER)
  install(TARGETS gmux-server RUNTIME DESTINATION bin)
  install(DIRECTORY vendor/iterm2-color-schemes/ghostty/
          DESTINATION share/gmux/themes)
  install(FILES vendor/iterm2-color-schemes/LICENSE
          DESTINATION share/gmux/themes)
  install(FILES terminfo/xterm-kitty.terminfo terminfo/KITTY-LICENSE
          DESTINATION share/gmux/terminfo)
endif()
