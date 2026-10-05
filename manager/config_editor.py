"""Edit the host's shared config using its existing live-reload mechanism."""
import subprocess

from PySide6.QtCore import Signal, Qt
from PySide6.QtWidgets import QComboBox, QDialog, QDialogButtonBox, QFormLayout, QLabel, QSpinBox, QVBoxLayout

DEFAULTS = {"font": "Monaspace Argon Frozen, monospace", "font-size": "24", "theme": ""}


def config_values(text):
    values = dict(DEFAULTS)
    for line in text.splitlines():
        key, equals, value = line.partition("=")
        if equals and key.strip() in values:
            values[key.strip()] = value.strip()
    return values


def config_text(original, values):
    lines, seen = [], set()
    for line in original.splitlines():
        key, equals, _ = line.partition("=")
        key = key.strip()
        if equals and key in values:
            line = f"{key} = {values[key]}"
            seen.add(key)
        lines.append(line)
    lines.extend(f"{key} = {value}" for key, value in values.items() if key not in seen)
    return "\n".join(lines) + "\n"


class RemoteConfig:
    def __init__(self, host):
        self.host = host
        self.preview = self.attachment = None
        self.original = None
        self.changed = False

    def load(self):
        host, api = self.host, self.host.api
        directory = api.remote_config_dir(host.name)
        self.path = directory + "/config"
        self.original = api.run_ssh(host.name,
            'if [ -f "$1" ]; then cat -- "$1"; fi', self.path).stdout
        themes = api.run_ssh(host.name, '''
for path in "$1/themes"/*; do
    [ -f "$path" ] || continue
    [ "${path##*/}" = LICENSE ] || printf '%s\\n' "${path##*/}"
done
''', directory).stdout.splitlines()
        client = host.client_program()
        fonts = subprocess.run([client, "--list-fonts"], check=True,
                               capture_output=True, text=True).stdout.splitlines()
        if not any(state == "attached" for state, _ in host.refresh()):
            # A private subdirectory keeps the disposable socket out of the
            # normal session list. It still reads the host's shared config.
            self.preview = api.run_ssh(host.name,
                'umask 077; mktemp -d "$1/preview.XXXXXX"', host.directory).stdout.strip()
            api.create_session(host.name, host.server, self.preview, "preview")
            self.attachment = api.start_attachment(host.name, self.preview, "preview", client)
        return config_values(self.original), fonts, themes

    def write(self, text):
        self.host.api.run_ssh(self.host.name, '''
set -eu
umask 077
temporary=$(mktemp "$1.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT
printf '%s' "$2" > "$temporary"
mv -f -- "$temporary" "$1"
''', self.path, text)

    def update(self, values):
        # Mark first: a lost SSH reply does not prove the rename failed.
        self.changed = True
        self.write(config_text(self.original, values))

    def finish(self, save):
        if not save and self.changed:
            self.write(self.original)
            self.changed = False
        if self.attachment:
            self.host.api.close_attachment(self.attachment)
            self.attachment = None
        if self.preview:
            self.host.api.delete_session(self.host.name, self.host.server, self.preview, "preview")
            # Kill acknowledges before the daemon necessarily unlinks its
            # socket. Only remove the socket in our private preview directory.
            self.host.api.run_ssh(self.host.name,
                'rm -f -- "$1/preview.sock"; rmdir -- "$1"', self.preview)
            self.preview = None


class ConfigEditor(QDialog):
    completed = Signal(object, object)

    def __init__(self, parent, host):
        super().__init__(parent)
        self.remote = RemoteConfig(host)
        self.worker = parent.worker
        self.pending = False
        self.finishing = None
        self.loaded = False
        self.setWindowTitle(f"Config · {host.name}")
        self.resize(520, 240)
        layout = QVBoxLayout(self)
        self.status = QLabel("Loading host configuration…")
        self.status.setTextFormat(Qt.TextFormat.PlainText)
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        form = QFormLayout()
        self.font, self.theme = QComboBox(), QComboBox()
        for widget in (self.font, self.theme):
            widget.setEditable(True)
            widget.completer().setFilterMode(Qt.MatchFlag.MatchContains)
        self.size = QSpinBox()
        self.size.setRange(6, 96)
        form.addRow("Font", self.font)
        form.addRow("Font size", self.size)
        form.addRow("Theme", self.theme)
        layout.addLayout(form)
        self.buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
        self.buttons.accepted.connect(lambda: self.finish(True))
        self.buttons.rejected.connect(self.reject)
        layout.addWidget(self.buttons)
        self.completed.connect(self.done_work)
        self.run(self.remote.load)

    def run(self, action, *args):
        self.pending = True
        for widget in (self.font, self.theme, self.size, self.buttons):
            widget.setEnabled(False)
        future = self.worker.submit(action, *args)
        def done(result):
            try:
                value = result.result()
            except Exception as error:
                self.completed.emit(None, str(error))
            else:
                self.completed.emit(value, None)
        future.add_done_callback(done)

    def done_work(self, result, error):
        self.pending = False
        for widget in (self.font, self.theme, self.size, self.buttons):
            widget.setEnabled(True)
        if error:
            self.status.setText(error)
            self.finishing = None
            self.buttons.button(QDialogButtonBox.StandardButton.Save).setEnabled(self.loaded)
            return
        if self.finishing is not None:
            super().done(QDialog.DialogCode.Accepted if self.finishing else QDialog.DialogCode.Rejected)
            return
        if result is not None:
            self.loaded = True
            values, fonts, themes = result
            self.font.addItems(sorted(set(fonts + [values["font"], "monospace"])))
            self.theme.addItems([""] + sorted(set(themes + [values["theme"]]) - {""}))
            self.font.setCurrentText(values["font"])
            self.theme.setCurrentText(values["theme"])
            try:
                self.size.setValue(int(values["font-size"]))
            except ValueError:
                self.size.setValue(24)
            self.font.activated.connect(self.preview)
            self.theme.activated.connect(self.preview)
            self.font.lineEdit().editingFinished.connect(self.preview)
            self.theme.lineEdit().editingFinished.connect(self.preview)
            self.size.editingFinished.connect(self.preview)
        self.status.setText("Live preview affects this host's sessions. Save keeps changes; Cancel restores the original config.")

    def preview(self, *_):
        if not self.pending:
            self.run(self.remote.update, {"font": self.font.currentText(),
                "font-size": str(self.size.value()), "theme": self.theme.currentText()})

    def finish(self, save):
        if not self.pending:
            self.finishing = save
            values = {"font": self.font.currentText(),
                      "font-size": str(self.size.value()), "theme": self.theme.currentText()}
            def finish():
                if save:
                    self.remote.update(values)
                self.remote.finish(save)
            self.run(finish)

    def reject(self):
        self.finish(False)

    def closeEvent(self, event):
        event.ignore()
        self.reject()
