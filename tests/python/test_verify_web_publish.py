import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "verify_web_publish", ROOT / "scripts/verify_web_publish.py"
)
web = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(web)


class Response:
    def __init__(self, payload: bytes, url: str):
        self.payload = payload
        self.url = url

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self):
        return self.payload

    def geturl(self):
        return self.url


def manifest(version: str) -> bytes:
    return json.dumps(
        {
            "version": version,
            "builds": [
                {
                    "chipFamily": "ESP32-S3",
                    "parts": [
                        {"path": "firmware.factory.bin", "offset": 0},
                        {"path": "unifont.bin", "offset": 0x340000},
                    ],
                }
            ],
        }
    ).encode()


class WebPublishTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.firmware = root / "firmware.bin"
        self.font = root / "font.bin"
        self.hero = root / "hero.gif"
        self.demo = root / "demo.gif"
        self.poster = root / "hero.png"
        self.demo_manifest = root / "demo-manifest.json"
        self.cover = root / "m5burner-cover.png"
        self.index = root / "index.html"
        self.firmware.write_bytes(b"factory")
        self.font.write_bytes(b"AUF2font")
        self.hero.write_bytes(b"hero-gif")
        self.demo.write_bytes(b"demo-gif")
        self.poster.write_bytes(b"hero-png")
        self.demo_manifest.write_bytes(b'{"safe":true}\n')
        self.cover.write_bytes(b"cover-png")
        self.index.write_bytes(b"<html>exact social hash</html>")
        self.base = "https://example.invalid/installer/"

    @property
    def media(self):
        return {
            "img/hero.gif": self.hero,
            "img/demo.gif": self.demo,
            "img/hero.png": self.poster,
            "img/demo-manifest.json": self.demo_manifest,
            "img/m5burner-cover.png": self.cover,
        }

    def tearDown(self):
        self.temporary.cleanup()

    def response(self, payload: bytes, path: str, origin: str | None = None):
        base = origin or self.base
        return Response(payload, base.rstrip("/") + "/" + path)

    def test_exact_public_manifest_and_bytes_are_accepted(self):
        responses = [
            self.response(manifest("1.2.3 (advui · engine 2.7.26)"), "manifest.json"),
            self.response(self.firmware.read_bytes(), "firmware.factory.bin"),
            self.response(self.font.read_bytes(), "unifont.bin"),
            self.response(self.hero.read_bytes(), "img/hero.gif"),
            self.response(self.demo.read_bytes(), "img/demo.gif"),
            self.response(self.poster.read_bytes(), "img/hero.png"),
            self.response(self.demo_manifest.read_bytes(), "img/demo-manifest.json"),
            self.response(self.cover.read_bytes(), "img/m5burner-cover.png"),
            self.response(self.index.read_bytes(), "index.html"),
        ]
        with mock.patch.object(web, "urlopen", side_effect=responses):
            web.verify(
                base_url=self.base,
                version="1.2.3 (advui · engine 2.7.26)",
                firmware_path=self.firmware,
                font_path=self.font,
                media_paths=self.media,
                index_path=self.index,
                attempts=1,
                delay=0,
            )

    def test_historical_archive_verifies_exact_bytes_without_root_media(self):
        archive_base = self.base + "versions/v1.2.3/"
        responses = [
            Response(manifest("1.2.3 (advui · engine 2.7.26)"), archive_base + "manifest.json"),
            Response(self.firmware.read_bytes(), archive_base + "firmware.factory.bin"),
            Response(self.font.read_bytes(), archive_base + "unifont.bin"),
        ]
        with mock.patch.object(web, "urlopen", side_effect=responses) as download:
            web.verify(
                base_url=self.base,
                version="1.2.3 (advui · engine 2.7.26)",
                firmware_path=self.firmware,
                font_path=self.font,
                media_paths=self.media,
                archive_tag="v1.2.3",
                attempts=1,
                delay=0,
            )
        self.assertEqual(download.call_count, 3)

    def test_historical_archive_accepts_legacy_shared_font_path(self):
        archive_base = self.base + "versions/v1.2.3/"
        legacy = json.loads(manifest("1.2.3 (advui · engine 2.7.26)"))
        legacy["builds"][0]["parts"][1]["path"] = "../../unifont.bin"
        responses = [
            Response(json.dumps(legacy).encode(), archive_base + "manifest.json"),
            Response(self.firmware.read_bytes(), archive_base + "firmware.factory.bin"),
            Response(self.font.read_bytes(), self.base + "unifont.bin"),
        ]
        with mock.patch.object(web, "urlopen", side_effect=responses):
            web.verify(
                base_url=self.base,
                version="1.2.3 (advui · engine 2.7.26)",
                firmware_path=self.firmware,
                font_path=self.font,
                media_paths=self.media,
                archive_tag="v1.2.3",
                attempts=1,
                delay=0,
            )

    def test_stale_manifest_is_retried(self):
        responses = [
            self.response(manifest("1.2.2 (advui · engine 2.7.26)"), "manifest.json"),
            self.response(manifest("1.2.3 (advui · engine 2.7.26)"), "manifest.json"),
            self.response(self.firmware.read_bytes(), "firmware.factory.bin"),
            self.response(self.font.read_bytes(), "unifont.bin"),
            self.response(self.hero.read_bytes(), "img/hero.gif"),
            self.response(self.demo.read_bytes(), "img/demo.gif"),
            self.response(self.poster.read_bytes(), "img/hero.png"),
            self.response(self.demo_manifest.read_bytes(), "img/demo-manifest.json"),
            self.response(self.cover.read_bytes(), "img/m5burner-cover.png"),
            self.response(self.index.read_bytes(), "index.html"),
        ]
        with mock.patch.object(web, "urlopen", side_effect=responses) as download:
            web.verify(
                base_url=self.base,
                version="1.2.3 (advui · engine 2.7.26)",
                firmware_path=self.firmware,
                font_path=self.font,
                media_paths=self.media,
                index_path=self.index,
                attempts=2,
                delay=0,
            )
        self.assertEqual(download.call_count, 10)

    def test_public_byte_drift_fails_closed(self):
        responses = [
            self.response(manifest("1.2.3 (advui · engine 2.7.26)"), "manifest.json"),
            self.response(b"wrong", "firmware.factory.bin"),
        ]
        with mock.patch.object(web, "urlopen", side_effect=responses):
            with self.assertRaisesRegex(web.WebPublishError, "bytes differ"):
                web.verify(
                    base_url=self.base,
                    version="1.2.3 (advui · engine 2.7.26)",
                    firmware_path=self.firmware,
                    font_path=self.font,
                    media_paths=self.media,
                    index_path=self.index,
                    attempts=1,
                    delay=0,
                )

    def test_cross_origin_redirect_fails_closed(self):
        responses = [self.response(manifest("1.2.3"), "manifest.json", "https://other.invalid")]
        with mock.patch.object(web, "urlopen", side_effect=responses):
            with self.assertRaisesRegex(web.WebPublishError, "outside its origin"):
                web.verify(
                    base_url=self.base,
                    version="1.2.3",
                    firmware_path=self.firmware,
                    font_path=self.font,
                    media_paths=self.media,
                    index_path=self.index,
                    attempts=1,
                    delay=0,
                )

    def test_stale_public_media_fails_closed(self):
        responses = [
            self.response(manifest("1.2.3"), "manifest.json"),
            self.response(self.firmware.read_bytes(), "firmware.factory.bin"),
            self.response(self.font.read_bytes(), "unifont.bin"),
            self.response(b"stale", "img/hero.gif"),
        ]
        with mock.patch.object(web, "urlopen", side_effect=responses):
            with self.assertRaisesRegex(web.WebPublishError, "img/hero.gif bytes differ"):
                web.verify(
                    base_url=self.base,
                    version="1.2.3",
                    firmware_path=self.firmware,
                    font_path=self.font,
                    media_paths=self.media,
                    index_path=self.index,
                    attempts=1,
                    delay=0,
                )
