import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("site_release_mode", ROOT / "scripts/site_release_mode.py")
site = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(site)


class SiteReleaseModeTests(unittest.TestCase):
    def mode(self, current, target, archives=()):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"version": current}))
            archive_root = root / "versions"
            archive_root.mkdir()
            for folder, label in archives:
                child = archive_root / folder
                child.mkdir()
                (child / "manifest.json").write_text(json.dumps({"version": label}))
            return site.release_mode(manifest, archive_root, target)

    def test_promote_repair_and_historical(self):
        self.assertEqual(self.mode("1.0.4 (advui · engine 2.7.26)", "1.0.5"), "promote")
        self.assertEqual(self.mode("1.0.5 (advui · engine 2.7.26)", "1.0.5"), "repair")
        with self.assertRaisesRegex(ValueError, "retained window"):
            self.mode("1.0.6 (advui · engine 2.7.26)", "1.0.5")

    def test_malformed_versions_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "target stable"):
            self.mode("1.0.4 (advui · engine 2.7.26)", "1.0")
        with self.assertRaisesRegex(ValueError, "current installer"):
            self.mode("latest", "1.0.5")

    def test_newer_archive_prevents_rollback_when_root_has_drifted(self):
        archives = (
            ("v1.0.3", "1.0.3 (advui · engine 2.7.26)"),
            ("v1.0.4", "1.0.4 (advui · engine 2.7.26)"),
        )
        self.assertEqual(
            self.mode("1.0.2 (advui · engine 2.7.26)", "1.0.3", archives),
            "historical",
        )
        self.assertEqual(
            self.mode("1.0.2 (advui · engine 2.7.26)", "1.0.4", archives),
            "repair",
        )
        self.assertEqual(
            self.mode("1.0.2 (advui · engine 2.7.26)", "1.0.5", archives),
            "promote",
        )

    def test_malformed_or_mismatched_archive_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "folder/manifest version mismatch"):
            self.mode(
                "1.0.2 (advui · engine 2.7.26)",
                "1.0.5",
                (("v1.0.4", "1.0.3 (advui · engine 2.7.26)"),),
            )
        with self.assertRaisesRegex(ValueError, "invalid installer archive directory"):
            self.mode(
                "1.0.2 (advui · engine 2.7.26)",
                "1.0.5",
                (("latest", "1.0.4 (advui · engine 2.7.26)"),),
            )

    def test_pruned_archive_is_retired_but_a_gap_in_the_window_fails(self):
        archives = (
            ("v1.0.6", "1.0.6 (advui · engine 2.7.26)"),
            ("v1.0.5", "1.0.5 (advui · engine 2.7.26)"),
            ("v1.0.4", "1.0.4 (advui · engine 2.7.26)"),
            ("v1.0.3", "1.0.3 (advui · engine 2.7.26)"),
            ("v1.0.2", "1.0.2 (advui · engine 2.7.26)"),
            ("v1.0.1", "1.0.1 (advui · engine 2.7.26)"),
        )
        self.assertEqual(
            self.mode("1.0.6 (advui · engine 2.7.26)", "1.0.0", archives),
            "retired",
        )
        with self.assertRaisesRegex(ValueError, "retained window"):
            self.mode("1.0.6 (advui · engine 2.7.26)", "1.0.0", archives[:-1])
        archives_with_gap = archives[:3] + archives[4:] + (
            ("v1.0.0", "1.0.0 (advui · engine 2.7.26)"),
        )
        with self.assertRaisesRegex(ValueError, "retained window"):
            self.mode("1.0.6 (advui · engine 2.7.26)", "1.0.3", archives_with_gap)

    def test_latest_tag_uses_the_maximum_of_site_and_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"version": "1.0.2 (advui · engine 2.7.26)"}))
            archives = root / "versions"
            child = archives / "v1.0.4"
            child.mkdir(parents=True)
            (child / "manifest.json").write_text(
                json.dumps({"version": "1.0.4 (advui · engine 2.7.26)"})
            )
            self.assertEqual(site.highest_published_tuple(manifest, archives), (1, 0, 4))
            self.assertEqual(max((1, 0, 4), site.target_tuple("1.0.5")), (1, 0, 5))
            self.assertEqual(max((1, 0, 4), site.target_tuple("1.0.3")), (1, 0, 4))


if __name__ == "__main__":
    unittest.main()
