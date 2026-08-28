import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("verify_release_ref", ROOT / "scripts/verify_release_ref.py")
release_ref = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = release_ref
SPEC.loader.exec_module(release_ref)


class ReleaseRefTests(unittest.TestCase):
    def test_annotated_tag_uses_peeled_commit(self):
        tag_object = "a" * 40
        commit = "b" * 40
        output = f"{tag_object}\trefs/tags/v1.2.3\n{commit}\trefs/tags/v1.2.3^{{}}\n"
        self.assertEqual(release_ref.parse_remote_refs(output, "v1.2.3"), commit)

    def test_lightweight_tag_uses_direct_commit(self):
        commit = "b" * 40
        self.assertEqual(
            release_ref.parse_remote_refs(f"{commit}\trefs/tags/v1.2.3\n", "v1.2.3"),
            commit,
        )

    def test_missing_duplicate_or_moved_tag_fails_closed(self):
        with self.assertRaisesRegex(release_ref.RefError, "missing"):
            release_ref.parse_remote_refs("", "v1.2.3")
        duplicate = f"{'a' * 40}\trefs/tags/v1.2.3\n{'b' * 40}\trefs/tags/v1.2.3\n"
        with self.assertRaisesRegex(release_ref.RefError, "duplicate"):
            release_ref.parse_remote_refs(duplicate, "v1.2.3")
        completed = mock.Mock(stdout=f"{'a' * 40}\trefs/tags/v1.2.3\n")
        with mock.patch.object(release_ref.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(release_ref.RefError, "moved"):
                release_ref.verify("origin", "v1.2.3", "b" * 40)


if __name__ == "__main__":
    unittest.main()
