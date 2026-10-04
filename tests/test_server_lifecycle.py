#!/usr/bin/env python3
"""Exercise control commands while a real server has an attached client."""

import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def main():
    binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/gmux-server").resolve())
    with tempfile.TemporaryDirectory(prefix="gmux-server-test-") as directory:
        path = str(Path(directory) / "terminal.sock")
        env = {**os.environ, "SHELL": "/bin/sh", "XDG_CONFIG_HOME": directory}

        def command(*args):
            return subprocess.run(
                [binary, *args], env=env, capture_output=True, timeout=3
            )

        started = command(path)
        assert started.returncode == 0, started.stderr.decode()
        try:
            # Repeated check -> attach transitions expose accepting a GUI into
            # the control slot before retiring the health check's closed socket.
            for _ in range(20):
                checked = command("--check", path)
                assert checked.returncode == 0
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(3)
                    client.connect(path)
                    payload = b"\x91\x91\x01"
                    client.sendall(struct.pack("!I", len(payload)) + payload)
                    response = client.recv(4)
                    assert response, "attachment rejected after health check"
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(3)
                client.connect(path)
                # MessagePack [[INPUT_ATTACH]], with the normal 32-bit frame.
                payload = b"\x91\x91\x01"
                client.sendall(struct.pack("!I", len(payload)) + payload)
                # Wait for a render frame to prove that attachment was applied.
                response = client.recv(4)
                assert response
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
    print("server lifecycle: idle check, repeated attached checks, attached kill passed")


if __name__ == "__main__":
    main()
