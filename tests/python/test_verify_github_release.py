import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "verify_github_release", ROOT / "scripts/verify_github_release.py"
)
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class GitHubReleaseVerifierTests(unittest.TestCase):
    def fixture(self, root: Path):
        asset = root / "local.bin"
        asset.write_bytes(b"exact")
        notes = root / "notes.md"
        notes.write_text("release notes\n")
        return asset, notes

    def runner(self, payload, asset_bytes=b"exact"):
        def run(command, **_kwargs):
            if command[1:3] == ["release", "view"]:
                return mock.Mock(stdout=json.dumps(payload))
            target = Path(command[command.index("--dir") + 1])
            name = command[command.index("--pattern") + 1]
            (target / name).write_bytes(asset_bytes)
            return mock.Mock(stdout="")
        return run

    def test_exact_draft_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset, notes = self.fixture(root)
            payload = {
                "name": "Meshtastic ADV v1.2.3",
                "isDraft": True,
                "isPrerelease": False,
                "body": "release notes",
                "assets": [{"name": "firmware.bin"}],
            }
            with mock.patch.object(release.subprocess, "run", side_effect=self.runner(payload)):
                release.verify_release(
                    "v1.2.3", "Meshtastic ADV v1.2.3",
                    {"firmware.bin": asset}, notes, "draft", False
                )

    def test_byte_mismatch_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset, notes = self.fixture(root)
            payload = {
                "name": "Meshtastic ADV v1.2.3",
                "isDraft": True,
                "isPrerelease": False,
                "body": "release notes",
                "assets": [{"name": "firmware.bin"}],
            }
            with mock.patch.object(
                release.subprocess, "run", side_effect=self.runner(payload, b"corrupt")
            ):
                with self.assertRaisesRegex(RuntimeError, "asset bytes differ"):
                    release.verify_release(
                        "v1.2.3", "Meshtastic ADV v1.2.3",
                        {"firmware.bin": asset}, notes, "either", False
                    )

    def test_asset_set_notes_and_state_are_enforced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset, notes = self.fixture(root)
            base = {
                "name": "Meshtastic ADV v1.2.3",
                "isDraft": False,
                "isPrerelease": False,
                "body": "wrong",
                "assets": [{"name": "firmware.bin"}, {"name": "stale.bin"}],
            }
            with mock.patch.object(release.subprocess, "run", side_effect=self.runner(base)):
                with self.assertRaisesRegex(RuntimeError, "already public"):
                    release.verify_release(
                        "v1.2.3", "Meshtastic ADV v1.2.3",
                        {"firmware.bin": asset}, notes, "draft", False
                    )
            base["isDraft"] = True
            with mock.patch.object(release.subprocess, "run", side_effect=self.runner(base)):
                with self.assertRaisesRegex(RuntimeError, "asset set differs"):
                    release.verify_release(
                        "v1.2.3", "Meshtastic ADV v1.2.3",
                        {"firmware.bin": asset}, notes, "either", False
                    )
            base["assets"] = [{"name": "firmware.bin"}]
            with mock.patch.object(release.subprocess, "run", side_effect=self.runner(base)):
                with self.assertRaisesRegex(RuntimeError, "notes differ"):
                    release.verify_release(
                        "v1.2.3", "Meshtastic ADV v1.2.3",
                        {"firmware.bin": asset}, notes, "either", False
                    )

    def test_title_is_enforced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset, notes = self.fixture(root)
            payload = {
                "name": "Wrong title",
                "isDraft": True,
                "isPrerelease": False,
                "body": "release notes",
                "assets": [{"name": "firmware.bin"}],
            }
            with mock.patch.object(release.subprocess, "run", side_effect=self.runner(payload)):
                with self.assertRaisesRegex(RuntimeError, "title differs"):
                    release.verify_release(
                        "v1.2.3", "Meshtastic ADV v1.2.3",
                        {"firmware.bin": asset}, notes, "either", False
                    )

    def test_newer_public_stable_is_rejected_but_drafts_and_prereleases_are_ignored(self):
        allowed = [
            {"tag_name": "v1.2.4", "draft": True, "prerelease": False},
            {"tag_name": "v2.0.0-rc.1", "draft": False, "prerelease": True},
            {"tag_name": "v1.2.3", "draft": False, "prerelease": False},
        ]
        release.verify_no_newer_public_stable("v1.2.3", allowed)
        with self.assertRaisesRegex(RuntimeError, "newer public stable"):
            release.verify_no_newer_public_stable(
                "v1.2.3",
                allowed + [{"tag_name": "v1.10.0", "draft": False, "prerelease": False}],
            )


if __name__ == "__main__":
    unittest.main()
