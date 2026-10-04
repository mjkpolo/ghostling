import curses
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from test_gmuxctl import load_gmuxctl
from test_server_lifecycle import snapshot_records


class Screen:
    def __init__(self, keys=()):
        self.keys = iter(keys)

    def getmaxyx(self):
        return 24, 100

    def getch(self):
        return next(self.keys)

    def __getattr__(self, name):
        return lambda *args: None


class ConfigEditorTest(unittest.TestCase):
    def test_picker_typing_resets_selection_and_cancel_restores_preview(self):
        gmux = load_gmuxctl()
        options = ["Alpha", "Beta", "Gamma"]
        screen = Screen([14, 14, ord("a"), 10])
        preview = mock.Mock()
        with mock.patch.object(gmux.curses, "newwin", return_value=screen):
            selected = gmux.config_picker(screen, "font", options, "Alpha", preview)
        self.assertEqual(selected, "Alpha")
        self.assertEqual(preview.call_args_list,
                         [mock.call("Beta"), mock.call("Gamma"), mock.call("Alpha")])
        screen = Screen([14, 27])
        preview.reset_mock()
        with mock.patch.object(gmux.curses, "newwin", return_value=screen):
            self.assertEqual(gmux.config_picker(screen, "font", options, "Alpha", preview), "Alpha")
        self.assertEqual(preview.call_args_list, [mock.call("Beta"), mock.call("Alpha")])

    def test_config_edit_preserves_comments_and_matches_subsequences(self):
        gmux = load_gmuxctl()
        original = "# keep me\nfont-size = 20\nunknown = keep\n"
        values = gmux.config_values(original)
        values["font-size"] = "32"
        result = gmux.config_text(original, values)
        self.assertIn("# keep me\nfont-size = 32\nunknown = keep\n", result)
        self.assertEqual(gmux.fuzzy_options(["Catppuccin Mocha", "Nord"], "ctm"),
                         ["Catppuccin Mocha"])

    @unittest.skipUnless(Path("build/gmux-server").exists(), "build the server first")
    def test_real_preview_changes_without_saving_and_cleans_up(self):
        # Actual server, shell, config watcher and wire; replace only SSH and UI.
        for save in (False, True):
            with self.subTest(save=save), tempfile.TemporaryDirectory() as temporary:
                gmux = load_gmuxctl()
                root = Path(temporary)
                config_dir = root / "config"
                config_dir.mkdir()
                (config_dir / "themes").mkdir()
                config = config_dir / "config"
                original = "font = monospace\nfont-size = 24\ntheme =\n"
                config.write_text(original)
                connections = []
                stopped_server = None

                def local_ssh(host, script, *args, check=True):
                    return subprocess.run(["sh", "-s", "--", *args], input=script,
                        capture_output=True, text=True, check=check, timeout=5)

                def attach(host, directory, name, client):
                    connection = socket.socket(socket.AF_UNIX)
                    connection.settimeout(3)
                    connection.connect(f"{directory}/{name}.sock")
                    connections.append(connection)
                    payload = b"\x91\x91\x01"
                    connection.sendall(struct.pack("!I", len(payload)) + payload)
                    records = snapshot_records(connection)
                    self.assertTrue(any(r[0] == 7 and r[2] == 24 for r in records))
                    return {"client": SimpleNamespace(terminate=connection.close,
                            wait=lambda **kw: None)}

                def picker(screen, key, options, current, preview):
                    nonlocal stopped_server
                    self.assertEqual(key, "font-size")
                    preview("32")
                    while True:
                        records = snapshot_records(connections[0])
                        if any(r[0] == 7 and r[2] == 32 for r in records):
                            break
                    self.assertEqual(config.read_text(), original)
                    if not save:
                        # Delay the daemon's unlink after --kill has queued
                        # shutdown. The editor must own removal of its socket
                        # pathname instead of racing daemon cleanup.
                        credentials = connections[0].getsockopt(
                            socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
                        stopped_server, _, _ = struct.unpack("3i", credentials)
                        os.kill(stopped_server, signal.SIGSTOP)
                    return "32"

                screen = Screen([curses.KEY_DOWN, 10, ord("s" if save else "q")])
                with mock.patch.object(gmux, "run_ssh", side_effect=local_ssh), \
                        mock.patch.object(gmux, "remote_config_dir", return_value=str(config_dir)), \
                        mock.patch.object(gmux, "start_attachment", side_effect=attach), \
                        mock.patch.object(gmux, "stop_attachment"), \
                        mock.patch.object(gmux, "config_picker", side_effect=picker):
                    try:
                        gmux.edit_config(screen, "local-test", str(Path("build/gmux-server").resolve()),
                                         str(root), str(Path("build/gmux").resolve()))
                        self.assertEqual(list(root.glob("preview.*")), [])
                    finally:
                        if stopped_server:
                            os.kill(stopped_server, signal.SIGCONT)
                self.assertEqual(gmux.config_values(config.read_text())["font-size"],
                                 "32" if save else "24")
                self.assertEqual(list(root.glob("preview.*")), [])
