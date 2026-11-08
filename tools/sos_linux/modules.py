"""Collect the game's own modules and turn them into plain ELF for relinker."""

from __future__ import annotations

from pathlib import Path

from . import selfextract

MODULE_DIRS = ("sce_module", "Media/Modules", "Media/Plugins")


def find_modules(dump: Path) -> dict[str, Path]:
    """First occurrence wins, in the order the Windows launcher scans."""
    found: dict[str, Path] = {}
    for relative in MODULE_DIRS:
        directory = dump / relative
        if not directory.is_dir():
            continue
        for path in sorted(directory.glob("*.prx")):
            found.setdefault(path.name, path)
    return found


def prepare_modules(dump: Path, target: Path) -> tuple[list[str], list[str]]:
    """Returns (converted names, failed names)."""
    converted: list[str] = []
    failed: list[str] = []
    target.mkdir(parents=True, exist_ok=True)
    for name, source in find_modules(dump).items():
        destination = target / name
        if destination.exists():
            destination.unlink()
        if selfextract.convert_or_copy(source, destination):
            converted.append(name)
        else:
            destination.unlink(missing_ok=True)
            failed.append(name)
    return converted, failed
