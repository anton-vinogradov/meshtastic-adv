import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/backup-node.sh"
VALID_EXPORT = """channel_url: https://example.invalid/#fixture
config:
  lora:
    region: US
  security:
    privateKey: QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=
module_config:
  mqtt:
    enabled: false
owner: Fixture
owner_short: FX
"""


class BackupNodeTests(unittest.TestCase):
    def run_backup(
        self,
        cli_exit: int,
        content: str = VALID_EXPORT,
        cli_script: str | None = None,
        backup_timeout: int = 10,
    ):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        temp = Path(temporary.name)
        port = temp / "serial-port"
        port.touch()
        cli = temp / "meshtastic"
        cli.write_text(cli_script or f"#!/bin/sh\nprintf '%s' '{content}'\nexit {cli_exit}\n")
        cli.chmod(0o700)
        output = temp / "backup"
        result = subprocess.run(
            ["bash", str(SCRIPT), "--port", str(port), "--cli", str(cli), "--out", str(output)],
            cwd=ROOT,
            env={
                **os.environ,
                "MESHTASTIC_BACKUP_ATTEMPTS": "1",
                "MESHTASTIC_BACKUP_TIMEOUT": str(backup_timeout),
            },
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        )
        return result, output

    def test_complete_zero_exit_export_is_accepted(self):
        result, output = self.run_backup(0)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("OK", result.stdout)
        backups = list(output.glob("config-*.yaml"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].stat().st_mode & 0o777, 0o600)

    def test_nonzero_cli_exit_cannot_be_promoted_to_full_backup(self):
        result, _ = self.run_backup(7)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("\nOK", result.stdout)

    def test_key_only_zero_exit_export_is_not_a_full_backup(self):
        result, _ = self.run_backup(
            0,
            "config:\n  security:\n    privateKey: QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=\n",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("\nOK", result.stdout)

    def test_opaque_private_key_separator_is_accepted(self):
        export = VALID_EXPORT.replace(
            "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=",
            "opaque1:QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=",
        )
        result, _ = self.run_backup(0, export)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_hung_export_is_terminated_by_the_deadline(self):
        result, _ = self.run_backup(
            0,
            cli_script="""#!/bin/sh
case "$*" in
    *--export-config*) while :; do :; done ;;
    *) exit 0 ;;
esac
""",
            backup_timeout=1,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("timed out after 1s", result.stdout)

    def test_default_output_is_project_local_and_ignored(self):
        script = SCRIPT.read_text()
        self.assertIn("$REPO_ROOT/hil-artifacts/backups/", script)
        self.assertNotIn("/claude/meshtastic-backups", script)

    def test_invalid_explicit_cli_fails_instead_of_falling_back_to_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            port = root / "serial-port"
            port.touch()
            fallback = root / "meshtastic"
            fallback.write_text("#!/bin/sh\ntouch \"$FALLBACK_USED\"\nexit 0\n")
            fallback.chmod(0o700)
            used = root / "fallback-used"
            result = subprocess.run(
                [
                    "bash", str(SCRIPT), "--port", str(port), "--cli",
                    str(root / "missing-cli"), "--out", str(root / "backup"),
                ],
                cwd=ROOT,
                env={
                    **os.environ,
                    "PATH": f"{root}:{os.environ.get('PATH', '')}",
                    "FALLBACK_USED": str(used),
                },
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 2)
        self.assertIn("MESHTASTIC_CLI is not executable", result.stderr)
        self.assertFalse(used.exists())

    def test_default_cli_comes_from_path_not_a_legacy_home_venv(self):
        script = SCRIPT.read_text()
        self.assertNotIn(".pio-core-venv", script)
        self.assertIn("command -v meshtastic", script)


if __name__ == "__main__":
    unittest.main()
