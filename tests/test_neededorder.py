import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from sos_linux import neededorder  # noqa: E402

def use_workspace_tmp() -> None:
    """Keep test scratch off /tmp: it is a tmpfs with a user quota here, and running out of it
    surfaces as OSError 122 inside unrelated tests."""
    root = Path(__file__).resolve().parents[1] / ".tmp-tests"
    root.mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(root)
    tempfile.tempdir = str(root)

use_workspace_tmp()



PREPARED = Path(__file__).resolve().parents[1] / "prepared" / "app.elf"
DEFERRED = ("PSN.prx", "SaveData.prx", "lib_burst_generated.prx")


def _tree_deferred():
    """The deferral list the prepared tree was built with.

    A test must not assume a particular policy: `SOS_NO_DEFER=1` builds a tree that deliberately makes
    every module a start-up dependency (used as a fast crash harness), so the tree records its own list.
    """
    record = PREPARED.parent / "defer-list"
    if not record.exists():
        return list(DEFERRED)
    return [line.strip() for line in record.read_text(errors="replace").splitlines() if line.strip()]


class ClassificationTest(unittest.TestCase):
    def test_guest_names_are_the_app0_module_links(self):
        self.assertTrue(neededorder.is_guest("$ORIGIN/app0/sce_module/libc.prx.guest.prx"))
        self.assertFalse(neededorder.is_guest("libkernel.prx"))
        self.assertFalse(neededorder.is_guest("$ORIGIN/libs/libc.prx"))


@unittest.skipUnless(PREPARED.exists(), "prepared/app.elf not built yet")
class PreparedTreeTest(unittest.TestCase):
    def test_deferred_modules_are_not_startup_dependencies(self):
        names = neededorder.needed_names(PREPARED)
        for module in _tree_deferred():
            self.assertFalse(any(n.endswith(f"/{module}.guest.prx") for n in names), module)

    def test_deferred_module_files_stay_available_for_later_loads(self):
        for module in DEFERRED:
            self.assertTrue((PREPARED.parent / "app0" / "sce_module" / f"{module}.guest.prx").exists(), module)

    def test_defer_redirects_slots_and_needs_a_real_anchor(self):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "app.elf"
            copy.write_bytes(PREPARED.read_bytes())
            self.assertEqual(neededorder.defer_guest_modules(copy, ("libc.prx",), "libkernel.prx"), ["libc.prx"])
            self.assertFalse(any(n.endswith("/libc.prx.guest.prx") for n in neededorder.needed_names(copy)))
            self.assertEqual(neededorder.defer_guest_modules(copy, ("libc.prx",), "libkernel.prx"), [])
            with self.assertRaises(neededorder.LayoutError):
                neededorder.defer_guest_modules(copy, ("PS5Util.prx",), "not-a-library.prx")


if __name__ == "__main__":
    unittest.main()
