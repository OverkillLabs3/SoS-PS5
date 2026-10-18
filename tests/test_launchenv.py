import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from sos_linux import __main__ as cli, settings  # noqa: E402


class GcWaitTest(unittest.TestCase):
    def setUp(self):
        self._saved = os.environ.pop("APS5_GC_WAIT_MS", None)

    def tearDown(self):
        os.environ.pop("APS5_GC_WAIT_MS", None)
        if self._saved is not None:
            os.environ["APS5_GC_WAIT_MS"] = self._saved

    def test_default_is_applied_when_nothing_is_set(self):
        self.assertEqual(cli.apply_gc_wait({}), {"APS5_GC_WAIT_MS": cli.GC_WAIT_DEFAULT})

    def test_command_line_value_wins(self):
        self.assertEqual(cli.apply_gc_wait({"APS5_GC_WAIT_MS": "250"}), {"APS5_GC_WAIT_MS": "250"})

    def test_ambient_value_wins(self):
        os.environ["APS5_GC_WAIT_MS"] = "1"
        self.assertEqual(cli.apply_gc_wait({}), {})


class ReseedTest(unittest.TestCase):
    def setUp(self):
        import tempfile
        from pathlib import Path
        self.root = Path(tempfile.mkdtemp())

    def test_reset_restores_the_seed_after_the_title_wrote_its_own_choices(self):
        settings.ensure_seed(self.root)
        self.assertTrue(settings.write_int(self.root, "resolutionIndex", 0))
        settings.reset(self.root, resolution_index=8)
        self.assertEqual(settings.read_int(self.root, "resolutionIndex"), 8)

    def test_reset_without_a_resolution_keeps_the_seed_value(self):
        settings.reset(self.root)
        self.assertEqual(settings.read_int(self.root, "resolutionIndex"), 0)


if __name__ == "__main__":
    unittest.main()
