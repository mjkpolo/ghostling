"""A small Qt host/session browser; terminal windows remain separate processes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import sys
import threading

from PySide6.QtCore import QObject, QSize, Qt, Signal, Slot
from PySide6.QtGui import QColor, QIcon, QPainter, QPixmap
from PySide6.QtWidgets import (
    QApplication, QBoxLayout, QHBoxLayout, QInputDialog, QLabel, QLineEdit,
    QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QPushButton,
    QMenu, QSystemTrayIcon, QVBoxLayout, QWidget,
)

from .backend import Host
from .ssh_hosts import hosts_from_config
from .desktop import ASSETS, Instance, install_launcher


def host_icon(name):
    """Locally generated, symmetric 5x5 identicon; never contacts a service."""
    digest = hashlib.sha256(name.casefold().encode()).digest()
    pixmap = QPixmap(48, 48)
    pixmap.fill(QColor("#eef1f6"))
    painter = QPainter(pixmap)
    color = QColor.fromHsv(int.from_bytes(digest[:2], "big") % 360, 155, 175)
    for y in range(5):
        for x in range(3):
            if digest[2 + y * 3 + x] & 1:
                for column in {x, 4 - x}:
                    painter.fillRect(4 + column * 8, 4 + y * 8, 8, 8, color)
    painter.end()
    return QIcon(pixmap)


class Results(QObject):
    finished = Signal(str, object, object)
    terminal_closed = Signal(str, int)


class Manager(QMainWindow):
    def __init__(self, config=None, client=None):
        super().__init__()
        self.config = config
        self.client = client
        self.hosts = {}
        self.entries = {}
        self.messages = {}
        self.busy = set()
        self.watching = set()
        self.refresh_pending = set()
        self.closing = False
        self.quitting = False
        self.tray = None
        # SSH never blocks Qt's event loop. A single worker keeps all existing
        # transport operations serialized; each host has its own SSH master.
        self.worker = ThreadPoolExecutor(max_workers=1)
        self.results = Results(self)
        self.results.finished.connect(self.completed)
        self.results.terminal_closed.connect(self.terminal_closed)
        self.setWindowTitle("gmux · Sessions")
        self.setWindowIcon(QIcon(str(ASSETS / "gmux-manager.svg")))
        self.resize(940, 610)
        self.setMinimumSize(360, 440)
        root = QWidget()
        layout = QHBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        sidebar = QWidget()
        self.sidebar = sidebar
        sidebar.setObjectName("sidebar")
        sidebar.setFixedWidth(250)
        left = QVBoxLayout(sidebar)
        self.sidebar_layout = left
        left.setContentsMargins(20, 24, 16, 20)
        brand = QLabel("gmux")
        brand.setObjectName("brand")
        left.addWidget(brand)
        left.addWidget(QLabel("YOUR HOSTS"))
        self.filter = QLineEdit()
        self.filter.setPlaceholderText("Find a host…")
        self.filter.textChanged.connect(self.filter_hosts)
        left.addWidget(self.filter)
        self.host_list = QListWidget()
        self.host_list.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.host_list.setIconSize(QSize(36, 36))
        self.host_list.currentItemChanged.connect(self.show_host)
        self.host_list.itemActivated.connect(lambda _: self.connect_host())
        left.addWidget(self.host_list, 1)
        reload_button = QPushButton("Reload hosts")
        reload_button.setToolTip("Read ~/.ssh/config and included files again")
        reload_button.clicked.connect(self.reload_hosts)
        left.addWidget(reload_button)
        hint = QLabel("Hosts come from ~/.ssh/config.\nEdit that file to add a host.")
        hint.setWordWrap(True)
        hint.setObjectName("hint")
        left.addWidget(hint)
        # Swaybar cannot display Qt's exported D-Bus tray menu. Keep an
        # explicit exit available even when closing the window hides it.
        self.quit_button = QPushButton("Quit manager")
        self.quit_button.clicked.connect(self.quit_manager)
        left.addWidget(self.quit_button)
        layout.addWidget(sidebar)
        pane = QWidget()
        right = QVBoxLayout(pane)
        self.content_layout = right
        right.setContentsMargins(30, 28, 30, 24)
        heading = QHBoxLayout()
        self.title = QLabel("Your sessions")
        self.title.setTextFormat(Qt.TextFormat.PlainText)
        self.title.setObjectName("title")
        heading.addWidget(self.title, 1)
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self.connect_host)
        heading.addWidget(self.connect_button)
        right.addLayout(heading)
        self.status = QLabel("Choose a host to get started.")
        self.status.setTextFormat(Qt.TextFormat.PlainText)
        self.status.setWordWrap(True)
        right.addWidget(self.status)
        self.session_list = QListWidget()
        self.session_list.setTextElideMode(Qt.TextElideMode.ElideRight)
        self.session_list.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.session_list.itemActivated.connect(lambda _: self.open_session())
        self.session_list.currentItemChanged.connect(self.update_actions)
        right.addWidget(self.session_list, 1)
        actions = QHBoxLayout()
        self.actions = actions
        self.new_button = QPushButton("New session")
        self.new_button.clicked.connect(self.new_session)
        self.open_button = QPushButton("Open terminal")
        self.open_button.setObjectName("primary")
        self.open_button.clicked.connect(self.open_session)
        self.delete_button = QPushButton("Delete…")
        self.delete_button.clicked.connect(self.delete_session)
        self.install_button = QPushButton("Install server…")
        self.install_button.clicked.connect(self.install_server)
        actions.addWidget(self.new_button)
        actions.addWidget(self.install_button)
        actions.addStretch()
        actions.addWidget(self.delete_button)
        actions.addWidget(self.open_button)
        right.addLayout(actions)
        layout.addWidget(pane, 1)
        self.setCentralWidget(root)
        self.setStyleSheet("""
            QWidget { font-size: 14px; }
            QMainWindow { background: #fafbfe; }
            #sidebar { background: #edf0f6; border-right: 1px solid #dbe0eb; }
            QLabel { color: #25334b; background: transparent; }
            #brand { font-size: 30px; font-weight: 700; margin-bottom: 20px; }
            #title { font-size: 25px; font-weight: 600; }
            #hint { font-size: 12px; color: #65738a; margin-top: 12px; }
            QLineEdit { background: white; color: #25334b; border: 1px solid #d7deea;
                        border-radius: 6px; padding: 9px; }
            QListWidget { background: transparent; color: #25334b; border: none;
                          outline: none; margin-top: 12px; }
            QListWidget::item { padding: 12px 8px; margin-bottom: 5px; border-radius: 7px; }
            QListWidget::item:selected { background: #dce5fa; color: #193875; }
            QListWidget::item:hover { background: #e5eaf3; }
            QPushButton { color: #25334b; background: #ffffff; border: 1px solid #d7deea;
                          border-radius: 6px; padding: 9px 13px; }
            QPushButton:hover { background: #eaf0fc; }
            QPushButton:disabled { color: #949cad; background: #f2f4f8; }
            #primary { background: #345cba; color: white; border: none; }
            #primary:disabled { background: #a6b5d8; }
        """)
        self.reload_hosts()

    def enable_tray(self):
        self.tray = QSystemTrayIcon(self.windowIcon(), self)
        self.tray.setToolTip("gmux Manager")
        menu = QMenu(self)
        menu.addAction("Show manager", self.show_manager)
        menu.addSeparator()
        menu.addAction("Quit", self.quit_manager)
        self.tray.setContextMenu(menu)
        self.tray.activated.connect(self.tray_activated)
        self.tray.show()

    def show_manager(self):
        self.showNormal()
        self.raise_()
        self.activateWindow()

    def tray_activated(self, reason):
        if reason in (QSystemTrayIcon.ActivationReason.Trigger,
                      QSystemTrayIcon.ActivationReason.DoubleClick):
            self.show_manager()

    def quit_manager(self):
        self.quitting = True
        self.close()
        if not self.closing:
            self.quitting = False
        else:
            QApplication.instance().quit()

    def resizeEvent(self, event):
        if hasattr(self, "actions"):
            compact = self.width() < 700
            self.sidebar.setFixedWidth(150 if compact else 250)
            self.sidebar_layout.setContentsMargins(8 if compact else 20, 24,
                                                   8 if compact else 16, 20)
            self.content_layout.setContentsMargins(12 if compact else 30, 28,
                                                   12 if compact else 30, 24)
            self.actions.setDirection(QBoxLayout.Direction.TopToBottom if compact
                                      else QBoxLayout.Direction.LeftToRight)
        super().resizeEvent(event)

    def current_host(self):
        item = self.host_list.currentItem()
        return item.text() if item else None

    def filter_hosts(self, text):
        for index in range(self.host_list.count()):
            item = self.host_list.item(index)
            item.setHidden(text.casefold() not in item.text().casefold())

    def reload_hosts(self):
        previous = self.current_host()
        try:
            names = hosts_from_config(self.config)
        except (OSError, ValueError) as error:
            self.status.setText(f"Could not read SSH config: {error}")
            return
        self.host_list.clear()
        for name in names:
            item = QListWidgetItem(host_icon(name), name)
            self.host_list.addItem(item)
            if name == previous:
                self.host_list.setCurrentItem(item)
        self.filter_hosts(self.filter.text())
        if not names:
            self.status.setText("No named hosts found. Add a Host entry to ~/.ssh/config, then reload.")
        self.update_actions()

    def show_host(self, *_):
        name = self.current_host()
        self.title.setText(name or "Your sessions")
        self.session_list.clear()
        for state, session in self.entries.get(name, []):
            label = {"live": "Available", "attached": "Attached", "stale": "Stale socket"}.get(state, state)
            item = QListWidgetItem(f"{session}\n{label}")
            item.setToolTip(session)
            item.setData(Qt.ItemDataRole.UserRole, (state, session))
            self.session_list.addItem(item)
        self.status.setText(self.messages.get(name, "Connect to discover sessions on this host."))
        self.update_actions()

    def update_actions(self, *_):
        name = self.current_host()
        host = self.hosts.get(name)
        idle = bool(name) and name not in self.busy
        ready = idle and host is not None and host.server is not None
        self.connect_button.setEnabled(idle)
        self.connect_button.setText("Refresh" if name in self.entries else "Connect")
        self.new_button.setEnabled(ready)
        selected = self.session_list.currentItem()
        state = selected.data(Qt.ItemDataRole.UserRole)[0] if selected else None
        self.open_button.setEnabled(ready and state == "live")
        self.delete_button.setEnabled(ready and selected is not None)
        self.install_button.setVisible(bool(idle and host and host.connection and not host.server))

    def submit(self, name, action, *args):
        if not name or name in self.busy:
            return
        if name not in self.hosts:
            self.hosts[name] = Host(name, self.client)
        host = self.hosts[name]
        self.busy.add(name)
        self.messages[name] = f"{action.replace('_', ' ').capitalize()}…"
        self.show_host()
        future = self.worker.submit(getattr(host, action), *args)

        def done(result):
            try:
                entries = result.result()
            except Exception as error:
                self.results.finished.emit(name, None, str(error))
            else:
                self.results.finished.emit(name, entries, None)
        future.add_done_callback(done)

    @Slot(str, object, object)
    def completed(self, name, entries, error):
        self.busy.discard(name)
        # Each child gets one OS wait, not a timer or a remote polling loop.
        for attachment in self.hosts[name].attachments:
            process = attachment["client"]
            key = (name, process.pid)
            if key not in self.watching:
                self.watching.add(key)
                threading.Thread(target=self.wait_terminal, args=(name, process),
                                 daemon=True).start()
        if error:
            self.messages[name] = str(error)
        else:
            self.entries[name] = entries
            self.messages[name] = (f"Connected · {len(entries)} session(s)" if self.hosts[name].server
                                   else "Connected. gmux-server is missing; choose Install server.")
        if name == self.current_host():
            self.show_host()
        if name in self.refresh_pending:
            self.refresh_pending.discard(name)
            self.submit(name, "refresh")

    def wait_terminal(self, name, process):
        process.wait()
        if not self.closing:
            self.results.terminal_closed.emit(name, process.pid)

    @Slot(str, int)
    def terminal_closed(self, name, pid):
        self.watching.discard((name, pid))
        if self.closing:
            return
        if name in self.busy:
            self.refresh_pending.add(name)
        else:
            self.submit(name, "refresh")

    def connect_host(self):
        self.submit(self.current_host(), "connect")

    def new_session(self):
        name = self.current_host()
        session, accepted = QInputDialog.getText(self, "New session", "Session name")
        if accepted and session:
            self.submit(name, "create", session)

    def open_session(self):
        item = self.session_list.currentItem()
        if item and item.data(Qt.ItemDataRole.UserRole)[0] == "live":
            self.submit(self.current_host(), "open", item.data(Qt.ItemDataRole.UserRole)[1])

    def delete_session(self):
        item = self.session_list.currentItem()
        if not item:
            return
        name = self.current_host()
        state, session = item.data(Qt.ItemDataRole.UserRole)
        message = QMessageBox(self)
        message.setTextFormat(Qt.TextFormat.PlainText)
        message.setWindowTitle("Delete session")
        message.setText(f"Delete {session} on {name}?\n"
                        + ("Only the stale socket will be removed." if state == "stale"
                           else "This terminates the session and its running applications."))
        message.setStandardButtons(QMessageBox.StandardButton.Cancel | QMessageBox.StandardButton.Yes)
        message.setDefaultButton(QMessageBox.StandardButton.Cancel)
        if message.exec() == QMessageBox.StandardButton.Yes:
            self.submit(name, "delete", session)

    def install_server(self):
        name = self.current_host()
        destination, accepted = QInputDialog.getText(
            self, "Install server", f"Download verified gmux-server and install on {name}.\nRemote directory:",
            text="~/.local/bin")
        if accepted and destination.strip():
            self.submit(name, "install_server", destination.strip())

    def closeEvent(self, event):
        if self.tray and QSystemTrayIcon.isSystemTrayAvailable() and not self.quitting:
            self.hide()
            event.ignore()
            return
        if self.busy:
            self.status.setText("Please wait for the current operation before quitting.")
            event.ignore()
            return
        if any(host.attachments for host in self.hosts.values()):
            answer = QMessageBox.question(self, "Quit manager?",
                "Close this manager's terminal windows and SSH connections? Server sessions will keep running.",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel,
                QMessageBox.StandardButton.Cancel)
            if answer != QMessageBox.StandardButton.Yes:
                event.ignore()
                return
        # No tasks remain. Cleanup happens on the worker after the window
        # closes, never concurrently with an SSH operation.
        self.closing = True
        if self.tray:
            self.tray.hide()
        for host in self.hosts.values():
            self.worker.submit(host.close)
        self.worker.shutdown(wait=False)
        event.accept()
        # main() disables quit-on-last-window so hiding can preserve sessions.
        if self.tray:
            QApplication.instance().quit()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", help="terminal executable (skip release downloads)")
    parser.add_argument("--install-desktop", action="store_true",
                        help="install a Linux application launcher for this Python environment")
    args = parser.parse_args()
    if args.install_desktop:
        print(install_launcher())
        return 0
    app = QApplication(sys.argv[:1])
    app.setApplicationName("gmux-manager")
    app.setDesktopFileName("gmux-manager")
    instance = Instance(app)
    if instance.activate_existing():
        return 0
    window = Manager(client=args.client)
    if not instance.listen(window.show_manager):
        window.close()
        return 0
    app.aboutToQuit.connect(instance.close)
    window.enable_tray()
    app.setQuitOnLastWindowClosed(False)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
