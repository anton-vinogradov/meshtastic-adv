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
