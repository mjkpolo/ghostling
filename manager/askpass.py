"""OpenSSH askpass: the answer goes only to SSH's private stdout pipe."""
import os
import sys

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication, QInputDialog, QLineEdit, QMessageBox


def prompt(text):
    if os.environ.get("SSH_ASKPASS_PROMPT") == "confirm":
        dialog = QMessageBox()
        dialog.setWindowTitle("gmux · Verify SSH host")
        dialog.setTextFormat(Qt.TextFormat.PlainText)
        dialog.setText(text)
        dialog.setInformativeText("Verify this fingerprint with the host administrator before accepting.")
        dialog.setStandardButtons(QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        dialog.setDefaultButton(QMessageBox.StandardButton.No)
        return "yes" if dialog.exec() == QMessageBox.StandardButton.Yes else None
    dialog = QInputDialog()
    dialog.setWindowTitle("gmux · SSH authentication")
    dialog.setLabelText(text)
    dialog.setTextEchoMode(QLineEdit.EchoMode.Password)
    return dialog.textValue() if dialog.exec() else None


def main():
    app = QApplication(sys.argv[:1])
    answer = prompt(sys.argv[1] if len(sys.argv) > 1 else "SSH password:")
    if answer is None:
        return 1
    sys.stdout.write(answer + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
