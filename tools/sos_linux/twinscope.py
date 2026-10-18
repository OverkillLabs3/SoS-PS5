"""Point a module's ``DT_NEEDED`` at the library that actually exports what it imports.

A title's own module (``PSN.prx``) names a ``.native`` wrapper in its ``DT_NEEDED``, but the symbol it
imports lives in the plain twin: ``wTpfglkmv34`` (``sceMsgDialogProgressBarSetValue``) is exported by
``libs/libSceMsgDialog.prx`` and not by ``libs/libSceMsgDialog.native.prx``. ``dlopen`` therefore fails
and the title retries the load forever, which is where the boot stalls (boykopovar/AnyPS5#906).

Loading the plain twin process-wide is not an option: the two libraries export overlapping NIDs (all 30
of ``libSceAvPlayer``'s, 14 of ``libSceSaveData``'s, 6 of ``libSceMsgDialog``'s), so bringing the twin
into the global scope rebinds other modules to the wrong implementation and the title fails earlier
instead. A dependency of a ``dlopen``'d object is resolved in that object's own local scope, so the
correction is made in the module: rewrite its ``DT_NEEDED`` string to name the library that provides the
symbol.

The replacement name is always shorter than the wrapper name it replaces, so it fits in the same string
table slot and no table has to grow.
"""

from __future__ import annotations

from pathlib import Path

from .modinit import dynamic_table
from .neededorder import LayoutError, _segments, _to_offset

DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10

# Measured against the dump in hand: PSN imports `wTpfglkmv34` (`sceMsgDialogProgressBarSetValue`), which
# only the plain library exports. Extend this table when another module's load fails on a symbol that
# lives in a plain twin, and only when the twin pair really overlaps in the global scope.
SWAPS = (
    ("PSN.prx.guest.prx", "libSceMsgDialog.native.prx", "libSceMsgDialog.prx"),
)


def needed_strings(data: bytes) -> list[tuple[int, str]]:
    """Return (file offset, text) for every ``DT_NEEDED`` name in a module image."""
    offset, count = dynamic_table(data)
    strtab = strsz = None
    entries = []
    for index in range(count):
        start = offset + index * 16
        tag = int.from_bytes(data[start : start + 8], "little")
        value = int.from_bytes(data[start + 8 : start + 16], "little")
        if tag == DT_STRTAB:
            strtab = value
        elif tag == DT_STRSZ:
            strsz = value
        elif tag == DT_NEEDED:
            entries.append(value)
        elif tag == 0:
            break
    if strtab is None:
        raise ValueError("module has no DT_STRTAB")
    loads, _ = _segments(data)
    base = _to_offset(loads, strtab)
    end = base + (strsz or 4096)
    found = []
    for value in entries:
        start = base + value
        if start >= end:
            raise ValueError(f"DT_NEEDED offset {value} is past the string table")
        found.append((start, data[start : data.index(0, start)].decode("utf-8")))
    return found


def rewrite_needed(data: bytearray, old: str, new: str) -> bytearray:
    """Replace one ``DT_NEEDED`` name in place. Returns the image unchanged when the name is absent."""
    if len(new) >= len(old):
        raise ValueError(f"{new} is not shorter than {old}; the slot cannot hold it")
    for start, text in needed_strings(bytes(data)):
        if text != old:
            continue
        span = len(old) + 1
        patched = new.encode("utf-8") + bytes(span - len(new.encode("utf-8")))
        return bytearray(data[:start]) + bytearray(patched) + bytearray(data[start + span :])
    return data


def apply(prepared: Path, swaps: tuple[tuple[str, str, str], ...]) -> list[str]:
    """Rewrite ``(module, from, to)`` entries under ``prepared``; idempotent, reports what changed."""
    done = []
    for name, old, new in swaps:
        for candidate in (prepared / "app0" / "sce_module" / name, prepared / "app0" / name):
            if not candidate.exists():
                continue
            data = bytearray(candidate.read_bytes())
            patched = rewrite_needed(data, old, new)
            if patched != data:
                candidate.write_bytes(bytes(patched))
                done.append(f"{candidate.name}: {old} -> {new}")
            elif any(text == new for _, text in needed_strings(bytes(data))):
                done.append(f"{candidate.name}: already {new}")
            break
    return done
