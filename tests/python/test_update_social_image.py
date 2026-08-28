import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "update_social_image", ROOT / "scripts/update_social_image.py"
)
social = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(social)


class SocialImageTests(unittest.TestCase):
    def test_exact_two_social_urls_are_bound_to_content_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "hero.png"
            image.write_bytes(b"poster")
            html = root / "index.html"
            url = "https://example.invalid/img/hero.png"
            html.write_text(
                f'<meta property="og:image" content="{url}?old=1" />\n'
                f'<meta name="twitter:image" content="{url}" />\n',
                encoding="utf-8",
            )
            digest = social.update(html, image, url)
            self.assertEqual(digest, hashlib.sha256(b"poster").hexdigest())
            self.assertEqual(html.read_text().count(f"{url}?sha256={digest}"), 2)

    def test_ambiguous_or_insecure_metadata_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "hero.png"
            image.write_bytes(b"poster")
            html = root / "index.html"
            html.write_text('<meta property="og:image" content="http://bad.invalid/x" />\n')
            with self.assertRaisesRegex(RuntimeError, "HTTPS|exact matching"):
                social.update(html, image, "http://bad.invalid/x")

    def test_inline_demo_and_poster_urls_are_bound_to_exact_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            demo = root / "demo.gif"
            poster = root / "hero.png"
            demo.write_bytes(b"demo")
            poster.write_bytes(b"poster")
            html = root / "index.html"
            html.write_text(
                '<img src="./img/hero.png?old=1" '
                'data-animation="./img/demo.gif" '
                'data-poster="./img/hero.png">',
                encoding="utf-8",
            )
            demo_digest, poster_digest = social.bind_inline_demo(html, demo, poster)
            updated = html.read_text()
            self.assertEqual(demo_digest, hashlib.sha256(b"demo").hexdigest())
            self.assertEqual(poster_digest, hashlib.sha256(b"poster").hexdigest())
            self.assertEqual(updated.count(f"hero.png?sha256={poster_digest}"), 2)
            self.assertEqual(updated.count(f"demo.gif?sha256={demo_digest}"), 1)

    def test_missing_inline_binding_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            demo = root / "demo.gif"
            poster = root / "hero.png"
            demo.write_bytes(b"demo")
            poster.write_bytes(b"poster")
            html = root / "index.html"
            html.write_text('<img src="./img/hero.png">', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "exactly one"):
                social.bind_inline_demo(html, demo, poster)
