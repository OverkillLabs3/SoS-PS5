import struct
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from sos_linux import selfextract  # noqa: E402

def use_workspace_tmp() -> None:
    """Keep test scratch off /tmp: it is a tmpfs with a user quota here, and running out of it
    surfaces as OSError 122 inside unrelated tests."""
    root = Path(__file__).resolve().parents[1] / ".tmp-tests"
    root.mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(root)
    tempfile.tempdir = str(root)

use_workspace_tmp()



ENTRY_SIZE = 32
PHDR_SIZE = 56
PAYLOAD = b"guest code" * 16


def synthetic_self(entries, phdrs, payload: bytes) -> bytes:
    """SELF-shaped container: 0x20 header, entry table, embedded ELF, program headers, segments."""
    elf_base = 0x20 + len(entries) * ENTRY_SIZE
    phoff = 64
    out = bytearray(selfextract.SELF_MAGIC + bytes(0x18 - 4) + struct.pack("<H", len(entries)) + bytes(2))
    out += bytes(0x20 - len(out))
    for props, offset, size in entries:
        out += struct.pack("<QQQQ", props, offset, size, size)
    elf_header = bytearray(64)
    elf_header[0:4] = selfextract.ELF_MAGIC
    elf_header[0x20:0x28] = struct.pack("<Q", phoff)
    elf_header[0x38:0x3A] = struct.pack("<H", len(phdrs))
    out += elf_header
    for type_, flags, offset, filesz in phdrs:
        out += struct.pack("<IIQQQQQQ", type_, flags, offset, 0, 0, filesz, filesz, 0x1000)
    for index, (props, _offset, _size) in enumerate(entries):
        if not props & 0x800:
            continue
        target = phdrs[index][2]
        if len(out) < target + len(payload):
            out += bytes(target + len(payload) - len(out))
        out[target:target + len(payload)] = payload
    return bytes(out)


class SelfExtractTest(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())

    def write(self, name: str, data: bytes) -> Path:
        path = self.directory / name
        path.write_bytes(data)
        return path

    def test_rejects_non_self(self):
        source = self.write("plain.bin", b"not a self at all")
        self.assertFalse(selfextract.extract(source, self.directory / "out.elf"))

    def test_converts_single_segment(self):
        blob = synthetic_self([(0x800, 0x1000, len(PAYLOAD))], [(1, 5, 0x1000, len(PAYLOAD))], PAYLOAD)
        target = self.directory / "one.elf"
        self.assertTrue(selfextract.extract(self.write("one.self", blob), target))
        self.assertTrue(selfextract.is_elf(target))
        self.assertIn(PAYLOAD, target.read_bytes())

    def test_encrypted_segment_is_refused(self):
        blob = synthetic_self([(0x800 | 0xA, 0x1000, 16)], [(1, 5, 0x1000, 16)], b"x" * 16)
        target = self.directory / "locked.elf"
        with self.assertRaises(selfextract.SelfError):
            selfextract.extract(self.write("locked.self", blob), target)
        self.assertFalse(target.exists())

    def test_convert_or_copy_passes_elf_through(self):
        elf = selfextract.ELF_MAGIC + bytes(70)
        target = self.directory / "copy.elf"
        self.assertTrue(selfextract.convert_or_copy(self.write("already.elf", elf), target))
        self.assertEqual(target.read_bytes(), elf)

    @unittest.skipUnless(Path("game/eboot.bin").exists(), "player dump not present")
    def test_real_dump_executable(self):
        target = self.directory / "eboot.elf"
        self.assertTrue(selfextract.extract(Path("game/eboot.bin"), target))
        self.assertTrue(selfextract.is_elf(target))
        self.assertGreater(target.stat().st_size, 10_000_000)


if __name__ == "__main__":
    unittest.main()
