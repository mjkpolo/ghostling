#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
projects_dir=$(dirname -- "$project_dir")

export PATH="$projects_dir/zig-0.16.0:$PATH"
export ZIG_GLOBAL_CACHE_DIR="$projects_dir/.cache/zig"
export ZIG_LOCAL_CACHE_DIR="$project_dir/build/.zig-cache"

cmake -S "$project_dir" -B "$project_dir/build"
cmake --build "$project_dir/build"
