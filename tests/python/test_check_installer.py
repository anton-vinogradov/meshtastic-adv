import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_installer", ROOT / "scripts/check-installer.py")
check_installer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check_installer
SPEC.loader.exec_module(check_installer)


class InstallerConsistencyTests(unittest.TestCase):
    def fixture(self, engine: str = "2.7.26") -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        docs = Path(temporary.name)
        archive = docs / "versions/v1.2.3"
        archive.mkdir(parents=True)
        image = bytearray(check_installer.APP_OFFSET + 128)
        image[check_installer.APP_OFFSET] = 0xE9
        image[check_installer.APP_OFFSET + 16:check_installer.APP_OFFSET + 16 + len(engine)] = engine.encode()
        (docs / "firmware.factory.bin").write_bytes(image)
        (archive / "firmware.factory.bin").write_bytes(image)
        with (docs / "unifont.bin").open("wb") as font:
            font.write(b"AUF2")
            font.seek(check_installer.FONT_SIZE_AUF2 - 1)
            font.write(b"\0")
        manifest = {
            "version": f"1.2.3 (advui · engine {engine})",
            "builds": [{"parts": [
                {"path": "firmware.factory.bin", "offset": 0},
                {"path": "unifont.bin", "offset": check_installer.FONT_OFFSET},
            ]}],
        }
        (docs / "manifest.json").write_text(json.dumps(manifest))
        archived_manifest = json.loads(json.dumps(manifest))
        archived_manifest["builds"][0]["parts"][1]["path"] = "../../unifont.bin"
        (archive / "manifest.json").write_text(json.dumps(archived_manifest))
        (docs / "versions/index.json").write_text('["v1.2.3"]')
        return docs

    def test_valid_installer(self):
        self.assertEqual(check_installer.validate(self.fixture())["engine"], "2.7.26")

    def test_repository_installer_is_consistent(self):
        result = check_installer.validate(ROOT / "docs")
        self.assertRegex(result["release"], r"^\d+\.\d+\.\d+$")

    def test_rejects_engine_not_present_in_factory(self):
        docs = self.fixture()
        manifest = json.loads((docs / "manifest.json").read_text())
        manifest["version"] = "1.2.3 (advui · engine 2.8.0)"
        (docs / "manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            check_installer.validate(docs)

    def test_rejects_archive_byte_drift(self):
        docs = self.fixture()
        path = docs / "versions/v1.2.3/firmware.factory.bin"
        path.write_bytes(path.read_bytes() + b"drift")
        with self.assertRaises(ValueError):
            check_installer.validate(docs)

    def test_rejects_incomplete_font(self):
        docs = self.fixture()
        (docs / "unifont.bin").write_bytes(b"AUF2truncated")
        with self.assertRaises(ValueError):
            check_installer.validate(docs)


if __name__ == "__main__":
    unittest.main()
