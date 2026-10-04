#!/usr/bin/env bash
set -euo pipefail

./build.sh
./tests/run.sh
./build.sh --server-musl
server=build/server-musl/gmux-server
python3 tests/test_server_lifecycle.py "$server"
# Reject accidentally dynamic artifacts, including static PIE interpreters.
program_headers=$(readelf -l "$server")
dynamic_section=$(readelf -d "$server")
if [[ "$program_headers" == *INTERP* || "$dynamic_section" == *NEEDED* ]]; then
  echo 'Release server must be fully static' >&2
  exit 1
fi

artifact_dir="$PWD/artifacts"
rm -rf "$artifact_dir"
mkdir -p "$artifact_dir"
cp build/gmux "$server" "$artifact_dir/"
cp gmuxctl "$artifact_dir/"
"$server" --version | cut -d ' ' -f 2 > "$artifact_dir/VERSION"
test "$(cat "$artifact_dir/VERSION")" != unknown
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
  file "$server"
  readelf -d "$server"
  printf '\nRequired GLIBC symbol versions:\n'
  readelf --version-info build/gmux "$server" \
    | grep -o 'GLIBC_[0-9.]*' | sort -Vu
} > "$artifact_dir/dependencies.txt"

(
  cd "$artifact_dir"
  sha256sum gmux gmux-server gmuxctl gmux-themes.tar.gz \
    xterm-ghostty.terminfo GHOSTTY-LICENSE VERSION > SHA256SUMS
)
