"""SSH provisioning and socket forwarding for the Qt manager."""

from contextlib import contextmanager
from functools import cache
import hashlib
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import stat
import tarfile
import tempfile
from urllib.request import urlopen

SESSION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
HERE = Path(__file__).resolve().parents[1]
RELEASE_REPOSITORY = os.environ.get(
    "GMUX_RELEASE_REPOSITORY", "mjkpolo/ghostling")
RELEASE_BASE = os.environ.get(
    "GMUX_RELEASE_BASE",
    f"https://github.com/{RELEASE_REPOSITORY}/releases/latest/download")
SSH_CONTROL = None
SSH_OPTIONS = []

def ssh_command(host, *arguments):
    """Ignore configured forwards for short-lived management commands."""
    return ["ssh", "-S", SSH_CONTROL, "-o", "ClearAllForwardings=yes",
            *SSH_OPTIONS, "--", host, *arguments]

def control_command(*arguments):
    # The master already authenticated using ssh_config. Control requests use
    # only its private socket, so ssh_config cannot add unwanted forwards.
    return ["ssh", "-F", "/dev/null", "-S", SSH_CONTROL,
            *arguments, "gmux"]

@contextmanager
def ssh_connection(host, batch_mode=False):
    global SSH_CONTROL, SSH_OPTIONS
    with tempfile.TemporaryDirectory(prefix="ssh-", dir=local_dir()) as temp:
        SSH_CONTROL = str(Path(temp) / "control")
        SSH_OPTIONS = (["-o", "BatchMode=yes", "-o", "ConnectTimeout=15",
                        "-o", "StrictHostKeyChecking=yes", "-o", "ServerAliveInterval=15",
                        "-o", "ServerAliveCountMax=2"] if batch_mode else [])
        try:
            environment = os.environ.copy()
            options = SSH_OPTIONS
            if batch_mode:
                # Only initial authentication may prompt. Subsequent commands
                # reuse the master and fail rather than prompting invisibly.
                helper = Path(temp) / "askpass"
                helper.write_text("#!/bin/sh\nexec " + shlex.quote(sys.executable)
                                  + " " + shlex.quote(str(Path(__file__).with_name("askpass.py")))
                                  + ' "$@"\n')
                helper.chmod(0o700)
                environment.update(SSH_ASKPASS=str(helper), SSH_ASKPASS_REQUIRE="force")
                options = ["-o", "BatchMode=no", "-o", "StrictHostKeyChecking=ask", *SSH_OPTIONS]
            result = subprocess.run([
                "ssh", "-M", "-N", "-f", "-S", SSH_CONTROL,
                *options,
                "-o", "ClearAllForwardings=yes", "-o", "ControlPersist=no",
                "-o", "StreamLocalBindMask=0177", "--", host,
            ], capture_output=batch_mode, text=True, env=environment,
                stdin=subprocess.DEVNULL)
            if result.returncode:
                raise RuntimeError(result.stderr.strip() if batch_mode
                                   else f"SSH connection to {host} failed")
            yield
        finally:
            if Path(SSH_CONTROL).exists():
                subprocess.run(control_command("-O", "exit"),
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            SSH_CONTROL = None
            SSH_OPTIONS = []

def cache_dir():
    root = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    directory = root / "gmux"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = directory.lstat()
    if (stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode)
            or info.st_uid != os.getuid()):
        raise RuntimeError(f"insecure gmux cache directory: {directory}")
    directory.chmod(0o700)
    return directory

@cache
def release_checksums():
    try:
        with urlopen(f"{RELEASE_BASE}/SHA256SUMS", timeout=5) as response:
            text = response.read().decode("ascii")
    except (OSError, UnicodeError) as error:
        raise RuntimeError(f"could not download gmux release checksums: {error}")
    checksums = {}
    for line in text.splitlines():
        fields = line.split(None, 1)
        if len(fields) == 2 and re.fullmatch(r"[0-9a-fA-F]{64}", fields[0]):
            checksums[fields[1].lstrip("*")] = fields[0].lower()
    return checksums

def release_asset(name, executable=False):
    if name == "gmux" and executable and (platform.system() != "Linux" or platform.machine() not in (
            "x86_64", "AMD64")):
        raise RuntimeError(
            "published gmux binaries currently support Linux x86_64 only")
    expected = release_checksums().get(name)
    if not expected:
        raise RuntimeError(f"release checksum is missing for {name}")
    destination = cache_dir() / name
    if destination.is_file():
        actual = hashlib.sha256(destination.read_bytes()).hexdigest()
        if actual == expected:
            if executable: destination.chmod(0o700)
            return destination
    temporary = destination.with_name(f".{name}.{os.getpid()}.download")
    try:
        print(f"Downloading {name} from {RELEASE_REPOSITORY}...", flush=True)
        with urlopen(f"{RELEASE_BASE}/{name}", timeout=60) as response:
            with temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
        actual = hashlib.sha256(temporary.read_bytes()).hexdigest()
        if actual != expected:
            raise RuntimeError(f"checksum mismatch for downloaded {name}")
        temporary.chmod(0o700 if executable else 0o600)
        temporary.replace(destination)
    except OSError as error:
        raise RuntimeError(f"could not download {name}: {error}")
    finally:
        temporary.unlink(missing_ok=True)
    return destination

def local_program(name, explicit=None):
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if not path.is_file() or not os.access(path, os.X_OK):
            raise RuntimeError(f"{name} not found: {path}")
        return path
    adjacent = HERE / name
    if adjacent.is_file() and os.access(adjacent, os.X_OK):
        return adjacent
    installed = shutil.which(name)
    if installed:
        return Path(installed).resolve()
    cached = cache_dir() / name
    if cached.is_file() and os.access(cached, os.X_OK):
        return cached
    return release_asset(name, executable=True)

def binary_version(output, name):
    fields = output.strip().split()
    return fields[1] if len(fields) == 2 and fields[0] == name else None

def latest_version():
    try:
        # Older releases predate VERSION. Keep them usable during migration.
        if "VERSION" not in release_checksums():
            return None
        version = release_asset("VERSION").read_text().strip()
        if version == "unknown" or not re.fullmatch(r"[A-Za-z0-9._-]+", version):
            raise RuntimeError("invalid release version")
        return version
    except (OSError, RuntimeError) as error:
        print(f"Update check unavailable; keeping installed binaries: {error}",
              file=sys.stderr)
        return None

def update_client(client, version):
    try:
        current = subprocess.run([str(client), "--version"], capture_output=True,
                                 text=True, timeout=5)
        if current.returncode == 0 and binary_version(current.stdout, "gmux") == version:
            return client
    except (OSError, subprocess.TimeoutExpired):
        pass
    # Own our cache, not arbitrary PATH entries (possibly package-managed).
    return release_asset("gmux", executable=True)

def update_server(host, server, version):
    current = run_ssh(host, '"$1" --version', server, check=False)
    if (current.returncode == 0
            and binary_version(current.stdout, "gmux-server") == version):
        return server
    print(f"Updating remote gmux-server to {version} (existing sessions keep running).")
    return copy_server(host, release_asset("gmux-server", executable=True),
                       str(Path(server).parent), release_themes=True)

REMOTE_DIR = r'''
set -eu
if [ -n "${GMUX_SOCKET_DIR:-}" ]; then
    directory=$GMUX_SOCKET_DIR
elif [ -n "${XDG_RUNTIME_DIR:-}" ]; then
    directory=$XDG_RUNTIME_DIR/gmux
else
    directory=/tmp/gmux-$(id -u)
fi
umask 077
mkdir -p "$directory"
[ -d "$directory" ] && [ ! -L "$directory" ]
[ "$(stat -c %u "$directory")" = "$(id -u)" ]
chmod 700 "$directory"
printf '%s\n' "$directory"
'''

def run_ssh(host, script, *args, check=True):
    remote = "sh -s -- " + " ".join(shlex.quote(arg) for arg in args)
    command = ssh_command(host, remote)
    result = subprocess.run(command, input=script, text=True, capture_output=True)
    if check and result.returncode:
        detail = result.stderr.strip() or result.stdout.strip() or "no error output"
        raise RuntimeError(f"{host}: {detail} (exit {result.returncode})")
    return result

def provision_terminfo(host, directory=None, checked=False):
    directory_script = r'''
set -eu
if [ -n "${XDG_DATA_HOME:-}" ]; then
    directory=$XDG_DATA_HOME/gmux/terminfo
else
    directory=$HOME/.local/share/gmux/terminfo
fi
umask 077
mkdir -p "$directory"
chmod 700 "$directory"
printf '%s\n' "$directory"
'''
    if directory is None:
        directory = run_ssh(host, directory_script).stdout.strip()
    verify = 'test -r "$1/x/xterm-ghostty" -o -r "$1/78/xterm-ghostty"\n'
    if not checked and run_ssh(
            host, verify, directory, check=False).returncode == 0:
        return
    source = HERE / "terminfo" / "xterm-ghostty.terminfo"
    if not source.is_file():
        source = release_asset("xterm-ghostty.terminfo")
    has_tic = run_ssh(host, "command -v tic >/dev/null 2>&1",
                      check=False).returncode == 0
    if has_tic:
        # Discovery returns a path, not an existing directory. tic cannot
        # create missing parents on a first installation.
        command = (f"set -eu; umask 077; mkdir -p -- {shlex.quote(directory)}; "
                   f"tic -x -o {shlex.quote(directory)} "
                   "/dev/stdin")
        subprocess.run(ssh_command(host, command), input=source.read_text(),
                       text=True, check=True)
    else:
        with tempfile.TemporaryDirectory(prefix="gmux-terminfo-") as temp:
            subprocess.run(["tic", "-x", "-o", temp, str(source)],
                           check=True)
            entries = list(Path(temp).glob("*/xterm-ghostty"))
            if len(entries) != 1:
                raise RuntimeError("local tic did not create xterm-ghostty")
            remote_entry = f"{directory}/{entries[0].parent.name}/xterm-ghostty"
            command = (f"umask 077; mkdir -p "
                       f"{shlex.quote(str(Path(remote_entry).parent))}; "
                       f"cat > {shlex.quote(remote_entry)}; "
                       f"chmod 600 {shlex.quote(remote_entry)}")
            subprocess.run(ssh_command(host, command),
                           input=entries[0].read_bytes(), check=True)
    run_ssh(host, verify, directory)

def remote_config_dir(host):
    return run_ssh(host, r'''
set -eu
directory=${XDG_CONFIG_HOME:-$HOME/.config}/gmux
umask 077
mkdir -p "$directory"
printf '%s\n' "$directory"
''').stdout.strip()

def provision_themes(host, local_binary=None, force=False, release=False):
    directory = remote_config_dir(host)
    if not force and run_ssh(host,
            'test -r "$1/themes/.gmux-installed"', directory,
            check=False).returncode == 0:
        return
    candidates = [HERE / "themes", HERE / "build/themes"]
    if local_binary:
        candidates.insert(0, Path(local_binary).resolve().parent / "themes")
    source = None if release else next(
        (p for p in candidates if (p / "LICENSE").is_file()), None)
    with tempfile.TemporaryDirectory(prefix="gmux-themes-") as temporary:
        archive = Path(temporary) / "themes.tar.gz"
        if source:
            with tarfile.open(archive, "w:gz") as bundle:
                for entry in sorted(source.iterdir()):
                    if entry.is_file() and not entry.is_symlink():
                        bundle.add(entry, arcname=f"themes/{entry.name}")
        else:
            archive = release_asset("gmux-themes.tar.gz")
        command = (f"set -eu; umask 077; cd {shlex.quote(directory)}; "
                   "tar -xzf - --skip-old-files --no-same-owner --no-same-permissions; "
                   "touch themes/.gmux-installed")
        with archive.open("rb") as stream:
            subprocess.run(ssh_command(host, command), stdin=stream, check=True)

def copy_server(host, local_binary, destination="~/.local/bin", release_themes=False):
    binary = Path(local_binary).expanduser().resolve()
    if not binary.is_file():
        raise RuntimeError(f"server binary not found: {binary}")
    script = r'''
set -eu
destination=$1
case "$destination" in
    '~') destination=$HOME ;;
    '~/'*) destination=$HOME/${destination#\~/} ;;
esac
umask 077
mkdir -p "$destination"
printf '%s\n' "$destination"
'''
    resolved = run_ssh(host, script, destination).stdout.strip()
    # Replace atomically: an existing session may still be running this binary.
    command = (f"set -eu; umask 077; cd {shlex.quote(resolved)}; "
               "temporary=$(mktemp .gmux-server.XXXXXX); "
               "trap 'rm -f -- \"$temporary\"' EXIT; "
               "cat > \"$temporary\"; chmod 700 \"$temporary\"; "
               "mv -f -- \"$temporary\" gmux-server")
    with binary.open("rb") as source:
        subprocess.run(ssh_command(host, command), stdin=source, check=True)
    server = f"{resolved}/gmux-server"
    provision_themes(host, binary, force=True, release=release_themes)
    return server

def sessions(host, server, directory):
    script = r'''
set -eu
server=$1
directory=$2
for path in "$directory"/*.sock; do
    [ -S "$path" ] || continue
    [ "$(stat -c %u "$path")" = "$(id -u)" ] || continue
    name=${path##*/}
    name=${name%.sock}
    set +e
    "$server" --check "$path" >/dev/null 2>&1
    status=$?
    set -e
    if [ "$status" -eq 0 ]; then
        printf 'live\t%s\n' "$name"
    elif [ "$status" -eq 2 ]; then
        printf 'attached\t%s\n' "$name"
    else
        printf 'stale\t%s\n' "$name"
    fi
done
'''
    result = run_ssh(host, script, server, directory)
    return [tuple(line.split("\t", 1)) for line in result.stdout.splitlines()
            if "\t" in line]

def create_session(host, server, directory, name):
    if not SESSION_RE.fullmatch(name):
        raise RuntimeError("names may contain only letters, numbers, . _ and -")
    script = r'''
set -eu
server=$1
path=$2/$3.sock
[ ! -e "$path" ]
GMUX_TERMINFO=${XDG_DATA_HOME:-$HOME/.local/share}/gmux/terminfo \
    "$server" "$path"
'''
    run_ssh(host, script, server, directory, name)

def delete_session(host, server, directory, name):
    if not SESSION_RE.fullmatch(name):
        raise RuntimeError("invalid session name")
    script = r'''
set -eu
server=$1
path=$2/$3.sock
[ -S "$path" ] || exit 0
[ "$(stat -c %u "$path")" = "$(id -u)" ]
set +e
"$server" --check "$path" >/dev/null 2>&1
status=$?
set -e
if [ "$status" -eq 0 ] || [ "$status" -eq 2 ]; then
    "$server" --kill "$path"
else
    rm -- "$path"
fi
'''
    run_ssh(host, script, server, directory, name)

def local_dir():
    configured = os.environ.get("GMUX_SOCKET_DIR")
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    path = Path(configured or (Path(runtime) / "gmux" if runtime
                               else f"/tmp/gmux-{os.getuid()}"))
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if (stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode)
            or info.st_uid != os.getuid() or info.st_mode & 0o077):
        raise RuntimeError(f"insecure local socket directory: {path}")
    return path

def remote_state(host):
    """Fetch all normal startup state in one SSH round trip."""
    script = REMOTE_DIR + r'''
socket_directory=$directory
if command -v gmux-server >/dev/null 2>&1; then
    candidate=$(command -v gmux-server)
elif [ -x "$HOME/.local/bin/gmux-server" ]; then
    candidate=$HOME/.local/bin/gmux-server
else
    candidate=
fi
if [ -n "$candidate" ] && "$candidate" 2>&1 | grep -q -- --check; then
    server=$candidate
else
    server=
fi
if [ -n "${XDG_DATA_HOME:-}" ]; then
    terminfo=$XDG_DATA_HOME/gmux/terminfo
else
    terminfo=$HOME/.local/share/gmux/terminfo
fi
if [ -r "$terminfo/x/xterm-ghostty" ] || [ -r "$terminfo/78/xterm-ghostty" ]; then
    has_terminfo=yes
else
    has_terminfo=no
fi
printf 'server\t%s\nsockets\t%s\nterminfo\t%s\nhas_terminfo\t%s\n' \
    "$server" "$socket_directory" "$terminfo" "$has_terminfo"
'''
    values = dict(line.split("\t", 1)
                  for line in run_ssh(host, script).stdout.splitlines()
                  if "\t" in line)
    return (values.get("server") or None, values.get("sockets"),
            values.get("terminfo"), values.get("has_terminfo") == "yes")

def start_attachment(host, directory, name, client):
    if not SESSION_RE.fullmatch(name):
        raise RuntimeError("invalid session name")
    temporary = tempfile.TemporaryDirectory(prefix="attach-", dir=local_dir())
    local = Path(temporary.name) / "terminal.sock"
    remote = f"{directory}/{name}.sock"
    attachment = {"name": name, "directory": temporary,
                  "forward": f"{local}:{remote}"}
    try:
        # OpenSSH acknowledges the bind before starting the client. Each
        # attachment has its own path; never guess where another socket leads.
        subprocess.run(control_command("-O", "forward", "-L",
                                       attachment["forward"]),
                       check=True, capture_output=True, text=True)
        attachment["client"] = subprocess.Popen(
            [client, "--connect", str(local)])
        return attachment
    except Exception:
        stop_attachment(attachment)
        raise

def stop_attachment(attachment):
    subprocess.run(control_command("-O", "cancel", "-L",
                                   attachment["forward"]),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    attachment["directory"].cleanup()

def close_attachment(attachment):
    attachment["client"].terminate()
    attachment["client"].wait()
    stop_attachment(attachment)

def reap_attachments(attachments):
    active = []
    for attachment in attachments:
        # The manager's blocking wait thread sets returncode and notifies Qt.
        if attachment["client"].returncode is None:
            active.append(attachment)
        else:
            stop_attachment(attachment)
    return active
