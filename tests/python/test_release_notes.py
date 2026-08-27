import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("release_notes", ROOT / "scripts/release_notes.py")
release_notes = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(release_notes)


class ReleaseNotesTests(unittest.TestCase):
    def test_extracts_only_exact_section_and_canonical_link(self):
        changelog = """# Changelog

## [Unreleased]

Later.

## [1.2.3] - 2026-08-28

### Fixed

- Useful details.

## [1.2.2] - 2026-08-27

- Older details.

[1.2.3]: https://github.com/example/project/compare/v1.2.2...v1.2.3
"""
        self.assertEqual(
            release_notes.changelog_notes(changelog, "1.2.3"),
            "### Fixed\n\n- Useful details.\n\n"
            "[Full changelog](https://github.com/example/project/compare/v1.2.2...v1.2.3)\n",
        )

    def test_missing_duplicate_or_empty_section_fails_closed(self):
        fixtures = (
            "## [1.2.2]\n\nold\n[1.2.3]: https://example.test/compare\n",
            "## [1.2.3]\n\none\n## [1.2.3]\n\ntwo\n[1.2.3]: https://example.test/compare\n",
            "## [1.2.3]\n\n## [1.2.2]\n\nold\n[1.2.3]: https://example.test/compare\n",
        )
        for changelog in fixtures:
            with self.subTest(changelog=changelog), self.assertRaises(ValueError):
                release_notes.changelog_notes(changelog, "1.2.3")

    def test_missing_or_duplicate_comparison_link_fails_closed(self):
        for references in (
            "",
            "[1.2.3]: https://example.test/one\n[1.2.3]: https://example.test/two\n",
        ):
            changelog = f"## [1.2.3]\n\nnotes\n\n## [1.2.2]\n\nold\n\n{references}"
            with self.subTest(references=references), self.assertRaises(ValueError):
                release_notes.changelog_notes(changelog, "1.2.3")

    def test_stable_cli_writes_notes_but_prerelease_leaves_no_file(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            changelog = temp / "CHANGELOG.md"
            output = temp / "notes.md"
            changelog.write_text(
                "## [1.2.3]\n\n- notes\n\n## [1.2.2]\n\n- old\n\n"
                "[1.2.3]: https://example.test/compare\n"
            )
            stable = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts/release_notes.py"),
                    "v1.2.3",
                    "--changelog",
                    str(changelog),
                    "--output",
                    str(output),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(stable.returncode, 0, stable.stderr)
            self.assertIn("- notes", output.read_text())
            output.unlink()
            prerelease = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts/release_notes.py"),
                    "v1.2.3-rc.1",
                    "--changelog",
                    str(changelog),
                    "--output",
                    str(output),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(prerelease.returncode, 0, prerelease.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
