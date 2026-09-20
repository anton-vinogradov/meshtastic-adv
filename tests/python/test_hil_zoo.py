import contextlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import test_hil as support
from test_zoo_hil import ENDPOINT, IP, NODE, TOKEN, UNIX, receipt

hil = support.hil
zoo = hil.zoo_hil


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.artifacts = Path(self.temp.name) / "evidence"
        self.fixture = support.HilRunnerTests.production_fixture()
        self.fixture["zoo_hil"] = dict(UNIX)
        self.events = []
        self.fail_action = None
        self.lose_at = None
        self.error_at = None
        self.interrupt_at = None
        self.lease = zoo.Lease(UNIX, ENDPOINT, transport=self.transport)
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.dict(hil.os.environ, {"GITHUB_ACTIONS": ""}))
        self.stack.enter_context(mock.patch.object(zoo, "Lease", return_value=self.lease))
        self.hardware = {}
        layout = support.HilRunnerTests.flash_layout()
        fixed = {
            "validate_app_image": Path("/test-app.bin"),
            "validate_factory_image": (Path("/test-factory.bin"), layout),
            "stage_release_image": (Path("/staged-app.bin"), "a" * 64),
            "stage_factory_image": (Path("/staged-factory.bin"), layout, "b" * 64),
        }
        for name, result in fixed.items():
            self.stack.enter_context(mock.patch.object(hil, name, return_value=result))
        for name in ("build", "capture_config_fingerprint", "capture_filesystem_backup", "write_flash_marker",
                     "flash", "smoke", "message_flow", "visual", "restore_production_state",
                     "restore_config_backup", "production_wifi_ready_dump", "production_wifi_soak"):
            self.hardware[name] = self.stack.enter_context(mock.patch.object(
                hil, name, side_effect=lambda *args, _name=name, **kwargs: self.operation(_name, *args, **kwargs)))

    def transport(self, payload):
        action = payload["action"]
        self.events.append(action)
        if action == self.fail_action:
            raise OSError("private connection diagnostic " + TOKEN)
        if action == "release":
            return {"ok": True, "result": {"ip": IP, "released": True}}
        return receipt()

    def operation(self, name, *args, **kwargs):
        if name == "capture_config_fingerprint":
            name = "config-" + args[2]
        self.events.append(name)
        if name in ("restore_production_state", "config-after", "config-after-recovery", "restore_config_backup"):
            self.assertIsNone(zoo.CURRENT.get(), "USB recovery must not depend on lease ownership")
        elif name != "build":
            zoo.checkpoint()
        if name == self.lose_at:
            self.lease.lost = True
        if name == self.error_at:
            raise hil.HilError("simulated " + name + " failure")
        if name == self.interrupt_at:
            raise KeyboardInterrupt()
        if name.startswith("config-"):
            return b"same"
        if name == "production_wifi_ready_dump":
            return hil.ProductionWifiSnapshot(reboot_count=7, node_count=40)
        if name == "production_wifi_soak":
            return {"validated": True, "soak_seconds": 14400, "stable_reboot_counter": True}
        return mock.Mock(failed=0)

    def run_full(self, **overrides):
        kwargs = dict(timeout=1, skip_build=True, release_image=Path("/test-app.bin"),
                      factory_image=Path("/test-factory.bin"), production_wifi=True,
                      production_wifi_soak_seconds=14400, require_zoo_hil=True)
        kwargs.update(overrides)
        return hil.full_run(self.fixture, self.artifacts, **kwargs)

    def summary(self):
        return json.loads((self.artifacts / "summary.json").read_text())

    def test_success_holds_window_across_all_hardware_and_cleanup(self):
        self.assertEqual(self.run_full(skip_build=False), 0)
        self.assertEqual(self.events, ["build", "acquire", "production_wifi_ready_dump", "config-before",
                         "capture_filesystem_backup", "write_flash_marker", "flash", "smoke", "message_flow",
                         "visual", "smoke", "restore_production_state", "production_wifi_soak", "config-after", "release"])
        summary = self.summary()
        self.assertTrue(summary["zoo_hil"]["validated"])
        self.assertTrue(summary["zoo_hil"]["required"])
        self.assertTrue(summary["production_wifi_validated"])
        for secret in (TOKEN, IP, NODE, UNIX["socket"]):
            self.assertNotIn(secret, json.dumps(summary))
        self.assertIsNone(zoo.CURRENT.get())

    def test_acquire_failure_touches_no_hardware(self):
        self.fail_action = "acquire"
        with self.assertRaises(zoo.ZooLeaseError):
            self.run_full()
        for operation in self.hardware.values():
            operation.assert_not_called()
        self.assertFalse(self.summary()["hil_flash_attempted"])

    def test_lost_window_stops_next_stage_but_restores_production(self):
        self.lose_at = "smoke"
        with self.assertRaises(zoo.ZooLeaseError):
            self.run_full()
        self.hardware["message_flow"].assert_not_called()
        self.hardware["visual"].assert_not_called()
        self.hardware["production_wifi_soak"].assert_not_called()
        self.hardware["restore_production_state"].assert_called_once()
        summary = self.summary()
        self.assertTrue(summary["production_restored"])
        self.assertTrue(summary["configuration_preserved"])
        self.assertFalse(summary["zoo_hil"]["validated"])
        self.assertFalse(summary["production_wifi_validated"])
        self.assertEqual(self.events[-2:], ["config-after", "release"])

    def test_restore_failure_takes_priority_over_lease_failure(self):
        self.lose_at = "smoke"
        self.error_at = "restore_production_state"
        with self.assertRaisesRegex(hil.HilError, "restore_production_state"):
            self.run_full()
        self.assertFalse(self.summary()["production_restored"])
        self.assertEqual(self.events[-1], "release")

    def test_release_refusal_invalidates_otherwise_passing_run(self):
        self.fail_action = "release"
        with self.assertRaises(zoo.ZooLeaseError):
            self.run_full()
        self.assertTrue(self.summary()["configuration_preserved"])
        self.assertFalse(self.summary()["production_wifi_validated"])
        self.assertEqual(self.summary()["failure_type"], "ZooLeaseError")
        self.assertFalse(self.summary()["zoo_hil"]["release_acknowledged"])

    def test_loss_during_final_config_cleanup_is_not_green(self):
        self.lose_at = "config-after"
        with self.assertRaises(zoo.ZooLeaseError):
            self.run_full()
        self.assertTrue(self.summary()["configuration_preserved"])
        self.assertFalse(self.summary()["production_wifi_validated"])

    def test_baseline_failure_releases_without_flash(self):
        self.error_at = "production_wifi_ready_dump"
        with self.assertRaises(hil.HilError):
            self.run_full()
        self.hardware["flash"].assert_not_called()
        self.hardware["capture_config_fingerprint"].assert_not_called()
        self.assertEqual(self.events[-1], "release")

    def test_interrupt_still_restores_and_releases(self):
        self.interrupt_at = "smoke"
        with self.assertRaises(KeyboardInterrupt):
            self.run_full()
        self.assertTrue(self.summary()["production_restored"])
        self.assertEqual(self.summary()["failure_type"], "KeyboardInterrupt")
        self.assertEqual(self.events[-2:], ["config-after", "release"])

    def test_required_missing_config_fails_before_build_or_device(self):
        self.fixture.pop("zoo_hil")
        with self.assertRaisesRegex(hil.HilError, "coordination"):
            self.run_full(skip_build=False)
        for operation in self.hardware.values():
            operation.assert_not_called()
        self.assertEqual(self.events, [])

    def test_impossible_soak_rejected_before_acquire(self):
        with self.assertRaisesRegex(hil.HilError, "six-hour"):
            self.run_full(production_wifi_soak_seconds=21600)
        self.assertEqual(self.events, [])

    def enable_tunnel(self, *, fail_start=False, lose_at=None):
        self.fixture["zoo_hil"] = {"transport": "ssh", "ssh_host": "lab-zoo",
                                   "socket": UNIX["socket"], "python": "/python", "helper": "/helper"}
        self.fixture["production_wifi_transport"] = "zoo-ssh"
        link = mock.Mock()
        state = {"started": False, "closed": False, "lost": False}
        def start():
            self.assertTrue(self.lease.acquired)
            self.events.append("tunnel-start")
            if fail_start:
                state["lost"] = True
                raise hil.hil_tunnel.TunnelError("setup failed")
            state["started"] = True
        def check():
            if lose_at in self.events or state["lost"]:
                state["lost"] = True
                raise hil.hil_tunnel.TunnelError("lost")
        def finish():
            if not state["closed"]:
                self.assertFalse(self.lease.finished)
                self.events.append("tunnel-close")
                state["closed"] = True
        link.start.side_effect, link.check.side_effect, link.finish.side_effect = start, check, finish
        link.evidence.side_effect = lambda: {"kind": "zoo-ssh", "started": state["started"],
                                            "closed": state["closed"], "healthy": not state["lost"],
                                            "validated": all((state["started"], state["closed"], not state["lost"]))}
        self.stack.enter_context(mock.patch.object(hil.hil_tunnel, "Tunnel", return_value=link))
        return link

    def test_tunnel_spans_usb_restore_and_soak_and_closes_before_zoo_release(self):
        self.enable_tunnel()
        self.assertEqual(self.run_full(), 0)
        self.assertEqual(self.events[:3], ["acquire", "tunnel-start", "production_wifi_ready_dump"])
        self.assertEqual(self.events[-3:], ["config-after", "tunnel-close", "release"])
        self.assertTrue(self.summary()["production_wifi_transport"]["validated"])
        self.assertIsNone(zoo.GUARD.get())

    def test_tunnel_setup_failure_does_not_backup_or_flash(self):
        self.enable_tunnel(fail_start=True)
        with self.assertRaises(hil.hil_tunnel.TunnelError):
            self.run_full()
        for operation in self.hardware.values():
            operation.assert_not_called()
        self.assertFalse(self.summary()["production_wifi_validated"])
        self.assertEqual(self.events[-2:], ["tunnel-close", "release"])

    def test_tunnel_loss_during_usb_stops_suite_but_does_not_block_recovery(self):
        self.enable_tunnel(lose_at="smoke")
        with self.assertRaises(hil.hil_tunnel.TunnelError):
            self.run_full()
        self.hardware["message_flow"].assert_not_called()
        self.hardware["production_wifi_soak"].assert_not_called()
        self.hardware["restore_production_state"].assert_called_once()
        self.assertTrue(self.summary()["configuration_preserved"])
        self.assertFalse(self.summary()["production_wifi_transport"]["validated"])
        self.assertEqual(self.events[-3:], ["config-after", "tunnel-close", "release"])


class GuardTests(unittest.TestCase):
    def test_lost_lease_blocks_serial_write_and_fails_report_immediately(self):
        lease = mock.Mock()
        lease.check.side_effect = zoo.ZooLeaseError("lost")
        session = object.__new__(hil.HilSession)
        session.serial = mock.Mock()
        report = hil.Report("test")
        with zoo.scope(lease):
            with self.assertRaises(zoo.ZooLeaseError):
                session.send(b"test")
            with self.assertRaises(zoo.ZooLeaseError):
                report.check("must-stop", lambda: {})
        session.serial.write.assert_not_called()
        self.assertEqual(report.failed, 1)

    def test_lost_lease_during_dump_closes_interface_without_readiness_retry(self):
        interface = support.HilRunnerTests.phoneapi_interface()
        lease = mock.Mock()
        def opener(*args):
            lease.check.side_effect = zoo.ZooLeaseError("lost")
            return interface
        open_interface = mock.Mock(side_effect=opener)
        with zoo.scope(lease):
            with self.assertRaises(zoo.ZooLeaseError):
                hil.production_wifi_ready_dump(support.HilRunnerTests.production_fixture(), opener=open_interface)
        interface.close.assert_called_once()
        open_interface.assert_called_once()

    def test_public_evidence_keeps_schema_but_redacts_control_location(self):
        from test_demo_media import demo_media
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "fixture.json"
            fixture.write_text(json.dumps({"production_wifi_transport": "zoo-ssh",
                                          "zoo_hil": {"transport": "ssh", "ssh_host": "z8",
                                                       "socket": UNIX["socket"]}}))
            source = root / "source"
            source.mkdir()
            (source / "summary.json").write_text(json.dumps({"zoo_hil": {"validated": True},
                                                             "production_wifi_transport": {"kind": "zoo-ssh"}}))
            (source / "diagnostic.log").write_text("control z8 " + UNIX["socket"])
            demo_media.stage_evidence(source, None, fixture, root / "public")
            result = json.loads((root / "public/summary.json").read_text())
            self.assertTrue(result["zoo_hil"]["validated"])
            self.assertEqual(result["production_wifi_transport"]["kind"], "zoo-ssh")
            log = (root / "public/diagnostic.log").read_text()
            self.assertNotIn("z8", log)
            self.assertNotIn(UNIX["socket"], log)
