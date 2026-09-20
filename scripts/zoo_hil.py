"""Private, fail-closed Zoo HIL lease client. No device/mesh operations here."""
from __future__ import annotations

import contextlib
import contextvars
import ipaddress
import json
import math
from pathlib import PurePosixPath
import re
import shlex
import socket
import subprocess
import threading
import time


TTL = 120
RENEW_EVERY = 30
REQUEST_TIMEOUT = 10
ACQUIRE_WAIT = 30
MAX_WINDOW = 6 * 3600
RECOVERY_MARGIN = 30 * 60
MAX_MESSAGE = 4096
CURRENT = contextvars.ContextVar("zoo_hil_lease", default=None)
GUARD = contextvars.ContextVar("zoo_hil_transport", default=None)


class ZooLeaseError(RuntimeError):
    """Only fixed, non-private messages may cross the evidence boundary."""


class OwnershipClock:
    """Count host suspension too; clock discontinuities must never extend a lease."""
    def __init__(self):
        self.lock = threading.Lock()
        self.base_mono = self.last = time.monotonic()
        self.base_wall = self.last_wall = time.time()

    def __call__(self):
        with self.lock:
            wall, mono = time.time(), time.monotonic()
            now = max(mono, self.base_mono + wall - self.base_wall)
            if wall < self.last_wall or now < self.last:
                raise ZooLeaseError("host clock moved backwards; Zoo ownership cannot be assumed")
            self.last_wall, self.last = wall, now
            return now


def validate_config(value, endpoint):
    if not isinstance(value, dict):
        raise ZooLeaseError("zoo_hil must be a private control configuration object")
    transport = value.get("transport")
    keys = {"unix": {"transport", "socket"},
            "ssh": {"transport", "socket", "ssh_host", "python", "helper"}}
    if not isinstance(transport, str) or transport not in keys or set(value) != keys[transport]:
        raise ZooLeaseError("invalid zoo_hil transport or fields")
    for key in keys[transport] - {"transport", "ssh_host"}:
        path = value[key]
        if (not isinstance(path, str) or not path.startswith("/") or
                any(ord(c) < 32 or ord(c) == 127 for c in path) or
                ".." in PurePosixPath(path).parts or len(path) > 512):
            raise ZooLeaseError("zoo_hil paths must be absolute, without control characters or parent traversal")
    if transport == "ssh" and (
        not isinstance(value["ssh_host"], str) or
        not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,252}", value["ssh_host"])
    ):
        raise ZooLeaseError("zoo_hil ssh_host must be an explicit SSH host alias")
    if not isinstance(endpoint, dict):
        raise ZooLeaseError("Zoo coordination requires a production WiFi endpoint")
    try:
        ip = ipaddress.ip_address(endpoint["host"])
        valid = (ip.version == 4 and str(ip) == endpoint["host"] and not
                 (ip.is_multicast or ip.is_unspecified or ip.is_loopback or ip.is_reserved))
    except (KeyError, TypeError, ValueError):
        valid = False
    if (not valid or endpoint.get("port") != 4403 or
            not re.fullmatch(r"![0-9a-f]{8}", str(endpoint.get("expected_node_id", "")))):
        raise ZooLeaseError("Zoo coordination requires the DUT's explicit IPv4, TCP 4403 and node ID")
    return dict(value)


class Transport:
    def __init__(self, config):
        self.config = config

    def __call__(self, payload):
        body = json.dumps(payload).encode() + b"\n"
        try:
            if len(body) > MAX_MESSAGE:
                raise ValueError()
            if self.config["transport"] == "unix":
                deadline = time.monotonic() + REQUEST_TIMEOUT
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.settimeout(REQUEST_TIMEOUT)
                    client.connect(self.config["socket"])
                    client.sendall(body)
                    response = bytearray()
                    while b"\n" not in response:
                        client.settimeout(max(0.001, deadline - time.monotonic()))
                        if time.monotonic() >= deadline:
                            raise TimeoutError()
                        chunk = client.recv(min(1024, MAX_MESSAGE + 1 - len(response)))
                        if not chunk:
                            raise ValueError()
                        response.extend(chunk)
                        if len(response) > MAX_MESSAGE:
                            raise ValueError()
            else:
                # Host keys and authentication are provisioned by the operator.
                # Never pass ownership tokens in argv, env, shell text or logs.
                remote = shlex.join([self.config["python"], self.config["helper"],
                                     "--socket", self.config["socket"]])
                command = ["ssh", "-T", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
                           # Allow the lab SSH banner (~6 s); the whole RPC is
                           # still bounded by REQUEST_TIMEOUT, including auth.
                           "-o", "ConnectTimeout=8", "-o", "ConnectionAttempts=1",
                           "-o", "LogLevel=ERROR", self.config["ssh_host"], remote]
                completed = subprocess.run(command, input=body, stdout=subprocess.PIPE,
                                           stderr=subprocess.DEVNULL, timeout=REQUEST_TIMEOUT, check=False)
                # hilctl uses rc=1 for a structured refusal. SSH/transport failures
                # are not receipts; stdout must never be echoed on error.
                if completed.returncode not in (0, 1):
                    raise ValueError()
                response = completed.stdout
            if len(response) > MAX_MESSAGE:
                raise ValueError()
            decoded = json.loads(response)
            if not isinstance(decoded, dict) or type(decoded.get("ok")) is not bool:
                raise ValueError()
            if self.config["transport"] == "ssh" and (completed.returncode == 0) != decoded["ok"]:
                raise ValueError()
            return decoded
        except Exception:
            raise ZooLeaseError("Zoo control transport failed or returned an invalid response") from None


class Lease:
    def __init__(self, config, endpoint, *, transport=None, clock=None):
        self.config = validate_config(config, endpoint)
        self.ip, self.node_id = endpoint["host"], endpoint["expected_node_id"]
        self.transport = transport or Transport(self.config)
        self.clock = clock or OwnershipClock()
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.thread = None
        self.token = None
        self.deadline = 0.0
        self.maximum_deadline = 0.0
        self.acquired = False
        self.lost = False
        self.released = False
        self.finished = False
        self.renewals = 0

    def _accept(self, response, started, *, initial):
        if not isinstance(response, dict) or response.get("ok") is not True:
            raise ZooLeaseError("Zoo refused the HIL window request")
        result = response.get("result")
        if not isinstance(result, dict):
            raise ZooLeaseError("Zoo returned an invalid HIL window receipt")
        token = result.get("token")
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{43}", token):
            raise ZooLeaseError("Zoo returned an invalid ownership token")
        # Retain a syntactically valid acquisition token only in memory, even if
        # another receipt field is invalid, so finally can attempt release.
        if initial:
            self.token = token
        if (result.get("ip") != self.ip or result.get("node_id") != self.node_id or
                result.get("phase") != "ready" or token != self.token):
            raise ZooLeaseError("Zoo HIL window receipt identity or ownership mismatch")
        remaining, maximum = result.get("remaining_seconds"), result.get("maximum_remaining_seconds")
        for value in (remaining, maximum):
            if type(value) not in (int, float) or not math.isfinite(value):
                raise ZooLeaseError("Zoo HIL window receipt has an invalid deadline")
        if not 0 < remaining <= TTL or not remaining <= maximum <= MAX_WINDOW:
            raise ZooLeaseError("Zoo HIL window receipt has an invalid deadline")
        # Start before the round trip, not after: a slow reply must never extend
        # the server's reservation in our local view. Maximum never increases.
        deadline = started + remaining
        maximum_deadline = started + maximum
        if not initial:
            maximum_deadline = min(self.maximum_deadline, maximum_deadline)
        if self.clock() + REQUEST_TIMEOUT >= min(deadline, maximum_deadline):
            raise ZooLeaseError("Zoo HIL window has insufficient remaining ownership time")
        self.deadline = min(deadline, maximum_deadline)
        self.maximum_deadline = maximum_deadline

    def start(self):
        if self.acquired or self.token or self.finished:
            raise ZooLeaseError("Zoo HIL window cannot be acquired twice")
        try:
            retry_deadline = time.monotonic() + ACQUIRE_WAIT
            while True:
                started = self.clock()
                response = self.transport({"action": "acquire", "ip": self.ip,
                                           "node_id": self.node_id, "ttl": TTL})
                # Retry only an explicit no-lease-issued busy response, before
                # any hardware operation. An ambiguous/lost response is terminal.
                if not (isinstance(response, dict) and response.get("ok") is False and response.get("code") == "busy"):
                    break
                remaining = retry_deadline - time.monotonic()
                if remaining <= 0 or self.stop.wait(min(1.0, remaining)):
                    break
            with self.lock:
                self._accept(response, started, initial=True)
                self.acquired = True
            self.thread = threading.Thread(target=self._monitor, name="zoo-hil-renew", daemon=True)
            self.thread.start()
        except Exception:
            self.lost = True
            raise ZooLeaseError("Zoo HIL window acquisition failed; hardware work was not admitted") from None

    def check(self):
        with self.lock:
            try:
                now = self.clock()
            except Exception:
                self.lost = True
                raise ZooLeaseError("host clock anomaly; Zoo ownership cannot be assumed") from None
            if not self.acquired or self.lost or self.finished or now >= self.deadline:
                self.lost = True
                raise ZooLeaseError("Zoo HIL ownership lost; further test operations are forbidden")

    def renew(self):
        try:
            self.check()
            started = self.clock()
            response = self.transport({"action": "renew", "ip": self.ip, "token": self.token, "ttl": TTL})
            with self.lock:
                # No response may resurrect ownership that expired during a request.
                self.check()
                self._accept(response, started, initial=False)
                self.renewals += 1
        except Exception:
            with self.lock:
                self.lost = True
            raise ZooLeaseError("Zoo HIL window renewal failed; ownership cannot be recovered in this run") from None

    def _monitor(self):
        while not self.stop.wait(RENEW_EVERY):
            try:
                self.renew()
            except Exception:
                with self.lock:
                    self.lost = True
                return  # Never silently reacquire or retry a broken lease.

    def finish(self):
        """Best-effort release after all device sessions/cleanup; never mask recovery."""
        if self.finished:
            return
        self.stop.set()
        if self.thread is not None:
            try:
                self.thread.join(timeout=REQUEST_TIMEOUT + 2)
                if self.thread.is_alive():
                    self.lost = True
            except RuntimeError:  # start() failed; still release the acquired token
                self.lost = True
        if self.acquired:
            try:
                self.check()
            except ZooLeaseError:
                pass
        if self.token:
            try:
                reply = self.transport({"action": "release", "ip": self.ip, "token": self.token})
                result = reply.get("result") if isinstance(reply, dict) else None
                self.released = (reply.get("ok") is True and isinstance(result, dict) and
                                 result.get("ip") == self.ip and result.get("released") is True)
            except Exception:
                self.released = False
        self.finished = True
        self.token = None

    def evidence(self):
        with self.lock:
            return {"requested": True, "acquired": self.acquired,
                    "ownership_preserved": self.acquired and not self.lost,
                    "release_acknowledged": self.released, "renewals": self.renewals,
                    "validated": self.finished and self.acquired and not self.lost and self.released}


def checkpoint():
    lease = CURRENT.get()
    if lease is not None:
        lease.check()
    guard = GUARD.get()
    if guard is not None:
        guard.check()


@contextlib.contextmanager
def scope(lease, guard=None):
    token = CURRENT.set(lease)
    guard_token = GUARD.set(guard)
    try:
        yield
    finally:
        try:
            try:
                if guard is not None:
                    guard.finish()
            finally:
                if lease is not None:
                    lease.finish()
        finally:
            GUARD.reset(guard_token)
            CURRENT.reset(token)


def recovery_call(operation, *args, **kwargs):
    """USB safety recovery must remain possible after loss of TCP ownership."""
    token = CURRENT.set(None)
    guard_token = GUARD.set(None)
    try:
        return operation(*args, **kwargs)
    finally:
        GUARD.reset(guard_token)
        CURRENT.reset(token)
