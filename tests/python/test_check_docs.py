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
        self.assertGreater(result["references"], 40)

    def test_installer_has_social_preview_metadata(self):
        site = (ROOT / "docs/index.html").read_text()
        self.assertIn('property="og:image"', site)
        self.assertIn('/meshtastic-adv/img/hero.png', site)
        self.assertIn('name="twitter:card" content="summary_large_image"', site)
        self.assertIn('name="twitter:image:alt"', site)

    def test_site_demo_is_user_controlled_and_legacy_private_gallery_is_gone(self):
        site = (ROOT / "docs/index.html").read_text()
        script = (ROOT / "docs/site.js").read_text()
        css = (ROOT / "docs/site.css").read_text()
        self.assertIn('src="./img/hero.png"', site)
        self.assertIn('data-animation="./img/hero.gif"', site)
        self.assertIn('id="demo-toggle"', site)
        self.assertIn(".demo-toggle[hidden] { display: none; }", css)
        self.assertIn('toggle.textContent = playing ? "Stop animation"', script)
        self.assertNotIn('aria-pressed', script)
        self.assertIn('playing = !playing', script)
        for name in ("nodes.png", "chat.png", "chats.png", "settings.png", "lora.png"):
            self.assertFalse((ROOT / "docs/img" / name).exists())
            self.assertNotIn(name, (ROOT / "README.md").read_text())
            self.assertNotIn(name, (ROOT / "README.ru.md").read_text())

    def test_readme_install_ctas_are_outside_the_raw_html_block(self):
        for name in ("README.md", "README.ru.md"):
            content = (ROOT / name).read_text()
            self.assertRegex(content, r"</p>\n\n> \*\*\[▶ ")

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

    def test_html_fragment_ids_are_checked_locally(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            docs = root / "docs"
            docs.mkdir()
            (root / "README.md").write_text("# Demo\n")
            (docs / "index.html").write_text('<a href="#install">Install</a><section id="install"></section>')
            check_docs.validate(root)
            (docs / "index.html").write_text('<a href="#missing">Broken</a><section id="install"></section>')
            with self.assertRaisesRegex(ValueError, "missing anchor #missing"):
                check_docs.validate(root)


if __name__ == "__main__":
    unittest.main()
