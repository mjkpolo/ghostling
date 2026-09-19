# Ghostling

## Project Direction and User Preferences

- This is a personal project for fun and learning, with no deadline. Optimize
  collaboration for enjoyment, understanding, and exploration rather than speed
  to delivery. Explain mechanisms and tradeoffs, use discoveries as teaching
  opportunities, and welcome relevant rabbit holes to explore together.
- Whenever Codex materially writes code and creates or amends the corresponding
  commit, ALWAYS include this exact Git commit trailer:
  `Co-authored-by: Codex <noreply@openai.com>`.
- Survey prior art before treating an architectural idea as novel. Study who has
  tried related designs, what they actually transmit, and their tradeoffs. Do not
  assume the idea has never been implemented, or invent reasons for adoption.
- Brainstorm the network representation without requiring another terminal as
  the receiving client: terminal state, structured display updates, drawing
  commands, pixels, and hybrids are open questions, not settled decisions.
- For the current prototype, mirror libghostty's exposed render values as
  directly and field-for-field as possible. Do not create a generalized terminal
  model or “maintainable” abstraction layer. Opaque handles, iterators, borrowed
  pointers, and pointer-bearing structs cannot cross processes; copy their
  pointed-to data into the message while retaining libghostty names and semantics.
- MessagePack is a candidate implementation detail for framing and primitive
  serialization, not a new model. Evaluate its actual C packing/unpacking cost
  before adopting it; it does not automatically serialize arbitrary C structs.
- See `RESEARCH.md` for the initial notes, `NETWORK_MODEL_SURVEY.md` for the
  source-backed protocol and architecture comparison, and `RABBIT_HOLES.md` for
  questions deliberately parked for later. WezTerm is a close existing
  architectural example; Zellij remains the primary reference for multiplexer
  behavior.
- The user relies on terminal multiplexers and prefers Zellij. Use the official
  Zellij source as the primary reference when investigating multiplexer designs
  or implementations: https://github.com/zellij-org/zellij, cloned locally at
  `../zellij` (`/home/ma148697/codex_projects/zellij`).
- Project idea: a persistent, headless terminal backend on a server, using
  libghostty for terminal emulation, with a dedicated desktop client that connects
  to that backend and renders its output. The desktop client is intended to
  connect only to the headless backend, rather than act as a standalone terminal.
- Motivation: avoid the extra terminal-emulation and escape-sequence translation
  layer between a traditional multiplexer and its host terminal, including their
  differing terminfo/capability expectations. Retain the benefits of multiplexing.
- This is currently an architectural direction, not an implemented remote system.
  Protocol, transport, session management, and pane-layout details are undecided.
  Distinguish user requirements from implementation proposals in future work.
- Keep new tools, dependencies, and caches inside `~/codex_projects`. Zig 0.16.0
  is installed at `../zig-0.16.0/zig`; use it rather than the older Zig on PATH.
  Set `ZIG_GLOBAL_CACHE_DIR=/home/ma148697/codex_projects/.cache/zig` and
  `ZIG_LOCAL_CACHE_DIR=/home/ma148697/codex_projects/ghostling/build/.zig-cache`.
- The current prototype embeds Monaspace Argon Frozen Regular and Italic at size
  24 and loads all 953 Unicode characters mapped by these bundled font files.
- The user runs Sway and remaps Caps Lock to Ctrl through XKB. This prototype
  currently uses GLFW's X11 backend through XWayland. The remapping reportedly
  fails in Ghostling; the backend is confirmed, but the cause is not diagnosed.

## Building

- Requires CMake 3.19+, Ninja, a C compiler, and Zig 0.16.x on PATH
- Configure: `cmake -B build -G Ninja`
- Build: `cmake --build build`
- Release build: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
- Run: `./build/ghostling`
- Clean: `cmake --build build --target clean`

## Code Conventions

- C (not C++), single-file project in `main.c`
- Never put side-effect calls inside `assert()` — removed in release builds
- Comment heavily — explain *why*, not just *what*

## Libghostty API Reference

- The main header is `build/_deps/ghostty-src/zig-out/include/ghostty/vt.h`
- These are generated/fetched during the build; run a build first if they
  don't exist

## Updating Libghostty

- Update CMakeLists.txt first to point to the new version
- Clean the build folder immediately to avoid stale libghostty builds
- After cleaning, perform a rebuild to test for any API changes
