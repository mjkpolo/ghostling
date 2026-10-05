"""Manager tests need no Qt, SSH connection, or changes to real SSH config."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from manager.ssh_hosts import hosts_from_config
from manager.backend import Host


class HostsTest(unittest.TestCase):
    def test_aliases_includes_comments_and_cycles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / "config"
            original = '# keep me\nHost alpha beta *.example !excluded\nInclude "hosts/*.conf"\nhost=ALPHA gamma # comment\nMatch all\n'
            config.write_text(original)
            (root / "hosts").mkdir()
            (root / "hosts/more.conf").write_text('Host delta\nInclude config\n')
            self.assertEqual(hosts_from_config(config), ["alpha", "beta", "delta", "gamma"])
            self.assertEqual(config.read_text(), original)
            (root / "hosts/more.conf").write_text('Host epsilon\n')
            self.assertEqual(hosts_from_config(config), ["alpha", "beta", "epsilon", "gamma"])

    def test_missing_and_malformed_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config"
            self.assertEqual(hosts_from_config(config), [])
            self.assertFalse(config.exists())
            config.write_text('Host "unterminated\n')
            with self.assertRaises(ValueError):
                hosts_from_config(config)

    def test_hosts_have_independent_control_state(self):
        a, b = Host("alpha"), Host("beta")
        a.api.SSH_CONTROL = "/tmp/alpha"
        b.api.SSH_CONTROL = "/tmp/beta"
        self.assertIn("/tmp/alpha", a.api.ssh_command("alpha"))
        self.assertNotIn("/tmp/beta", a.api.ssh_command("alpha"))
        self.assertIn("/tmp/beta", b.api.ssh_command("beta"))

    def test_connect_reuses_master_and_provisions_only_missing_terminfo(self):
        host = Host("alpha")
        with mock.patch.object(host.api, "ssh_connection") as connection, \
             mock.patch.object(host.api, "remote_state", return_value=("server", "/sockets", "/terms", True)), \
             mock.patch.object(host.api, "sessions", return_value=[("live", "work")]), \
             mock.patch.object(host.api, "provision_terminfo") as provision:
            self.assertEqual(host.connect(), [("live", "work")])
            host.connect()
            connection.assert_called_once_with("alpha", batch_mode=True)
            provision.assert_not_called()
            host.close()
            connection.return_value.__exit__.assert_called_once()

    def test_gui_ssh_prompts_only_for_initial_authentication(self):
        host = Host("alpha")
        with tempfile.TemporaryDirectory() as temporary, \
             mock.patch.object(host.api, "local_dir", return_value=Path(temporary)), \
             mock.patch.object(host.api.subprocess, "run", return_value=mock.Mock(returncode=0)) as run:
            with host.api.ssh_connection("alpha", batch_mode=True):
                command = run.call_args.args[0]
                self.assertLess(command.index("StrictHostKeyChecking=ask"),
                                command.index("StrictHostKeyChecking=yes"))
                self.assertLess(command.index("BatchMode=no"), command.index("BatchMode=yes"))
                env = run.call_args.kwargs["env"]
                self.assertEqual(env["SSH_ASKPASS_REQUIRE"], "force")
                helper = Path(env["SSH_ASKPASS"])
                self.assertEqual(helper.stat().st_mode & 0o777, 0o700)
                self.assertIn("BatchMode=yes", host.api.ssh_command("alpha", "true"))
            self.assertFalse(helper.exists())
            self.assertIsNone(host.api.SSH_CONTROL)
            self.assertEqual(host.api.SSH_OPTIONS, [])

    def test_install_downloads_release_and_uses_remote_directory(self):
        host = Host("alpha")
        with mock.patch.object(host.api, "run_ssh", return_value=mock.Mock(stdout="Linux x86_64\n")), \
             mock.patch.object(host.api, "release_asset", return_value=Path("/cache/gmux-server")) as download, \
             mock.patch.object(host.api, "copy_server", return_value="/remote/bin/gmux-server") as copy, \
             mock.patch.object(host, "connect", return_value=[]) as connect:
            self.assertEqual(host.install_server("~/.local/bin"), [])
            download.assert_called_once_with("gmux-server", executable=True)
            copy.assert_called_once_with("alpha", Path("/cache/gmux-server"),
                                         "~/.local/bin", release_themes=True)
            connect.assert_called_once()

    def test_install_rejects_unsupported_remote_before_download(self):
        host = Host("alpha")
        with mock.patch.object(host.api, "run_ssh", return_value=mock.Mock(stdout="Linux aarch64\n")), \
             mock.patch.object(host.api, "release_asset") as download:
            with self.assertRaisesRegex(RuntimeError, "Linux aarch64"):
                host.install_server("~/.local/bin")
            download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
