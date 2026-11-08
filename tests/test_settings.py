import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from sos_linux import settings  # noqa: E402


class SettingsCodecTest(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())

    def test_seed_size_is_header_plus_four_bytes_per_char(self):
        data = settings.seed()
        self.assertEqual(data[:12], settings.HEADER)
        self.assertEqual(len(data), len(settings.HEADER) + 4 * len(settings.DEFAULT_BODY))

    def test_every_character_is_four_bytes(self):
        self.assertEqual(settings.encode("ab"), settings.HEADER + b"a\x00\x00\x00b\x00\x00\x00")

    def test_missing_file_seeds_defaults(self):
        settings.ensure_seed(self.directory)
        self.assertTrue(settings.path_of(self.directory).exists())
        self.assertEqual(settings.read_int(self.directory, "resolutionIndex"), 0)

    def test_write_keeps_the_file_identical_except_digits(self):
        settings.ensure_seed(self.directory)
        before = settings.path_of(self.directory).read_bytes()
        self.assertTrue(settings.write_int(self.directory, "resolutionIndex", 6))
        after = settings.path_of(self.directory).read_bytes()
        self.assertEqual(len(before), len(after))
        self.assertEqual(sum(1 for a, b in zip(before, after) if a != b), 1)
        self.assertEqual(settings.read_int(self.directory, "resolutionIndex"), 6)

    def test_different_digit_count_is_a_no_op(self):
        settings.ensure_seed(self.directory)
        self.assertFalse(settings.write_int(self.directory, "resolutionIndex", 12))
        self.assertEqual(settings.read_int(self.directory, "resolutionIndex"), 0)

    def test_unknown_key_reads_none(self):
        settings.ensure_seed(self.directory)
        self.assertIsNone(settings.read_int(self.directory, "notAKey"))

    def test_resolution_map_matches_launcher(self):
        self.assertEqual([index for index, _ in settings.RESOLUTIONS], [0, 6, 8])


if __name__ == "__main__":
    unittest.main()
