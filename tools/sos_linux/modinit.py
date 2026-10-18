"""Neutralize a module's ELF initialisation, without touching its symbols.

The dump ships its own copy of the system ``libc.prx`` (``sce_module/libc.prx``), so a prepared tree
ends up with two libc implementations in one process: the vendored one under ``libs/`` and the
converted bundled copy under ``app0/sce_module/``. Both perform the PS5 application-heap handshake,
and each one drives the title's allocator initialisation, so the title builds its 256 MB pool twice.
The bundled copy still has to be loaded (other bundled modules resolve symbols from it), but its
start-up callbacks must not run.

The callbacks are stubbed by replacing their first instruction with ``ret`` (0xC3). Clearing
``DT_INIT`` instead is wrong: the loader calls it regardless and lands in the trap-filled padding
(``SIGTRAP`` a few bytes into the mapping).
"""

from __future__ import annotations

from pathlib import Path

from .neededorder import _segments, _to_offset

DT_INIT = 12
DT_INIT_ARRAY = 25
DT_INIT_ARRAYSZ = 27
PT_DYNAMIC = 2

RET = 0xC3


def dynamic_table(data: bytes) -> tuple[int, int]:
    """Return (file offset, entry count) of the PT_DYNAMIC table."""
    phoff = int.from_bytes(data[32:40], "little")
    phentsize = int.from_bytes(data[54:56], "little")   # e_phentsize is at 0x36, e_phnum at 0x38
    phnum = int.from_bytes(data[56:58], "little")
    for index in range(phnum):
        start = phoff + index * phentsize
        if int.from_bytes(data[start : start + 4], "little") != PT_DYNAMIC:
            continue
        # p_offset, not the vaddr mapped through PT_LOAD: a converted module can carry a dynamic
        # segment whose vaddr falls outside every loaded range (PSN.prx), while its file offset is
        # always correct.
        offset = int.from_bytes(data[start + 8 : start + 16], "little")
        filesz = int.from_bytes(data[start + 32 : start + 40], "little")
        return offset, filesz // 16
    raise ValueError("no PT_DYNAMIC")


def init_targets(data: bytes) -> list[int]:
    """Virtual addresses of the module's start-up callbacks."""
    table, count = dynamic_table(data)
    values: dict[int, int] = {}
    for index in range(count):
        slot = table + index * 16
        tag = int.from_bytes(data[slot : slot + 8], "little")
        if tag in (DT_INIT, DT_INIT_ARRAY, DT_INIT_ARRAYSZ):
            values[tag] = int.from_bytes(data[slot + 8 : slot + 16], "little")
    targets = []
    if values.get(DT_INIT):
        targets.append(values[DT_INIT])
    array, size = values.get(DT_INIT_ARRAY, 0), values.get(DT_INIT_ARRAYSZ, 0)
    if array and size:
        loads, _ = _segments(data)
        base = _to_offset(loads, array)
        for entry in range(size // 8):
            pointer = int.from_bytes(data[base + entry * 8 : base + entry * 8 + 8], "little")
            if pointer:
                targets.append(pointer)
    return targets


def neutralize(path: Path) -> int:
    """Make every start-up callback of the module return immediately; returns how many were stubbed."""
    data = bytearray(path.read_bytes())
    loads, _ = _segments(bytes(data))
    stubbed = 0
    for target in init_targets(bytes(data)):
        offset = _to_offset(loads, target)
        if data[offset] != RET:
            data[offset] = RET
            stubbed += 1
    if not stubbed:
        raise ValueError(f"{path.name}: no start-up callbacks found")
    path.write_bytes(bytes(data))
    return stubbed
