import base64
import hashlib
import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "vendor_esp_web_tools", ROOT / "scripts/vendor-esp-web-tools.py"
)
vendor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vendor)


class WebInstallerTests(unittest.TestCase):
    def test_flasher_executes_only_same_origin_scripts(self):
        html = (ROOT / "docs/index.html").read_text()
        self.assertIn("script-src 'self'", html)
        self.assertIn("connect-src 'self'", html)
        self.assertIn('./vendor/esp-web-tools/install-button.js', html)
        self.assertNotIn("unpkg.com", html)
        self.assertNotIn("goatcounter", html.lower())
        self.assertNotIn("gc.zgo.at", html)

    def test_vendored_bundle_matches_the_reviewed_release_hashes(self):
        directory = ROOT / "docs/vendor/esp-web-tools"
        self.assertEqual((directory / "VERSION").read_text(), f"esp-web-tools {vendor.VERSION}\n")
        for name, expected in vendor.FILES.items():
            digest = hashlib.sha256((directory / name).read_bytes()).digest()
            self.assertEqual(base64.b64encode(digest).decode(), expected, name)
        self.assertEqual(
            hashlib.sha256((directory / "LICENSE").read_bytes()).hexdigest(),
            vendor.LICENSE_HASH,
        )


if __name__ == "__main__":
    unittest.main()
