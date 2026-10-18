import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from sos_linux import layout, modules  # noqa: E402


class LayoutTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.build = self.root / "build"
        self.dump = self.root / "game"
        self.prepared = self.root / "prepared"
        (layout.libs_source(self.build)).mkdir(parents=True)
        layout.relinker_binary(self.build).parent.mkdir(parents=True)
        layout.relinker_binary(self.build).write_bytes(b"x" * 100)
        for name in range(3):
            (layout.libs_source(self.build) / f"lib{name}.prx").write_bytes(b"z" * (name + 1))
        (self.dump / "Media").mkdir(parents=True)
        (self.dump / "sce_sys").mkdir()
        (self.root / "prep").mkdir()
        self.eboot = self.root / "prep" / "eboot.elf"
        self.eboot.write_bytes(b"e" * 10)
        import os
        self.stamp_mtime = os.stat(layout.relinker_binary(self.build)).st_mtime

    def test_stamp_identity(self):
        stamp_mtime = self.stamp_mtime
        stamp = layout.stamp_value(self.eboot, layout.relinker_binary(self.build), layout.libs_source(self.build))
        self.assertEqual(stamp.split()[:3], ["1", "10", "100"])
        self.eboot.write_bytes(b"e" * 11)
        self.assertNotEqual(layout.stamp_value(self.eboot, layout.relinker_binary(self.build), layout.libs_source(self.build)), stamp)
        layout.relinker_binary(self.build).write_bytes(b"x" * 101)
        self.assertNotEqual(layout.stamp_value(self.eboot, layout.relinker_binary(self.build), layout.libs_source(self.build)), stamp)
        layout.relinker_binary(self.build).write_bytes(b"x" * 100)
        import os
        os.utime(layout.relinker_binary(self.build), (stamp_mtime + 10, stamp_mtime + 10))
        self.assertNotEqual(layout.stamp_value(self.eboot, layout.relinker_binary(self.build), layout.libs_source(self.build)), stamp)

    def test_state_transitions(self):
        stamp = layout.stamp_value(self.eboot, layout.relinker_binary(self.build), layout.libs_source(self.build))
        self.assertEqual(layout.state(self.prepared, stamp), "absent")
        self.prepared.mkdir()
        (self.prepared / "app.elf").write_bytes(b"a")
        self.assertEqual(layout.state(self.prepared, stamp), "current")
        layout.write_stamp(self.prepared / "prep", "1 1 1")
        self.assertEqual(layout.state(self.prepared, stamp), "stale")

    def test_resource_links_are_reusable(self):
        layout.link_resources(self.dump, self.prepared)
        first = (self.prepared / "app0" / "Media").readlink()
        layout.link_resources(self.dump, self.prepared)
        self.assertEqual(first, (self.prepared / "app0" / "Media").readlink())
        self.assertTrue((self.prepared / "app0" / "Media").is_symlink())

    def test_resource_link_refuses_real_directory(self):
        (self.prepared / "app0" / "Media").mkdir(parents=True)
        with self.assertRaises(RuntimeError):
            layout.link_resources(self.dump, self.prepared)

    def test_copy_libs_counts_and_skips_identical(self):
        self.assertEqual(layout.copy_libs(self.build, self.prepared), 3)
        self.assertEqual(layout.copy_libs(self.build, self.prepared), 3)
        (self.prepared / "libs" / "lib1.prx").write_bytes(b"different contents")
        layout.copy_libs(self.build, self.prepared)
        self.assertEqual((self.prepared / "libs" / "lib1.prx").read_bytes(), b"z" * 2)

    def test_drop_outputs_removes_generated_files(self):
        (self.prepared / "app0" / "sce_module").mkdir(parents=True)
        (self.prepared / "app0" / "sce_module" / "libc.prx").write_bytes(b"m")
        (self.prepared / "app.elf").write_bytes(b"a")
        (self.prepared / "app.registry.json").write_bytes(b"{}")
        layout.drop_outputs(self.prepared)
        self.assertFalse((self.prepared / "app.elf").exists())
        self.assertFalse((self.prepared / "app.registry.json").exists())
        self.assertFalse((self.prepared / "prep" / "sce_module").exists())
        self.assertTrue((self.prepared / "app0" / "sce_module").exists())

    def test_stamp_changes_when_a_library_is_rebuilt(self):
        stamp = layout.stamp_value(self.eboot, layout.relinker_binary(self.build), layout.libs_source(self.build))
        (layout.libs_source(self.build) / "lib1.prx").write_bytes(b"z" * 2)
        import os
        os.utime(layout.libs_source(self.build) / "lib1.prx", (self.stamp_mtime + 20, self.stamp_mtime + 20))
        self.assertNotEqual(
            layout.stamp_value(self.eboot, layout.relinker_binary(self.build), layout.libs_source(self.build)),
            stamp,
        )

    def test_patched_library_older_than_unpatched_is_reported(self):
        import os
        source = layout.libs_source(self.build)
        unpatched = source / layout.UNPATCHED_DIR
        unpatched.mkdir()
        (unpatched / "lib1.prx").write_bytes(b"u" * 2)
        patched = self.stamp_mtime - 60
        os.utime(source / "lib1.prx", (patched, patched))
        os.utime(unpatched / "lib1.prx", (patched - 10, patched - 10))
        self.assertEqual(layout.libraries_out_of_date(source), [])
        os.utime(unpatched / "lib1.prx", (patched + 120, patched + 120))
        self.assertEqual(layout.libraries_out_of_date(source), ["lib1.prx"])

    def test_absent_unpatched_directory_is_not_a_failure(self):
        self.assertEqual(layout.libraries_out_of_date(layout.libs_source(self.build)), [])


class ModulesTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.dump = self.root / "game"
        for name in ("libc.prx", "dup.prx"):
            (self.dump / "sce_module").mkdir(parents=True, exist_ok=True)
            (self.dump / "sce_module" / name).write_bytes(b"\x7fELF1")
        (self.dump / "Media" / "Modules").mkdir(parents=True)
        (self.dump / "Media" / "Modules" / "dup.prx").write_bytes(b"\x7fELF2")
        (self.dump / "Media" / "Plugins").mkdir()
        (self.dump / "Media" / "Plugins" / "libfmod.prx").write_bytes(b"\x7fELF3")

    def test_gathers_all_three_locations_and_prefers_first(self):
        found = self.root / "out"
        converted, failed = modules.prepare_modules(self.dump, found)
        self.assertEqual(converted, ["dup.prx", "libc.prx", "libfmod.prx"])
        self.assertEqual(failed, [])
        self.assertEqual((found / "dup.prx").read_bytes(), b"\x7fELF1")

if __name__ == "__main__":
    unittest.main()
