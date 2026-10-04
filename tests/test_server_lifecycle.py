#!/usr/bin/env python3
"""Exercise control commands while a real server has an attached client."""

import os
import io
from pathlib import Path
import socket
import select
import struct
import subprocess
import sys
import tempfile
import time


def snapshot_records(client):
    def receive(size):
        data = b""
        while len(data) < size:
            part = client.recv(size - len(data))
            assert part, "server disconnected"
            data += part
        return data

    data = io.BytesIO(receive(struct.unpack("!I", receive(4))[0]))

    # Minimal decoder for the snapshot types used by this integration test.
    def value():
        tag = data.read(1)[0]
        if tag < 128:
            return tag
        if tag >= 0xE0:
            return tag - 256
        if 0xD0 <= tag <= 0xD3:
            return int.from_bytes(data.read(1 << (tag - 0xD0)), "big", signed=True)
        if 0xA0 <= tag <= 0xBF:
            return data.read(tag & 31).decode()
        if tag in (0xD9, 0xDA, 0xDB, 0xC4, 0xC5, 0xC6):
            base = 0xD9 if tag >= 0xD9 else 0xC4
            size = int.from_bytes(data.read(1 << (tag - base)), "big")
            result = data.read(size)
            return result.decode() if tag >= 0xD9 else result
        if tag in (0xC2, 0xC3):
            return tag == 0xC3
        if 0xCC <= tag <= 0xCF:
            return int.from_bytes(data.read(1 << (tag - 0xCC)), "big")
        size = (tag & 15) if 0x90 <= tag <= 0x9F else None
        if tag in (0xDC, 0xDD):
            size = int.from_bytes(data.read(2 if tag == 0xDC else 4), "big")
        assert size is not None, f"unexpected header tag {tag:x}"
        return [value() for _ in range(size)]

    return value()


def snapshot_header(client):
    records = snapshot_records(client)
    assert records[0][0] == 1
    return records[0]


def main():
    binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/gmux-server").resolve())
    with tempfile.TemporaryDirectory(prefix="gmux-server-test-") as directory:
        path = str(Path(directory) / "terminal.sock")
        env = {**os.environ, "SHELL": "/bin/sh", "XDG_CONFIG_HOME": directory}
        terminfo = Path(directory) / "terminfo"
        terminfo.mkdir(mode=0o700)
        source = Path(__file__).resolve().parents[1] / "terminfo/xterm-ghostty.terminfo"
        subprocess.run(["tic", "-x", "-o", str(terminfo), str(source)], check=True)
        shell = Path(directory) / "shell"
        observed_env = Path(directory) / "shell-env"
        shell.write_text('#!/bin/sh\nprintf "%s\\n" "$TERM" "$TERMINFO" > "$GMUX_TEST_ENV"\nexec /bin/sh\n')
        shell.chmod(0o700)
        env.update(SHELL=str(shell), GMUX_TERMINFO=str(terminfo),
                   GMUX_TEST_ENV=str(observed_env))

        def command(*args):
            return subprocess.run(
                [binary, *args], env=env, capture_output=True, timeout=3
            )

        version = command("--version")
        assert version.returncode == 0 and version.stdout.startswith(b"gmux-server ")
        assert not (Path(directory) / "gmux").exists(), "--version created config"
        client_binary = str(Path(binary).with_name("gmux"))
        if Path(client_binary).exists():
            client_version = subprocess.run([client_binary, "--version"],
                env={**env, "DISPLAY": "", "WAYLAND_DISPLAY": ""},
                capture_output=True, timeout=3)
            assert client_version.returncode == 0
            assert client_version.stdout.split()[1] == version.stdout.split()[1]
            assert not (Path(directory) / "gmux").exists()
        started = command(path)
        assert started.returncode == 0, started.stderr.decode()
        try:
            # Repeated check -> attach transitions expose accepting a GUI into
            # the control slot before retiring the health check's closed socket.
            for index in range(20):
                checked = command("--check", path)
                assert checked.returncode == 0
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(3)
                    client.connect(path)
                    payload = b"\x91\x91\x01"
                    client.sendall(struct.pack("!I", len(payload)) + payload)
                    records = snapshot_records(client)
                    config = next(record for record in records if record[0] == 7)
                    assert config[2] == (24 if index == 0 else 25), config
                    if index == 0:
                        payload = b"\x91\x92\x0a\x01"  # INPUT_FONT_DELTA +1
                        client.sendall(struct.pack("!I", len(payload)) + payload)
                        while True:
                            records = snapshot_records(client)
                            if any(record[0] == 7 and record[2] == 25 for record in records):
                                break
            assert observed_env.read_text().splitlines() == ["xterm-ghostty", str(terminfo)]
            print("server PTY: TERM=xterm-ghostty; TERMINFO=private compiled Ghostty database")
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(3)
                client.connect(path)
                # MessagePack [[INPUT_ATTACH]], with the normal 32-bit frame.
                payload = b"\x91\x91\x01"
                client.sendall(struct.pack("!I", len(payload)) + payload)
                # Wait for a render frame to prove that attachment was applied.
                baseline = snapshot_header(client)
                while select.select([client], [], [], 0.1)[0]:
                    baseline = snapshot_header(client)
                config = Path(directory) / "gmux/config"
                theme = Path(directory) / "test-theme"
                theme.write_text("background = #123456\nselection-background = #abcdef\n")
                config.write_text(f"theme = {theme}\n")
                header = snapshot_header(client)
                assert header[3] == [0x12, 0x34, 0x56], header
                assert header[13] == [0xAB, 0xCD, 0xEF] and header[15]
                # Editor-style replacement must preserve the directory watch.
                replacement = config.with_suffix(".new")
                replacement.write_text("# theme removed: restore defaults\n")
                replacement.replace(config)
                header = snapshot_header(client)
                assert header[3] == baseline[3] and not header[15], header
                config.write_text("theme = /nonexistent/gmux-test-theme\n")
                records = snapshot_records(client)
                error = next(record[1] for record in records if record[0] == 8)
                assert "config:1" in error and "/nonexistent/gmux-test-theme" in error
                config.write_text(f"theme = {theme}\n")
                assert snapshot_header(client)[3] == [0x12, 0x34, 0x56]
                started_checks = time.monotonic()
                for _ in range(6):
                    checked = command("--check", path)
                    assert checked.returncode == 2, checked.stderr.decode()
                assert time.monotonic() - started_checks < 3
                killed = command("--kill", path)
                assert killed.returncode == 0
                deadline = time.monotonic() + 3
                while Path(path).exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert not Path(path).exists(), "kill did not stop attached server"
        finally:
            if Path(path).exists():
                command("--kill", path)
    print("server lifecycle: config reload on save/rename, default reset, missing-theme recovery, checks and kill passed")


if __name__ == "__main__":
    main()
