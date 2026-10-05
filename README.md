# gmux

Persistent local and remote terminals, managed in one desktop window.

- **gmux-manager**: Python/Qt host, folder, session, and config manager.
- **gmux**: Linux GTK terminal window.
- **gmux-server**: persistent headless terminal powered by libghostty.

An experimental learning project. Current binary releases target Linux x86-64.

## Install the manager

Install Python 3.9 or newer and OpenSSH. Download the manager wheel from the
[latest release](https://github.com/mjkpolo/ghostling/releases/latest), then:

```sh
python3 -m venv ~/.local/share/gmux-manager-venv
~/.local/share/gmux-manager-venv/bin/pip install ./gmux_manager-0.1.0-py3-none-any.whl
~/.local/share/gmux-manager-venv/bin/gmux-manager --install-desktop
```

Launch **gmux Manager** from your application launcher, or run:

```sh
~/.local/share/gmux-manager-venv/bin/gmux-manager
```

No compilation is needed. Pip installs PySide6; system graphics libraries are
still required, including `libEGL.so.1` (`libegl1` on Ubuntu). The terminal window
also requires GTK4 runtime libraries. Keep the virtual environment—the desktop
launcher uses it.

Installation locations:

- Manager environment: `~/.local/share/gmux-manager-venv` in the commands above.
- Desktop entry: `~/.local/share/applications/gmux-manager.desktop`.
- Launcher icon: `~/.local/share/icons/hicolor/scalable/apps/gmux-manager.svg`.
- Downloaded binaries: `~/.cache/gmux`.

The desktop entry and icon honor `XDG_DATA_HOME`; downloads honor `XDG_CACHE_HOME`.
The launcher has no client/server path overrides. Installed binaries are found
through **PATH**, not a hardcoded bin directory. To use `~/.local/bin`, include
it in your desktop session's PATH and in the remote noninteractive SSH PATH.

The manager checks release versions for both binaries. It downloads an updated
client into its own cache without overwriting PATH installations. Server upgrades
require confirmation, with a warning before stopping running sessions. If release
checks are unavailable, installed binaries remain usable. The manager itself is
updated with pip, not automatically.

Alternatively, install the development branch into that environment:

```sh
~/.local/share/gmux-manager-venv/bin/pip install --upgrade \
  "git+https://github.com/mjkpolo/ghostling.git@gmux-codex"
```

There is no PyPI publication. To update from a release, install the new wheel
with `pip install --force-reinstall /path/to/the.whl`, then restart the manager.

## Use it

1. Select **localhost** or a remote host, then **Connect**.
2. If needed, choose **Install server…** (default: `~/.local/bin`). This also
   installs themes and the terminal definition. The client is downloaded when
   absent from PATH and the manager's cache.
3. Choose **New session**, then **Open terminal**, or double-click a session.

**localhost** always appears and connects directly through Unix sockets; it
does not need an SSH server. Remote hosts come from named entries in
`~/.ssh/config` and its includes. Edit those files yourself, then **Reload hosts**.

Remote connections use OpenSSH keys, agents, or graphical password/MFA prompts.
Unknown host fingerprints require explicit approval. Credentials are not saved.
Remote metadata management requires `python3`, with no extra Python packages.

### Folders and names

Connecting checks the installed server against the latest release. If different,
the manager asks before upgrading. Running sessions trigger an explicit warning:
accepting stops their shells and applications; declining leaves them untouched.
The check compares the installed binary, not the version inside existing server
processes. Manually replacing the binary does not update already-running sessions.

Use **New folder**, **Rename folder**, **Move…**, and **Rename…** to organize
sessions. **Open folder** opens every available session in the selected folder,
skipping attached sessions. **Delete folder** moves its sessions to Unfiled;
it does not stop them. Session **Delete** terminates a session after confirmation.

Sockets have stable random IDs. Names and folders are separate metadata, so
renaming never moves a socket. Existing named sockets remain usable under
**Unfiled**. Saved names override application titles in manager-launched windows.
Renames update owned windows immediately; **Refresh** picks up other managers'
changes. Folder/session metadata uses a separate file lock and atomic replacement.

Closing a terminal leaves its server running. Closing the manager hides it in
the tray when available; **Quit manager** exits. Relaunching restores the same
manager window. Without a tray, closing exits normally. Sessions do not survive
reboots and remain subject to the host's logout/cleanup policies.

### Appearance

Choose **Config…** to edit the host's font, font size, and theme. Open terminals
preview changes in place. If none are attached, a disposable preview terminal
opens and is removed when you finish. **Save** keeps changes; **Cancel** restores
the original config. Preview temporarily edits the shared config, so do not edit
it elsewhere simultaneously. A forcibly killed manager may leave its last preview.

Configuration belongs to the session host (localhost counts as a host):

```ini
# ~/.config/gmux/config, or $XDG_CONFIG_HOME/gmux/config
font = Monaspace Argon Frozen, monospace
font-size = 24
theme =
```

Fonts must be installed on the GUI machine. Themes live in the host's
`~/.config/gmux/themes`; metadata lives alongside the config in `sessions.json`.
The server reloads config changes automatically. There is no separate client config.

Socket directories are private (`0700`), with `0600` sockets. `GMUX_SOCKET_DIR`
overrides their location; otherwise gmux uses `$XDG_RUNTIME_DIR/gmux` or
`/tmp/gmux-<uid>`. Remote sockets are forwarded through SSH, not exposed over TCP.

## Run without the manager

Download `gmux` and `gmux-server` from a release and make them executable.
The server is static musl with baseline x86-64 instructions. The GTK client is
built on Rocky Linux 9.4 and needs compatible system libraries.

```sh
gmux-server /path/to/private-directory/work.sock
gmux --connect /path/to/private-directory/work.sock
gmux-server --kill /path/to/private-directory/work.sock
```

For remote use, forward the server's Unix socket with SSH and connect the client
to the local forwarded socket. The manager handles this automatically.

## Build and test

Install CMake, a C/C++ toolchain, GTK4 development packages, Git, curl, and tar.

```sh
git clone --recurse-submodules --branch gmux-codex https://github.com/mjkpolo/ghostling.git
cd ghostling
./build.sh
./tests/run.sh
```

`build.sh` downloads the pinned Zig version when needed. Use `--server-only` to
skip GTK, or `--server-musl` for the portable static server. Binaries normally
appear in `build/`. Use `gmux-manager --client /absolute/path/to/gmux` to test a
specific terminal binary.

With the manager dependencies installed:

```sh
QT_QPA_PLATFORM=offscreen python -m unittest manager.test_ui manager.test_config
```

## Credits and license

Originally based on [Ghostling](https://github.com/ghostty-org/ghostling), using
[Ghostty](https://ghostty.org)'s terminal library. See [LICENSE](LICENSE) and
dependency licenses; releases include Ghostty's license.
