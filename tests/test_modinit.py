import struct
import tempfile
import unittest
from pathlib import Path

from sos_linux import modinit

EHDR = 64
PHDR = 56
DYN_OFF = 0x400
FUNC_OFF = 0x800
ARRAY_OFF = 0x900
ENTRY_A = 0xA00
ENTRY_B = 0xA40
TOTAL = 0x1000


def build_module() -> bytes:
    """Smallest thing that parses as an ELF shared object with start-up callbacks."""
    data = bytearray(TOTAL)
    data[0:4] = b"\x7fELF"
    data[4] = 2  # ELFCLASS64
    data[5] = 1  # little endian
    data[6] = 1  # EV_CURRENT
    struct.pack_into("<H", data, 16, 3)  # ET_DYN
    struct.pack_into("<H", data, 18, 62)  # EM_X86_64
    struct.pack_into("<I", data, 20, 1)
    struct.pack_into("<Q", data, 32, EHDR)  # e_phoff
    struct.pack_into("<H", data, 52, 64)  # e_ehsize
    struct.pack_into("<H", data, 54, PHDR)  # e_phentsize
    struct.pack_into("<H", data, 56, 2)  # e_phnum

    def program_header(index: int, p_type: int, p_offset: int, p_vaddr: int, p_filesz: int) -> None:
        start = EHDR + index * PHDR
        struct.pack_into("<I", data, start, p_type)
        struct.pack_into("<I", data, start + 4, 7)  # PF_R | PF_W | PF_X
        struct.pack_into("<Q", data, start + 8, p_offset)
        struct.pack_into("<Q", data, start + 16, p_vaddr)
        struct.pack_into("<Q", data, start + 32, p_filesz)
        struct.pack_into("<Q", data, start + 40, p_filesz)

    program_header(0, 1, 0, 0, TOTAL)  # PT_LOAD over the whole file
    program_header(1, modinit.PT_DYNAMIC, DYN_OFF, DYN_OFF, 4 * 16)

    entries = [
        (modinit.DT_INIT, FUNC_OFF),
        (modinit.DT_INIT_ARRAY, ARRAY_OFF),
        (modinit.DT_INIT_ARRAYSZ, 16),
        (0, 0),  # DT_NULL
    ]
    for index, (tag, value) in enumerate(entries):
        struct.pack_into("<QQ", data, DYN_OFF + index * 16, tag, value)
    struct.pack_into("<Q", data, ARRAY_OFF, ENTRY_A)
    struct.pack_into("<Q", data, ARRAY_OFF + 8, ENTRY_B)
    for offset in (FUNC_OFF, ENTRY_A, ENTRY_B):
        data[offset] = 0x55  # "push rbp", i.e. definitely not a ret
    return bytes(data)


class ModinitTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "libc.prx.guest.prx"
        self.path.write_bytes(build_module())

    def test_finds_every_callback(self) -> None:
        self.assertEqual(modinit.init_targets(bytes(self.path.read_bytes())), [FUNC_OFF, ENTRY_A, ENTRY_B])

    def test_stubs_callbacks_and_keeps_symbols_intact(self) -> None:
        before = self.path.read_bytes()
        self.assertEqual(modinit.neutralize(self.path), 3)
        after = self.path.read_bytes()
        self.assertEqual(len(before), len(after))
        for offset in (FUNC_OFF, ENTRY_A, ENTRY_B):
            self.assertEqual(after[offset], modinit.RET)
        differing = [index for index in range(len(before)) if before[index] != after[index]]
        self.assertEqual(differing, [FUNC_OFF, ENTRY_A, ENTRY_B])

    def test_dynamic_table_is_located_through_phdrs(self) -> None:
        table, count = modinit.dynamic_table(bytes(self.path.read_bytes()))
        self.assertEqual((table, count), (DYN_OFF, 4))


if __name__ == "__main__":
    unittest.main()
