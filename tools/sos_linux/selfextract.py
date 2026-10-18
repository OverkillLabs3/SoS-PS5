"""SELF container to plain ELF, ported from launcher/launcher.cpp ExtractSelf."""

from __future__ import annotations

import struct
from pathlib import Path

SELF_MAGIC = b"\x4f\x15\x3d\x1d"
ELF_MAGIC = b"\x7fELF"
ENTRY_SIZE = 32
PHDR64_SIZE = 56
CHUNK = 1 << 22


class SelfError(Exception):
    pass


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == ELF_MAGIC
    except OSError:
        return False


def _read_at(handle, offset: int, size: int) -> bytes:
    handle.seek(offset)
    data = handle.read(size)
    if len(data) != size:
        raise SelfError(f"unexpected end of file at {offset}")
    return data


def _entries(head: bytes):
    count = struct.unpack_from("<H", head, 0x18)[0]
    if count == 0 or count > 4095:
        raise SelfError(f"bad entry count {count}")
    return count


def extract(self_path: Path, elf_path: Path) -> bool:
    """Convert one SELF file. Returns True on success; leaves no partial output."""
    elf_path.parent.mkdir(parents=True, exist_ok=True)
    with self_path.open("rb") as source:
        if source.seek(0, 2) < 0x20:
            return False
        head = _read_at(source, 0, 0x20)
        if head[:4] != SELF_MAGIC:
            return False
        entry_count = _entries(head)
        entries = _read_at(source, 0x20, entry_count * ENTRY_SIZE)
        elf_base = 0x20 + entry_count * ENTRY_SIZE
        elf_head = _read_at(source, elf_base, 64)
        if elf_head[:4] != ELF_MAGIC:
            raise SelfError("no ELF header inside container")

        phoff = struct.unpack_from("<Q", elf_head, 0x20)[0]
        phnum = struct.unpack_from("<H", elf_head, 0x38)[0]
        if phnum == 0 or phnum > 511:
            raise SelfError(f"bad program header count {phnum}")
        phdrs = _read_at(source, elf_base + phoff, phnum * PHDR64_SIZE)

        parsed = [struct.unpack_from("<IIQQQQQQ", phdrs, i * PHDR64_SIZE) for i in range(phnum)]
        segments = [struct.unpack_from("<QQQQ", entries, i * ENTRY_SIZE) for i in range(entry_count)]
        for props, _offset, _size, _mem in segments:
            if (props & 0x800) and (props & 0xA):
                raise SelfError("segment is still encrypted")

        end = max((p[2] + p[5] for p in parsed), default=0)

        temporary = elf_path.with_name(elf_path.name + ".tmp")
        try:
            with temporary.open("wb") as target:
                target.seek(0)
                target.write(elf_head)
                target.seek(phoff)
                target.write(phdrs)
                for props, offset, file_size, _mem in segments:
                    if not props & 0x800:
                        continue
                    index = (props >> 20) & 0xFFF
                    if index >= phnum:
                        continue
                    remaining = min(file_size, parsed[index][5])
                    source.seek(offset)
                    target.seek(parsed[index][2])
                    while remaining:
                        part = source.read(min(CHUNK, remaining))
                        if not part:
                            raise SelfError("truncated segment")
                        target.write(part)
                        remaining -= len(part)
                target.truncate(end)
            temporary.replace(elf_path)
        finally:
            temporary.unlink(missing_ok=True)
    return True


def convert_or_copy(self_path: Path, elf_path: Path) -> bool:
    """ELF in, byte copy out; SELF in, extracted ELF out; anything else, False."""
    if is_elf(self_path):
        elf_path.parent.mkdir(parents=True, exist_ok=True)
        elf_path.write_bytes(self_path.read_bytes())
        return True
    try:
        return extract(self_path, elf_path)
    except SelfError:
        return False
