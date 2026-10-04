#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

# Use the normal build's pinned headers and static libraries; no test framework
# or dependency download is needed. Run ./build.sh first.
mkdir -p build/tests
cc=${CC:-cc}
flags=(-g -Wall -Wextra -Wno-unused-function)
if [[ ${SANITIZE:-0} == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
includes=(-Ibuild/_deps/ghostty-src/include -Ivendor/msgpack-c/include
          -Ibuild/vendor/msgpack-c/include -Ibuild/vendor/msgpack-c/include/msgpack)

"$cc" "${flags[@]}" tests/test_config.c -o build/tests/config
build/tests/config

"$cc" "${flags[@]}" "${includes[@]}" tests/test_transport.c \
  build/_deps/ghostty-src/zig-out/lib/libghostty-vt.a \
  build/vendor/msgpack-c/libmsgpack-c.a -lutil -lm -o build/tests/transport
build/tests/transport

# pkg-config emits shell words for compiler/linker flags.
read -r -a gtk_flags <<< "$(pkg-config --cflags --libs gtk4)"
"$cc" "${flags[@]}" "${includes[@]}" tests/client_review.c \
  build/vendor/msgpack-c/libmsgpack-c.a "${gtk_flags[@]}" -lm \
  -o build/tests/client
build/tests/client

PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v
python3 tests/test_server_lifecycle.py build/gmux-server
