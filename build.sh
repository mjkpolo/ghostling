#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
projects_dir=$(dirname -- "$project_dir")
zig_dir="$projects_dir/zig-0.16.0"

if [[ ! -x "$zig_dir/zig" ]]; then
  case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)
      zig_platform=x86_64-linux
      zig_sha=70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
      ;;
    Linux-aarch64)
      zig_platform=aarch64-linux
      zig_sha=ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17
      ;;
    *)
      printf 'Install Zig 0.16.0 at %s for this platform.\n' "$zig_dir" >&2
      exit 1
      ;;
  esac

  zig_archive=$(mktemp "$projects_dir/zig-0.16.0.XXXXXX.tar.xz")
  trap 'rm -f "$zig_archive"' EXIT
  curl -fL --retry 3 -o "$zig_archive" \
    "https://ziglang.org/download/0.16.0/zig-$zig_platform-0.16.0.tar.xz"
  printf '%s  %s\n' "$zig_sha" "$zig_archive" | sha256sum -c -
  tar -xJf "$zig_archive" -C "$projects_dir"
  mv "$projects_dir/zig-$zig_platform-0.16.0" "$zig_dir"
fi

if [[ "$("$zig_dir/zig" version)" != 0.16.0 ]]; then
  printf 'Expected Zig 0.16.0 at %s\n' "$zig_dir/zig" >&2
  exit 1
fi

export PATH="$zig_dir:$PATH"
export ZIG_GLOBAL_CACHE_DIR="$projects_dir/.cache/zig"
export ZIG_LOCAL_CACHE_DIR="$project_dir/build/.zig-cache"

if [[ "${1:-}" == --server-only ]]; then
  cmake -S "$project_dir" -B "$project_dir/build" \
    -DCMAKE_BUILD_TYPE=Release -DGMUX_BUILD_CLIENT=OFF
  cmake --build "$project_dir/build" --target gmux-server
else
  cmake -S "$project_dir" -B "$project_dir/build" \
    -DCMAKE_BUILD_TYPE=Release -DGMUX_BUILD_CLIENT=ON
  cmake --build "$project_dir/build"
fi
