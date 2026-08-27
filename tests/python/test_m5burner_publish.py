import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path


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


class FakeSession:
    def __init__(self, versions=()):
        self.cookies = {"m5_auth_token": "fixture-token"}
        self.headers = {}
        self.firmware = {
            "fid": m5.FID,
            "name": "Meshtastic ADV",
            "description": "fixture",
            "category": "cardputer",
            "author": "fixture",
            "github": "https://example.invalid",
            "versions": [dict(item) for item in versions],
        }
        self.uploads = 0
        self.publishes = 0

    def post(self, url, **kwargs):
        if url == m5.LOGIN_API:
            return Response({"status": 1})
        self.uploads += 1
        self.firmware["versions"].append(
            {"version": kwargs["data"]["version"], "file": "uploaded.bin", "published": False}
        )
        return Response({"status": 1})

    def get(self, url, **_kwargs):
        return Response([self.firmware])

    def put(self, url, **_kwargs):
        self.publishes += 1
        file_id = url.split("/")[-2]
        for version in self.firmware["versions"]:
            if version.get("file") == file_id:
                version["published"] = True
        return Response({"status": 1})


class M5BurnerPublisherTests(unittest.TestCase):
    def publish(self, session, version="1.0.1"):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "firmware.bin"
            binary.write_bytes(b"firmware")
            return m5.publish(
                session,
                email="fixture@example.invalid",
                password="secret",
                version=version,
                binary=binary,
            )

    def test_existing_published_version_is_verified_without_mutation(self):
        session = FakeSession([{"version": "1.0.1", "file": "ready.bin", "published": True}])
        self.assertEqual(self.publish(session), "ready.bin")
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.publishes, 0)

    def test_half_published_version_is_resumed_instead_of_skipped(self):
        session = FakeSession([{"version": "1.0.1", "file": "pending.bin", "published": False}])
        self.assertEqual(self.publish(session), "pending.bin")
        self.assertEqual(session.uploads, 0)
        self.assertEqual(session.publishes, 1)

    def test_missing_version_is_uploaded_published_and_verified(self):
        session = FakeSession()
        self.assertEqual(self.publish(session), "uploaded.bin")
        self.assertEqual(session.uploads, 1)
        self.assertEqual(session.publishes, 1)

    def test_public_verification_requires_exact_file_and_published_flag(self):
        session = FakeSession([{"version": "1.0.1", "file": "wrong.bin", "published": True}])
        with self.assertRaisesRegex(RuntimeError, "public catalog"):
            m5.verify_public_version(session, "1.0.1", "expected.bin", attempts=1, delay=0)


if __name__ == "__main__":
    unittest.main()
