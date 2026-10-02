# Ghostling - Minimal libghostty Terminal

Ghostling is a demo project meant to highlight a minimum
functional terminal built on the libghostty C API in a
[single C file](https://github.com/ghostty-org/ghostling/blob/main/main.c).

The Linux client uses GTK4/GDK for native Wayland/X11 windowing and Cairo/Pango
for its initial renderer. The authoritative terminal lives in a separate,
persistent `gmux-server` process and communicates with the GUI over a Unix
socket.

> [!WARNING]
>
> The Ghostling terminal isn't meant to be a full featured, daily use
> terminal. It is a minimal viable terminal based on libghostty. Also, since
> this is basically a demo, I didn't carefully audit every single place for
> correctness, and this is C, so you've been warned!

<p align="center">
  <img src="demo.gif" alt="Ghostling Demo" />
</p>

## What is Libghostty?

Libghostty is an embeddable library extracted from [Ghostty's](https://ghostty.org) core,
exposing a C and Zig API so any application can embed correct, fast terminal
emulation.

Ghostling uses **libghostty-vt**, a zero-dependency library (not even libc) that
handles VT sequence parsing, terminal state management (cursor position,
styles, text reflow, scrollback, etc.), and renderer state management. It
contains no renderer drawing or windowing code; the consumer (Ghostling, in
this case) provides its own. The core logic is extracted directly from Ghostty
and inherits all of its real-world benefits: excellent, accurate, and complete
terminal emulation support, SIMD-optimized parsing, leading Unicode support,
highly optimized memory usage, and a robust, fuzzed, and tested codebase, all
proven by millions of daily active users of Ghostty GUI.

## Features

Despite being a minimal, thin layer above libghostty, look at all the
features you _do get_:

- Resize with text reflow
- Full 24-bit color and 256-color palette support
- Bold, italic, and inverse text styles
- Unicode and multi-codepoint grapheme handling (no shaping or layout)
- Keyboard input with modifier support (Shift, Ctrl, Alt, Super)
- Kitty keyboard protocol
- Kitty graphics protocol
- Mouse tracking (X10, normal, button, and any-event modes)
- Mouse reporting formats (SGR, URxvt, UTF8, X10)
- Scroll wheel support (viewport scrollback or forwarded to applications)
- Scrollbar with mouse drag-to-scroll
- Focus reporting (CSI I / CSI O)
- And more. Effectively all the terminal emulation features supported
  by Ghostty!

### What Is Coming

These features aren't properly exposed by libghostty-vt yet but will be:

- OSC clipboard support

These are things that could work but haven't been tested or aren't
implemented in Ghostling itself:

- Windows support (libghostty-vt supports Windows)

This list is incomplete and we'll add things as we find them.

### What You Won't Ever Get

libghostty is focused on core terminal emulation features. As such,
you don't get features that are provided by the GUI above the terminal
emulation layer, such as:

- Tabs
- Multiple windows
- Splits
- Session management
- Configuration file or GUI
- Search UI (although search internals are provided by libghostty-vt)

These are the things that libghostty consumers are expected to implement
on their own, if they want them. This example doesn't implement these
to try to stay as minimal as possible.

### Current input work

There are some known issues with this demo:

- GTK now supplies event-driven key press, repeat, release, modifier, and
  consumed-modifier information. Compose/IME commit handling and the complete
  physical-key mapping are still being ported from Ghostty's GTK frontend.

## Building

Requirements:

- [CMake](https://cmake.org/) 3.19+
- [Ninja](https://ninja-build.org/)
- A C compiler
- `curl`, `sha256sum`, and `tar` for `./build.sh` to fetch Zig 0.16.0
- Linux (Ubuntu/Debian): `sudo apt install -y ninja-build build-essential git libgtk-4-dev libfontconfig-dev`

```sh
./build.sh
./build/gmux-server "$HOME/.gmux.sock"
./build/gmux --connect "$HOME/.gmux.sock"
```
```

For a headless server build on another machine, clone with submodules and run:

```sh
mkdir -p "$HOME/codex_projects"
cd "$HOME/codex_projects"
git clone --recurse-submodules -b gmux-codex git@github.com:mjkpolo/ghostling.git
cd ghostling
./build.sh --server-only
```

The script downloads checksum-verified Zig 0.16.0 into the parent directory of
the checkout when it is absent. The server embeds libghostty-vt and MessagePack,
but uses the build system's glibc and libm dynamically. Build on a glibc 2.34
system to target glibc 2.34.

> [!WARNING]
>
> Debug builds are VERY SLOW since Ghostty included a lot of extra
> safety and correctness checks. Do not benchmark debug builds.

For a release (optimized) build:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

After the initial configuration, you only need to run the build step:

```sh
cmake --build build
```

To clean up the build directory:

```sh
cmake --build build --target clean
```

## Linux release artifacts

- `gmux` and `gmux-server` are built on Rocky Linux 9.4 for a glibc 2.34
  baseline. Download the single `gmux-rocky-linux-9.4-x86_64` artifact and
  copy `gmux-server` to the remote host while keeping `gmux` locally.

Run both binaries directly:

```sh
mkdir -p "$XDG_RUNTIME_DIR/gmux"
./gmux-server "$XDG_RUNTIME_DIR/gmux/gmux.sock"
./gmux --connect "$XDG_RUNTIME_DIR/gmux/gmux.sock"
```

### Client configuration

The GTK client loads `gmux/config` from GLib's user configuration directory.
On Linux this is `$XDG_CONFIG_HOME`, falling back to `~/.config` when that
variable is unset. For example, `~/.config/gmux/config` may contain:

```ini
font = Monaspace Argon Frozen
font-size = 24
```

The font must be available through Fontconfig. `font-size` accepts integer
sizes from 6 through 96. `Ctrl+Shift++` and `Ctrl+Shift+-` adjust the font size
for the running client.

## FAQ

### Why Not Zig?

libghostty-vt has a fully capable and proven Zig API. Ghostty GUI itself
uses this and is a good -- although complex -- example of how to use it.
However, this demo is meant to showcase the minimal C API since C is so
much more broadly used and accessible to a wide variety of developers and
language ecosystems.

### What about Rust or any other language?

libghostty-vt has a C API and can have zero dependencies, so it can be used
with minimally thin bindings in basically any language. I'm not sure yet if
the Ghostty project will maintain official bindings for languages other than C
and Zig, but I hope the community will create and maintain bindings for many
languages!

### Does libghostty require GTK?

**No no no!** libghostty has no opinion about the renderer or GUI framework
used; it's even standalone WASM-compatible for browsers and other environments.

libghostty provides a [high-performance render state API](https://libghostty.tip.ghostty.org/group__render.html)
which only keeps track of the _state_ required to build a renderer. This is the
same API used by Ghostty GUI for Metal and OpenGL rendering and in this repository
for the Cairo/Pango renderer in this repository. You can layer any renderer on
top of this!

### Why CMake and GTK?

I needed to pick _something_. Really, any build system and any library
could be used. CMake is widely used and supported. GTK gives the Linux client
native Wayland input, layout-aware key events, IME integration, clipboard
access, and an event loop close to the one used by Ghostty itself.
