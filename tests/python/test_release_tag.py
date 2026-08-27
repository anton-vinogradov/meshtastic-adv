import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("release_tag", ROOT / "scripts/release_tag.py")
release_tag = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(release_tag)


class ReleaseTagTests(unittest.TestCase):
    def test_stable_tag(self):
        self.assertEqual(release_tag.parse_tag("v1.0.0"), ("1.0.0", False))

    def test_prerelease_tags(self):
        self.assertEqual(release_tag.parse_tag("v1.0.0-rc.1"), ("1.0.0-rc.1", True))
        self.assertEqual(release_tag.parse_tag("v2.7.26-beta"), ("2.7.26-beta", True))

    def test_invalid_or_ambiguous_tags_fail_closed(self):
        for tag in ("1.0.0", "v1.0", "v01.0.0", "v1.0.0-01", "v1.0.0+local", "vx.y.z"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release_tag.parse_tag(tag)

    def test_cli_emits_github_output_syntax(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "scripts/release_tag.py"), "v1.0.0-rc.1"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "version=1.0.0-rc.1\nprerelease=true\n")


if __name__ == "__main__":
    unittest.main()
