import copy
import hashlib
import importlib.util
import struct
import sys
import tempfile
import types
import unittest
import zlib
from pathlib import Path
from unittest import mock

try:
    from PIL import Image
except ImportError:
    Image = None


ROOT = Path(__file__).resolve().parents[2]
if "requests" not in sys.modules:
    requests_stub = types.ModuleType("requests")
    requests_stub.RequestException = RuntimeError
    requests_stub.Session = lambda: None
    sys.modules["requests"] = requests_stub
SPEC = importlib.util.spec_from_file_location("m5burner_publish", ROOT / "scripts/m5burner_publish.py")
m5 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m5)


class Response:
    def __init__(self, payload):
        self.payload = payload

    def raise_for_status(self):
        return None

    def json(self):
        return self.payload


class BinaryResponse:
    def __init__(self, payload, url=None):
        self.payload = payload
        self.url = url or f"{m5.CDN}/ready.bin"

    def raise_for_status(self):
        return None

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def iter_content(self, chunk_size):
        return (self.payload[i:i + chunk_size] for i in range(0, len(self.payload), chunk_size))


def png(rgb, compression=6):
    def chunk(name, value):
        checksum = zlib.crc32(name + value) & 0xFFFFFFFF
        return struct.pack(">I", len(value)) + name + value + struct.pack(">I", checksum)

    scanline = b"\x00" + bytes(rgb) * 1000
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 1000, 1000, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanline * 1000, compression))
        + chunk(b"IEND", b"")
    )


class FakeSession:
    def __init__(self, versions=()):
        self.cookies = {"m5_auth_token": "fixture-token"}
        self.headers = {}
        self.firmware = {
            "fid": m5.FID,
            "name": "Meshtastic ADV",
            "description": "fixture",
            "category": "cardputer",
            "author": "randoom",
            "github": "https://github.com/anton-vinogradov/meshtastic-adv",
            "cover": "ready.png",
            "versions": [dict(item) for item in versions],
        }
        self.public_firmware = copy.deepcopy(self.firmware)
        self.public_firmware["versions"] = [
            item for item in self.public_firmware["versions"] if item.get("published") is True
        ]
        self.uploads = 0
        self.updates = 0
        self.publishes = 0
        self.uploaded_cover = None
        self.staged_cover_id = None

    def post(self, url, **kwargs):
        if url == m5.LOGIN_API:
            return Response({"status": 1})
        self.uploads += 1
        if "cover" in kwargs["files"]:
            self.uploaded_cover = kwargs["files"]["cover"].read()
            self.staged_cover_id = "uploaded.png"
        self.firmware["versions"].append(
            {"version": kwargs["data"]["version"], "file": "uploaded.bin", "published": False}
        )
        return Response({"status": 1})

    def get(self, url, **_kwargs):
        return Response([self.firmware])

    def put(self, url, **_kwargs):
        if "/version/" in url:
            self.updates += 1
            if "cover" in _kwargs.get("files", {}):
                self.uploaded_cover = _kwargs["files"]["cover"].read()
                self.staged_cover_id = "updated.png"
            old_file_id = url.rsplit("/", 1)[-1]
            for version in self.firmware["versions"]:
                if version.get("file") == old_file_id:
                    version["file"] = "updated.bin"
            return Response({"status": 1})
        self.publishes += 1
        file_id = url.split("/")[-2]
        for version in self.firmware["versions"]:
            if version.get("file") == file_id:
                version["published"] = True
        if self.staged_cover_id:
            self.firmware["cover"] = self.staged_cover_id
        self.public_firmware = copy.deepcopy(self.firmware)
        self.public_firmware["versions"] = [
            item for item in self.public_firmware["versions"] if item.get("published") is True
        ]
        return Response({"status": 1})


class M5BurnerPublisherTests(unittest.TestCase):
    @staticmethod
    def public_get(session, firmware_bytes, cover_bytes, *, reject_cover_before_publish=False):
        def get(url, **_kwargs):
            if url == m5.PUBLIC_API:
                return Response([session.public_firmware])
            if url.startswith(m5.COVER_CDN + "/"):
                if reject_cover_before_publish and session.publishes == 0:
                    raise AssertionError("pending cover must not be checked before publish")
                return BinaryResponse(cover_bytes, url=url)
            return BinaryResponse(firmware_bytes, url=url)
        return get

    def publish(self, session, version="1.0.1"):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "firmware.bin"
            binary.write_bytes(b"firmware")
            cover = Path(directory) / "cover.png"
            cover.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" + (1000).to_bytes(4, "big") * 2)
            with mock.patch.object(
                m5.requests,
                "get",
                side_effect=self.public_get(session, b"firmware", cover.read_bytes()),
                create=True,
            ):
                return m5.publish(
                    session,
                    email="fixture@example.invalid",
                    password="secret",
                    version=version,
                    binary=binary,
                    cover=cover,
                    listing_title="Meshtastic ADV",
                    listing_description="fixture",
                )

    def test_existing_published_version_is_verified_without_mutation(self):
        session = FakeSession([{"version": "1.0.1", "file": "ready.bin", "published": True}])
        self.assertEqual(self.publish(session), "ready.bin")
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.updates, 0)
        self.assertEqual(session.publishes, 0)

    def test_half_published_version_is_resumed_instead_of_skipped(self):
        session = FakeSession([{"version": "1.0.1", "file": "pending.bin", "published": False}])
        self.assertEqual(self.publish(session), "updated.bin")
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.updates, 1)
        self.assertEqual(session.publishes, 1)
        self.assertIsNotNone(session.uploaded_cover)

    def test_pending_cover_is_verified_only_after_publish(self):
        session = FakeSession([{"version": "1.0.1", "file": "pending.bin", "published": False}])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "firmware.bin"
            binary.write_bytes(b"firmware")
            cover = root / "cover.png"
            cover.write_bytes(png((5, 6, 10)))
            with mock.patch.object(
                m5.requests,
                "get",
                side_effect=self.public_get(
                    session, b"firmware", cover.read_bytes(), reject_cover_before_publish=True
                ),
                create=True,
            ):
                self.assertEqual(
                    m5.publish(
                        session,
                        email="fixture@example.invalid",
                        password="secret",
                        version="1.0.1",
                        binary=binary,
                        cover=cover,
                        listing_title="Meshtastic ADV",
                        listing_description="fixture",
                    ),
                    "updated.bin",
                )
        self.assertEqual(session.publishes, 1)

    def test_wrong_pending_binary_is_never_published(self):
        session = FakeSession([{"version": "1.0.1", "file": "pending.bin", "published": False}])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "firmware.bin"
            binary.write_bytes(b"expected")
            cover = root / "cover.png"
            cover.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" + (1000).to_bytes(4, "big") * 2)
            public_get = self.public_get(session, b"stale-or-corrupt", cover.read_bytes())
            with mock.patch.object(
                m5.requests,
                "get",
                side_effect=public_get,
                create=True,
            ):
                with self.assertRaisesRegex(RuntimeError, "public binary did not converge"):
                    m5.publish(
                        session,
                        email="fixture@example.invalid",
                        password="secret",
                        version="1.0.1",
                        binary=binary,
                        cover=cover,
                        listing_title="Meshtastic ADV",
                        listing_description="fixture",
                    )
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.updates, 1)
        self.assertEqual(session.publishes, 0)

    def test_missing_version_is_uploaded_published_and_verified(self):
        session = FakeSession()
        self.assertEqual(self.publish(session), "uploaded.bin")
        self.assertEqual(session.uploads, 1)
        self.assertEqual(session.updates, 0)
        self.assertEqual(session.publishes, 1)

    def test_exact_square_png_cover_is_uploaded(self):
        session = FakeSession()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "firmware.bin"
            binary.write_bytes(b"firmware")
            cover = root / "cover.png"
            cover.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" + (1000).to_bytes(4, "big") * 2)
            with mock.patch.object(
                m5.requests,
                "get",
                side_effect=self.public_get(session, b"firmware", cover.read_bytes()),
                create=True,
            ):
                m5.publish(
                    session,
                    email="fixture@example.invalid",
                    password="secret",
                    version="1.0.1",
                    binary=binary,
                    cover=cover,
                    listing_title="Meshtastic ADV",
                    listing_description="fixture",
                )
            self.assertEqual(session.uploaded_cover, cover.read_bytes())

    def test_invalid_cover_fails_before_login(self):
        session = FakeSession()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "firmware.bin"
            binary.write_bytes(b"firmware")
            cover = root / "cover.png"
            cover.write_bytes(b"not a png")
            with self.assertRaisesRegex(RuntimeError, "not a PNG"):
                m5.publish(
                    session,
                    email="fixture@example.invalid",
                    password="secret",
                    version="1.0.1",
                    binary=binary,
                    cover=cover,
                    listing_title="Meshtastic ADV",
                    listing_description="fixture",
                )
        self.assertEqual(session.uploads, 0)

    def test_duplicate_version_fails_before_any_mutation(self):
        session = FakeSession(
            [
                {"version": "1.0.1", "file": "one.bin", "published": False},
                {"version": "1.0.1", "file": "two.bin", "published": False},
            ]
        )
        with self.assertRaisesRegex(RuntimeError, "duplicate version"):
            self.publish(session)
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.updates, 0)
        self.assertEqual(session.publishes, 0)

    def test_historical_absent_or_pending_version_cannot_be_published(self):
        for older in (None, {"version": "1.0.1", "file": "old.bin", "published": False}):
            versions = [{"version": "1.0.2", "file": "new.bin", "published": True}]
            if older:
                versions.append(older)
            session = FakeSession(versions)
            with self.assertRaisesRegex(RuntimeError, "historical"):
                self.publish(session)
            self.assertEqual(session.uploads, 0)
            self.assertEqual(session.updates, 0)
            self.assertEqual(session.publishes, 0)

    def test_historical_already_published_version_is_verify_only(self):
        session = FakeSession(
            [
                {"version": "1.0.2", "file": "new.bin", "published": True},
                {"version": "1.0.1", "file": "ready.bin", "published": True},
            ]
        )
        self.assertEqual(self.publish(session), "ready.bin")
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.updates, 0)
        self.assertEqual(session.publishes, 0)

    def test_historical_public_rerun_ignores_current_global_listing_and_cover(self):
        session = FakeSession(
            [
                {"version": "1.0.2", "file": "new.bin", "published": True},
                {"version": "1.0.1", "file": "ready.bin", "published": True},
            ]
        )
        session.firmware["description"] = "copy for a newer release"
        session.firmware["cover"] = "newer.png"
        session.public_firmware["description"] = "copy for a newer release"
        session.public_firmware["cover"] = "newer.png"
        self.assertEqual(self.publish(session), "ready.bin")

    def test_latest_listing_drift_fails_before_login_or_mutation(self):
        session = FakeSession()
        session.firmware["description"] = "stale"
        session.public_firmware["description"] = "stale"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "firmware.bin"
            binary.write_bytes(b"firmware")
            cover = root / "cover.png"
            cover.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" + (1000).to_bytes(4, "big") * 2)
            with mock.patch.object(
                m5.requests, "get", return_value=Response([session.firmware]), create=True
            ):
                with self.assertRaisesRegex(RuntimeError, "listing metadata differs"):
                    m5.publish(
                        session,
                        email="fixture@example.invalid",
                        password="secret",
                        version="1.0.1",
                        binary=binary,
                        cover=cover,
                        listing_title="Meshtastic ADV",
                        listing_description="fixture",
                    )
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.updates, 0)

    def test_latest_public_cover_requires_exact_pixels(self):
        session = FakeSession([{"version": "1.0.1", "file": "ready.bin", "published": True}])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "firmware.bin"
            binary.write_bytes(b"firmware")
            cover = root / "cover.png"
            cover.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" + (1000).to_bytes(4, "big") * 2)
            with mock.patch.object(
                m5.requests,
                "get",
                side_effect=self.public_get(session, b"firmware", b"stale-cover"),
                create=True,
            ):
                with self.assertRaisesRegex(RuntimeError, "public cover did not converge"):
                    m5.publish(
                        session,
                        email="fixture@example.invalid",
                        password="secret",
                        version="1.0.1",
                        binary=binary,
                        cover=cover,
                        listing_title="Meshtastic ADV",
                        listing_description="fixture",
                    )

    @unittest.skipUnless(Image is not None, "Pillow is installed in the firmware CI job")
    def test_public_cover_allows_lossless_png_reencoding(self):
        with tempfile.TemporaryDirectory() as directory:
            cover = Path(directory) / "cover.png"
            cover.write_bytes(png((5, 6, 10), compression=1))
            public = png((5, 6, 10), compression=9)
            self.assertNotEqual(hashlib.sha256(cover.read_bytes()).digest(), hashlib.sha256(public).digest())
            with mock.patch.object(
                m5.requests,
                "get",
                return_value=BinaryResponse(public, url=f"{m5.COVER_CDN}/ready.png"),
                create=True,
            ):
                m5.verify_public_cover("ready.png", cover, attempts=1, delay=0)

    @unittest.skipUnless(Image is not None, "Pillow is installed in the firmware CI job")
    def test_public_cover_rejects_any_pixel_change(self):
        with tempfile.TemporaryDirectory() as directory:
            cover = Path(directory) / "cover.png"
            cover.write_bytes(png((5, 6, 10)))
            with mock.patch.object(
                m5.requests,
                "get",
                return_value=BinaryResponse(
                    png((5, 6, 11)), url=f"{m5.COVER_CDN}/ready.png"
                ),
                create=True,
            ):
                with self.assertRaisesRegex(RuntimeError, "cover did not converge"):
                    m5.verify_public_cover("ready.png", cover, attempts=1, delay=0)

    def test_public_verification_requires_exact_file_and_published_flag(self):
        session = FakeSession([{"version": "1.0.1", "file": "wrong.bin", "published": True}])
        with mock.patch.object(m5.requests, "get", return_value=Response([session.firmware]), create=True):
            with self.assertRaisesRegex(RuntimeError, "public catalog"):
                m5.verify_public_version("1.0.1", "expected.bin", attempts=1, delay=0)

    def test_public_binary_requires_exact_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "firmware.bin"
            binary.write_bytes(b"expected")
            with mock.patch.object(
                m5.requests,
                "get",
                return_value=BinaryResponse(b"different"),
                create=True,
            ):
                with self.assertRaisesRegex(RuntimeError, "public binary did not converge"):
                    m5.verify_public_binary("ready.bin", binary, attempts=1, delay=0)

    def test_public_binary_rejects_cross_origin_redirect(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "firmware.bin"
            binary.write_bytes(b"firmware")
            response = BinaryResponse(b"firmware", url="https://example.invalid/ready.bin")
            with mock.patch.object(m5.requests, "get", return_value=response, create=True):
                with self.assertRaisesRegex(RuntimeError, "public binary did not converge"):
                    m5.verify_public_binary("ready.bin", binary, attempts=1, delay=0)

    def test_listing_copy_parser_is_strict(self):
        with tempfile.TemporaryDirectory() as directory:
            listing = Path(directory) / "listing.txt"
            listing.write_text("Title: Exact title\nnotes\n---\nExact body\n", encoding="utf-8")
            self.assertEqual(m5.parse_listing_copy(listing), ("Exact title", "Exact body"))
            listing.write_text("Title: Exact title\n---\none\n---\ntwo\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "exactly one"):
                m5.parse_listing_copy(listing)

    def test_listing_only_cli_is_credential_free_and_read_only(self):
        session = FakeSession()
        with tempfile.TemporaryDirectory() as directory:
            listing = Path(directory) / "listing.txt"
            listing.write_text("Title: Meshtastic ADV\nnotes\n---\nfixture\n", encoding="utf-8")
            with (
                mock.patch.object(m5.sys, "argv", ["m5burner_publish.py", "--check-listing-only", "--listing", str(listing)]),
                mock.patch.object(m5.requests, "get", return_value=Response([session.firmware]), create=True),
                mock.patch.object(m5.requests, "Session", side_effect=AssertionError("must not login"), create=True),
                mock.patch.dict(m5.os.environ, {}, clear=True),
            ):
                self.assertIsNone(m5.main())
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.updates, 0)
        self.assertEqual(session.publishes, 0)

    def test_malformed_listing_only_cli_fails_before_network(self):
        with tempfile.TemporaryDirectory() as directory:
            listing = Path(directory) / "listing.txt"
            listing.write_text("not reviewed", encoding="utf-8")
            with (
                mock.patch.object(m5.sys, "argv", ["m5burner_publish.py", "--check-listing-only", "--listing", str(listing)]),
                mock.patch.object(m5.requests, "get", side_effect=AssertionError("must not use network"), create=True),
            ):
                with self.assertRaises(SystemExit):
                    m5.main()


if __name__ == "__main__":
    unittest.main()
