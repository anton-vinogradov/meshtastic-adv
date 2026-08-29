import argparse
import importlib.util
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("verify_runner", ROOT / "scripts/verify.py")
verify = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verify
SPEC.loader.exec_module(verify)


def args(**overrides):
    values = {
        "quick": False,
        "firmware_only": False,
        "skip_build": False,
        "rf": False,
        "usb": False,
        "production_wifi": False,
        "release_image": None,
        "config_backup": None,
        "backup_root": None,
        "capture_rf_backups": False,
        "rf_profile_from": None,
        "rf_expect_region": None,
        "rf_expect_tx_power": None,
        "fixture": ROOT / "hil/fixture.local.json",
        "timeout": 240,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class VerifyRunnerTests(unittest.TestCase):
    def test_default_plan_is_complete_and_has_no_hardware(self):
        plan = verify.build_plan(args(), Path("/tmp/evidence"))
        names = [stage.name for stage in plan]
        self.assertEqual(len(plan), 8)
        self.assertFalse(any(stage.hardware for stage in plan))
        self.assertLess(names.index("firmware/build-hil"), names.index("firmware/build-release"))
        self.assertIn("firmware/size-release", names)
        self.assertIn("firmware/size-hil", names)

    def test_hardware_requires_explicit_flags_and_reuses_staged_images(self):
        plan = verify.build_plan(
            args(rf=True, usb=True, skip_build=True, backup_root=Path("/tmp/backups")),
            Path("/tmp/evidence"),
        )
        hardware = [stage for stage in plan if stage.hardware]
        self.assertEqual([stage.name for stage in hardware], ["hardware/usb-cardputer", "hardware/rf-private"])
        self.assertFalse(any(stage.name.startswith("firmware/build-") for stage in plan))
        self.assertIn("--skip-build", hardware[0].command)

    def test_exact_release_app_is_forwarded_to_usb_hil(self):
        image = Path("/tmp/exact-release.bin")
        plan = verify.build_plan(
            args(usb=True, skip_build=True, release_image=image), Path("/tmp/evidence")
        )
        command = next(stage.command for stage in plan if stage.name == "hardware/usb-cardputer")
        self.assertEqual(command[command.index("--release-image") + 1], str(image))

    def test_external_config_backup_is_usb_only_and_forwarded(self):
        backup = Path("/persistent/recovery/config.yaml")
        plan = verify.build_plan(
            args(usb=True, skip_build=True, config_backup=backup), Path("/tmp/evidence")
        )
        command = next(stage.command for stage in plan if stage.name == "hardware/usb-cardputer")
        self.assertEqual(command[command.index("--config-backup") + 1], str(backup))

        source = verify.parser()
        with self.assertRaises(SystemExit):
            verify.validate_args(args(config_backup=backup), source)

    def test_production_wifi_is_exact_release_usb_only_and_forwarded(self):
        image = Path("/tmp/exact-release.bin")
        plan = verify.build_plan(
            args(
                usb=True,
                skip_build=True,
                release_image=image,
                production_wifi=True,
            ),
            Path("/tmp/evidence"),
        )
        command = next(stage.command for stage in plan if stage.name == "hardware/usb-cardputer")
        self.assertIn("--production-wifi", command)

        source = verify.parser()
        with self.assertRaises(SystemExit):
            verify.validate_args(args(production_wifi=True), source)
        with self.assertRaises(SystemExit):
            verify.validate_args(args(usb=True, production_wifi=True), source)

    def test_rf_release_options_are_forwarded(self):
        plan = verify.build_plan(
            args(
                rf=True,
                skip_build=True,
                backup_root=Path("/tmp/backups"),
                capture_rf_backups=True,
                rf_profile_from="lab-us",
                rf_expect_region="US",
                rf_expect_tx_power=26,
            ),
            Path("/tmp/evidence"),
        )
        command = next(stage.command for stage in plan if stage.name == "hardware/rf-private")
        self.assertIn("--capture-backups", command)
        self.assertEqual(command[command.index("--profile-from") + 1], "lab-us")
        self.assertEqual(command[command.index("--expect-region") + 1], "US")
        self.assertEqual(command[command.index("--expect-tx-power") + 1], "26")

    def test_quick_plan_has_only_host_checks(self):
        plan = verify.build_plan(args(quick=True), Path("/tmp/evidence"))
        self.assertEqual(len(plan), 4)
        self.assertTrue(all(stage.name.startswith(("syntax/", "host/")) for stage in plan))

    def test_report_contains_failed_junit_case(self):
        report = {
            "suite": "meshtastic-adv-verification",
            "status": "failed",
            "results": [
                {
                    "name": "host/unit",
                    "status": "failed",
                    "returncode": 1,
                    "duration_seconds": 0.25,
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            verify.write_report(report, root)
            suite = ET.parse(root / "junit.xml").getroot()
        self.assertEqual(suite.attrib["tests"], "1")
        self.assertEqual(suite.attrib["failures"], "1")


if __name__ == "__main__":
    unittest.main()
