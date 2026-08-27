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
            "builds": [{"chipFamily": "ESP32-S3", "parts": [
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

    def test_rejects_duplicate_or_unordered_index(self):
        docs = self.fixture()
        (docs / "versions/index.json").write_text('["v1.2.3", "v1.2.3"]')
        with self.assertRaises(ValueError):
            check_installer.validate(docs)

    def test_rejects_corrupt_older_archive(self):
        docs = self.fixture()
        source = docs / "versions/v1.2.3"
        older = docs / "versions/v1.2.2"
        older.mkdir()
        older_manifest = json.loads((source / "manifest.json").read_text())
        older_manifest["version"] = "1.2.2 (advui · engine 2.7.26)"
        (older / "manifest.json").write_text(json.dumps(older_manifest))
        (older / "firmware.factory.bin").write_bytes(b"truncated")
        (docs / "versions/index.json").write_text('["v1.2.3", "v1.2.2"]')
        with self.assertRaises(ValueError):
            check_installer.validate(docs)

    def test_rejects_unindexed_archive_directory(self):
        docs = self.fixture()
        (docs / "versions/v1.2.2").mkdir()
        with self.assertRaises(ValueError):
            check_installer.validate(docs)

    def test_prune_keeps_newest_six_and_rewrites_exact_index(self):
        docs = self.fixture()
        source = docs / "versions/v1.2.3"
        for patch in range(2, 9):
            name = f"v1.2.{patch}"
            if name == "v1.2.3":
                continue
            archive = docs / "versions" / name
            archive.mkdir()
            manifest = json.loads((source / "manifest.json").read_text())
            manifest["version"] = f"1.2.{patch} (advui · engine 2.7.26)"
            (archive / "manifest.json").write_text(json.dumps(manifest))
            image = bytearray((source / "firmware.factory.bin").read_bytes())
            (archive / "firmware.factory.bin").write_bytes(image)
        kept = check_installer.prune_archives(docs)
        self.assertEqual(kept, [f"v1.2.{patch}" for patch in range(8, 2, -1)])
        self.assertFalse((docs / "versions/v1.2.2").exists())
        self.assertEqual(json.loads((docs / "versions/index.json").read_text()), kept)

    def test_prune_rejects_all_unsafe_entries_before_deleting(self):
        docs = self.fixture()
        oldest = docs / "versions/v0.0.1"
        oldest.mkdir()
        (docs / "versions/v unsafe").mkdir()
        with self.assertRaises(ValueError):
            check_installer.prune_archives(docs)
        self.assertTrue(oldest.exists())

    def test_archive_rejects_noncanonical_or_prerelease_versions(self):
        for name in ("v01.2.3", "v1.2.3-rc.1", "v1.2.3+local"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                check_installer.archive_key(name)


if __name__ == "__main__":
    unittest.main()
