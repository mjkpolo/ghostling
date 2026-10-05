"""Discover concrete SSH aliases; leave effective option handling to OpenSSH."""
import glob
from pathlib import Path
import shlex


def hosts_from_config(path=None):
    path = Path(path) if path else Path.home() / ".ssh/config"
    aliases, visited = {"localhost": "localhost"}, set()

    def read(source):
        source = source.resolve()
        if source in visited or not source.is_file():
            return
        visited.add(source)
        for line in source.read_text().splitlines():
            # ssh_config permits both 'Host foo' and 'Host=foo'.
            lexer = shlex.shlex(line, posix=True, punctuation_chars="=")
            lexer.whitespace_split = True
            fields = list(lexer)
            if len(fields) > 1 and fields[1] == "=":
                fields.pop(1)
            if not fields:
                continue
            if fields[0].lower() == "host":
                for alias in fields[1:]:
                    if not any(char in alias for char in "*?!["):
                        aliases.setdefault(alias.casefold(), alias)
            elif fields[0].lower() == "include":
                for pattern in fields[1:]:
                    expanded = Path(pattern).expanduser()
                    if not expanded.is_absolute():
                        expanded = path.parent / expanded
                    for match in sorted(glob.glob(str(expanded))):
                        read(Path(match))
    read(path)
    return sorted(aliases.values(), key=str.casefold)
