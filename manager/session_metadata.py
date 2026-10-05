"""Small remote JSON catalog. Executed by python3 on the SSH host."""
import fcntl
import json
import os
from pathlib import Path
import sys
import tempfile
import uuid


def transact(directory, operation, args):
    root = Path(directory)
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    if root.is_symlink() or root.stat().st_uid != os.getuid():
        raise ValueError("Unsafe session metadata directory")
    root.chmod(0o700)
    # Lock a separate inode: replacing the JSON must not replace our lock.
    fd = os.open(root / "sessions.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        path = root / "sessions.json"
        data = json.loads(path.read_text()) if path.exists() else {"folders": {}, "sessions": {}}
        if operation != "read":
            name = args.get("name", "")
            if operation in ("folder", "rename_folder", "session", "rename"):
                if not name.strip() or len(name) > 100 or any(ord(c) < 32 for c in name):
                    raise ValueError("Names must contain 1–100 printable characters")
            if operation == "folder":
                if name == "Unfiled" or name in data["folders"].values():
                    raise ValueError("Folder name already exists or is reserved")
                data["folders"][uuid.uuid4().hex] = name
            elif operation == "rename_folder":
                if args["id"] not in data["folders"]:
                    raise ValueError("Folder no longer exists")
                if name == "Unfiled" or any(value == name and key != args["id"]
                        for key, value in data["folders"].items()):
                    raise ValueError("Folder name already exists or is reserved")
                data["folders"][args["id"]] = name
            elif operation in ("session", "rename", "move"):
                identity = args["id"]
                item = data["sessions"].setdefault(identity, {"name": identity, "folder": ""})
                if operation != "move":
                    item["name"] = name
                if operation != "rename":
                    folder = args["folder"]
                    if folder and folder not in data["folders"]:
                        raise ValueError("Folder no longer exists")
                    item["folder"] = folder
            elif operation == "delete":
                data["sessions"].pop(args["id"], None)
            elif operation == "delete_folder":
                data["folders"].pop(args["id"], None)
                for item in data["sessions"].values():
                    if item["folder"] == args["id"]:
                        item["folder"] = ""
            else:
                raise ValueError("Unknown metadata operation")
            fd, temporary = tempfile.mkstemp(prefix=".sessions-", dir=root)
            try:
                with os.fdopen(fd, "w") as output:
                    json.dump(data, output, ensure_ascii=False, indent=2)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, path)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
        return data


if __name__ == "__main__":
    print(json.dumps(transact(sys.argv[1], sys.argv[2], json.loads(sys.argv[3]))))
