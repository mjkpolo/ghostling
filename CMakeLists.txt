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
endif()

# --- libghostty-vt -----------------------------------------------------------
# Virtual terminal emulator library from Ghostty. Requires `zig` on PATH.
# The Ghostty CMakeLists.txt delegates to `zig build lib-vt` to produce the
# static and shared libraries. We link the static target below. See
# https://ghostty.org/docs/install/build for the
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

# --- Our Project -------------------------------------------------------------
# Refresh on every build, but rewrite the header only when the revision changes.
add_custom_target(gmux-version
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR}
    -DOUTPUT=${CMAKE_BINARY_DIR}/gmux_version.h
    -P ${CMAKE_SOURCE_DIR}/ci/version.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/gmux_version.h)
if (GMUX_BUILD_CLIENT)
  add_executable(gmux gmux_client.c)
  target_compile_features(gmux PRIVATE c_std_11)
  target_compile_definitions(gmux PRIVATE GMUX_CLIENT)
  target_include_directories(gmux PRIVATE
    "${ghostty_SOURCE_DIR}/include"
  )
  target_link_libraries(gmux PkgConfig::GTK4 msgpack-c m)
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
foreach(binary gmux gmux-server)
  if(TARGET ${binary})
    add_dependencies(${binary} gmux-version)
    target_compile_definitions(${binary} PRIVATE GMUX_VERSION_HEADER)
    target_include_directories(${binary} PRIVATE ${CMAKE_BINARY_DIR})
  endif()
endforeach()

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
  install(FILES terminfo/xterm-ghostty.terminfo terminfo/GHOSTTY-LICENSE
          DESTINATION share/gmux/terminfo)
endif()
