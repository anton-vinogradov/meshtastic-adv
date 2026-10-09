"""Exercise the actual nested Nordic HIL case, including protocol/cleanup wiring."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("nordic_hil_runner", ROOT / "scripts/hil.py")
hil = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = hil
SPEC.loader.exec_module(hil)


class NordicHilTests(unittest.TestCase):
    def exercise_case(self, *, capture_failure=False):
        session = mock.MagicMock()
        session.query.return_value = {"node": "aabbccdd"}
        session.wait_ready.return_value = {"node": "aabbccdd", "boot": "ready"}
        text = "Ä Å Ö ä å ö / Æ Ø æ ø / É ñ ß".encode("utf-8")
        session.wait_state.return_value = {
            "messages": "1", "incoming": "1", "text_len": str(len(text)), "text_fnv": hil.fnv1a32(text),
        }

        def frame(expected_mode):
            # HilSession.frame expects a UI mode, not a screenshot name.
            self.assertEqual(expected_mode, "node")
            if capture_failure:
                raise hil.HilError("simulated framebuffer failure")
            return {"mode": "node", "fnv": "12345678"}

        session.frame.side_effect = frame
        original_check = hil.Report.check

        def selected_check(report, name, action):
            if name == "ingress/incoming-nordic-dm":
                return original_check(report, name, action)

        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(hil, "resolve_role", return_value={"port": "/dev/mock"}), \
                mock.patch.object(hil, "HilSession", return_value=session), \
                mock.patch.object(hil.Report, "check", selected_check):
            report = hil.message_flow({"schema": 2}, Path(directory))
        self.assertEqual(len(report.cases), 1)
        self.assertEqual(session.inject.call_count, 1)
        self.assertEqual(session.inject.call_args.args[0], hil.make_text_frame(0xBEEF1, 0xAABBCCDD, 0x1110, text.decode()))
        return report, session

    def test_real_nordic_case_uses_node_frame_and_removes_its_fixture(self):
        report, session = self.exercise_case()
        self.assertEqual(report.failed, 0, [case.message for case in report.cases])
        self.assertEqual(session.clear.call_count, 2)  # this isolated case's start and cleanup
        self.assertEqual(session.home.call_count, 2)

    def test_real_nordic_case_cleans_up_even_when_capture_fails(self):
        report, session = self.exercise_case(capture_failure=True)
        self.assertEqual(report.failed, 1)
        self.assertIn("simulated framebuffer failure", report.cases[0].message)
        self.assertEqual(session.clear.call_count, 2)
        self.assertEqual(session.home.call_count, 2)


if __name__ == "__main__":
    unittest.main()
