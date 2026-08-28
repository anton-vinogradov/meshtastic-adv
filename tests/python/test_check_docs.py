import importlib.util
import json
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

    def test_demo_plays_inline_and_remains_user_controlled(self):
        site = (ROOT / "docs/index.html").read_text()
        script = (ROOT / "docs/site.js").read_text()
        css = (ROOT / "docs/site.css").read_text()
        manifest = json.loads((ROOT / "docs/img/demo-manifest.json").read_text())
        demo_hash = manifest["outputs"]["screen"]["sha256"]
        poster_hash = manifest["outputs"]["poster"]["sha256"]
        self.assertIn(f'src="./img/hero.png?sha256={poster_hash}"', site)
        self.assertIn(f'data-animation="./img/demo.gif?sha256={demo_hash}"', site)
        self.assertIn(f'data-poster="./img/hero.png?sha256={poster_hash}"', site)
        duration = sum(
            frame["delay_ms"]
            for frame in json.loads((ROOT / "docs/img/demo-story.json").read_text())["frames"]
        )
        self.assertIn(f'data-duration-ms="{duration}"', site)
        self.assertIn('id="demo-toggle"', site)
        self.assertIn(".demo-toggle[hidden] { display: none; }", css)
        self.assertIn("aspect-ratio: 16 / 9", css)
        self.assertIn('toggle.textContent = playing ? "Stop 16-second demo"', script)
        self.assertIn('matchMedia("(prefers-reduced-motion: reduce)")', script)
        self.assertIn("connection?.saveData", script)
        self.assertIn("window.setTimeout(stop, duration)", script)
        self.assertNotIn('aria-pressed', script)
        self.assertIn("if (playing) stop()", script)
        for name in ("README.md", "README.ru.md"):
            readme = (ROOT / name).read_text()
            self.assertIn(f'<img src="docs/img/demo.gif?sha256={demo_hash}"', readme)
            self.assertNotIn("open the 16-second demo", readme)
            self.assertNotIn("открыть 16-секундное демо", readme)
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

    def test_html_data_media_and_query_strings_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            docs = root / "docs"
            media = docs / "img"
            media.mkdir(parents=True)
            (root / "README.md").write_text("# Demo\n")
            (media / "demo.gif").write_bytes(b"gif")
            (docs / "index.html").write_text(
                '<img data-animation="./img/demo.gif?sha256=abc" '
                'data-poster="./img/missing.png?sha256=def">'
            )
            with self.assertRaisesRegex(ValueError, "missing"):
                check_docs.validate(root)
            (media / "missing.png").write_bytes(b"png")
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
