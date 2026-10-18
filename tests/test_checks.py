import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from sos_linux import checks  # noqa: E402
from sos_linux.__main__ import main  # noqa: E402


def code_for(function, *args):
    try:
        function(*args)
    except checks.PrepError as error:
        return error.code
    return None


class PreconditionTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.dump = self.root / "game"
        self.dump.mkdir()

    def test_missing_executable(self):
        self.assertEqual(code_for(checks.executable, self.dump), "E_DUMP_MISSING")

    def test_incomplete_dump_names_what_is_missing(self):
        (self.dump / "eboot.bin").write_bytes(b"")
        try:
            checks.dump_complete(self.dump)
        except checks.PrepError as error:
            self.assertEqual(error.code, "E_DUMP_PARTIAL")
            for item in ("sce_sys", "Media"):
                self.assertIn(item, str(error))
        else:
            self.fail("expected E_DUMP_PARTIAL")

    def test_missing_build_tree(self):
        self.assertEqual(code_for(checks.build_tree, self.root / "nope"), "E_BUILD_MISSING")

    def test_space_error_reports_numbers(self):
        try:
            checks.MIN_FREE_BYTES = 2**62
            checks.free_space(self.root)
        except checks.PrepError as error:
            self.assertEqual(error.code, "E_SPACE")
            self.assertIn("need", str(error))
            self.assertIn("have", str(error))
        finally:
            checks.MIN_FREE_BYTES = 2 * 1024**3

    def test_cli_status_reports_without_traceback(self):
        from contextlib import redirect_stderr, redirect_stdout
        import io

        errors = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(errors):
            code = main(["status", "--dump", str(self.dump), "--out", str(self.root / "prepared")])
        self.assertEqual(code, 0)
        self.assertNotIn("Traceback", errors.getvalue())

    def test_cli_prepare_fails_with_named_code(self):
        from contextlib import redirect_stderr, redirect_stdout
        import io

        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = main(["prepare", "--dump", str(self.dump), "--out", str(self.root / "prepared")])
        self.assertEqual(code, 2)
        self.assertIn("E_DUMP", err.getvalue())
        self.assertNotIn("Traceback", err.getvalue())


if __name__ == "__main__":
    unittest.main()


class LaunchLockOrderingTest(unittest.TestCase):
    def test_a_refused_launch_keeps_the_previous_logs(self) -> None:
        from sos_linux import gamelog

        with tempfile.TemporaryDirectory() as tmp:
            prepared = Path(tmp)
            (prepared / "app.elf").write_bytes(b"")
            logs = prepared / "logs"
            logs.mkdir()
            err = logs / "game.err"
            err.write_text("keep me\n")
            held = gamelog.acquire_lock(prepared)
            try:
                with self.assertRaises(gamelog.AlreadyRunning):
                    gamelog.launch(prepared)
            finally:
                held.close()
            self.assertEqual(err.read_text(), "keep me\n")


