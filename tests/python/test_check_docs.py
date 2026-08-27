import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_docs", ROOT / "scripts/check-docs.py")
check_docs = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check_docs
SPEC.loader.exec_module(check_docs)


class DocumentationTests(unittest.TestCase):
    def test_repository_documentation_links_are_valid(self):
        result = check_docs.validate(ROOT)
        self.assertGreaterEqual(result["documents"], 9)
        self.assertGreater(result["references"], 60)

    def test_installer_has_social_preview_metadata(self):
        site = (ROOT / "docs/index.html").read_text()
        self.assertIn('property="og:image"', site)
        self.assertIn('/meshtastic-adv/img/hero.png', site)
        self.assertIn('name="twitter:card" content="summary_large_image"', site)

    def test_missing_local_asset_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("# Demo\n\n![missing](docs/nope.png)\n")
            with self.assertRaisesRegex(ValueError, "missing docs/nope.png"):
                check_docs.validate(root)

    def test_missing_markdown_anchor_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("# Demo\n\n[bad](HARDWARE.md#missing)\n")
            (root / "HARDWARE.md").write_text("# Hardware\n")
            with self.assertRaisesRegex(ValueError, "missing anchor #missing"):
                check_docs.validate(root)


if __name__ == "__main__":
    unittest.main()
