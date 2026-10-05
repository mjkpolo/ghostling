"""SSH/UDS operations, with one private master per host."""
import importlib.machinery
import importlib.util
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

    def connect(self):
        if self.connection is None:
            connection = self.api.ssh_connection(self.name, batch_mode=True)
            connection.__enter__()
            self.connection = connection
        self.server, self.directory, terminfo, installed = self.api.remote_state(self.name)
        if self.server and not installed:
            self.api.provision_terminfo(self.name, terminfo, checked=True)
        return self.refresh()

    def refresh(self):
        self.attachments = self.api.reap_attachments(self.attachments)
        return self.api.sessions(self.name, self.server, self.directory) if self.server else []

    def install_server(self, destination):
        platform = self.api.run_ssh(self.name, "uname -sm").stdout.strip()
        if platform != "Linux x86_64":
            raise RuntimeError(f"Published servers require Linux x86_64; this host is {platform}.")
        binary = self.api.release_asset("gmux-server", executable=True)
        self.server = self.api.copy_server(self.name, binary, destination,
                                          release_themes=True)
        return self.connect()

    def create(self, name):
        self.api.create_session(self.name, self.server, self.directory, name)
        return self.refresh()

    def delete(self, name):
        self.api.delete_session(self.name, self.server, self.directory, name)
        return self.refresh()

    def open(self, name):
        self.attachments = self.api.reap_attachments(self.attachments)
        if any(a["name"] == name for a in self.attachments):
            raise RuntimeError("This session already has a terminal window open.")
        client = str(self.api.local_program("gmux", self.client))
        self.attachments.append(self.api.start_attachment(
            self.name, self.directory, name, client))
        return self.refresh()

    def close(self):
        try:
            for attachment in self.attachments:
                self.api.close_attachment(attachment)
            self.attachments.clear()
        finally:
            if self.connection:
                self.connection.__exit__(None, None, None)
                self.connection = None
