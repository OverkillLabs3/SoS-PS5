"""Prepared tree: host libraries, resource links, and the freshness stamp."""

from __future__ import annotations

import hashlib
import shutil
from pathlib import Path

RESOURCE_LINKS = ("Media", "sce_sys")
STAMP_FORMAT = "1"
UNPATCHED_DIR = "unpatched"


def _same(a: Path, b: Path) -> bool:
    one, two = a.stat(), b.stat()
    return one.st_size == two.st_size and int(one.st_mtime) == int(two.st_mtime)


def libs_source(build_dir: Path) -> Path:
    return build_dir / "core" / "libs" / "libs"


def relinker_binary(build_dir: Path) -> Path:
    return build_dir / "core" / "relinker" / "relinker"


def libraries_out_of_date(source_dir: Path) -> list[str]:
    """Libraries whose patched copy is older than the unpatched build it derives from.

    Building a single target refreshes `unpatched/` but not the patched copies that
    `copy_libs` installs, so a prepared tree can otherwise mix an old engine with a
    new relinker.
    """
    unpatched = source_dir / UNPATCHED_DIR
    if not unpatched.is_dir():
        return []
    stale = []
    for built in sorted(unpatched.glob("*.prx")):
        patched = source_dir / built.name
        if patched.exists() and patched.stat().st_mtime < built.stat().st_mtime:
            stale.append(built.name)
    return stale


def libs_digest(source_dir: Path) -> str:
    parts = []
    for library in sorted(source_dir.glob("*.prx")):
        info = library.stat()
        parts.append(f"{library.name} {info.st_size} {int(info.st_mtime)}")
    return hashlib.sha256("\n".join(parts).encode()).hexdigest()[:16]


def stamp_value(eboot_elf: Path, relinker: Path, source_dir: Path) -> str:
    """The launcher stamps on sizes only; mtime of the relinker is added so a rebuild
    that keeps the same file size still triggers a re-prepare. The library digest is
    added because the relinker is not the only input: a rebuilt engine library must also
    force a re-prepare."""
    def size(path: Path) -> int:
        return path.stat().st_size if path.exists() else 0

    def mtime(path: Path) -> int:
        return int(path.stat().st_mtime) if path.exists() else 0

    return f"{STAMP_FORMAT} {size(eboot_elf)} {size(relinker)} {mtime(relinker)} {libs_digest(source_dir)}"


def read_stamp(prep_dir: Path) -> str:
    path = prep_dir / "prepared.stamp"
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8").splitlines()[0].strip() if path.stat().st_size else ""


def write_stamp(prep_dir: Path, value: str) -> None:
    prep_dir.mkdir(parents=True, exist_ok=True)
    (prep_dir / "prepared.stamp").write_text(value + "\n", encoding="utf-8")


def link_resources(dump: Path, prepared: Path) -> None:
    app0 = prepared / "app0"
    app0.mkdir(parents=True, exist_ok=True)
    for name in RESOURCE_LINKS:
        source = dump / name
        if not source.exists():
            continue
        link = app0 / name
        wanted = source.resolve()
        if link.is_symlink():
            if link.resolve() == wanted:
                continue
            link.unlink()
        elif link.exists():
            raise RuntimeError(f"E_LAYOUT: {link} exists and is not a symlink")
        link.symlink_to(wanted)


def copy_libs(build_dir: Path, prepared: Path) -> int:
    source_dir = libs_source(build_dir)
    target = prepared / "libs"
    target.mkdir(parents=True, exist_ok=True)
    copied = 0
    for source in sorted(source_dir.glob("*.prx")):
        destination = target / source.name
        if destination.exists() and _same(destination, source):
            continue
        shutil.copyfile(source, destination)
        copied += 1
    return len(list(target.glob("*.prx")))


def state(prepared: Path, stamp: str) -> str:
    """absent | stale | current, mirroring the launcher's delete-then-relink logic."""
    if not (prepared / "app.elf").exists():
        return "absent"
    stored = read_stamp(prepared / "prep")
    if not stored:
        write_stamp(prepared / "prep", stamp)
        return "current"
    return "current" if stored == stamp else "stale"


def drop_outputs(prepared: Path) -> None:
    for name in ("app.elf", "app.registry.json"):
        (prepared / name).unlink(missing_ok=True)
    modules = prepared / "prep" / "sce_module"
    if modules.exists():
        shutil.rmtree(modules)
