import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_size", ROOT / "scripts/check-size.py")
check_size = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check_size
SPEC.loader.exec_module(check_size)


class SizeGateTests(unittest.TestCase):
    def write_map(self, text: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "firmware.map"
        path.write_text(text)
        return path

    def test_map_sections_include_noinit_dram(self):
        path = self.write_map(
            """
 .iram0.vectors  0x40370000  0x10
 .iram0.text     0x40370010  0x20
 .dram0.data     0x3fc90000  0x30
 .noinit         0x3fc90030  0x40
 .dram0.bss      0x3fc90070  0x50
"""
        )
        self.assertEqual(
            check_size.map_sections(path),
            {
                ".iram0.vectors": 0x10,
                ".iram0.text": 0x20,
                ".dram0.data": 0x30,
                ".noinit": 0x40,
                ".dram0.bss": 0x50,
            },
        )

    def test_map_sections_reject_missing_noinit(self):
        path = self.write_map(
            """
 .iram0.vectors  0x40370000  0x10
 .iram0.text     0x40370010  0x20
 .dram0.data     0x3fc90000  0x30
 .dram0.bss      0x3fc90030  0x50
"""
        )
        with self.assertRaises(SystemExit):
            check_size.map_sections(path)


if __name__ == "__main__":
    unittest.main()
