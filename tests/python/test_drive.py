import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("drive", ROOT / "scripts/drive.py")
drive = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(drive)


class DriveScreenshotTests(unittest.TestCase):
    def test_complete_rgb332_frame_is_decoded(self):
        self.assertEqual(drive.decode_rows(2, 2, ["0001", "02ff"]), bytes([0, 1, 2, 255]))

    def test_partial_frame_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "received 1/2 rows"):
            drive.decode_rows(2, 2, ["0001"])

    def test_short_or_non_hex_row_is_rejected(self):
        for row in ("00", "00GG"):
            with self.subTest(row=row), self.assertRaisesRegex(ValueError, "exactly 4"):
                drive.decode_rows(2, 1, [row])

    def test_capture_name_cannot_escape_output_directory(self):
        for name in ("../frame", "/tmp/frame", ".."):
            with self.subTest(name=name):
                self.assertIsNone(drive.SAFE_NAME_RE.fullmatch(name))


if __name__ == "__main__":
    unittest.main()
