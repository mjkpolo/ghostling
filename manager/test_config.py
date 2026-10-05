import unittest
from unittest import mock

from .config_editor import RemoteConfig, config_text, config_values


class ConfigTest(unittest.TestCase):
    def test_comments_and_unknown_options_survive(self):
        original = "# keep me\nother = yes\nfont-size = 12\n"
        values = config_values(original)
        values["font-size"] = "30"
        text = config_text(original, values)
        self.assertIn("# keep me\nother = yes\nfont-size = 30\n", text)

    def test_preview_lifecycle(self):
        for attached in (False, True):
            with self.subTest(attached=attached):
                host = mock.Mock(name="host")
                host.name, host.server, host.directory = "example", "/bin/server", "/sockets"
                host.refresh.return_value = [("attached" if attached else "live", "work")]
                host.api.remote_config_dir.return_value = "/config/gmux"
                host.api.local_program.return_value = "/bin/gmux"
                host.api.run_ssh.side_effect = [mock.Mock(stdout="font-size = 24\n"),
                    mock.Mock(stdout="Theme One\n"), mock.Mock(stdout="/sockets/preview.unique\n")]
                remote = RemoteConfig(host)
                with mock.patch("manager.config_editor.subprocess.run", return_value=mock.Mock(stdout="monospace\n")):
                    values, _, _ = remote.load()
                self.assertEqual(host.api.create_session.called, not attached)
                host.api.run_ssh.side_effect = None
                values["font-size"] = "30"
                remote.update(values)
                self.assertIn("font-size = 30", host.api.run_ssh.call_args.args[-1])
                remote.finish(False)
                calls = host.api.run_ssh.call_args_list
                self.assertTrue(any(call.args[-1] == "font-size = 24\n" for call in calls))
                self.assertEqual(host.api.delete_session.called, not attached)
                self.assertIsNone(remote.preview)
