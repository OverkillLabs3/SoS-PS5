import struct
import tempfile
import unittest
from pathlib import Path

from sos_linux import twinscope

EHDR = 64
PHDR = 56
DYN_OFF = 0x400
STRTAB_OFF = 0x800
TOTAL = 0x1000
NAMES = ("libkernel.prx", "libSceMsgDialog.native.prx")


def build_module(names=NAMES) -> bytes:
    data = bytearray(TOTAL)
    data[0:4] = b"\x7fELF"
    data[4] = 2
    data[5] = 1
    data[6] = 1
    struct.pack_into("<H", data, 16, 3)
    struct.pack_into("<H", data, 18, 62)
    struct.pack_into("<I", data, 20, 1)
    struct.pack_into("<Q", data, 32, EHDR)
    struct.pack_into("<H", data, 52, 64)
    struct.pack_into("<H", data, 54, PHDR)
    struct.pack_into("<H", data, 56, 2)
    for index, (p_type, p_offset, p_vaddr, p_filesz) in enumerate(
        ((1, 0, 0, TOTAL), (2, DYN_OFF, DYN_OFF, (len(names) + 3) * 16))
    ):
        start = EHDR + index * PHDR
        struct.pack_into("<I", data, start, p_type)
        struct.pack_into("<I", data, start + 4, 7)
        struct.pack_into("<Q", data, start + 8, p_offset)
        struct.pack_into("<Q", data, start + 16, p_vaddr)
        struct.pack_into("<Q", data, start + 32, p_filesz)
        struct.pack_into("<Q", data, start + 40, p_filesz)
    blob = b"".join(name.encode("utf-8") + b"\0" for name in names)
    data[STRTAB_OFF : STRTAB_OFF + len(blob)] = blob
    positions, at = [], 0
    for name in names:
        positions.append(at)
        at += len(name) + 1
    entries = [(twinscope.DT_NEEDED, index) for index in positions]
    entries += [(twinscope.DT_STRTAB, STRTAB_OFF), (twinscope.DT_STRSZ, len(blob)), (0, 0)]
    for index, (tag, value) in enumerate(entries):
        start = DYN_OFF + index * 16
        struct.pack_into("<q", data, start, tag)
        struct.pack_into("<Q", data, start + 8, value)
    return bytes(data)


class TwinScopeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.data = build_module()

    def test_needed_names_are_read_with_their_offsets(self) -> None:
        found = twinscope.needed_strings(self.data)
        self.assertEqual([text for _, text in found], list(NAMES))

    def test_rewrite_shortens_the_slot_and_keeps_it_terminated(self) -> None:
        patched = twinscope.rewrite_needed(bytearray(self.data), NAMES[1], "libSceMsgDialog.prx")
        self.assertEqual(
            [text for _, text in twinscope.needed_strings(bytes(patched))],
            [NAMES[0], "libSceMsgDialog.prx"],
        )

    def test_rewrite_is_idempotent_and_applies_through_a_tree(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            module = root / "app0" / "sce_module" / "PSN.prx.guest.prx"
            module.parent.mkdir(parents=True)
            module.write_bytes(self.data)
            swaps = (("PSN.prx.guest.prx", NAMES[1], "libSceMsgDialog.prx"),)
            first = twinscope.apply(root, swaps)
            self.assertEqual(len(first), 1)
            self.assertIn("->", first[0])
            second = twinscope.apply(root, swaps)
            self.assertEqual(len(second), 1)
            self.assertIn("already", second[0])

    def test_a_longer_replacement_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            twinscope.rewrite_needed(bytearray(self.data), NAMES[1], "libSceMsgDialog.native.x.prx")


if __name__ == "__main__":
    unittest.main()
