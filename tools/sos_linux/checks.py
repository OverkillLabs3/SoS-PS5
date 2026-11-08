"""Preconditions, with the named errors from data-model.md section 5."""

from __future__ import annotations

import ctypes.util
import os
import shutil
from pathlib import Path

from . import layout

REQUIRED_ITEMS = ("sce_sys", "Media")
MIN_FREE_BYTES = 2 * 1024 ** 3


class PrepError(Exception):
    def __init__(self, code: str, message: str, log: Path | None = None):
        super().__init__(message)
        self.code = code
        self.log = log


def executable(dump: Path) -> Path:
    for name in ("eboot.bin", "eboot.elf"):
        path = dump / name
        if path.exists():
            return path
    raise PrepError("E_DUMP_MISSING", f"game executable not found in {dump} (eboot.bin or eboot.elf)")


def dump_complete(dump: Path) -> None:
    missing = [item for item in REQUIRED_ITEMS if not (dump / item).exists()]
    if missing:
        raise PrepError("E_DUMP_PARTIAL", f"missing required items: {', '.join(missing)}")


def build_tree(build_dir: Path) -> None:
    if not layout.libs_source(build_dir).is_dir() or not layout.relinker_binary(build_dir).exists():
        raise PrepError("E_BUILD_MISSING", f"{build_dir} has no libraries or relinker - run scripts/build.sh first")


def libraries_fresh(build_dir: Path) -> None:
    stale = layout.libraries_out_of_date(layout.libs_source(build_dir))
    if stale:
        shown = ", ".join(stale[:3]) + (" ..." if len(stale) > 3 else "")
        raise PrepError(
            "E_LIBS_STALE",
            f"{len(stale)} libraries are older than their unpatched build ({shown})"
            " - a single-target build leaves the patched copies behind; run scripts/build.sh",
        )


def free_space(out_dir: Path) -> None:
    try:
        usage = shutil.disk_usage(out_dir.parent if not out_dir.exists() else out_dir)
    except OSError:
        return
    if usage.free < MIN_FREE_BYTES:
        raise PrepError(
            "E_SPACE",
            f"insufficient free space: need {MIN_FREE_BYTES // 2**20} MiB, have {usage.free // 2**20} MiB",
        )


def display() -> None:
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        raise PrepError("E_DISPLAY", "no display server; run under X11 or XWayland")


def vulkan() -> None:
    if ctypes.util.find_library("vulkan") is None:
        raise PrepError("E_VULKAN", "libvulkan not found - no Vulkan 1.3 driver available")
    for directory in (Path("/etc/vulkan/icd.d"), Path("/usr/share/vulkan/icd.d"), Path("/usr/local/share/vulkan/icd.d")):
        if directory.is_dir() and any(directory.glob("*.json")):
            return
    raise PrepError("E_VULKAN", "no Vulkan ICD manifest found - no Vulkan 1.3 driver available")


