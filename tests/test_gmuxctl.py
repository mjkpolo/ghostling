import hashlib
import io
import importlib.machinery
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


def load_gmuxctl():
    path = Path(__file__).resolve().parents[1] / "gmuxctl"
    loader = importlib.machinery.SourceFileLoader("gmuxctl", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class ReleaseDownloadTest(unittest.TestCase):
    def test_client_versions_skip_current_and_update_old_or_unversioned(self):
        gmuxctl = load_gmuxctl()
        client = Path("/installed/gmux")
        for output, status, update in (("gmux abc\n", 0, False),
                                       ("gmux old\n", 0, True),
                                       ("", 1, True)):
            with self.subTest(output=output), \
                    mock.patch.object(gmuxctl.subprocess, "run", return_value=
                                      SimpleNamespace(stdout=output, returncode=status)), \
                    mock.patch.object(gmuxctl, "release_asset",
                                      return_value=Path("/cache/gmux")) as download:
                result = gmuxctl.update_client(client, "abc")
                self.assertEqual(result, Path("/cache/gmux") if update else client)
                self.assertEqual(download.called, update)

    def test_server_update_copies_matching_release_and_themes_only_if_needed(self):
        gmuxctl = load_gmuxctl()
        for output, update in (("gmux-server abc\n", False),
                               ("gmux-server old\n", True), ("", True)):
            with self.subTest(output=output), \
                    mock.patch.object(gmuxctl, "run_ssh", return_value=
                                      SimpleNamespace(stdout=output, returncode=0)), \
                    mock.patch.object(gmuxctl, "release_asset",
                                      return_value=Path("/cache/gmux-server")), \
                    mock.patch.object(gmuxctl, "copy_server") as copy:
                gmuxctl.update_server("host", "/remote/bin/gmux-server", "abc")
                if update:
                    copy.assert_called_once_with("host", Path("/cache/gmux-server"),
                                                 "/remote/bin", release_themes=True)
                else:
                    copy.assert_not_called()

    def test_unavailable_version_check_keeps_installed_binaries(self):
        gmuxctl = load_gmuxctl()
        with mock.patch.object(gmuxctl, "release_checksums",
                               side_effect=RuntimeError("offline")), \
                mock.patch("builtins.print"):
            self.assertIsNone(gmuxctl.latest_version())

    def test_server_copy_includes_themes_and_repairs_missing_bundle(self):
        gmuxctl = load_gmuxctl()

        def local_ssh(host, script, *args, check=True):
            return subprocess.run(["sh", "-s", "--", *args], input=script,
                                  text=True, capture_output=True, check=check)

        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(gmuxctl, "remote_config_dir",
                                  return_value=str(Path(temporary) / "config")), \
                mock.patch.object(gmuxctl, "run_ssh", side_effect=local_ssh), \
                mock.patch.object(gmuxctl, "ssh_command",
                                  side_effect=lambda host, command:
                                  ["sh", "-c", command]) as command:
            root = Path(temporary)
            (root / "config").mkdir()
            source = root / "source"
            source.mkdir()
            binary = source / "gmux-server"
            binary.write_bytes(b"test binary")
            (source / "themes").mkdir()
            (source / "themes/LICENSE").write_text("test license")
            (source / "themes/Test Theme").write_text("background = 123456\n")
            server = gmuxctl.copy_server("example", binary, str(root / "remote bin"))
            themes = root / "config/themes"
            self.assertEqual((themes / "Test Theme").read_text(),
                             "background = 123456\n")
            command.reset_mock()
            gmuxctl.provision_themes("example", binary)
            command.assert_not_called()
            shutil.rmtree(themes)
            gmuxctl.provision_themes("example", binary)
            self.assertTrue((themes / "Test Theme").is_file())

    def test_download_is_verified_and_cached(self):
        gmuxctl = load_gmuxctl()
        with tempfile.TemporaryDirectory() as release_temp:
            with tempfile.TemporaryDirectory() as cache_temp:
                release = Path(release_temp)
                cache = Path(cache_temp)
                payload = b"published gmux binary\n"
                (release / "gmux").write_bytes(payload)
                digest = hashlib.sha256(payload).hexdigest()
                (release / "SHA256SUMS").write_text(f"{digest}  gmux\n")
                with mock.patch.object(gmuxctl, "RELEASE_BASE",
                                       release.as_uri()), \
                        mock.patch.object(gmuxctl, "cache_dir",
                                          return_value=cache), \
                        mock.patch("builtins.print") as output:
                    downloaded = gmuxctl.release_asset(
                        "gmux", executable=True)
                    self.assertEqual(downloaded.read_bytes(), payload)
                    self.assertEqual(
                        downloaded.stat().st_mode & 0o777, 0o700)
                    self.assertTrue(output.called)
                    output.reset_mock()
                    (release / "gmux").unlink()
                    self.assertEqual(
                        gmuxctl.release_asset("gmux", executable=True),
                        downloaded)
                    output.assert_not_called()

    def test_checksum_redirect_does_not_change_asset_endpoint(self):
        gmuxctl = load_gmuxctl()
        payload = b"published gmux binary\n"
        digest = hashlib.sha256(payload).hexdigest()
        checksums = io.BytesIO(f"{digest}  gmux\n".encode())
        checksums.geturl = lambda: (
            "https://release-assets.githubusercontent.com/123/checksum-object?signature=abc")
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(gmuxctl, "cache_dir", return_value=Path(temporary)), \
                mock.patch.object(gmuxctl, "urlopen",
                                  side_effect=[checksums, io.BytesIO(payload)]) as download, \
                mock.patch("builtins.print"):
            self.assertEqual(gmuxctl.release_asset("gmux").read_bytes(), payload)
        self.assertEqual(download.call_args_list, [
            mock.call(f"{gmuxctl.RELEASE_BASE}/SHA256SUMS", timeout=5),
            mock.call(f"{gmuxctl.RELEASE_BASE}/gmux", timeout=60),
        ])

    def test_existing_remote_terminfo_is_not_copied(self):
        gmuxctl = load_gmuxctl()
        responses = [
            SimpleNamespace(stdout="/tmp/gmux-terminfo\n", returncode=0),
            SimpleNamespace(stdout="", returncode=0),
        ]
        with mock.patch.object(gmuxctl, "run_ssh",
                               side_effect=responses) as run_ssh, \
                mock.patch.object(gmuxctl, "release_asset") as download:
            gmuxctl.provision_terminfo("example")
        self.assertEqual(run_ssh.call_count, 2)
        download.assert_not_called()

    @unittest.skipUnless(shutil.which("tic") and shutil.which("infocmp"),
                         "requires ncurses terminfo tools")
    def test_terminfo_creates_missing_parents_and_skips_existing_entry(self):
        gmuxctl = load_gmuxctl()

        # Replace only SSH transport: execute the actual provisioning commands
        # and tic locally, including main()'s pre-discovered-directory path.
        def local_ssh(host, script, *args, check=True):
            return subprocess.run(["sh", "-s", "--", *args], input=script,
                                  text=True, capture_output=True, check=check)

        for checked in (False, True):
            with self.subTest(checked=checked), \
                    tempfile.TemporaryDirectory() as temporary, \
                    mock.patch.object(gmuxctl, "run_ssh", side_effect=local_ssh), \
                    mock.patch.object(gmuxctl, "ssh_command",
                                      side_effect=lambda host, command:
                                      ["sh", "-c", command]) as command:
                directory = Path(temporary) / "missing parent" / "gmux" / "terminfo"
                # An old Kitty entry must not make Ghostty provisioning skip.
                if not checked:
                    directory.mkdir(parents=True, mode=0o700)
                    (directory / "x").mkdir(mode=0o700)
                    (directory / "x/xterm-kitty").write_bytes(b"old entry")
                gmuxctl.provision_terminfo("example", str(directory), checked=checked)
                self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
                entries = list(directory.glob("*/xterm-ghostty"))
                self.assertEqual(len(entries), 1)
                result = subprocess.run(["infocmp", "-A", str(directory),
                                         "xterm-ghostty"], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                before = entries[0].stat().st_mtime_ns
                command.reset_mock()
                gmuxctl.provision_terminfo("example", str(directory))
                command.assert_not_called()
                self.assertEqual(entries[0].stat().st_mtime_ns, before)

    def test_explicit_forward_is_not_cleared(self):
        gmuxctl = load_gmuxctl()
        gmuxctl.SSH_CONTROL = "/private/control"
        command = gmuxctl.control_command("-O", "forward", "-L", "a:b")
        self.assertNotIn("ClearAllForwardings=yes", command)
        self.assertEqual(command[1:5], ["-F", "/dev/null", "-S",
                                       "/private/control"])
        self.assertEqual(command[-5:], ["-O", "forward", "-L", "a:b", "gmux"])

    def test_cached_client_starts_without_network(self):
        gmuxctl = load_gmuxctl()
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            client = cache / "gmux"
            client.write_bytes(b"cached executable")
            client.chmod(0o700)
            with mock.patch.object(gmuxctl, "HERE", cache / "elsewhere"), \
                    mock.patch.object(gmuxctl.shutil, "which", return_value=None), \
                    mock.patch.object(gmuxctl, "cache_dir", return_value=cache), \
                    mock.patch.object(gmuxctl, "release_asset") as download:
                self.assertEqual(gmuxctl.local_program("gmux"), client)
                download.assert_not_called()

    def test_remote_state_uses_one_ssh_result(self):
        gmuxctl = load_gmuxctl()
        output = SimpleNamespace(stdout=(
            "/tmp/noise\nserver\t/bin/gmux-server\n"
            "sockets\t/run/user/1/gmux\n"
            "terminfo\t/home/me/.local/share/gmux/terminfo\n"
            "has_terminfo\tyes\n"))
        with mock.patch.object(gmuxctl, "run_ssh",
                               return_value=output) as run_ssh:
            state = gmuxctl.remote_state("example")
        self.assertEqual(state, ("/bin/gmux-server", "/run/user/1/gmux",
                                 "/home/me/.local/share/gmux/terminfo", True))
        run_ssh.assert_called_once()


class AttachmentTest(unittest.TestCase):
    def test_attachments_own_distinct_private_paths_and_cleanup(self):
        gmuxctl = load_gmuxctl()
        gmuxctl.SSH_CONTROL = "/private/control"
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(gmuxctl, "local_dir",
                                  return_value=Path(directory)), \
                mock.patch.object(gmuxctl.subprocess, "run") as run, \
                mock.patch.object(gmuxctl.subprocess, "Popen") as launch:
            first = gmuxctl.start_attachment("host", "/remote", "one", "gmux")
            second = gmuxctl.start_attachment("host", "/remote", "two", "gmux")
            self.assertNotEqual(first["directory"].name, second["directory"].name)
            for attachment in (first, second):
                self.assertEqual(Path(attachment["directory"].name).stat().st_mode
                                 & 0o777, 0o700)
            self.assertEqual(launch.call_count, 2)
            gmuxctl.stop_attachment(first)
            self.assertFalse(Path(first["directory"].name).exists())
            self.assertTrue(Path(second["directory"].name).exists())
            self.assertIn("cancel", run.call_args.args[0])
            gmuxctl.stop_attachment(second)

    def test_failed_client_launch_cancels_forward_and_removes_directory(self):
        gmuxctl = load_gmuxctl()
        gmuxctl.SSH_CONTROL = "/private/control"
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(gmuxctl, "local_dir",
                                  return_value=Path(directory)), \
                mock.patch.object(gmuxctl.subprocess, "run") as run, \
                mock.patch.object(gmuxctl.subprocess, "Popen",
                                  side_effect=OSError("no client")):
            with self.assertRaisesRegex(OSError, "no client"):
                gmuxctl.start_attachment("host", "/remote", "one", "gmux")
            self.assertEqual(list(Path(directory).iterdir()), [])
            self.assertIn("cancel", run.call_args.args[0])


class TuiTest(unittest.TestCase):
    def test_session_attached_elsewhere_does_not_launch_another_window(self):
        gmuxctl = load_gmuxctl()
        screen = mock.Mock()
        screen.getmaxyx.return_value = (10, 40)
        screen.getch.side_effect = [10, ord("q")]
        with mock.patch.object(gmuxctl.curses, "curs_set"), \
                mock.patch.object(gmuxctl, "sessions",
                                  return_value=[("attached", "busy")]), \
                mock.patch.object(gmuxctl, "start_attachment") as start:
            gmuxctl.tui(screen, "host", "server", "/remote", "gmux")
        start.assert_not_called()

    def test_prompt_blocks_and_restores_polling_even_after_error(self):
        gmuxctl = load_gmuxctl()
        screen = mock.Mock()
        screen.getmaxyx.return_value = (10, 40)
        screen.getstr.side_effect = KeyboardInterrupt
        with mock.patch.object(gmuxctl.curses, "echo"), \
                mock.patch.object(gmuxctl.curses, "noecho") as noecho:
            with self.assertRaises(KeyboardInterrupt):
                gmuxctl.prompt(screen, "Name: ")
        self.assertEqual(screen.timeout.call_args_list,
                         [mock.call(-1), mock.call(250)])
        noecho.assert_called_once()

    def test_long_list_scrolls_and_error_closes_attachments(self):
        gmuxctl = load_gmuxctl()
        screen = mock.Mock()
        screen.getmaxyx.return_value = (10, 40)
        screen.getch.side_effect = [ord("j")] * 15 + [10, 10, KeyboardInterrupt]

        def draw(row, column, text, limit):
            self.assertLess(row, 10)
            self.assertGreaterEqual(row, 0)
            self.assertLess(limit, 40)

        screen.addnstr.side_effect = draw
        attachment = {"name": "session-15", "client": mock.Mock()}
        attachment["client"].poll.return_value = None
        with mock.patch.object(gmuxctl.curses, "curs_set"), \
                mock.patch.object(gmuxctl, "sessions", return_value=[
                    ("live", f"session-{i}") for i in range(30)]), \
                mock.patch.object(gmuxctl, "start_attachment",
                                  return_value=attachment) as start, \
                mock.patch.object(gmuxctl, "stop_attachment") as stop:
            with self.assertRaises(KeyboardInterrupt):
                gmuxctl.tui(screen, "host", "server", "/remote", "gmux")
        start.assert_called_once_with("host", "/remote", "session-15", "gmux")
        stop.assert_called_once_with(attachment)
        attachment["client"].terminate.assert_called_once()
        attachment["client"].wait.assert_called_once()


if __name__ == "__main__":
    unittest.main()
