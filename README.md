# gmux — a small remote terminal built from Ghostling

This is a learning project based on
[Ghostling](https://github.com/ghostty-org/ghostling), a minimal terminal using
libghostty's C API.

The Linux client uses GTK4/GDK for native Wayland/X11 windowing and Cairo/Pango
for its initial renderer. The authoritative terminal lives in a separate,
persistent `gmux-server` process and communicates with the GUI over a Unix
socket.

> [!WARNING]
>
> This is an experimental terminal, not a replacement for a mature terminal's
> compatibility and security testing.

<p align="center">
  <img src="demo.gif" alt="Ghostling Demo" />
</p>

## What is Libghostty?

Libghostty is an embeddable library extracted from [Ghostty's](https://ghostty.org) core,
exposing a C and Zig API so any application can embed correct, fast terminal
emulation.

The server uses **libghostty-vt** for VT parsing, cursor and cell state,
styles, text reflow, scrollback, and render-state snapshots. It contains no
windowing or drawing code. Our GTK client draws the resulting rows and images;
it does not parse the application's VT output again.

## Features

Despite being a minimal, thin layer above libghostty, look at all the
features you _do get_:

- Resize with text reflow
- Full 24-bit color and 256-color palette support
- Bold, italic, and inverse text styles
- Unicode text, Pango/HarfBuzz shaping, and ligatures
- Keyboard input with modifier support (Shift, Ctrl, Alt, Super)
- Kitty keyboard protocol
- Kitty graphics protocol
- Mouse tracking (X10, normal, button, and any-event modes)
- Mouse reporting formats (SGR, URxvt, UTF8, X10)
- Scroll wheel support (viewport scrollback or forwarded to applications)
- Scrollbar position indicator
- Focus reporting (CSI I / CSI O)
- Local text selection and clipboard shortcuts
- OSC 52 clipboard writes
- Configurable installed fonts and server-owned color themes
- SSH session management through `gmuxctl`

### Limitations

There are some known issues with this demo:

- GTK now supplies event-driven key press, repeat, release, modifier, and
  consumed-modifier information. Compose/IME commit handling and the complete
  physical-key mapping are still being ported from Ghostty's GTK frontend.
- The current client targets Linux. There are no tabs, splits, or search UI.
- A server accepts one attached GUI at a time; open separate sessions for
  simultaneous windows.
- Double-forking detaches from a shell, but cannot override a host policy that
  terminates processes at logout. Persistence depends on the host's session
  management; the SSH transport alone does not guarantee it.

## Building

Requirements:

- [CMake](https://cmake.org/) 3.19+
- [Ninja](https://ninja-build.org/)
- A C compiler
- `curl`, `sha256sum`, and `tar` for `./build.sh` to fetch Zig 0.16.0
- Linux (Ubuntu/Debian): `sudo apt install -y ninja-build build-essential git libgtk-4-dev`

```sh
./build.sh
install -d -m 700 "$XDG_RUNTIME_DIR/gmux"
./build/gmux-server "$XDG_RUNTIME_DIR/gmux/default.sock"
./build/gmux --connect "$XDG_RUNTIME_DIR/gmux/default.sock"
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
but normal local builds use the build system's libc dynamically. For the
portable x86-64 Linux release server, run `./build.sh --server-musl` instead.
The output is `build/server-musl/gmux-server`, statically linked with musl
and compiled for baseline x86-64 CPUs. It needs no installed libc or GTK.

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

### Regression tests

After building both binaries:

```sh
./tests/run.sh
# Optional memory/undefined-behavior checks for our C code:
SANITIZE=1 ./tests/run.sh
```

The tests exercise queued socket/PTY writes, fragmented messages and EOF,
malformed input, long graphemes, RGB/RGBA Kitty images, row-cache resizing, local shortcuts, config
creation, session-manager behavior, and health/kill commands while attached.
They use private temporary sockets and do not connect to a remote host. The
vendored libraries are the normal build's libraries, not sanitizer rebuilds.

## Linux releases

- Every push to `gmux-codex` publishes a GitHub release containing `gmux`,
  `gmux-server`, `gmuxctl`, themes, terminfo, and `SHA256SUMS`. The binaries
  use Rocky Linux 9.4 for the GTK client's glibc 2.34 baseline; the server
  is fully static musl with baseline x86-64 CPU instructions. CI checks that
  the server has no dynamic loader or shared-library dependencies and runs
  its lifecycle tests. The Actions run also
  retains the combined `gmux-rocky-linux-9.4-x86_64` artifact.

Run both binaries directly:

```sh
mkdir -p "$XDG_RUNTIME_DIR/gmux"
./gmux-server "$XDG_RUNTIME_DIR/gmux/gmux.sock"
./gmux --connect "$XDG_RUNTIME_DIR/gmux/gmux.sock"
```

Or let the session manager provision `xterm-ghostty`, create secure socket
directories, detect stale sessions, and manage SSH Unix-socket forwarding:

```sh
./gmuxctl my-ssh-host
```

`gmuxctl` is a standalone Python script. It first looks beside itself and on
`PATH`, then in its download cache; if `gmux` is missing, it downloads and
SHA-256 verifies the latest Rocky release into `$XDG_CACHE_HOME/gmux` (or
`~/.cache/gmux`). Version checks reuse current binaries and update outdated ones.
If the remote
host lacks `gmux-server`, it asks before downloading the verified server and
copying it to `~/.local/bin` or another directory you choose. An explicit
local build can be selected with `--client` or `--server-binary`.

The session list remains open when you attach: each Enter launches another
GTK window in the background, so different sessions on the same host can be
open at once. One private OpenSSH ControlMaster connection serves the manager
and all of its windows. It uses your SSH host, authentication, and jump-host
configuration, while excluding configured port forwards. Each attachment gets
its own private directory and Unix-socket forward; existing sockets are never
reused or replaced. Closing a window removes its forward. Quitting the manager
closes its windows and SSH connection; the remote sessions remain subject to
the remote host's normal session-lifetime policy.

The manager uses `$GMUX_SOCKET_DIR`, then `$XDG_RUNTIME_DIR/gmux`, and finally
`/tmp/gmux-$UID`. It requires the directory to be owned by the current user
with mode `0700`; session sockets use mode `0600`. Remote `xterm-ghostty`
terminfo is installed privately under the user's data directory. No root
access is needed for this installation. The bundled definition comes from the
same pinned Ghostty revision as libghostty-vt (see [terminfo provenance](terminfo/README.md)).
New sessions use `TERM=xterm-ghostty` once provisioned; without that entry they
fall back to `xterm-256color`. Existing sessions keep the environment they started with.

### Configuration

Only the server loads `$XDG_CONFIG_HOME/gmux/config`, falling back to
`~/.config/gmux/config` when that variable is unset or empty. The client never
reads or creates a config file. For example, on the server:

```ini
font = Monaspace Argon Frozen, monospace
font-size = 24
theme = Catppuccin Frappe
```

The server creates this directory and a commented starter configuration on
first run. Existing configuration files are never overwritten.

Fonts are discovered by Pango through the system font configuration; gmux no
longer bundles or extracts font files. Install Monaspace yourself to use it,
or select another installed family. The default family list falls back to
`monospace` when Monaspace is unavailable. `font-size` accepts integer
sizes from 6 through 96. `Ctrl+Shift++` and `Ctrl+Shift+-` adjust the font size
for the server session, so reconnecting preserves the current size. This runtime
override lasts for the server's lifetime (or until the config is reloaded); it
does not rewrite the shared config. The server sends font choices to the client,
which resolves them against locally installed fonts.
Named themes are searched in the config directory's `themes` folder, `$GMUX_THEME_DIR`, the
`themes` directory beside `gmux-server`, and the system gmux data directories.
An absolute theme file path is also accepted.
`gmuxctl` installs themes in the server's config directory (`~/.config/gmux/themes`
normally), preserving existing files so customized themes are not overwritten.
Existing theme folders are not deleted. To use a theme in an old location,
specify its absolute path or set `GMUX_THEME_DIR` to that directory.

On Linux, running servers watch the config directory with inotify. Saving
`config` (including an editor's atomic rename) reloads fonts and theme and redraws
attached clients without PTY activity or polling. Clearing/removing `theme`
restores defaults; an unavailable theme leaves the current colors intact.
Editing a theme file itself requires saving `config` again. Network filesystems may not notify this machine
about edits performed on a different host.

Config syntax/size errors and unavailable themes are sent to GTK as **SERVER
ERROR** dialogs. Unavailable primary fonts produce **CLIENT ERROR** dialogs and
use monospace as a fallback. Messages include the source file, line, option and
configured value. Invalid themes do not prevent starting a session, so an attached
client can display the diagnostic. Update both binaries for the new config records.

### Live config editor

In `gmuxctl`, press **c** or **e** to edit the host's shared config. Enter opens
a fuzzy-search picker for installed client fonts, sizes 6–96, or server themes.
Use arrows or Ctrl-N/Ctrl-P to navigate; typing resets to the first match.
Highlighted choices immediately update a separate preview window in place.
Enter accepts a choice; Escape restores the value before opening the picker.

The preview runs in an isolated temporary server session/config. Unsaved changes
are sent only to that temporary config, never the real host config. **s** saves
the real config atomically and triggers reload in existing servers; **q** cancels.
Both close and remove the preview. The editor preserves comments and unknown keys
without adding concurrent-editor locking. Saving affects all sessions using
that host config, not only the highlighted session.

`GMUX_PREVIEW_COMMAND` specifies a command to run **on the remote host**. It
defaults to `echo gmux`, followed by a shell so the preview stays open. For example:

```sh
GMUX_PREVIEW_COMMAND='printf "\\033[31mred\\033[0m normal\\n"' ./gmuxctl my-host
```

## Versions and updates

`gmux --version` and `gmux-server --version` print the source revision
(`-dirty` means a local build with uncommitted changes). Releases include a
checksum-verified `VERSION` file. On startup, `gmuxctl` checks the latest release:
matching binaries are reused; differing or old unversioned binaries are replaced.
Client updates live in gmuxctl's private download cache rather than overwriting
package-managed PATH entries. Remote server updates replace the installed binary
atomically and copy matching themes. Existing sessions keep their old executable;
only newly created sessions use the updated server. This is not an automatic
migration of running sessions or a guarantee of cross-version protocol compatibility.
Explicit `--client`/`--server-binary` overrides opt out of the corresponding update.
If the release check is unavailable, installed binaries remain usable.

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
C interface to Ghostty's render state. Our server copies those values into
MessagePack messages; the client maintains rows and renders them with
Cairo/Pango. Ghostty's own GUI uses its native Zig rendering stack.

### Why CMake and GTK?

I needed to pick _something_. Really, any build system and any library
could be used. CMake is widely used and supported. GTK gives the Linux client
native Wayland input, layout-aware key events, clipboard
access, and an event loop close to the one used by Ghostty itself.

### What did the source review change?

See [CHANGE_AUDIT.md](CHANGE_AUDIT.md) for the change-by-change evidence,
historical failing lines, local reproductions, and removals. The comparisons
below describe architecture, not claims that these implementations were copied.

- **Fonts:** Ghostty and WezTerm both bundle fonts and can open them directly
  from memory. Our Pango integration instead extracted them to temporary files
  and registered them with Fontconfig. We removed that extra lifecycle and use
  installed fonts, leaving discovery, fallback, weight, and style to Pango.
  This is a simplification for our Linux client, not a claim that bundled fonts
  are bad. Sources:
  [Ghostty embedded fonts](https://github.com/ghostty-org/ghostty/blob/76895d97b74ff6b24c2b1543bcd69ccc18048a4d/src/font/embedded.zig#L8-L19),
  [Ghostty memory faces](https://github.com/ghostty-org/ghostty/blob/76895d97b74ff6b24c2b1543bcd69ccc18048a4d/src/font/face/freetype.zig#L79-L89),
  [WezTerm built-ins](https://github.com/wezterm/wezterm/blob/b09b56c29c1e367e598b60ca266e2cc9038751e0/wezterm-font/src/parser.rs#L816-L884),
  [WezTerm memory stream](https://github.com/wezterm/wezterm/blob/b09b56c29c1e367e598b60ca266e2cc9038751e0/wezterm-font/src/ftwrap.rs#L1287-L1302).
- **Queued writes:** Ghostty retains owned PTY-write buffers until completion.
  gmux now retains its unwritten suffix too, using the existing single-threaded
  poll/GLib loops. No worker thread, locks, or queue dependency is needed.
  [Ghostty queueWrite](https://github.com/ghostty-org/ghostty/blob/76895d97b74ff6b24c2b1543bcd69ccc18048a4d/src/termio/Exec.zig#L403-L470).
- **Render boundary:** WezTerm sends semantic dirty lines and metadata, not
  rendered text pixels. Our row cache remains a sensible smaller analogue;
  adopting WezTerm's full pane/session interfaces would add machinery we do
  not need. We are not wire-compatible: WezTerm has stable row IDs, sequence
  numbers, and on-demand line retrieval; gmux sends changed viewport/overscan
  rows and currently resends visible image pixels with snapshots.
  [WezTerm change computation](https://github.com/wezterm/wezterm/blob/b09b56c29c1e367e598b60ca266e2cc9038751e0/wezterm-mux-server-impl/src/sessionhandler.rs#L51-L157).

The review also removed duplicated config/kill implementations and obsolete
Raylib container/profiling files. It did not replace the renderer with a GPU
pipeline or complete IME support. Complex-script cell positioning, per-redraw
image conversion, and the fixed 63-byte grapheme payload remain limitations;
oversized graphemes display a replacement glyph instead of overrunning memory.
