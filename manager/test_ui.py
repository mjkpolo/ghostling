"""Run with QT_QPA_PLATFORM=offscreen python -m unittest manager.test_ui."""
import tempfile
import time
from pathlib import Path
import unittest
import subprocess
import sys
from unittest import mock

from PySide6.QtWidgets import QApplication
from PySide6.QtCore import Qt
from .app import Manager, host_icon
from .desktop import Instance
from . import askpass


class ManagerTest(unittest.TestCase):
    def test_askpass_password_and_cancel(self):
        with mock.patch.dict("os.environ", {"SSH_ASKPASS_PROMPT": ""}), \
             mock.patch.object(askpass, "QInputDialog") as dialog:
            dialog.return_value.exec.return_value = 1
            dialog.return_value.textValue.return_value = "test-only"
            self.assertEqual(askpass.prompt("Password:"), "test-only")
            dialog.return_value.setTextEchoMode.assert_called_once_with(
                askpass.QLineEdit.EchoMode.Password)
            dialog.return_value.exec.return_value = 0
            self.assertIsNone(askpass.prompt("Password:"))

    def test_askpass_host_confirmation_defaults_to_no(self):
        with mock.patch.dict("os.environ", {"SSH_ASKPASS_PROMPT": "confirm"}), \
             mock.patch.object(askpass.QMessageBox, "exec", return_value=askpass.QMessageBox.StandardButton.No):
            self.assertIsNone(askpass.prompt("Test fingerprint"))
        with mock.patch.dict("os.environ", {"SSH_ASKPASS_PROMPT": "confirm"}), \
             mock.patch.object(askpass.QMessageBox, "exec", return_value=askpass.QMessageBox.StandardButton.Yes):
            self.assertEqual(askpass.prompt("Test fingerprint"), "yes")

    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def test_icons_and_read_only_reload(self):
        self.assertEqual(host_icon("alpha").pixmap(48).toImage(),
                         host_icon("ALPHA").pixmap(48).toImage())
        self.assertNotEqual(host_icon("alpha").pixmap(48).toImage(),
                            host_icon("beta").pixmap(48).toImage())
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config"
            config.write_text("Host alpha beta\n")
            window = Manager(config=config)
            self.assertEqual(window.host_list.count(), 3)
            window.filter.setText("beta")
            self.assertTrue(window.host_list.item(0).isHidden())
            window.host_list.setCurrentRow(1)
            self.assertEqual(window.title.text(), "beta")
            config.write_text("Host beta gamma\n")
            window.reload_hosts()
            self.assertEqual(window.current_host(), "beta")
            self.assertEqual(config.read_text(), "Host beta gamma\n")
            window.close()

    def test_async_results_stay_with_their_host(self):
        class FakeHost:
            def __init__(self, name, client):
                self.name, self.server, self.connection = name, "server", True
                self.attachments = []
                self.catalog = {"folders": {}, "sessions": {}}
            def folder(self, identity):
                return ""
            def label(self, identity):
                return identity
            def connect(self):
                time.sleep(0.03)
                return [("live", self.name + "-work")]
            def close(self):
                pass
        with tempfile.TemporaryDirectory() as temporary, \
             mock.patch("manager.app.Host", FakeHost):
            config = Path(temporary) / "config"
            config.write_text("Host alpha beta\n")
            window = Manager(config=config)
            window.host_list.setCurrentRow(0)
            window.connect_button.click()
            window.host_list.setCurrentRow(1)
            window.connect_button.click()
            deadline = time.monotonic() + 3
            while window.busy and time.monotonic() < deadline:
                self.app.processEvents()
                time.sleep(0.01)
            self.assertFalse(window.busy)
            self.assertEqual(window.title.text(), "beta")
            self.assertEqual(window.entries["alpha"], [("live", "alpha-work")])
            self.assertEqual(window.session_list.item(0).data(Qt.ItemDataRole.UserRole),
                             ("live", "beta-work"))
            window.close()

    def test_install_prompts_for_remote_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config"
            config.write_text("Host alpha\n")
            window = Manager(config=config)
            window.host_list.setCurrentRow(0)
            with mock.patch("manager.app.QInputDialog.getText", return_value=("~/bin", True)) as prompt, \
                 mock.patch.object(window, "submit") as submit:
                window.install_server()
                self.assertEqual(prompt.call_args.kwargs["text"], "~/.local/bin")
                submit.assert_called_once_with("alpha", "install_server", "~/bin")
            window.close()

    def test_child_exit_triggers_one_refresh_without_polling(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config"
            config.write_text("Host alpha\n")
            window = Manager(config=config)
            window.host_list.setCurrentRow(0)
            process = subprocess.Popen([sys.executable, "-c", "input()"], stdin=subprocess.PIPE)
            host = mock.Mock(server="server", attachments=[{"client": process}])
            host.catalog = {"folders": {}, "sessions": {}}
            host.folder.return_value = ""
            host.label.side_effect = lambda identity: identity
            window.hosts["alpha"] = host
            try:
                with mock.patch.object(window, "submit") as submit, \
                     mock.patch.object(process, "poll", side_effect=AssertionError("must not poll")):
                    window.completed("alpha", [("attached", "work")], None)
                    self.app.processEvents()
                    submit.assert_not_called()
                    process.stdin.write(b"done\n")
                    process.stdin.flush()
                    deadline = time.monotonic() + 3
                    while not submit.called and time.monotonic() < deadline:
                        self.app.processEvents()
                        time.sleep(0.01)
                    submit.assert_called_once_with("alpha", "refresh")
                    self.assertEqual(process.returncode, 0)
            finally:
                process.stdin.close()
                process.wait(timeout=3)
                host.attachments = []
                window.close()

    def test_exit_during_operation_queues_refresh(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config"
            window = Manager(config=config)
            window.hosts["alpha"] = mock.Mock(server="server", attachments=[])
            window.busy.add("alpha")
            with mock.patch.object(window, "submit") as submit:
                window.terminal_closed("alpha", 123)
                submit.assert_not_called()
                window.completed("alpha", [], None)
                submit.assert_called_once_with("alpha", "refresh")
            window.close()

    def test_close_hides_to_tray_and_show_restores_window(self):
        with tempfile.TemporaryDirectory() as temporary, \
             mock.patch("manager.app.QSystemTrayIcon.isSystemTrayAvailable", return_value=True):
            window = Manager(config=Path(temporary) / "config")
            window.tray = mock.Mock()
            window.show()
            self.app.processEvents()
            window.close()
            self.assertFalse(window.isVisible())
            self.assertFalse(window.closing)
            window.show_manager()
            self.assertTrue(window.isVisible())
            window.quit_button.click()
            self.assertTrue(window.closing)

    def test_no_tray_quits_instead_of_hiding(self):
        with tempfile.TemporaryDirectory() as temporary, \
             mock.patch("manager.app.QSystemTrayIcon.isSystemTrayAvailable", return_value=False):
            window = Manager(config=Path(temporary) / "config")
            window.tray = mock.Mock()
            window.close()
            self.assertTrue(window.closing)

    def test_single_instance_activation_and_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            first, second = Instance(directory=directory), Instance(directory=directory)
            show = mock.Mock()
            self.assertFalse(first.activate_existing())
            self.assertTrue(first.listen(show))
            try:
                self.assertTrue(second.activate_existing())
                deadline = time.monotonic() + 1
                while not show.called and time.monotonic() < deadline:
                    self.app.processEvents()
                show.assert_called_once()
                self.assertFalse(second.lock.tryLock(0))
            finally:
                first.close()
            self.assertTrue(second.listen(mock.Mock()))
            second.close()


if __name__ == "__main__":
    unittest.main()
