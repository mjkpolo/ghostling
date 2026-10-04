# Ghostty terminfo

`xterm-ghostty.terminfo` is generated without modification from the same
Ghostty revision pinned for libghostty-vt:
[`befcdfd2c3a1cb24d9ec886e93c95b2b5daa7028`](https://github.com/ghostty-org/ghostty/blob/befcdfd2c3a1cb24d9ec886e93c95b2b5daa7028/src/terminfo/ghostty.zig).
It uses that revision's `Source.encode` output. The accompanying
`GHOSTTY-LICENSE` is the upstream MIT license.

Ghostty's default TERM is `xterm-ghostty`; `ghostty` is an alias in the entry.
libghostty-vt does not set a shell's environment: gmux-server does that when
spawning the PTY. It uses `xterm-ghostty` when the private compiled entry is
available, otherwise `xterm-256color`, as before.

When updating the pinned Ghostty dependency, refresh this generated definition
from `src/terminfo/ghostty.zig` using `src/terminfo/Source.zig`'s encoder too.
Do not merely rename another terminal's entry. The manager compiles this source
with `tic -x`; it does not remove previously installed Kitty entries.
