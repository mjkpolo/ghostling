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


def load_remote():
    path = Path(__file__).resolve().parents[1] / "manager/remote.py"
    loader = importlib.machinery.SourceFileLoader("remote", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class ReleaseDownloadTest(unittest.TestCase):
    def test_unknown_release_version_does_not_replace_installed_binaries(self):
        remote = load_remote()
        with mock.patch.object(remote, "release_checksums", return_value={"VERSION": "hash"}), \
                mock.patch.object(remote, "release_asset") as asset, \
                mock.patch.object(remote.sys, "stderr", new_callable=io.StringIO) as errors:
            asset.return_value.read_text.return_value = "unknown\n"
            self.assertIsNone(remote.latest_version())
            self.assertIn("keeping installed binaries", errors.getvalue())

    def test_remote_failure_displays_stderr_instead_of_ssh_argument_list(self):
        remote = load_remote()
        result = SimpleNamespace(returncode=1, stdout="", stderr="failed to start server: Input/output error\n")
        with mock.patch.object(remote.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "hpc: failed to start server: Input/output error"):
                remote.run_ssh("hpc", "test command")
            self.assertIs(remote.run_ssh("hpc", "test command", check=False), result)

    def test_client_versions_skip_current_and_update_old_or_unversioned(self):
        remote = load_remote()
        client = Path("/installed/gmux")
        for output, status, update in (("gmux abc\n", 0, False),
                                       ("gmux old\n", 0, True),
                                       ("", 1, True)):
            with self.subTest(output=output), \
                    mock.patch.object(remote.subprocess, "run", return_value=
                                      SimpleNamespace(stdout=output, returncode=status)), \
                    mock.patch.object(remote, "release_asset",
                                      return_value=Path("/cache/gmux")) as download:
                result = remote.update_client(client, "abc")
                self.assertEqual(result, Path("/cache/gmux") if update else client)
                self.assertEqual(download.called, update)

    def test_server_update_copies_matching_release_and_themes_only_if_needed(self):
        remote = load_remote()
        for output, update in (("gmux-server abc\n", False),
                               ("gmux-server old\n", True), ("", True)):
            with self.subTest(output=output), \
                    mock.patch.object(remote, "run_ssh", return_value=
                                      SimpleNamespace(stdout=output, returncode=0)), \
                    mock.patch.object(remote, "release_asset",
                                      return_value=Path("/cache/gmux-server")), \
                    mock.patch.object(remote, "copy_server") as copy:
                remote.update_server("host", "/remote/bin/gmux-server", "abc")
                if update:
                    copy.assert_called_once_with("host", Path("/cache/gmux-server"),
                                                 "/remote/bin", release_themes=True)
                else:
                    copy.assert_not_called()

    def test_unavailable_version_check_keeps_installed_binaries(self):
        remote = load_remote()
        with mock.patch.object(remote, "release_checksums",
                               side_effect=RuntimeError("offline")), \
                mock.patch("builtins.print"):
            self.assertIsNone(remote.latest_version())

    def test_server_copy_includes_themes_and_repairs_missing_bundle(self):
        remote = load_remote()

        def local_ssh(host, script, *args, check=True):
            return subprocess.run(["sh", "-s", "--", *args], input=script,
                                  text=True, capture_output=True, check=check)

        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(remote, "remote_config_dir",
                                  return_value=str(Path(temporary) / "config")), \
                mock.patch.object(remote, "run_ssh", side_effect=local_ssh), \
                mock.patch.object(remote, "ssh_command",
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
            server = remote.copy_server("example", binary, str(root / "remote bin"))
            themes = root / "config/themes"
            self.assertEqual((themes / "Test Theme").read_text(),
                             "background = 123456\n")
            command.reset_mock()
            remote.provision_themes("example", binary)
            command.assert_not_called()
            shutil.rmtree(themes)
            remote.provision_themes("example", binary)
            self.assertTrue((themes / "Test Theme").is_file())

    def test_download_is_verified_and_cached(self):
        remote = load_remote()
        with tempfile.TemporaryDirectory() as release_temp:
            with tempfile.TemporaryDirectory() as cache_temp:
                release = Path(release_temp)
                cache = Path(cache_temp)
                payload = b"published gmux binary\n"
                (release / "gmux").write_bytes(payload)
                digest = hashlib.sha256(payload).hexdigest()
                (release / "SHA256SUMS").write_text(f"{digest}  gmux\n")
                with mock.patch.object(remote, "RELEASE_BASE",
                                       release.as_uri()), \
                        mock.patch.object(remote, "cache_dir",
                                          return_value=cache), \
                        mock.patch("builtins.print") as output:
                    downloaded = remote.release_asset(
                        "gmux", executable=True)
                    self.assertEqual(downloaded.read_bytes(), payload)
                    self.assertEqual(
                        downloaded.stat().st_mode & 0o777, 0o700)
                    self.assertTrue(output.called)
                    output.reset_mock()
                    (release / "gmux").unlink()
                    self.assertEqual(
                        remote.release_asset("gmux", executable=True),
                        downloaded)
                    output.assert_not_called()

    def test_checksum_redirect_does_not_change_asset_endpoint(self):
        remote = load_remote()
        payload = b"published gmux binary\n"
        digest = hashlib.sha256(payload).hexdigest()
        checksums = io.BytesIO(f"{digest}  gmux\n".encode())
        checksums.geturl = lambda: (
            "https://release-assets.githubusercontent.com/123/checksum-object?signature=abc")
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(remote, "cache_dir", return_value=Path(temporary)), \
                mock.patch.object(remote, "urlopen",
                                  side_effect=[checksums, io.BytesIO(payload)]) as download, \
                mock.patch("builtins.print"):
            self.assertEqual(remote.release_asset("gmux").read_bytes(), payload)
        self.assertEqual(download.call_args_list, [
            mock.call(f"{remote.RELEASE_BASE}/SHA256SUMS", timeout=5),
            mock.call(f"{remote.RELEASE_BASE}/gmux", timeout=60),
        ])

    def test_existing_remote_terminfo_is_not_copied(self):
        remote = load_remote()
        responses = [
            SimpleNamespace(stdout="/tmp/gmux-terminfo\n", returncode=0),
            SimpleNamespace(stdout="", returncode=0),
        ]
        with mock.patch.object(remote, "run_ssh",
                               side_effect=responses) as run_ssh, \
                mock.patch.object(remote, "release_asset") as download:
            remote.provision_terminfo("example")
        self.assertEqual(run_ssh.call_count, 2)
        download.assert_not_called()

    @unittest.skipUnless(shutil.which("tic") and shutil.which("infocmp"),
                         "requires ncurses terminfo tools")
    def test_terminfo_creates_missing_parents_and_skips_existing_entry(self):
        remote = load_remote()

        # Replace only SSH transport: execute the actual provisioning commands
        # and tic locally, including main()'s pre-discovered-directory path.
        def local_ssh(host, script, *args, check=True):
            return subprocess.run(["sh", "-s", "--", *args], input=script,
                                  text=True, capture_output=True, check=check)

        for checked in (False, True):
            with self.subTest(checked=checked), \
                    tempfile.TemporaryDirectory() as temporary, \
                    mock.patch.object(remote, "run_ssh", side_effect=local_ssh), \
                    mock.patch.object(remote, "ssh_command",
                                      side_effect=lambda host, command:
                                      ["sh", "-c", command]) as command:
                directory = Path(temporary) / "missing parent" / "gmux" / "terminfo"
                # An old Kitty entry must not make Ghostty provisioning skip.
                if not checked:
                    directory.mkdir(parents=True, mode=0o700)
                    (directory / "x").mkdir(mode=0o700)
                    (directory / "x/xterm-kitty").write_bytes(b"old entry")
                remote.provision_terminfo("example", str(directory), checked=checked)
                self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
                entries = list(directory.glob("*/xterm-ghostty"))
                self.assertEqual(len(entries), 1)
                result = subprocess.run(["infocmp", "-A", str(directory),
                                         "xterm-ghostty"], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                before = entries[0].stat().st_mtime_ns
                command.reset_mock()
                remote.provision_terminfo("example", str(directory))
                command.assert_not_called()
                self.assertEqual(entries[0].stat().st_mtime_ns, before)

    def test_explicit_forward_is_not_cleared(self):
        remote = load_remote()
        remote.SSH_CONTROL = "/private/control"
        command = remote.control_command("-O", "forward", "-L", "a:b")
        self.assertNotIn("ClearAllForwardings=yes", command)
        self.assertEqual(command[1:5], ["-F", "/dev/null", "-S",
                                       "/private/control"])
        self.assertEqual(command[-5:], ["-O", "forward", "-L", "a:b", "gmux"])

    def test_cached_client_starts_without_network(self):
        remote = load_remote()
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            client = cache / "gmux"
            client.write_bytes(b"cached executable")
            client.chmod(0o700)
            with mock.patch.object(remote, "HERE", cache / "elsewhere"), \
                    mock.patch.object(remote.shutil, "which", return_value=None), \
                    mock.patch.object(remote, "cache_dir", return_value=cache), \
                    mock.patch.object(remote, "release_asset") as download:
                self.assertEqual(remote.local_program("gmux"), client)
                download.assert_not_called()

    def test_remote_state_uses_one_ssh_result(self):
        remote = load_remote()
        output = SimpleNamespace(stdout=(
            "/tmp/noise\nserver\t/bin/gmux-server\n"
            "sockets\t/run/user/1/gmux\n"
            "terminfo\t/home/me/.local/share/gmux/terminfo\n"
            "has_terminfo\tyes\n"))
        with mock.patch.object(remote, "run_ssh",
                               return_value=output) as run_ssh:
            state = remote.remote_state("example")
        self.assertEqual(state, ("/bin/gmux-server", "/run/user/1/gmux",
                                 "/home/me/.local/share/gmux/terminfo", True))
        run_ssh.assert_called_once()


class AttachmentTest(unittest.TestCase):
    def test_attachments_own_distinct_private_paths_and_cleanup(self):
        remote = load_remote()
        remote.SSH_CONTROL = "/private/control"
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(remote, "local_dir",
                                  return_value=Path(directory)), \
                mock.patch.object(remote.subprocess, "run") as run, \
                mock.patch.object(remote.subprocess, "Popen") as launch:
            first = remote.start_attachment("host", "/remote", "one", "gmux")
            second = remote.start_attachment("host", "/remote", "two", "gmux")
            self.assertNotEqual(first["directory"].name, second["directory"].name)
            for attachment in (first, second):
                self.assertEqual(Path(attachment["directory"].name).stat().st_mode
                                 & 0o777, 0o700)
            self.assertEqual(launch.call_count, 2)
            remote.stop_attachment(first)
            self.assertFalse(Path(first["directory"].name).exists())
            self.assertTrue(Path(second["directory"].name).exists())
            self.assertIn("cancel", run.call_args.args[0])
            remote.stop_attachment(second)

    def test_failed_client_launch_cancels_forward_and_removes_directory(self):
        remote = load_remote()
        remote.SSH_CONTROL = "/private/control"
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(remote, "local_dir",
                                  return_value=Path(directory)), \
                mock.patch.object(remote.subprocess, "run") as run, \
                mock.patch.object(remote.subprocess, "Popen",
                                  side_effect=OSError("no client")):
            with self.assertRaisesRegex(OSError, "no client"):
                remote.start_attachment("host", "/remote", "one", "gmux")
            self.assertEqual(list(Path(directory).iterdir()), [])
            self.assertIn("cancel", run.call_args.args[0])



if __name__ == "__main__":
    unittest.main()
