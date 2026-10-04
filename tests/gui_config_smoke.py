#!/usr/bin/env python3
"""Optional local Wayland smoke test: requires sway and grim, not run by CI."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    repo = Path(__file__).resolve().parents[1]
    output = repo / "build/config-gui-test"
    output.mkdir(parents=True, exist_ok=True)
    # Keep Unix socket paths below sockaddr_un's 108-byte limit.
    with tempfile.TemporaryDirectory(prefix="gmux-gui-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        sway_config = root / "sway.conf"
        sway_config.write_text("output HEADLESS-1 resolution 1280x800\n"
                               "default_border pixel 0\nfocus_follows_mouse no\n")
        env = {**os.environ, "XDG_RUNTIME_DIR": str(runtime),
               "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman",
               "WLR_LIBINPUT_NO_DEVICES": "1", "GDK_BACKEND": "wayland"}
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("SWAYSOCK", None)
        client = None
        server = repo / "build/gmux-server"
        sock = root / "test.sock"
        with (output / "gui.log").open("w") as log:
            compositor = subprocess.Popen(["sway", "-c", str(sway_config)],
                env=env, stdout=log, stderr=log)
            try:
                def wait(check):
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        value = check()
                        if value:
                            return value
                        time.sleep(.05)
                    raise AssertionError("GUI test timed out")

                display = wait(lambda: list(runtime.glob("wayland-*.lock")))[0]
                ipc = wait(lambda: list(runtime.glob("sway-ipc.*.sock")))[0]
                env.update(WAYLAND_DISPLAY=display.name[:-5], SWAYSOCK=str(ipc))
                config_dir = root / "server/gmux"
                config_dir.mkdir(parents=True)
                config = config_dir / "config"
                theme = root / "theme"
                theme.write_text("background = #203040\nforeground = #ffffff\n")
                config.write_text(f"font = monospace\nfont-size = 24\ntheme = {theme}\n")
                workload = root / "shell"
                workload.write_text("#!/bin/sh\nprintf 'gmux live config preview\\n"
                                    "regular \\033[1mbold\\033[0m \\033[3mitalic\\033[0m\\n'\n"
                                    "exec /bin/sh\n")
                workload.chmod(0o700)
                subprocess.run([server, sock], check=True,
                    env={**env, "XDG_CONFIG_HOME": str(root / "server"), "SHELL": str(workload)})
                client = subprocess.Popen([repo / "build/gmux", "--connect", sock],
                    env={**env, "XDG_CONFIG_HOME": str(root / "client-config")},
                    stdout=log, stderr=log)

                def windows():
                    tree = json.loads(subprocess.check_output(
                        ["swaymsg", "-t", "get_tree", "-r"], env=env))
                    def walk(node):
                        yield node
                        for child in node.get("nodes", []) + node.get("floating_nodes", []):
                            yield from walk(child)
                    return [node for node in walk(tree) if node.get("pid") == client.pid]

                wait(windows)
                def capture(name):
                    time.sleep(.4)
                    subprocess.run(["grim", str(output / name)], env=env, check=True)

                capture("before.png")
                theme.write_text("background = #402030\nforeground = #ffffff\n")
                config.write_text(f"font = monospace\nfont-size = 36\ntheme = {theme}\n")
                capture("after.png")
                assert (output / "before.png").read_bytes() != (output / "after.png").read_bytes()
                assert not (root / "client-config").exists(), "client created a config directory"
                config.write_text("font = gmux-deliberately-missing-font\nfont-size = 36\n"
                                  "theme = gmux-deliberately-missing-theme\n")
                wait(lambda: {"CLIENT ERROR", "SERVER ERROR"}.issubset(
                    {node.get("name") for node in windows()}))
                capture("errors.png")
                subprocess.run(["swaymsg", '[title="CLIENT ERROR"] kill'], env=env,
                               check=True, stdout=subprocess.DEVNULL)
                capture("server-error.png")
                print(f"GTK: live config redraw, no client config, both error dialogs passed: {output}")
            finally:
                if client:
                    client.terminate()
                    client.wait(timeout=3)
                if sock.exists():
                    subprocess.run([server, "--kill", sock], timeout=3)
                compositor.terminate()
                compositor.wait(timeout=3)


if __name__ == "__main__":
    main()
