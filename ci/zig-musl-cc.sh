#!/bin/sh
# Use the pinned Zig on build.sh's PATH for both libc and CPU portability.
exec zig cc -target x86_64-linux-musl -mcpu=baseline "$@"
