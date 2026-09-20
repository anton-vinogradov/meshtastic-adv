"""Owned, non-reconnecting SSH transport for the identity-bound PhoneAPI gate."""
from __future__ import annotations

import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import time

import zoo_hil


class TunnelError(RuntimeError):
    """Fixed diagnostics only: no private hosts, paths or SSH stderr in evidence."""


def validate_fixture(fixture):
    mode = fixture.get("production_wifi_transport", "direct")
    if mode not in ("direct", "zoo-ssh"):
        raise TunnelError("production_wifi_transport must be direct or zoo-ssh")
    if mode == "zoo-ssh":
        config = fixture.get("zoo_hil")
        endpoint = fixture.get("devices", {}).get("dut", {}).get("production_wifi")
        zoo_hil.validate_config(config, endpoint)
        if config["transport"] != "ssh":
            raise TunnelError("zoo-ssh requires an SSH Zoo control endpoint")
    return mode


class Tunnel:
    def __init__(self, config, endpoint, lease):
        zoo_hil.validate_config(config, endpoint)
        if config["transport"] != "ssh" or lease is None:
            raise TunnelError("SSH transport requires a Zoo SSH lease")
        self.host = config["ssh_host"]
        self.target = (endpoint["host"], endpoint["port"])
        self.lease = lease
        self.process = None
        self.directory = None
        self.path = None
        self.started = self.lost = self.finished = self.closed = False

    def start(self):
        if self.process is not None or self.finished or self.lost:
            raise TunnelError("SSH transport cannot be started twice")
        self.lease.check()  # No forwarding before the real Zoo admission.
        try:
            # A short private path avoids both AF_UNIX path limits on macOS and
            # TCP port allocation races. Nothing is exposed on a network port.
            self.directory = tempfile.TemporaryDirectory(prefix="adv-hil-", dir="/tmp")
            self.path = Path(self.directory.name) / "phoneapi.sock"
            os.chmod(self.directory.name, 0o700)
            args = ["ssh", "-F", "/dev/null", "-T"]
            for option in ("BatchMode=yes", "StrictHostKeyChecking=yes", "ConnectTimeout=8",
                           "ConnectionAttempts=1", "ExitOnForwardFailure=yes", "ForwardAgent=no",
                           "ForwardX11=no", "PermitLocalCommand=no", "ControlMaster=no",
                           "ControlPath=none", "ControlPersist=no", "ServerAliveInterval=15",
                           "ServerAliveCountMax=3", "StreamLocalBindMask=0177", "LogLevel=ERROR"):
                args.extend(("-o", option))
            # Ignore ssh_config: an unrelated LocalForward/LocalCommand must not
            # run in a hardware test. The host must be a real hostname/address;
            # default keys/agent and the existing known_hosts are still used.
            # Keep a read-only lifetime channel open. If the controller dies,
            # its stdin pipe and data sockets close; cat sees EOF and SSH exits.
            # Unlike ssh -N with /dev/null, this cannot intentionally daemonize
            # forever after the owner disappears. No server files are written.
            args.extend(("-L", f"{self.path}:{self.target[0]}:{self.target[1]}", self.host, "/bin/cat"))
            self.process = subprocess.Popen(args, stdin=subprocess.PIPE,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 15
            while True:
                self.lease.check()
                if self.process.poll() is not None:
                    raise TunnelError("SSH forwarding setup failed")
                try:
                    entry = self.path.lstat()
                except FileNotFoundError:
                    entry = None
                if entry is not None:
                    if (not stat.S_ISSOCK(entry.st_mode) or entry.st_uid != os.getuid()
                            or stat.S_IMODE(entry.st_mode) & 0o077):
                        raise TunnelError("SSH forwarding socket is not private")
                    self.started = True
                    self.check()
                    return  # Do not probe: a probe would open the DUT's one TCP slot.
                if time.monotonic() >= deadline:
                    raise TunnelError("SSH forwarding setup timed out")
                time.sleep(0.05)
        except BaseException as exc:
            self.lost = True
            self.finish()
            if isinstance(exc, (TunnelError, zoo_hil.ZooLeaseError, KeyboardInterrupt, SystemExit)):
                raise
            raise TunnelError("SSH forwarding setup failed") from None

    def check(self):
        self.lease.check()
        if (not self.started or self.lost or self.finished or self.process is None
                or self.process.poll() is not None):
            self.lost = True
            raise TunnelError("SSH transport lost; this run cannot reconnect")

    def connect(self, host, port, timeout):
        self.check()
        if (host, port) != self.target:
            self.lost = True
            raise TunnelError("SSH transport target differs from the admitted DUT")
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            connection.settimeout(timeout)
            connection.connect(str(self.path))
            self.check()
            connection.settimeout(None)
            return connection
        except BaseException:
            connection.close()
            raise

    def finish(self):
        if self.finished:
            return
        try:
            if self.process is not None:
                if self.process.poll() is not None:
                    self.lost = True
                else:
                    self.process.stdin.close()
                    try:
                        self.process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.lost = True
                        self.process.terminate()
                        try:
                            self.process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            self.process.kill()
                            self.process.wait(timeout=5)
                    if self.process.returncode != 0:
                        self.lost = True
        except Exception:
            self.lost = True
        finally:
            cleaned = True
            if self.process is not None:
                try:
                    if self.process.poll() is None:
                        self.lost = True
                        self.process.kill()
                        self.process.wait(timeout=5)
                except Exception:
                    self.lost = True
                    cleaned = False
                try:
                    self.process.stdin.close()
                except Exception:
                    self.lost = True
                    cleaned = False
            if self.directory is not None:
                try:
                    self.directory.cleanup()
                except Exception:
                    self.lost = True
                    cleaned = False
            self.closed = cleaned
            self.finished = True

    def evidence(self):
        return {"kind": "zoo-ssh", "started": self.started, "closed": self.closed,
                "healthy": self.started and not self.lost,
                "validated": self.started and not self.lost and self.finished and self.closed}
