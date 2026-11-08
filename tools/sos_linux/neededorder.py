"""Keep deferred guest modules out of the executable's startup dependency list.

The relinker lists every converted module as a DT_NEEDED of the executable, so ld.so runs each
module's lifecycle at load time. Windows defers some modules instead
(``WindowsGuestStartup::callLifecycle`` returns early), and their lifecycle runs when the game
loads them. ``defer_guest_modules`` reproduces that on Linux: the slot is pointed at an existing
host library (ld.so dedups), and the module file stays in place for the runtime to dlopen later.
"""

from __future__ import annotations

import struct
from pathlib import Path

PT_LOAD = 1
PT_DYNAMIC = 2
DT_NEEDED = 1
DT_STRTAB = 5


class LayoutError(Exception):
    pass


def _segments(data: bytes) -> tuple[list[tuple[int, int, int]], tuple[int, int]]:
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    loads: list[tuple[int, int, int]] = []
    dynamic = (0, 0)
    for index in range(phnum):
        ptype, _flags, offset, vaddr, _paddr, filesz, _memsz, _align = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
        if ptype == PT_LOAD:
            loads.append((vaddr, offset, filesz))
        elif ptype == PT_DYNAMIC:
            dynamic = (offset, filesz)
    if dynamic[0] == 0:
        raise LayoutError("no PT_DYNAMIC segment")
    return loads, dynamic


def _to_offset(loads: list[tuple[int, int, int]], vaddr: int) -> int:
    for base, offset, filesz in loads:
        if base <= vaddr < base + filesz:
            return offset + (vaddr - base)
    raise LayoutError(f"vaddr {vaddr:#x} is not inside a loaded file range")


def is_guest(name: str) -> bool:
    return name.startswith("$ORIGIN/app0/")


def needed_names(app: Path) -> list[str]:
    data = app.read_bytes()
    loads, (dynamic, dynamicsize) = _segments(data)
    strtab = 0
    slots = []
    for index in range(dynamicsize // 16):
        tag, value = struct.unpack_from("<qQ", data, dynamic + index * 16)
        if tag == 0:
            break
        if tag == DT_NEEDED:
            slots.append(dynamic + index * 16 + 8)
        elif tag == DT_STRTAB and strtab == 0:
            strtab = _to_offset(loads, value)
    names = []
    for slot in slots:
        start = strtab + struct.unpack_from("<Q", data, slot)[0]
        names.append(data[start:data.index(0, start)].decode("utf-8"))
    return names


def defer_guest_modules(app: Path, module_names: tuple[str, ...], anchor: str) -> list[str]:
    """Point the deferred modules' DT_NEEDED slots at an existing host library."""
    data = bytearray(app.read_bytes())
    loads, (dynamic, dynamicsize) = _segments(bytes(data))
    slots: list[int] = []
    strtab = 0
    for index in range(dynamicsize // 16):
        tag, value = struct.unpack_from("<qQ", data, dynamic + index * 16)
        if tag == 0:
            break
        if tag == DT_NEEDED:
            slots.append(dynamic + index * 16 + 8)
        elif tag == DT_STRTAB and strtab == 0:
            strtab = _to_offset(loads, value)

    def name_at(slot: int) -> str:
        start = strtab + struct.unpack_from("<Q", data, slot)[0]
        return bytes(data[start:data.index(0, start)]).decode("utf-8")

    anchor_offset = next((struct.unpack_from("<Q", data, slot)[0] for slot in slots if name_at(slot) == anchor), None)
    if anchor_offset is None:
        raise LayoutError(f"{anchor} is not among the executable's DT_NEEDED entries")
    deferred = []
    for slot in slots:
        current = name_at(slot)
        for module in module_names:
            if current.endswith(f"/{module}.guest.prx"):
                struct.pack_into("<Q", data, slot, anchor_offset)
                deferred.append(module)
                break
    if deferred:
        app.write_bytes(bytes(data))
    return deferred


def order_host_libraries_first(app: Path) -> tuple[int, int]:
    """Put host libraries before guest modules in the executable's DT_NEEDED.

    ld.so binds the title's imports depth-first through that list, so a title that ships its own
    ``libc.prx`` gets Sony's libc - whose heap needs the kernel's process-parameter handshake that a
    dumped executable cannot reproduce - and dies on uninitialised allocations. Binding the host
    libraries first gives the title AnyPS5's working implementations; ``-Bsymbolic`` keeps those
    libraries bound to themselves internally. Idempotent, and stable within each group.
    """
    data = bytearray(app.read_bytes())
    loads, (dynamic, dynamicsize) = _segments(bytes(data))
    slots: list[int] = []
    strtab = 0
    for index in range(dynamicsize // 16):
        tag, value = struct.unpack_from("<qQ", data, dynamic + index * 16)
        if tag == 0:
            break
        if tag == DT_NEEDED:
            slots.append(dynamic + index * 16 + 8)
        elif tag == DT_STRTAB and strtab == 0:
            strtab = _to_offset(loads, value)
    if strtab == 0 or not slots:
        raise LayoutError("no DT_NEEDED or DT_STRTAB")

    def name_at(slot: int) -> str:
        start = strtab + struct.unpack_from("<Q", data, slot)[0]
        return bytes(data[start:data.index(0, start)]).decode("utf-8")

    names = [name_at(slot) for slot in slots]
    hosts = [name for name in names if not is_guest(name)]
    guests = [name for name in names if is_guest(name)]
    wanted = hosts + guests
    if names == wanted:
        return len(hosts), len(guests)
    offsets = {name: struct.unpack_from("<Q", data, slot)[0] for name, slot in zip(names, slots)}
    for slot, name in zip(slots, wanted):
        struct.pack_into("<Q", data, slot, offsets[name])
    app.write_bytes(bytes(data))
    return len(hosts), len(guests)
