#!/usr/bin/env bash
set -euo pipefail

./build.sh --server-only

artifact_dir="$PWD/artifacts"
rm -rf "$artifact_dir"
mkdir -p "$artifact_dir"
cp build/gmux-server "$artifact_dir/"

{
  cat /etc/rocky-release
  printf '\ngmux-server:\n'
  file build/gmux-server
  ldd build/gmux-server
  printf '\nRequired GLIBC symbol versions:\n'
  readelf --version-info build/gmux-server \
    | grep -o 'GLIBC_[0-9.]*' | sort -Vu
} > "$artifact_dir/dependencies.txt"
