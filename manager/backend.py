"""SSH/UDS operations, with one private master per host."""
import importlib.machinery
import importlib.util
import json
import uuid
from pathlib import Path


class Host:
    def __init__(self, name, client=None):
        self.name = name
        # The transport has module-local master state. Independent instances
        # keep each host's connection and cached release metadata isolated.
        script = Path(__file__).with_name("remote.py")
        loader = importlib.machinery.SourceFileLoader("gmux_remote", str(script))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.api = importlib.util.module_from_spec(spec)
        loader.exec_module(self.api)
        self.client = client
        self.connection = None
        self.server = self.directory = None
        self.attachments = []
        self.catalog = {"folders": {}, "sessions": {}}
        self.pending_upgrade = None

    def metadata(self, operation="read", **args):
        # Standard-library helper; no remote package installation required.
        directory = self.api.remote_config_dir(self.name)
        script = Path(__file__).with_name("session_metadata.py").read_text()
        result = self.api.run_ssh(self.name, 'exec python3 -c "$1" "$2" "$3" "$4"',
                                 script, directory, operation, json.dumps(args))
        self.catalog = json.loads(result.stdout)
        return self.catalog

    def label(self, identity):
        return self.catalog["sessions"].get(identity, {}).get("name", identity)

    def folder(self, identity):
        return self.catalog["sessions"].get(identity, {}).get("folder", "")

    def connect(self):
        if self.connection is None:
            connection = self.api.ssh_connection(self.name, batch_mode=True)
            connection.__enter__()
            self.connection = connection
        self.server, self.directory, terminfo, installed = self.api.remote_state(self.name)
        if self.server and not installed:
            self.api.provision_terminfo(self.name, terminfo, checked=True)
        entries = self.refresh()
        self.pending_upgrade = None
        if self.server:
            version = self.api.latest_version()
            if version:
                current = self.api.run_ssh(self.name, '"$1" --version', self.server, check=False)
                installed_version = self.api.binary_version(current.stdout, "gmux-server")
                if current.returncode or installed_version != version:
                    self.pending_upgrade = (version, [identity for state, identity in entries
                                                     if state in ("live", "attached")])
        return entries

    def upgrade_server(self, version, approved_sessions):
        # Fetch and verify before disrupting any running work.
        binary = self.api.release_asset("gmux-server", executable=True)
        self.api.release_asset("gmux-themes.tar.gz")
        entries = self.api.sessions(self.name, self.server, self.directory)
        running = {identity for state, identity in entries if state in ("live", "attached")}
        if not running.issubset(set(approved_sessions)):
            raise RuntimeError("New sessions appeared since confirmation. Reconnect and review the upgrade again.")
        for identity in running:
            self.api.delete_session(self.name, self.server, self.directory, identity)
        self.server = self.api.copy_server(self.name, binary,
            str(Path(self.server).parent), release_themes=True)
        self.pending_upgrade = None
        return self.refresh()

    def refresh(self):
        self.attachments = self.api.reap_attachments(self.attachments)
        if self.server:
            self.metadata()
            for attachment in self.attachments:
                self.update_title(attachment)
        return self.api.sessions(self.name, self.server, self.directory) if self.server else []

    def update_title(self, attachment):
        path = Path(attachment["directory"].name) / "title"
        title = self.label(attachment["name"])
        if not path.exists() or path.read_text() != title:
            temporary = path.with_suffix(".tmp")
            temporary.write_text(title)
            temporary.replace(path)

    def install_server(self, destination):
        platform = self.api.run_ssh(self.name, "uname -sm").stdout.strip()
        if platform != "Linux x86_64":
            raise RuntimeError(f"Published servers require Linux x86_64; this host is {platform}.")
        binary = self.api.release_asset("gmux-server", executable=True)
        self.server = self.api.copy_server(self.name, binary, destination,
                                          release_themes=True)
        return self.connect()

    def create(self, name, folder=""):
        identity = uuid.uuid4().hex
        self.metadata("session", id=identity, name=name, folder=folder)
        self.api.create_session(self.name, self.server, self.directory, identity)
        return self.refresh()

    def delete(self, name):
        self.api.delete_session(self.name, self.server, self.directory, name)
        self.metadata("delete", id=name)
        return self.refresh()

    def rename(self, identity, name):
        self.metadata("rename", id=identity, name=name)
        return self.refresh()

    def move(self, identity, folder):
        self.metadata("move", id=identity, folder=folder)
        return self.refresh()

    def new_folder(self, name):
        self.metadata("folder", name=name)
        return self.refresh()

    def rename_folder(self, identity, name):
        self.metadata("rename_folder", id=identity, name=name)
        return self.refresh()

    def open_folder(self, folder):
        for state, identity in self.refresh():
            if state == "live" and self.folder(identity) == folder:
                self.open(identity)
        return self.refresh()

    def delete_folder(self, identity):
        self.metadata("delete_folder", id=identity)
        return self.refresh()

    def open(self, name):
        self.attachments = self.api.reap_attachments(self.attachments)
        if any(a["name"] == name for a in self.attachments):
            raise RuntimeError("This session already has a terminal window open.")
        client = self.client_program()
        self.attachments.append(self.api.start_attachment(
            self.name, self.directory, name, client, title=self.label(name)))
        return self.refresh()

    def client_program(self):
        client = self.api.local_program("gmux", self.client)
        # An explicit --client is a deliberate development/testing override.
        # Normal launches check releases, updating only the manager's cache.
        if not self.client:
            version = self.api.latest_version()
            if version:
                client = self.api.update_client(client, version)
        return str(client)

    def close(self):
        try:
            for attachment in self.attachments:
                self.api.close_attachment(attachment)
            self.attachments.clear()
        finally:
            if self.connection:
                self.connection.__exit__(None, None, None)
                self.connection = None
