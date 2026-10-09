"""Wait for real USB application readiness without weakening reboot/TX gates."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("hil_startup_runner", ROOT / "scripts/hil.py")
hil = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = hil
SPEC.loader.exec_module(hil)


class HilStartupTests(unittest.TestCase):
    def test_ready_wait_accepts_a_36_second_startup_without_any_mutation(self):
        session = object.__new__(hil.HilSession)
        session.serial = mock.Mock()
        elapsed = [0.0]

        def query(*, timeout):
            self.assertEqual(timeout, 2)
            elapsed[0] += 2.0
            return {"splash": "1" if elapsed[0] >= 36 else "0", "backend": "onboard"}

        session.query = mock.Mock(side_effect=query)
        session.send = mock.Mock()
        with mock.patch.object(hil.time, "monotonic", side_effect=lambda: elapsed[0]), \
                mock.patch.object(hil.time, "sleep", side_effect=lambda delay: elapsed.__setitem__(0, elapsed[0] + delay)):
            state = session.wait_ready()
        self.assertEqual(state["splash"], "1")
        self.assertGreaterEqual(elapsed[0], 36)
        self.assertLess(elapsed[0], hil.HIL_USB_READY_SECONDS)
        session.send.assert_not_called()  # only the mocked read-only query ran
        session.serial.reset_input_buffer.assert_called_once_with()

    def test_ready_wait_is_bounded_and_does_not_reset_a_changed_boot(self):
        session = object.__new__(hil.HilSession)
        session.serial = mock.Mock()
        session.wait_state = mock.Mock(side_effect=hil.UnexpectedReboot("changed boot"))
        with self.assertRaisesRegex(hil.UnexpectedReboot, "changed boot"):
            session.wait_ready()
        session.wait_state.assert_called_once_with(
            {"splash": "1", "backend": "onboard"}, timeout=60.0,
        )

    def test_ready_wait_times_out_instead_of_sending_reboot_or_clear(self):
        session = object.__new__(hil.HilSession)
        session.serial = mock.Mock()
        elapsed = [0.0]

        def query(*, timeout):
            elapsed[0] += 2.0
            raise hil.HilError("not ready")

        session.query = mock.Mock(side_effect=query)
        session.send = mock.Mock()
        with mock.patch.object(hil.time, "monotonic", side_effect=lambda: elapsed[0]), \
                mock.patch.object(hil.time, "sleep", side_effect=lambda delay: elapsed.__setitem__(0, elapsed[0] + delay)), \
                self.assertRaisesRegex(hil.HilError, "state mismatch"):
            session.wait_ready()
        self.assertGreaterEqual(elapsed[0], 60)
        self.assertLess(elapsed[0], 63)
        session.send.assert_not_called()

    def test_message_startup_failure_is_red_even_for_out_of_case_reboot(self):
        for error in (hil.HilError("not ready"), hil.UnexpectedReboot("changed boot")):
            with self.subTest(error=type(error).__name__):
                session = mock.MagicMock()
                session.wait_ready.side_effect = error
                with tempfile.TemporaryDirectory() as directory, \
                        mock.patch.object(hil, "resolve_role", return_value={"port": "/dev/mock"}), \
                        mock.patch.object(hil, "HilSession", return_value=session), \
                        mock.patch("builtins.print"):
                    report = hil.message_flow({"schema": 2}, Path(directory))
                self.assertEqual(report.failed, 1)
                session.clear.assert_not_called()
                session.inject.assert_not_called()
                session.close.assert_called_once_with()

    def test_message_handshake_precedes_fixture_clear(self):
        session = mock.MagicMock()
        session.wait_ready.return_value = {"node": "00000001", "boot": "ready"}
        session.query.return_value = {"node": "00000001"}
        session.clear.return_value = {
            "v": "2", "node": "00000001", "messages": "0", "reactions": "0", "incoming": "0",
            "buffers": "1", "heap": "13000", "min_heap": "13000",
        }
        original_check = hil.Report.check

        def only_start(report, name, action):
            if name == "fixtures/isolated-clean-start":
                return original_check(report, name, action)

        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(hil, "resolve_role", return_value={"port": "/dev/mock"}), \
                mock.patch.object(hil, "HilSession", return_value=session), \
                mock.patch.object(hil.Report, "check", only_start), mock.patch("builtins.print"):
            report = hil.message_flow({"schema": 2}, Path(directory))
        self.assertEqual(report.failed, 0)
        self.assertLess(session.mock_calls.index(mock.call.wait_ready()), session.mock_calls.index(mock.call.clear()))
        self.assertEqual(report.cases[0].name, "fixtures/isolated-clean-start")

    def test_visual_never_starts_demo_or_retry_before_ready(self):
        session = mock.MagicMock()
        session.__enter__.return_value = session
        session.wait_ready.side_effect = hil.HilError("not ready")
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(hil, "resolve_role", return_value={"port": "/dev/mock"}), \
                mock.patch.object(hil, "HilSession", return_value=session) as constructor, \
                mock.patch.object(hil, "wait_for_usb_serial") as wait_usb, mock.patch("builtins.print"):
            report = hil.visual({"schema": 2}, Path(directory), 1)
        self.assertEqual(report.failed, 1)
        session.demo_frames.assert_not_called()
        wait_usb.assert_not_called()
        constructor.assert_called_once()

    def test_failed_post_reboot_handshake_does_not_reuse_previous_ready_flag(self):
        before = mock.MagicMock()
        before.wait_ready.return_value = {"node": "00000001", "boot": "before"}
        before.query.return_value = {
            "node": "00000001", "boot": "before",
            **{key: "0" for key in (
                "messages", "hist", "reactions", "incoming", "delivered", "failed", "sort", "names",
                "fav_channels", "fav_nodes", "fav_first",
            )},
        }
        before.persist.return_value = {"ok": "1", "messages": "0"}
        after = mock.MagicMock()
        after.wait_ready.side_effect = hil.UnexpectedReboot("changed boot during new session")
        original_check = hil.Report.check

        def only_reboot(report, name, action):
            if name in ("storage/persist-before-reboot", "storage/reboot-recovery"):
                return original_check(report, name, action)

        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(hil, "resolve_role", return_value={"port": "/dev/mock", "usb_serial": "02:11:22:33:44:55"}), \
                mock.patch.object(hil, "HilSession", side_effect=[before, after]), \
                mock.patch.object(hil, "wait_for_usb_serial", return_value={"port": "/dev/mock"}), \
                mock.patch.object(hil.Report, "check", only_reboot), \
                mock.patch.object(hil.time, "sleep"), mock.patch("builtins.print"):
            report = hil.message_flow({"schema": 2}, Path(directory))
        self.assertEqual(report.failed, 1)
        self.assertEqual(report.cases[-1].name, "storage/reboot-recovery")
        after.clear.assert_not_called()
        before.close.assert_called_once_with()
        after.close.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
