# HPC connection and config-editor failure — 2026-10-04

## Result

Reproduced the failure on the `hpc` SSH alias using the released binaries from
commit `c149317`. After fixing the CPU target, the **installed `gmuxctl hpc`**
successfully created a session, attached the released GTK client, opened the
remote config editor, changed its isolated preview from font size 24 to 25,
and cancelled. The server remained alive after the manager exited.

The real host config was not saved or modified. Its SHA-256 before and after was
`95428e41ccdf519845174d4480db2a4537bdfde2362347e38ae27261ad2d7be2`.
Only uniquely named diagnostic sessions were created and later removed.
No pre-existing session was stopped.

## 1. Server startup crashed on an unsupported CPU instruction

SSH authentication, session discovery, and SSH Unix-socket forwarding all worked.
The failing step was spawning the server. The visible error was:

```text
failed to start server on .../gmux-diagnostic-....sock: Input/output error
```

Tracing our own disposable server with `strace -f` showed successful socket
creation/bind/listen, followed immediately by:

```text
--- SIGILL {si_signo=SIGILL, si_code=ILL_ILLOPN, si_addr=0x51e5cb} ---
+++ killed by SIGILL (core dumped) +++
```

Core dumps were disabled on the host; the trace's signal annotation is not a
claim that a core file was saved. The parent received EOF on its readiness pipe
and reported its default `EIO`; the crashed daemon could not unlink its socket.

`addr2line` on the identical cached release binary resolved that address to
`compiler_rt.memcpy.copyFixedLength`, Zig's `compiler_rt/memcpy.zig:170`.
Disassembly showed:

```text
51e5cb: 62 f1 7c 48 10 06    vmovups (%rsi),%zmm0
```

This is an AVX-512 instruction. The tested HPC processor is an AMD EPYC 7V13:
its reported features include AVX2, **not AVX-512**. The release had specialized
libghostty, including its compiler-runtime memory helpers, for the build CPU.
Building inside Rocky constrains glibc compatibility, not CPU instruction sets.

The old [gmux CMake integration](https://github.com/mjkpolo/ghostling/blob/c149317c7b1e84b1ff9ee3af5de0afffd793a827/CMakeLists.txt#L25-L35)
supplied no CPU target. Pinned Ghostty starts with
[`b.standardTargetOptions`](https://github.com/ghostty-org/ghostty/blob/befcdfd2c3a1cb24d9ec886e93c95b2b5daa7028/src/build/Config.zig#L76-L94).
**Fix:** CMake now passes `-Dcpu=baseline` to the existing Ghostty build; no
new runtime detection layer or transport changes were needed.

The config editor hit the same problem because it starts another server for its
preview. This was not evidence of broken SSH forwarding or config serialization.

## 2. The TUI hid the useful remote error

The old [`run_ssh`](https://github.com/mjkpolo/ghostling/blob/c149317c7b1e84b1ff9ee3af5de0afffd793a827/gmuxctl#L189-L193)
captured stderr but raised `CalledProcessError`. Displaying `str(error)` showed
the long SSH argument list, truncated by the terminal width, rather than the
captured startup error. This was reproduced in the actual curses TUI.

**Fix:** checked remote failures now report the host, stderr (or stdout if
stderr is empty), and exit status. `check=False` still returns the result so
health/status probes can inspect expected nonzero statuses. A regression covers
both paths.

## 3. The published version was literally `unknown`

Both the downloaded GUI and installed HPC server printed `unknown`. The
[release build log](https://github.com/mjkpolo/ghostling/actions/runs/37231770594)
at `2026-10-04T20:22:35Z` contains:

```text
fatal: detected dubious ownership in repository at '/__w/ghostling/ghostling'
[  0%] Built target gmux-version
```

The old [version generator](https://github.com/mjkpolo/ghostling/blob/c149317c7b1e84b1ff9ee3af5de0afffd793a827/ci/version.cmake#L1-L7)
silently converted Git's failure into `unknown`, and packaging accepted it.
Thus version comparison could not distinguish such releases. It would also
replace a manually repaired server with the broken release because the valid
installed revision differs from `unknown`.

**Fixes:** use GitHub's `GITHUB_SHA` in CI; retain Git-based dirty revisions for
local builds; reject `unknown` during release packaging; and have gmuxctl skip
automatic replacement when release VERSION is `unknown`. A test generates a
CI header outside any Git repository, and another checks the updater guard.
Until a corrected release is published, the manager prints an update-check
warning but uses the installed working binaries.

## Verification and deployment

- Built a portable test server with baseline CPU and glibc 2.34 targeting.
  Its imported GLIBC versions end at **2.34**; dynamic dependencies are libc
  and libm (plus the ELF loader).
- The audit-only Zig C linker invocation crashed while linking. Linking the
  already compiled objects and static libraries with the host C linker
  succeeded; imported symbol versions were then checked before upload.
  This was a local test-build obstacle, not another observed HPC failure.
- First tested the new server under a private remote diagnostic directory,
  using the **unchanged released GUI**, real SSH forwarding, real curses and
  an isolated local headless Sway display.
- Then atomically replaced `~/.local/bin/gmux-server` on HPC and updated the
  local `~/.local/bin/gmuxctl`. Finally ran that exact installed manager without
  client/server overrides and repeated create, attach, editor, preview, cancel.
- Inspected screenshots showing the remote shell and both preview font sizes.
- Normal local build and complete regression suite passed: three C harnesses,
  **22 Python tests**, and real server lifecycle/config/terminfo checks.
- A fresh Rocky/GitHub release has **not** been built or pushed in this turn.
  Other remote hosts have not been repaired or tested.

Installed HPC server SHA-256:
`8aa0c3312274012402547ac5b59ea8a39a403eb783abd5e638adfd6184e665b4`.
Version: `c149317c7b1e84b1ff9ee3af5de0afffd793a827-dirty`.

Recovery copies:

- HPC: `~/.local/bin/gmux-server.before-hpc-fix-20261004` (the broken release).
- Local: `build/hpc-test/gmuxctl.before-fix`.

Local evidence under ignored `build/hpc-test/`: `startup-before.strace`,
`tui.log`, `attachment.png`, `editor.png`, and `editor-font-larger.png`.
Screenshots contain the host's normal login output and are intentionally not
added to the public repository. Remote diagnostic processes/files and their
stale sockets were removed after testing; configuration and installed themes
were left intact.

## Follow-up: static musl releases

At the user's request, the release server now builds with
`./build.sh --server-musl`. Both the C compiler and libghostty target
`x86_64-linux-musl` with a baseline CPU. The GTK client remains a Rocky/glibc
build. CMake's linker dependency-file option is disabled for the Zig toolchain;
the musl build linked successfully without the earlier manual linker workaround.

`file` reports a statically linked executable; `readelf` shows no interpreter
or dynamic section. Release packaging rejects a server with an interpreter or
shared-library dependencies. The static server passed the lifecycle suite
locally and on HPC: PTY startup, repeated detach/attach, font-size persistence,
live config reload, missing-theme recovery, check, and kill. All normal local
regressions also passed (including 22 Python tests).

The HPC test used a private temporary directory and did not replace the
installed server or modify the real config. This release change is not pushed.
