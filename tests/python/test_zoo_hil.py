import json
import math
from pathlib import Path
import subprocess
import sys
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import zoo_hil as zoo

IP, NODE, TOKEN = "192.0.2.20", "!aabbccdd", "T" * 43
ENDPOINT = {"host": IP, "port": 4403, "expected_node_id": NODE, "min_nodes": 32}
UNIX = {"transport": "unix", "socket": "/run/zoo-hil/control.sock"}
SSH = {"transport": "ssh", "ssh_host": "lab-zoo", "socket": "/run/zoo-hil/control.sock",
       "python": "/opt/zoo/.venv/bin/python", "helper": "/opt/zoo/collector/hilctl.py"}


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


def receipt(**changes):
    result = {"ip": IP, "node_id": NODE, "token": TOKEN, "phase": "ready",
              "remaining_seconds": 120, "maximum_remaining_seconds": 21600}
    result.update(changes)
    return {"ok": True, "result": result}


class LeaseTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.requests = []
        def transport(payload):
            self.requests.append(dict(payload))
            if payload["action"] == "release":
                return {"ok": True, "result": {"ip": IP, "released": True}}
            return receipt()
        self.transport = mock.Mock(side_effect=transport)
        self.lease = zoo.Lease(UNIX, ENDPOINT, transport=self.transport, clock=self.clock)
        self.thread = mock.patch.object(zoo.threading, "Thread")
        self.worker = self.thread.start().return_value
        self.worker.is_alive.return_value = False
        self.addCleanup(self.thread.stop)
        self.addCleanup(self.lease.finish)

    def test_default_success_lifecycle_is_sanitized(self):
        self.lease.start()
        self.clock.now += 30
        self.lease.renew()
        self.lease.check()
        self.lease.finish()
        evidence = self.lease.evidence()
        self.assertTrue(evidence["validated"])
        self.assertEqual(evidence["renewals"], 1)
        self.assertIsNone(self.lease.token)
        self.assertNotIn(TOKEN, json.dumps(evidence))
        self.assertNotIn(IP, json.dumps(evidence))
        self.assertNotIn(NODE, json.dumps(evidence))
        self.assertEqual([r["action"] for r in self.requests], ["acquire", "renew", "release"])

    def test_no_silent_reacquisition(self):
        self.lease.start()
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.start()
        self.assertEqual(len(self.requests), 1)

    def test_busy_can_retry_only_before_any_lease_is_acquired(self):
        self.transport.side_effect = [{"ok": False, "code": "busy"}, receipt(),
                                      {"ok": True, "result": {"ip": IP, "released": True}}]
        with mock.patch.object(self.lease.stop, "wait", return_value=False):
            self.lease.start()
        self.assertEqual(self.transport.call_count, 2)
        self.assertTrue(self.lease.acquired)

    def test_busy_wait_is_bounded(self):
        self.transport.side_effect = None
        self.transport.return_value = {"ok": False, "code": "busy"}
        with mock.patch.object(zoo.time, "monotonic", side_effect=[0, 31]):
            with self.assertRaises(zoo.ZooLeaseError):
                self.lease.start()
        self.assertEqual(self.transport.call_count, 1)

    def test_renewal_thread_start_failure_still_releases(self):
        self.worker.start.side_effect = RuntimeError("cannot start thread")
        self.worker.join.side_effect = RuntimeError("not started")
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.start()
        self.lease.finish()
        self.assertTrue(self.lease.released)
        self.assertFalse(self.lease.evidence()["validated"])

    def test_bad_receipts_fail_closed(self):
        for changes in ({"ip": "192.0.2.21"}, {"node_id": "!11223344"}, {"phase": "preparing"},
                        {"token": "bad"}, {"remaining_seconds": True}, {"remaining_seconds": math.inf},
                        {"remaining_seconds": math.nan}, {"remaining_seconds": 121},
                        {"maximum_remaining_seconds": 100}, {"maximum_remaining_seconds": 21601}):
            with self.subTest(changes=changes):
                candidate = zoo.Lease(UNIX, ENDPOINT, transport=mock.Mock(return_value=receipt(**changes)), clock=self.clock)
                with self.assertRaises(zoo.ZooLeaseError):
                    candidate.start()
                self.assertFalse(candidate.evidence()["validated"])
                candidate.finish()

    def test_invalid_receipt_with_valid_token_still_attempts_release(self):
        self.transport.side_effect = [receipt(ip="192.0.2.21"), {"ok": True, "result": {"ip": IP, "released": True}}]
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.start()
        self.lease.finish()
        self.assertEqual(self.transport.call_args.args[0], {"action": "release", "ip": IP, "token": TOKEN})
        self.assertFalse(self.lease.evidence()["validated"])

    def test_refusal_and_untrusted_error_text_are_not_echoed(self):
        self.transport.return_value = {"ok": False, "error": TOKEN + IP}
        self.transport.side_effect = None
        with self.assertRaises(zoo.ZooLeaseError) as error:
            self.lease.start()
        self.assertNotIn(TOKEN, str(error.exception))
        self.assertNotIn(IP, str(error.exception))

    def test_expired_ownership_is_sticky(self):
        self.lease.start()
        self.clock.now += 120
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.check()
        self.clock.now -= 100
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.check()
        self.lease.finish()
        self.assertFalse(self.lease.evidence()["ownership_preserved"])

    def test_slow_acquire_reply_cannot_extend_ttl(self):
        def slow(_payload):
            self.clock.now += 111
            return receipt()
        self.transport.side_effect = slow
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.start()

    def test_failed_renewal_stops_further_operations(self):
        self.lease.start()
        self.transport.side_effect = OSError(TOKEN)
        with self.assertRaises(zoo.ZooLeaseError) as error:
            self.lease.renew()
        self.assertNotIn(TOKEN, str(error.exception))
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.check()

    def test_server_restart_or_wrong_owner_does_not_reacquire(self):
        self.lease.start()
        self.transport.side_effect = None
        self.transport.return_value = {"ok": False, "code": "not_owned"}
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.renew()
        self.assertEqual([r["action"] for r in self.requests], ["acquire"])

    def test_renewal_token_cannot_change(self):
        self.lease.start()
        self.transport.side_effect = None
        self.transport.return_value = receipt(token="X" * 43)
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.renew()
        self.assertEqual(self.lease.token, TOKEN)

    def test_response_after_old_deadline_cannot_resurrect_lease(self):
        self.lease.start()
        def late(_payload):
            self.clock.now += 121
            return receipt()
        self.transport.side_effect = late
        with self.assertRaises(zoo.ZooLeaseError):
            self.lease.renew()

    def test_renewal_never_extends_absolute_window(self):
        self.lease.start()
        maximum = self.lease.maximum_deadline
        self.clock.now += 30
        self.lease.renew()
        self.assertEqual(self.lease.maximum_deadline, maximum)

    def test_release_failure_is_not_a_passing_run(self):
        self.lease.start()
        self.transport.side_effect = OSError(TOKEN)
        self.lease.finish()
        self.assertFalse(self.lease.evidence()["validated"])
        self.assertFalse(self.lease.evidence()["release_acknowledged"])

    def test_release_wrong_identity_is_rejected(self):
        self.lease.start()
        self.transport.side_effect = None
        self.transport.return_value = {"ok": True, "result": {"ip": "192.0.2.21", "released": True}}
        self.lease.finish()
        self.assertFalse(self.lease.evidence()["validated"])

    def test_finish_is_idempotent(self):
        self.lease.start()
        self.lease.finish()
        self.lease.finish()
        self.assertEqual([r["action"] for r in self.requests], ["acquire", "release"])

    def test_recovery_bypasses_lost_lease_but_test_does_not(self):
        self.lease.start()
        self.lease.lost = True
        with zoo.scope(self.lease):
            with self.assertRaises(zoo.ZooLeaseError):
                zoo.checkpoint()
            zoo.recovery_call(zoo.checkpoint)
            with self.assertRaises(zoo.ZooLeaseError):
                zoo.checkpoint()
        self.assertIsNone(zoo.CURRENT.get())

    def test_scope_cleans_up_on_interrupt(self):
        with self.assertRaises(KeyboardInterrupt):
            with zoo.scope(self.lease):
                self.lease.start()
                raise KeyboardInterrupt()
        self.assertTrue(self.lease.released)
        self.assertIsNone(zoo.CURRENT.get())


class ConfigTransportTests(unittest.TestCase):
    def test_clock_counts_suspend_when_monotonic_clock_does_not(self):
        values = {"wall": 1000.0, "mono": 100.0}
        with mock.patch.object(zoo.time, "time", side_effect=lambda: values["wall"]), \
                mock.patch.object(zoo.time, "monotonic", side_effect=lambda: values["mono"]):
            clock = zoo.OwnershipClock()
            self.assertEqual(clock(), 100)
            values.update(wall=1130, mono=101)
            self.assertEqual(clock(), 230)

    def test_clock_backward_step_fails_closed(self):
        values = {"wall": 1000.0, "mono": 100.0}
        with mock.patch.object(zoo.time, "time", side_effect=lambda: values["wall"]), \
                mock.patch.object(zoo.time, "monotonic", side_effect=lambda: values["mono"]):
            clock = zoo.OwnershipClock()
            values.update(wall=999, mono=101)
            with self.assertRaises(zoo.ZooLeaseError):
                clock()

    def test_configuration_pins_same_dut(self):
        self.assertEqual(zoo.validate_config(SSH, ENDPOINT), SSH)
        self.assertEqual(zoo.validate_config(UNIX, ENDPOINT), UNIX)
        for bad in ({}, dict(SSH, ssh_host="-oProxyCommand=bad"), dict(SSH, ssh_host="a;cmd"),
                    dict(UNIX, token=TOKEN), dict(UNIX, socket="relative"),
                    dict(UNIX, socket="/tmp/../bad"), dict(SSH, helper="/tmp/a\ncmd")):
            with self.subTest(bad=bad), self.assertRaises(zoo.ZooLeaseError):
                zoo.validate_config(bad, ENDPOINT)
        for endpoint in (None, dict(ENDPOINT, host="node.local"), dict(ENDPOINT, host="127.0.0.1")):
            with self.assertRaises(zoo.ZooLeaseError):
                zoo.validate_config(UNIX, endpoint)

    def test_ssh_uses_stdin_strict_host_keys_and_no_secret_arguments(self):
        with mock.patch.object(zoo.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, json.dumps(receipt()).encode())) as run:
            zoo.Transport(SSH)({"action": "renew", "ip": IP, "token": TOKEN})
        command = run.call_args.args[0]
        self.assertIn("BatchMode=yes", command)
        self.assertIn("StrictHostKeyChecking=yes", command)
        self.assertIn("ConnectTimeout=8", command)
        self.assertIn("ConnectionAttempts=1", command)
        self.assertNotIn(TOKEN, repr(command))
        self.assertIn(TOKEN.encode(), run.call_args.kwargs["input"])
        self.assertEqual(run.call_args.kwargs["timeout"], zoo.REQUEST_TIMEOUT)
        self.assertEqual(run.call_args.kwargs["stderr"], subprocess.DEVNULL)

    def test_transport_errors_never_echo_untrusted_output(self):
        for rc, body in ((255, TOKEN.encode()), (0, b"bad"), (0, b"x" * 4097), (1, json.dumps(receipt()).encode())):
            with self.subTest(rc=rc), mock.patch.object(zoo.subprocess, "run", return_value=subprocess.CompletedProcess([], rc, body)):
                with self.assertRaises(zoo.ZooLeaseError) as error:
                    zoo.Transport(SSH)({"action": "status"})
                self.assertNotIn(TOKEN, str(error.exception))

    def test_background_renewal_runs_without_main_thread_checkpoints(self):
        renewed = threading.Event()
        def transport(payload):
            if payload["action"] == "renew":
                renewed.set()
            if payload["action"] == "release":
                return {"ok": True, "result": {"ip": IP, "released": True}}
            return receipt()
        with mock.patch.object(zoo, "RENEW_EVERY", 0.01):
            lease = zoo.Lease(UNIX, ENDPOINT, transport=transport)
            try:
                lease.start()
                self.assertTrue(renewed.wait(1))
            finally:
                lease.finish()
        self.assertFalse(lease.thread.is_alive())
        self.assertTrue(lease.evidence()["validated"])


if __name__ == "__main__":
    unittest.main()
