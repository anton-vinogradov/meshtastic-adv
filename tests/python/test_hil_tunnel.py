import contextlib
import contextvars
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest import mock

import test_hil as support
from test_zoo_hil import ENDPOINT, SSH, UNIX

hil = support.hil
tunnel = hil.hil_tunnel
zoo = hil.zoo_hil


class TunnelTests(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.lease = mock.Mock()
        self.link = tunnel.Tunnel(SSH, ENDPOINT, self.lease)
        self.addCleanup(self.link.finish)
        self.process = mock.Mock()
        self.process.returncode = 0
        self.process.poll.return_value = None
        self.process.terminate.side_effect = lambda: setattr(self.process.poll, "return_value", 0)
        self.process.wait.side_effect = lambda **_kwargs: setattr(self.process.poll, "return_value", 0)
        self.process.kill.side_effect = lambda: setattr(self.process.poll, "return_value", -9)
        self.spawn = self.stack.enter_context(mock.patch.object(tunnel.subprocess, "Popen", return_value=self.process))
        entry = SimpleNamespace(st_mode=stat.S_IFSOCK | 0o600, st_uid=os.getuid())
        self.entry = self.stack.enter_context(mock.patch.object(Path, "lstat", return_value=entry))

    def test_private_single_target_no_config_commands_or_network_listener(self):
        self.link.start()
        self.assertTrue(self.link.path.parent.is_dir())
        self.assertEqual(stat.S_IMODE(self.link.path.parent.stat().st_mode), 0o700)
        args = self.spawn.call_args.args[0]
        self.assertEqual(args[:4], ["ssh", "-F", "/dev/null", "-T"])
        self.assertIn("StrictHostKeyChecking=yes", args)
        self.assertIn("BatchMode=yes", args)
        self.assertIn("ControlPath=none", args)
        self.assertIn("PermitLocalCommand=no", args)
        self.assertIn("StreamLocalBindMask=0177", args)
        self.assertEqual(args[-4:], ["-L", str(self.link.path) + ":192.0.2.20:4403", SSH["ssh_host"], "/bin/cat"])
        self.assertEqual(self.spawn.call_args.kwargs["stdin"], subprocess.PIPE)
        self.assertEqual(args.count("-L"), 1)
        self.assertNotIn("sudo", args)
        private_dir = self.link.path.parent
        self.link.finish()
        self.assertFalse(private_dir.exists())
        self.assertTrue(self.link.evidence()["validated"])
        for private in (str(private_dir), SSH["ssh_host"], ENDPOINT["host"]):
            self.assertNotIn(private, json.dumps(self.link.evidence()))
        self.process.terminate.assert_not_called()
        self.process.wait.assert_called_once()
        self.link.finish()
        self.process.wait.assert_called_once()

    def test_unadmitted_window_never_spawns(self):
        self.lease.check.side_effect = zoo.ZooLeaseError("refused")
        with self.assertRaises(zoo.ZooLeaseError):
            self.link.start()
        self.spawn.assert_not_called()

    def test_dead_ssh_is_sticky_and_cannot_restart(self):
        self.link.start()
        self.process.poll.return_value = 255
        with self.assertRaises(tunnel.TunnelError):
            self.link.check()
        self.process.poll.return_value = None
        with self.assertRaises(tunnel.TunnelError):
            self.link.start()
        self.link.finish()
        self.assertFalse(self.link.evidence()["validated"])
        self.spawn.assert_called_once()

    def test_wrong_target_never_opens_a_socket(self):
        self.link.start()
        with mock.patch.object(tunnel.socket, "socket") as create:
            with self.assertRaises(tunnel.TunnelError):
                self.link.connect("192.0.2.21", 4403, 5)
        create.assert_not_called()

    def test_connect_uses_unix_socket_not_a_substituted_logical_identity(self):
        self.link.start()
        connection = mock.Mock()
        with mock.patch.object(tunnel.socket, "socket", return_value=connection) as create:
            self.assertIs(self.link.connect(ENDPOINT["host"], 4403, 5), connection)
        create.assert_called_once_with(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.connect.assert_called_once_with(str(self.link.path))
        self.assertEqual(connection.settimeout.call_args_list, [mock.call(5), mock.call(None)])

    def test_connect_failure_closes_socket(self):
        self.link.start()
        connection = mock.Mock()
        connection.connect.side_effect = OSError("not ready")
        with mock.patch.object(tunnel.socket, "socket", return_value=connection):
            with self.assertRaises(OSError):
                self.link.connect(ENDPOINT["host"], 4403, 5)
        connection.close.assert_called_once()

    def test_setup_refusal_is_sanitized_and_cleans_temp_directory(self):
        self.spawn.side_effect = OSError("secret host and path")
        with self.assertRaisesRegex(tunnel.TunnelError, "^SSH forwarding setup failed$"):
            self.link.start()
        self.assertFalse(self.link.path.parent.exists())
        self.assertFalse(self.link.evidence()["validated"])

    def test_ssh_exiting_during_setup_cannot_pass(self):
        self.process.poll.return_value = 255
        with self.assertRaises(tunnel.TunnelError):
            self.link.start()
        self.assertFalse(self.link.path.parent.exists())

    def test_setup_timeout_closes_process(self):
        self.entry.side_effect = FileNotFoundError
        with mock.patch.object(tunnel.time, "monotonic", side_effect=[0, 16]):
            with self.assertRaisesRegex(tunnel.TunnelError, "timed out"):
                self.link.start()
        self.process.wait.assert_called_once()

    def test_insecure_or_non_socket_path_is_rejected(self):
        for mode in (stat.S_IFSOCK | 0o666, stat.S_IFREG | 0o600, stat.S_IFLNK | 0o700):
            with self.subTest(mode=mode):
                link = tunnel.Tunnel(SSH, ENDPOINT, self.lease)
                self.process.poll.return_value = None
                self.entry.return_value = SimpleNamespace(st_mode=mode, st_uid=os.getuid())
                with self.assertRaisesRegex(tunnel.TunnelError, "not private"):
                    link.start()
                self.assertFalse(link.evidence()["validated"])

    def test_forced_kill_is_recorded_as_failure(self):
        self.link.start()
        self.process.wait.side_effect = [subprocess.TimeoutExpired("ssh", 5), subprocess.TimeoutExpired("ssh", 5), 0]
        self.link.finish()
        self.process.kill.assert_called_once()
        self.assertFalse(self.link.evidence()["validated"])

    def test_exit_between_last_dump_and_cleanup_is_not_green(self):
        self.link.start()
        self.process.poll.return_value = 0
        self.link.finish()
        self.assertFalse(self.link.evidence()["validated"])

    def test_pipe_close_failure_still_kills_owned_child_and_removes_private_directory(self):
        self.link.start()
        self.process.stdin.close.side_effect = OSError("broken pipe")
        self.link.finish()
        self.process.kill.assert_called_once()
        self.assertFalse(self.link.path.parent.exists())
        self.assertFalse(self.link.evidence()["validated"])


class TransportIntegrationTests(unittest.TestCase):
    def test_fixture_mode_requires_ssh_coordination(self):
        fixture = support.HilRunnerTests.production_fixture()
        self.assertEqual(tunnel.validate_fixture(fixture), "direct")
        fixture["production_wifi_transport"] = "zoo-ssh"
        for config in (None, UNIX):
            fixture["zoo_hil"] = config
            with self.assertRaises((tunnel.TunnelError, zoo.ZooLeaseError)):
                tunnel.validate_fixture(fixture)
        fixture["zoo_hil"] = SSH
        self.assertEqual(tunnel.validate_fixture(fixture), "zoo-ssh")
        for mode in (True, {}, "auto", ""):
            fixture["production_wifi_transport"] = mode
            with self.assertRaises(tunnel.TunnelError):
                tunnel.validate_fixture(fixture)

    def test_scope_closes_tunnel_before_releasing_window_and_recovery_bypasses_both(self):
        events = []
        lease, guard = mock.Mock(), mock.Mock()
        guard.finish.side_effect = lambda: events.append("close-tunnel")
        lease.finish.side_effect = lambda: events.append("release-window")
        with zoo.scope(lease, guard):
            zoo.checkpoint()
            guard.check.assert_called_once()
            def recovery():
                self.assertIsNone(zoo.CURRENT.get())
                self.assertIsNone(zoo.GUARD.get())
                zoo.checkpoint()
            zoo.recovery_call(recovery)
            self.assertIs(zoo.GUARD.get(), guard)
        self.assertEqual(events, ["close-tunnel", "release-window"])
        self.assertIsNone(zoo.GUARD.get())

    def test_tunnel_failure_is_not_a_readiness_retry(self):
        opener = mock.Mock(side_effect=tunnel.TunnelError("lost"))
        with self.assertRaises(tunnel.TunnelError):
            hil.production_wifi_ready_dump(support.HilRunnerTests.production_fixture(), opener=opener)
        opener.assert_called_once()

    def test_interface_uses_guard_in_sdk_threads_and_never_direct_falls_back(self):
        class FakeTCP:
            def __init__(self, **kwargs):
                self.hostname, self.portNumber = kwargs["hostname"], kwargs["portNumber"]
            def connect(self):
                self.myConnect()
            def close(self):
                pass
            def _writeBytes(self, data):
                self.socket.sendall(data)
        module = ModuleType("meshtastic.tcp_interface")
        module.TCPInterface = FakeTCP
        lease, guard = mock.Mock(), mock.Mock()
        with mock.patch.dict(sys.modules, {"meshtastic.tcp_interface": module}), \
                mock.patch.object(hil.socket, "create_connection") as direct, zoo.scope(lease, guard):
            interface = hil.open_production_wifi_interface(ENDPOINT["host"], 4403, 90)
            guard.connect.assert_called_once_with(ENDPOINT["host"], 4403, 5)
            guard.check.side_effect = tunnel.TunnelError("lost")
            with self.assertRaises(tunnel.TunnelError):
                contextvars.Context().run(interface._writeBytes, b"test")
            direct.assert_not_called()
            guard.connect.return_value.sendall.assert_not_called()

    def test_tunnel_loss_aborts_report_instead_of_continuing_hardware_cases(self):
        report = hil.Report("tunnel-failure")
        with self.assertRaises(tunnel.TunnelError):
            report.check("must-stop", mock.Mock(side_effect=tunnel.TunnelError("lost")))
        self.assertEqual(report.failed, 1)
