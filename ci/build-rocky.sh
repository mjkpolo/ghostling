#!/usr/bin/env bash
set -euo pipefail

./build.sh
./tests/run.sh

artifact_dir="$PWD/artifacts"
rm -rf "$artifact_dir"
mkdir -p "$artifact_dir"
cp build/gmux build/gmux-server "$artifact_dir/"
cp gmuxctl "$artifact_dir/"
build/gmux-server --version | cut -d ' ' -f 2 > "$artifact_dir/VERSION"
test "$(build/gmux --version | cut -d ' ' -f 2)" = "$(cat "$artifact_dir/VERSION")"
cp -a build/themes "$artifact_dir/"
cp -a terminfo "$artifact_dir/"
cp terminfo/xterm-ghostty.terminfo terminfo/GHOSTTY-LICENSE "$artifact_dir/"
tar -C "$artifact_dir" -czf "$artifact_dir/gmux-themes.tar.gz" themes

{
  cat /etc/rocky-release
  printf '\ngmux:\n'
  file build/gmux
  ldd build/gmux
  printf '\ngmux-server:\n'
  file build/gmux-server
  ldd build/gmux-server
  printf '\nRequired GLIBC symbol versions:\n'
  readelf --version-info build/gmux build/gmux-server \
    | grep -o 'GLIBC_[0-9.]*' | sort -Vu
} > "$artifact_dir/dependencies.txt"

(
  cd "$artifact_dir"
  sha256sum gmux gmux-server gmuxctl gmux-themes.tar.gz \
    xterm-ghostty.terminfo GHOSTTY-LICENSE VERSION > SHA256SUMS
)
