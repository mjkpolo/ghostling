set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-musl-cc.sh")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
# Avoid the Zig 0.16 linker crash observed with CMake's dependency-file option.
set(CMAKE_LINK_DEPENDS_USE_LINKER FALSE)
set(GHOSTTY_ZIG_BUILD_FLAGS "-Dtarget=x86_64-linux-musl" CACHE STRING "")
