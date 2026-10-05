"""Desktop launcher installation and single-instance activation."""
import os
from pathlib import Path
import sys

from PySide6.QtCore import QLockFile, QObject
from PySide6.QtNetwork import QLocalServer, QLocalSocket

from .remote import local_dir


ASSETS = Path(__file__).with_name("assets")


def install_launcher():
    if sys.platform != "linux":
        raise RuntimeError("Desktop files are Linux-specific; use gmux-manager on macOS.")
    data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    applications = data / "applications"
    icons = data / "icons/hicolor/scalable/apps"
    applications.mkdir(parents=True, exist_ok=True)
    icons.mkdir(parents=True, exist_ok=True)
    # Absolute interpreter path works for venv/pipx installs even when the
    # desktop launcher's PATH does not contain that environment's bin folder.
    executable = sys.executable.replace("%", "%%")
    for char in ('\\', '"', '`', '$'):
        executable = executable.replace(char, '\\' + char)
    executable = executable.replace("\\", "\\\\")  # Desktop-entry string escaping
    module = __package__.split(".")[0]
    text = (ASSETS / "gmux-manager.desktop").read_text()
    text = text.replace("Exec=gmux-manager", f'Exec="{executable}" -m {module}.app')
    if module == "manager":
        raise RuntimeError("Install the package with pip before installing its desktop launcher.")
    target = applications / "gmux-manager.desktop"
    target.write_text(text)
    (icons / "gmux-manager.svg").write_bytes((ASSETS / "gmux-manager.svg").read_bytes())
    return target


class Instance(QObject):
    """Only the lock owner can remove a stale activation socket."""
    def __init__(self, parent=None, directory=None):
        super().__init__(parent)
        self.path = str((directory or local_dir()) / ".manager-ui")
        self.lock = QLockFile(self.path + ".lock")
        self.lock.setStaleLockTime(0)
        self.server = QLocalServer(self)
        self.server.setSocketOptions(QLocalServer.SocketOption.UserAccessOption)

    def activate_existing(self):
        socket = QLocalSocket()
        socket.connectToServer(self.path)
        if not socket.waitForConnected(500):
            return False
        socket.disconnectFromServer()
        return True

    def listen(self, show):
        if not self.lock.tryLock(0):
            # Another instance may still be starting its listener.
            if self.activate_existing():
                return False
            raise RuntimeError("The gmux manager is already starting or not responding.")
        QLocalServer.removeServer(self.path)
        if not self.server.listen(self.path):
            self.lock.unlock()
            raise RuntimeError(self.server.errorString())

        def activated():
            while self.server.hasPendingConnections():
                connection = self.server.nextPendingConnection()
                connection.disconnectFromServer()
                connection.deleteLater()
            show()
        self.server.newConnection.connect(activated)
        return True

    def close(self):
        self.server.close()
        self.lock.unlock()
