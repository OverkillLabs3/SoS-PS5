"""Offline PS5 NID to symbol-name lookup.

`data/stubs/by_library/*.txt` of the ps5rs project is a `<nid> <name>` table per library, pinned by
the URL in DATABASE. It is downloaded on demand into `.nidb/` (never committed) so the stub backlog
can be reported as real API names instead of opaque hashes.
"""

from __future__ import annotations

import re
import urllib.request
from pathlib import Path

API = "https://api.github.com/repos/claimore22/ps5rs/contents/data/stubs/by_library?ref={ref}"
RAW = "https://raw.githubusercontent.com/claimore22/ps5rs/{ref}/data/stubs/by_library/{name}"
REF = "2304be5131221bae0dac24b9767cabffda33f747"
LINE = re.compile(r"^(\S+)\s+(\S+)")


def cache_dir(root: Path) -> Path:
    return root / ".nidb"


def sync(root: Path) -> Path:
    """Fetch the tables once. Returns the cache directory."""
    directory = cache_dir(root)
    directory.mkdir(parents=True, exist_ok=True)
    listing = directory / "files.txt"
    if not listing.exists():
        with urllib.request.urlopen(API.format(ref=REF), timeout=60) as response:
            names = re.findall(r'"path": "data/stubs/by_library/([^"]+)"', response.read().decode())
        listing.write_text("\n".join(names), encoding="utf-8")
    for name in listing.read_text(encoding="utf-8").split():
        target = directory / name
        if target.exists() and target.stat().st_size:
            continue
        with urllib.request.urlopen(RAW.format(ref=REF, name=name), timeout=60) as response:
            target.write_bytes(response.read())
    return directory


def load(directory: Path) -> dict[str, tuple[str, str]]:
    table: dict[str, tuple[str, str]] = {}
    for path in sorted(directory.glob("*.txt")):
        library = path.name[:-4]
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if match := LINE.match(line):
                table.setdefault(match.group(1), (match.group(2), library))
    return table


def report(prepared: Path, root: Path) -> list[tuple[str, str, str]]:
    """(symbol, library, nid) for every stub the prepared tree exports, named where possible."""
    import subprocess

    table = load(cache_dir(root)) if cache_dir(root).exists() else {}
    defined = subprocess.run(["nm", "-D", "--defined-only", str(prepared / "libs" / "sos-stubs.prx")],
                             capture_output=True, text=True).stdout.splitlines()
    rows = []
    for line in defined:
        nid = line.split()[-1]
        if nid in ("sos_stub_report",) or not re.fullmatch(r"[A-Za-z0-9+/._-]{11}", nid):
            continue
        name, library = table.get(nid, ("?", "?"))
        rows.append((name, library, nid))
    return sorted(rows)
