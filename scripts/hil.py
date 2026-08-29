#!/usr/bin/env python3
"""Build, flash and exercise the Cardputer ADV hardware-in-the-loop image."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
import re
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
HIL_ENV = "m5stack-cardputer-adv-advui-hil"
RELEASE_ENV = "m5stack-cardputer-adv-advui"
EXPECTED_DEMO_FRAMES = [
    "splash", "nodes", "chat", "react", "chats", "emoji", "settings", "lora", "utc",
    "wifi", "mqtt", "unicode", "stress", "bscan", "bpin", "blink",
    *[f"a{i:02d}" for i in range(1, 20)],
]
PRODUCTION_WIFI_MIN_SOAK_DUMPS = 8
PRODUCTION_WIFI_MAX_SOAK_DUMPS = 64
PRODUCTION_WIFI_MIN_SOAK_SECONDS = 120.0
PRODUCTION_WIFI_READY_SECONDS = 90.0
PRODUCTION_WIFI_CLOSE_SETTLE_SECONDS = 3.0
PRODUCTION_WIFI_FINAL_QUIET_SECONDS = 15.0
PRODUCTION_WIFI_CONFIG_TIMEOUT_SECONDS = 30
MAX_APP_IMAGE_BYTES = 3_000_000
ESP32S3_FLASH_BYTES = 8 * 1024 * 1024
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_ENTRY_BYTES = 32
PARTITION_MAGIC = 0x50AA
REGION_CODES = {"UNSET": 0, "US": 1}
USB_IDENTITY_RE = re.compile(r"(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b")
SERIAL_PORT_RE = re.compile(r"(?i)(?:/dev/(?:cu|tty)\.[^\s]+|\bCOM\d+\b)")


class HilError(RuntimeError):
    pass


class UnexpectedReboot(HilError):
    pass


class DemoCaptureTransportError(HilError):
    pass


class ProductionWifiConnectionError(HilError):
    """A pre-baseline connectivity failure that may be retried during readiness."""


@dataclass(frozen=True)
class ProductionWifiSnapshot:
    """Private in-memory result of one complete production PhoneAPI config dump."""

    reboot_count: int
    node_count: int


@dataclass(frozen=True)
class FlashLayout:
    """Release artifact-derived regions touched by safe HIL recovery."""

    app_offset: int
    app_size: int
    filesystem_offset: int
    filesystem_size: int
    partition_table_sha256: str


def redact_lab_details(value: str) -> str:
    """Remove stable USB identities and ephemeral serial paths from public evidence."""
    value = USB_IDENTITY_RE.sub("<usb-identity>", value)
    return SERIAL_PORT_RE.sub("<serial-port>", value)


def write_owner_only_text(path: Path, value: str) -> None:
    """Create or replace a local identity/evidence file as 0600 from its first byte."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags, 0o600)
    try:
        os.fchmod(descriptor, 0o600)  # an existing file keeps its old mode on open()
        stream = os.fdopen(descriptor, "w", encoding="utf-8")
        descriptor = -1  # fdopen owns it now
        with stream:
            stream.write(value)
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def normalize_serial(value: str) -> str:
    return value.strip().replace("-", ":").upper()


def parse_fields(line: str, prefix: str) -> dict[str, str]:
    if not line.startswith(prefix):
        raise HilError(f"expected {prefix}, got {line!r}")
    fields: dict[str, str] = {}
    for item in line[len(prefix):].strip().split():
        if "=" in item:
            key, value = item.split("=", 1)
            fields[key] = value
    return fields


def protobuf_varint(value: int) -> bytes:
    """Encode an unsigned protobuf varint used by the small HIL fixture builder."""
    if value < 0:
        raise HilError("protobuf varint must be unsigned")
    encoded = bytearray()
    while value > 0x7F:
        encoded.append((value & 0x7F) | 0x80)
        value >>= 7
    encoded.append(value)
    return bytes(encoded)


def protobuf_key(field: int, wire_type: int) -> bytes:
    if field <= 0 or wire_type not in (0, 2, 5):
        raise HilError(f"unsupported protobuf key: field={field}, wire={wire_type}")
    return protobuf_varint((field << 3) | wire_type)


def protobuf_uint(field: int, value: int) -> bytes:
    return protobuf_key(field, 0) + protobuf_varint(value)


def protobuf_fixed32(field: int, value: int) -> bytes:
    if not 0 <= value <= 0xFFFFFFFF:
        raise HilError(f"fixed32 field {field} is out of range: {value}")
    return protobuf_key(field, 5) + struct.pack("<I", value)


def protobuf_bytes(field: int, value: bytes) -> bytes:
    return protobuf_key(field, 2) + protobuf_varint(len(value)) + value


def make_fromradio_frame(
    source: int,
    destination: int,
    packet_id: int,
    payload: bytes,
    *,
    portnum: int = 1,
    channel: int = 0,
    request_id: int = 0,
    reply_id: int = 0,
    emoji: int = 0,
    rx_time: int = 0,
) -> bytes:
    """Build the exact FromRadio->MeshPacket->Data wire shape consumed by AdvUI.

    The field numbers are part of Meshtastic's stable PhoneAPI schema. Keeping
    this tiny encoder local makes the release HIL independent of a particular
    generated-Python protobuf version; the firmware's nanopb decoder remains
    the authority that accepts or rejects every injected frame.
    """
    data = protobuf_uint(1, portnum) + protobuf_bytes(2, payload)
    if request_id:
        data += protobuf_fixed32(6, request_id)
    if reply_id:
        data += protobuf_fixed32(7, reply_id)
    if emoji:
        data += protobuf_fixed32(8, emoji)

    packet = protobuf_fixed32(1, source) + protobuf_fixed32(2, destination)
    if channel:
        packet += protobuf_uint(3, channel)
    packet += protobuf_bytes(4, data)
    if packet_id:
        packet += protobuf_fixed32(6, packet_id)
    if rx_time:
        packet += protobuf_fixed32(7, rx_time)
    return protobuf_bytes(2, packet)


def make_text_frame(
    source: int,
    destination: int,
    packet_id: int,
    text: str | bytes,
    *,
    channel: int = 0,
    reply_id: int = 0,
    emoji: int = 0,
    rx_time: int = 0,
) -> bytes:
    payload = text.encode("utf-8") if isinstance(text, str) else text
    return make_fromradio_frame(
        source,
        destination,
        packet_id,
        payload,
        channel=channel,
        reply_id=reply_id,
        emoji=emoji,
        rx_time=rx_time,
    )


def make_encrypted_frame(
    source: int, destination: int, packet_id: int, ciphertext: bytes, *, channel: int = 0
) -> bytes:
    """Build a valid FromRadio packet whose payload is still encrypted.

    The UI must never mistake this for decoded application data. In production
    the Meshtastic engine normally decrypts before dispatch, but rejecting the
    alternate protobuf oneof keeps malformed/replayed client traffic harmless.
    """
    packet = protobuf_fixed32(1, source) + protobuf_fixed32(2, destination)
    if channel:
        packet += protobuf_uint(3, channel)
    packet += protobuf_bytes(5, ciphertext)
    if packet_id:
        packet += protobuf_fixed32(6, packet_id)
    return protobuf_bytes(2, packet)


def make_routing_frame(
    source: int,
    destination: int,
    request_id: int,
    error_reason: int,
    *,
    packet_id: int = 0,
    variant: int = 3,
) -> bytes:
    if variant == 3:
        # Routing.variant.error_reason is oneof field 3. Emit it even for
        # NONE=0 so nanopb marks the oneof and treats it as a positive ACK.
        routing = protobuf_uint(3, error_reason)
    elif variant in (1, 2):
        # Empty RouteDiscovery request/reply: valid routing control traffic,
        # but explicitly not a delivery result for the referenced packet.
        routing = protobuf_bytes(variant, b"")
    else:
        raise HilError(f"unsupported Routing oneof variant: {variant}")
    return make_fromradio_frame(
        source, destination, packet_id, routing, portnum=5, request_id=request_id
    )


def make_queue_status_frame(error: int, packet_id: int, *, free: int = 0, maxlen: int = 16) -> bytes:
    """Build the PhoneAPI admission result emitted after sendToMesh()."""
    status = protobuf_uint(1, error)
    status += protobuf_uint(2, free)
    status += protobuf_uint(3, maxlen)
    status += protobuf_uint(4, packet_id)
    return protobuf_bytes(11, status)


def make_companion_my_info(node: int) -> bytes:
    return protobuf_bytes(3, protobuf_uint(1, node))


def make_companion_node_info(
    node: int,
    *,
    long_name: str = "",
    short_name: str = "",
    last_heard: int = 0,
    hops: int | None = None,
    battery: int | None = None,
    public_key: bytes = b"",
) -> bytes:
    user = b""
    if long_name:
        user += protobuf_bytes(2, long_name.encode("utf-8"))
    if short_name:
        user += protobuf_bytes(3, short_name.encode("utf-8"))
    if public_key:
        user += protobuf_bytes(8, public_key)
    info = protobuf_uint(1, node)
    if user:
        info += protobuf_bytes(2, user)
    if last_heard:
        info += protobuf_fixed32(5, last_heard)
    if battery is not None:
        info += protobuf_bytes(6, protobuf_uint(1, battery))
    if hops is not None:
        info += protobuf_uint(9, hops)
    return protobuf_bytes(4, info)


def make_companion_channel(index: int, name: str, role: int = 1) -> bytes:
    settings = protobuf_bytes(3, name.encode("utf-8"))
    channel = protobuf_uint(1, index) + protobuf_bytes(2, settings) + protobuf_uint(3, role)
    return protobuf_bytes(10, channel)


def make_companion_lora_config(preset: int, region: int, tx_power: int) -> bytes:
    lora = protobuf_uint(1, 1) + protobuf_uint(2, preset)
    lora += protobuf_uint(7, region) + protobuf_uint(10, tx_power)
    return protobuf_bytes(5, protobuf_bytes(6, lora))


def make_companion_device_config(role: int) -> bytes:
    return protobuf_bytes(5, protobuf_bytes(1, protobuf_uint(1, role)))


def make_companion_config_complete(request_id: int) -> bytes:
    return protobuf_uint(7, request_id)


def fnv1a32(value: bytes) -> str:
    digest = 2166136261
    for byte in value:
        digest = ((digest ^ byte) * 16777619) & 0xFFFFFFFF
    return f"{digest:08x}"


def require_serial():
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as exc:
        raise HilError("pyserial is required (install the hash-locked HIL requirements)") from exc
    return serial, list_ports


def ports() -> list[dict[str, object]]:
    _, list_ports = require_serial()
    found = []
    for port in list_ports.comports():
        if port.vid != 0x303A or port.pid != 0x1001 or not port.serial_number:
            continue
        found.append(
            {
                "port": port.device,
                "usb_serial": normalize_serial(port.serial_number),
                "description": port.description,
                "location": port.location,
            }
        )
    return sorted(found, key=lambda item: str(item["port"]))


def load_fixture(path: Path) -> dict[str, object]:
    try:
        fixture = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise HilError(f"cannot read fixture {path}: {exc}") from exc
    if fixture.get("schema") not in (1, 2) or not isinstance(fixture.get("devices"), dict):
        raise HilError(f"unsupported fixture schema in {path}")
    devices = fixture["devices"]
    dut = devices.get("dut")
    if not isinstance(dut, dict) or not dut.get("usb_serial"):
        raise HilError("fixture is missing devices.dut.usb_serial")
    dut["usb_serial"] = normalize_serial(str(dut["usb_serial"]))
    if "expected_region" in dut:
        region = str(dut["expected_region"]).strip().upper()
        if region not in REGION_CODES:
            raise HilError(f"unsupported devices.dut.expected_region: {region}")
        dut["expected_region"] = region
    if "expected_tx_power" in dut:
        power = dut["expected_tx_power"]
        if isinstance(power, bool) or not isinstance(power, int) or not 0 <= power <= 30:
            raise HilError("devices.dut.expected_tx_power must be an integer in 0..30")

    production_wifi = dut.get("production_wifi")
    if production_wifi is not None:
        validate_production_wifi_config(production_wifi)

    # Schema 1 called the second USB device a peer. Treat it as protected when
    # loading old local fixtures: current HIL never opens or mutates it.
    if fixture["schema"] == 1:
        peer = devices.get("peer")
        if not isinstance(peer, dict) or not peer.get("usb_serial"):
            raise HilError("fixture is missing devices.peer.usb_serial")
        peer["usb_serial"] = normalize_serial(str(peer["usb_serial"]))
        fixture["protected_devices"] = [{**peer, "reason": "legacy schema-1 peer"}]

    protected = fixture.get("protected_devices", [])
    if not isinstance(protected, list):
        raise HilError("protected_devices must be a list")
    serials = {dut["usb_serial"]}
    for index, device in enumerate(protected):
        if not isinstance(device, dict) or not device.get("usb_serial"):
            raise HilError(f"protected_devices[{index}] is missing usb_serial")
        device["usb_serial"] = normalize_serial(str(device["usb_serial"]))
        if device["usb_serial"] in serials:
            raise HilError("DUT and protected USB identities must be unique")
        serials.add(device["usb_serial"])

    wifi_peers = fixture.get("wifi_peers", [])
    if not isinstance(wifi_peers, list):
        raise HilError("wifi_peers must be a list")
    hosts: set[str] = set()
    for index, peer in enumerate(wifi_peers):
        if not isinstance(peer, dict) or not peer.get("host"):
            raise HilError(f"wifi_peers[{index}] is missing host")
        host = str(peer["host"]).strip()
        access = peer.get("access", "read-only")
        if access not in ("read-only", "test"):
            raise HilError(f"wifi_peers[{index}].access must be read-only or test")
        if host in hosts:
            raise HilError(f"duplicate WiFi peer host: {host}")
        peer["host"] = host
        peer["access"] = access
        hosts.add(host)
    return fixture


def validate_production_wifi_config(value: object) -> dict[str, object]:
    """Validate the private release-only endpoint without exposing its values."""
    required = {"host", "port", "expected_node_id", "min_nodes"}
    if not isinstance(value, dict) or set(value) != required:
        raise HilError("devices.dut.production_wifi must contain host, port, expected_node_id and min_nodes")

    host = value["host"]
    if (
        not isinstance(host, str)
        or not host.strip()
        or len(host.strip()) > 253
        or any(ord(character) <= 32 for character in host.strip())
        or any(character in host for character in "/?#@")
    ):
        raise HilError("devices.dut.production_wifi.host is invalid")

    port = value["port"]
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
        raise HilError("devices.dut.production_wifi.port must be an integer in 1..65535")

    node_id = value["expected_node_id"]
    if not isinstance(node_id, str) or not re.fullmatch(r"![0-9a-fA-F]{8}", node_id.strip()):
        raise HilError("devices.dut.production_wifi.expected_node_id must be ! plus eight hex digits")

    min_nodes = value["min_nodes"]
    if isinstance(min_nodes, bool) or not isinstance(min_nodes, int) or not 32 <= min_nodes <= 250:
        raise HilError("devices.dut.production_wifi.min_nodes must be an integer in 32..250")

    value["host"] = host.strip()
    value["expected_node_id"] = node_id.strip().lower()
    return value


def require_production_wifi_config(fixture: dict[str, object]) -> dict[str, object]:
    devices = fixture.get("devices")
    dut = devices.get("dut") if isinstance(devices, dict) else None
    config = dut.get("production_wifi") if isinstance(dut, dict) else None
    if config is None:
        raise HilError("release HIL requires devices.dut.production_wifi in its private fixture")
    return validate_production_wifi_config(config)


def require_usb_wifi_node_identity(dut: dict[str, object], state: dict[str, str]) -> None:
    """Bind the private production WiFi endpoint to the USB HIL application."""
    production_wifi = dut.get("production_wifi")
    if production_wifi is None:
        return
    config = validate_production_wifi_config(production_wifi)
    try:
        usb_node = int(state.get("node", "0"), 16)
        wifi_node = int(str(config["expected_node_id"])[-8:], 16)
    except (TypeError, ValueError):
        raise HilError("HIL USB firmware reported an invalid node identity") from None
    if usb_node == 0 or usb_node != wifi_node:
        raise HilError("HIL USB DUT and production WiFi endpoint identities do not match")


def resolve_role(fixture: dict[str, object], role: str) -> dict[str, object]:
    expected = fixture["devices"][role]
    visible = ports()
    matches = [item for item in visible if item["usb_serial"] == expected["usb_serial"]]
    if len(matches) != 1:
        raise HilError(
            f"{role} is not uniquely connected; matching={len(matches)} visible_usb={len(visible)}"
        )
    return {**expected, **matches[0], "role": role}


def wait_for_usb_serial(usb_serial: str, timeout: float = 20) -> dict[str, object]:
    expected = normalize_serial(usb_serial)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        matches = [item for item in ports() if item["usb_serial"] == expected]
        if len(matches) == 1:
            return matches[0]
        time.sleep(0.25)
    raise HilError("identity-bound device did not reappear after flashing")


def kick_device(usb_serial: str) -> dict[str, object]:
    """Hold native USB open long enough to leave the post-upload reset state."""
    serial, _ = require_serial()
    device = wait_for_usb_serial(usb_serial)
    handle = serial.Serial()
    handle.port = str(device["port"])
    handle.baudrate = 115200
    handle.timeout = 0.25
    handle.dtr = False
    handle.rts = False
    try:
        handle.open()
        # ESP32-S3 native USB can reappear before the application has actually
        # crossed the ROM/bootloader handoff. Keep the host side present while
        # draining (and deliberately discarding) early logs so a production
        # Wi-Fi soak does not depend on someone opening a serial terminal.
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            handle.read(512)
        handle.close()
    except Exception:
        try:
            handle.close()
        except Exception:
            pass
        raise HilError("identity-bound device serial reset failed") from None
    return device


def wifi_inventory(fixture: dict[str, object], timeout: float = 1) -> list[dict[str, object]]:
    """Probe configured PhoneAPI endpoints without sending an application payload."""
    found = []
    for peer in fixture.get("wifi_peers", []):
        host = str(peer["host"])
        port = int(peer.get("port", 4403))
        online = False
        error = ""
        try:
            with socket.create_connection((host, port), timeout=timeout):
                online = True
        except OSError as exc:
            error = str(exc)
        found.append({**peer, "port": port, "online": online, "error": error})
    return found


@contextlib.contextmanager
def quiet_phoneapi():
    """Keep library logs and private config fields out of unattended evidence."""
    with open(os.devnull, "w", encoding="utf-8") as sink:
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            yield


def open_production_wifi_interface(host: str, port: int, timeout: int) -> object:
    """Open one complete, non-reconnecting, read-only PhoneAPI config stream."""
    try:
        from meshtastic.tcp_interface import TCPInterface
    except ImportError as exc:
        raise HilError("Meshtastic Python is required (install the hash-locked HIL requirements)") from exc

    class FailClosedTCPInterface(TCPInterface):
        def myConnect(self) -> None:  # noqa: N802 - upstream API spelling
            connected = socket.create_connection(
                (self.hostname, self.portNumber), timeout=timeout
            )
            connected.settimeout(None)
            self.socket = connected

        def _reconnect(self) -> None:
            # A reconnect could hide a reboot inside one nominal config dump.
            # Every post-baseline connection is therefore exactly one TCP
            # session, and any loss is release-blocking.
            self._wantExit = True
            raise ConnectionError("production WiFi soak forbids PhoneAPI reconnect")

    return FailClosedTCPInterface(
        hostname=host,
        portNumber=port,
        noNodes=False,
        timeout=timeout,
    )


def production_wifi_dump(
    config: dict[str, object],
    opener: Callable[[str, int, int], object] | None = None,
    *,
    enforce_configured_minimum: bool = True,
) -> ProductionWifiSnapshot:
    """Receive and validate one full production config dump without persisting it."""
    config = validate_production_wifi_config(config)
    open_interface = opener or open_production_wifi_interface
    interface: object | None = None
    with quiet_phoneapi():
        try:
            interface = open_interface(
                str(config["host"]),
                int(config["port"]),
                PRODUCTION_WIFI_CONFIG_TIMEOUT_SECONDS,
            )
        except Exception as exc:
            raise ProductionWifiConnectionError(
                f"production PhoneAPI config dump failed ({type(exc).__name__})"
            ) from None

        try:
            connected = getattr(interface, "isConnected", None)
            if connected is None or not callable(getattr(connected, "is_set", None)):
                raise HilError("production PhoneAPI did not expose config-complete state")
            if not connected.is_set():
                raise ProductionWifiConnectionError("production PhoneAPI config dump was incomplete")

            my_info = getattr(interface, "myInfo", None)
            metadata = getattr(interface, "metadata", None)
            nodes = getattr(interface, "nodesByNum", None)
            if my_info is None or metadata is None or not isinstance(nodes, dict):
                raise ProductionWifiConnectionError("production PhoneAPI config dump was incomplete")

            expected_node = int(str(config["expected_node_id"])[-8:], 16)
            try:
                actual_node = int(getattr(my_info, "my_node_num", 0))
                reported_nodes = int(getattr(my_info, "nodedb_count", 0))
                reboot_count = int(getattr(my_info, "reboot_count", -1))
            except (TypeError, ValueError):
                raise HilError("production PhoneAPI reported invalid numeric metadata") from None

            if actual_node != expected_node:
                raise HilError("production PhoneAPI endpoint identity does not match the fixture")
            if str(getattr(my_info, "pio_env", "")) != RELEASE_ENV:
                raise HilError("production PhoneAPI endpoint reports the wrong firmware environment")
            # The pinned 2.7 Meshtastic Python protobuf exposes the legacy
            # camelCase field name used on this release line.
            if not bool(getattr(metadata, "hasWifi", False)):
                raise HilError("production PhoneAPI endpoint does not report WiFi capability")

            received_nodes = len(nodes)
            minimum_nodes = int(config["min_nodes"])
            if enforce_configured_minimum and (reported_nodes < minimum_nodes or received_nodes < minimum_nodes):
                raise HilError("production PhoneAPI NodeDB is below the fixture minimum")

            if reboot_count < 0:
                raise HilError("production PhoneAPI did not report a valid reboot counter")
            return ProductionWifiSnapshot(
                reboot_count=reboot_count,
                node_count=min(reported_nodes, received_nodes),
            )
        finally:
            if interface is not None:
                try:
                    interface.close()
                except Exception as exc:
                    raise ProductionWifiConnectionError(
                        f"production PhoneAPI close failed ({type(exc).__name__})"
                    ) from None


def production_wifi_ready_dump(
    fixture: dict[str, object],
    *,
    opener: Callable[[str, int, int], object] | None = None,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
    enforce_configured_minimum: bool = True,
) -> ProductionWifiSnapshot:
    """Wait for one complete production config stream after an app reset."""
    config = require_production_wifi_config(fixture)
    ready_deadline = monotonic() + PRODUCTION_WIFI_READY_SECONDS
    while True:
        try:
            return production_wifi_dump(
                config,
                opener=opener,
                enforce_configured_minimum=enforce_configured_minimum,
            )
        except ProductionWifiConnectionError as exc:
            remaining = ready_deadline - monotonic()
            if remaining <= 0:
                raise HilError(
                    f"production PhoneAPI did not become ready ({type(exc).__name__})"
                ) from None
            sleep(min(PRODUCTION_WIFI_CLOSE_SETTLE_SECONDS, remaining))


def production_wifi_soak(
    fixture: dict[str, object],
    *,
    opener: Callable[[str, int, int], object] | None = None,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, object]:
    """Prove repeated full production config dumps do not reboot or lose NodeDB state."""
    config = require_production_wifi_config(fixture)
    baseline = production_wifi_ready_dump(
        fixture,
        opener=opener,
        monotonic=monotonic,
        sleep=sleep,
        enforce_configured_minimum=True,
    )
    required_nodes = int(config["min_nodes"])
    if baseline.node_count < required_nodes:
        raise HilError("production PhoneAPI NodeDB is below the fixture minimum")

    started = monotonic()
    soak_dumps = 0
    minimum_node_count = baseline.node_count
    while (
        soak_dumps < PRODUCTION_WIFI_MIN_SOAK_DUMPS
        or monotonic() - started < PRODUCTION_WIFI_MIN_SOAK_SECONDS
    ):
        if soak_dumps >= PRODUCTION_WIFI_MAX_SOAK_DUMPS:
            raise HilError("production WiFi soak did not reach its minimum duration")
        sleep(PRODUCTION_WIFI_CLOSE_SETTLE_SECONDS)
        current = production_wifi_dump(
            config,
            opener=opener,
            enforce_configured_minimum=True,
        )
        soak_dumps += 1
        minimum_node_count = min(minimum_node_count, current.node_count)
        if current.node_count < required_nodes:
            raise HilError("production PhoneAPI NodeDB shrank during soak")
        if current.reboot_count != baseline.reboot_count:
            raise UnexpectedReboot("production firmware rebooted during the WiFi config-dump soak")

    sleep(PRODUCTION_WIFI_FINAL_QUIET_SECONDS)
    final = production_wifi_dump(
        config,
        opener=opener,
        enforce_configured_minimum=True,
    )
    minimum_node_count = min(minimum_node_count, final.node_count)
    if final.node_count < required_nodes:
        raise HilError("production PhoneAPI NodeDB shrank after soak")
    if final.reboot_count != baseline.reboot_count:
        raise UnexpectedReboot("production firmware rebooted after the WiFi config-dump soak")

    return {
        "validated": True,
        "identity_verified": True,
        "environment_verified": True,
        "wifi_capability_verified": True,
        "config_dump_cycles": 1 + soak_dumps + 1,
        "soak_seconds": round(monotonic() - started, 3),
        "minimum_node_count": minimum_node_count,
        "required_node_count": required_nodes,
        "configured_minimum_node_count": int(config["min_nodes"]),
        "baseline_node_count": baseline.node_count,
        "stable_reboot_counter": True,
        "mesh_writes": 0,
        "admin_writes": 0,
        "chat_writes": 0,
    }


def find_pio() -> str:
    candidates = [
        os.environ.get("PIO"),
        shutil.which("pio"),
        str(Path.home() / ".pio-core-venv/bin/pio"),
        str(Path.home() / ".platformio/penv/bin/pio"),
        str(ROOT.parent / ".venv-pio/bin/pio"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise HilError("PlatformIO not found; set PIO=/path/to/pio")


def find_meshtastic_cli() -> str:
    """Use one explicit/pinned CLI for both configuration export and restore."""
    explicit = os.environ.get("MESHTASTIC_CLI")
    if explicit:
        candidate = Path(explicit)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
        raise HilError("MESHTASTIC_CLI is not an executable file")

    sibling = Path(sys.executable).with_name("meshtastic")
    if sibling.is_file() and os.access(sibling, os.X_OK):
        return str(sibling.resolve())
    discovered = shutil.which("meshtastic")
    if discovered:
        return str(Path(discovered).resolve())
    raise HilError("meshtastic CLI not found in the active HIL environment")


def run_checked(command: list[str], cwd: Path = ROOT, redact_output: bool = False) -> None:
    rendered = " ".join(command)
    print("+", redact_lab_details(rendered) if redact_output else rendered, flush=True)
    if not redact_output:
        subprocess.run(command, cwd=cwd, check=True)
        return
    try:
        result = subprocess.run(
            command, cwd=cwd, check=False, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
    except OSError:
        raise HilError("identity-bound flash command could not start") from None
    if result.stdout:
        print(redact_lab_details(result.stdout), end="", flush=True)
    if result.returncode != 0:
        raise HilError(f"identity-bound flash command failed with exit code {result.returncode}")


def run_captured(command: list[str], cwd: Path = ROOT, redact_command: bool = False) -> str:
    rendered = " ".join(command)
    print("+", redact_lab_details(rendered) if redact_command else rendered, flush=True)
    try:
        result = subprocess.run(
            command, cwd=cwd, check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
    except subprocess.CalledProcessError as exc:
        if redact_command:
            raise HilError(f"identity check failed with exit code {exc.returncode}") from None
        raise
    except OSError:
        if redact_command:
            raise HilError("identity check could not start") from None
        raise
    return result.stdout


def find_esptool() -> list[str]:
    """Return an esptool command without ever delegating port choice to PlatformIO."""
    configured = os.environ.get("ESPTOOL")
    if configured:
        executable = shutil.which(configured) or (configured if Path(configured).is_file() else None)
        if executable:
            return [executable]

    # hil/requirements.txt pins the compatible major in the interpreter that
    # runs this script. Prefer it over PlatformIO's independent penv, which can
    # silently upgrade to a new CLI spelling between releases.
    if importlib.util.find_spec("esptool") is not None:
        return [sys.executable, "-m", "esptool"]

    executable = shutil.which("esptool") or shutil.which("esptool.py")
    if executable:
        return [executable]
    for python in (
        Path.home() / ".platformio/penv/bin/python",
        Path.home() / ".platformio/penv/Scripts/python.exe",
    ):
        if python.is_file():
            return [str(python), "-m", "esptool"]
    raise HilError("esptool not found; install the PlatformIO Espressif toolchain")


def firmware_image(env_name: str) -> Path:
    build_dir = FIRMWARE / ".pio/build" / env_name
    images = sorted(
        path for path in build_dir.glob(f"firmware-{env_name}-*.bin")
        if not path.name.endswith(".factory.bin")
    )
    if len(images) != 1:
        names = ", ".join(path.name for path in images) or "none"
        raise HilError(f"expected exactly one app image for {env_name}, found: {names}")
    return images[0]


def firmware_factory_image(env_name: str) -> Path:
    build_dir = FIRMWARE / ".pio/build" / env_name
    images = sorted(build_dir.glob(f"firmware-{env_name}-*.factory.bin"))
    if len(images) != 1:
        names = ", ".join(path.name for path in images) or "none"
        raise HilError(f"expected exactly one factory image for {env_name}, found: {names}")
    return images[0]


def parse_flash_layout(path: Path) -> FlashLayout:
    """Read the app/filesystem layout from the exact release partition table."""
    resolved = path.resolve()
    try:
        payload = resolved.read_bytes()
    except OSError as exc:
        raise HilError(f"cannot read release factory image {path}: {exc}") from exc
    table = payload[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + 0x1000]
    if len(table) != 0x1000:
        raise HilError("release factory image does not contain a complete partition table")

    partitions: dict[str, tuple[int, int, int, int]] = {}
    table_end = 0
    for index in range(0, len(table), PARTITION_ENTRY_BYTES):
        entry = table[index:index + PARTITION_ENTRY_BYTES]
        magic = struct.unpack_from("<H", entry)[0]
        if magic != PARTITION_MAGIC:
            table_end = index
            break
        _, partition_type, subtype, offset, size, raw_label, _ = struct.unpack("<HBBII16sI", entry)
        try:
            label = raw_label.split(b"\0", 1)[0].decode("ascii", errors="strict")
        except UnicodeDecodeError as exc:
            raise HilError("release partition table contains a non-ASCII label") from exc
        if not label or label in partitions:
            raise HilError("release partition table contains an invalid or duplicate label")
        if offset < PARTITION_TABLE_OFFSET + 0x1000 or size <= 0 or offset + size > ESP32S3_FLASH_BYTES:
            raise HilError(f"release partition {label} is outside the 8 MB flash")
        partitions[label] = (partition_type, subtype, offset, size)
    if table_end == 0:
        raise HilError("release partition table is empty or unterminated")

    try:
        app_type, _, app_offset, app_size = partitions["app0"]
        fs_type, fs_subtype, fs_offset, fs_size = partitions["spiffs"]
    except KeyError as exc:
        raise HilError("release partition table is missing app0 or spiffs") from exc
    app_subtype = partitions["app0"][1]
    if app_type != 0x00 or app_subtype != 0x10 or fs_type != 0x01 or fs_subtype != 0x82:
        raise HilError("release partition table has unexpected app0/spiffs types")
    ordered = sorted((offset, offset + size, label) for label, (_, _, offset, size) in partitions.items())
    for previous, current in zip(ordered, ordered[1:]):
        if previous[1] > current[0]:
            raise HilError(
                f"release partitions {previous[2]} and {current[2]} overlap"
            )
    if app_offset + app_size > fs_offset or fs_offset + fs_size > ESP32S3_FLASH_BYTES:
        raise HilError("release app0 and filesystem layout is invalid")
    return FlashLayout(
        app_offset=app_offset,
        app_size=app_size,
        filesystem_offset=fs_offset,
        filesystem_size=fs_size,
        partition_table_sha256=hashlib.sha256(table[:table_end]).hexdigest(),
    )


def validate_factory_image(path: Path, app_image: Path | None = None) -> tuple[Path, FlashLayout]:
    resolved = path.resolve()
    try:
        size = resolved.stat().st_size
        magic = resolved.read_bytes()[:1]
    except OSError as exc:
        raise HilError(f"cannot read release factory image {path}: {exc}") from exc
    if magic != b"\xe9" or not PARTITION_TABLE_OFFSET + 0x1000 <= size <= ESP32S3_FLASH_BYTES:
        raise HilError("release factory image has an invalid size or ESP magic")
    layout = parse_flash_layout(resolved)
    if app_image is not None:
        app = validate_app_image(app_image)
        if app.stat().st_size > layout.app_size:
            raise HilError("release app image does not fit the app0 partition")
        factory = resolved.read_bytes()
        app_payload = app.read_bytes()
        if factory[layout.app_offset:layout.app_offset + len(app_payload)] != app_payload:
            raise HilError("release factory/app bytes do not match")
    return resolved, layout


def validate_app_image(path: Path) -> Path:
    """Validate an app-partition image before allowing an identity-bound flash."""
    resolved = path.resolve()
    try:
        size = resolved.stat().st_size
        magic = resolved.read_bytes()[:1]
    except OSError as exc:
        raise HilError(f"cannot read release app image {path}: {exc}") from exc
    if magic != b"\xe9":
        raise HilError(f"release app image has invalid ESP magic: {path}")
    if not 64 * 1024 <= size <= MAX_APP_IMAGE_BYTES:
        raise HilError(
            f"release app image size {size} is outside 64 KiB..{MAX_APP_IMAGE_BYTES} bytes: {path}"
        )
    return resolved


def stage_release_image(image_override: Path | None, temporary_root: Path) -> tuple[Path, str]:
    """Pin recovery bytes outside .pio before HIL is allowed to touch hardware."""
    source = validate_app_image(image_override) if image_override is not None else firmware_image(RELEASE_ENV)
    source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
    staged = temporary_root / "release-app.bin"
    shutil.copyfile(source, staged)
    os.chmod(staged, 0o600)
    staged = validate_app_image(staged)
    digest = hashlib.sha256(staged.read_bytes()).hexdigest()
    if source_digest != digest:
        raise HilError("staged production recovery image did not match its source")
    return staged, digest


def stage_factory_image(
    image_override: Path | None, app_image: Path, temporary_root: Path
) -> tuple[Path, FlashLayout, str]:
    """Pin the exact partition table used to locate and recover LittleFS."""
    source = image_override if image_override is not None else firmware_factory_image(RELEASE_ENV)
    source, _ = validate_factory_image(source, app_image)
    source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
    staged = temporary_root / "release-factory.bin"
    shutil.copyfile(source, staged)
    os.chmod(staged, 0o600)
    staged, layout = validate_factory_image(staged, app_image)
    digest = hashlib.sha256(staged.read_bytes()).hexdigest()
    if digest != source_digest:
        raise HilError("staged production factory image did not match its source")
    return staged, layout, digest


def chip_mac(port: str) -> str:
    # Use the canonical esptool 5.x command spelling. The HIL interpreter pins
    # that major independently of PlatformIO's embedded toolchain.
    output = run_captured(
        find_esptool() + ["--chip", "esp32s3", "--port", port, "read-mac"],
        redact_command=True,
    )
    matches = re.findall(r"^MAC:\s*([0-9a-f:]{17})\s*$", output, re.IGNORECASE | re.MULTILINE)
    if not matches:
        raise HilError(f"esptool did not report a MAC for {port}")
    return normalize_serial(matches[-1])


def reuse_release_dependencies() -> None:
    """Avoid downloading the same 200 MB libdeps tree for the HIL environment."""
    source_dir = FIRMWARE / ".pio/libdeps" / RELEASE_ENV
    target_dir = FIRMWARE / ".pio/libdeps" / HIL_ENV
    if not source_dir.is_dir():
        return  # a clean machine lets PlatformIO install the HIL dependencies normally
    target_dir.mkdir(parents=True, exist_ok=True)
    linked = 0
    for source in source_dir.iterdir():
        target = target_dir / source.name
        if target.exists() or target.is_symlink():
            continue
        try:
            target.symlink_to(source.resolve(), target_is_directory=source.is_dir())
            linked += 1
        except OSError:
            return  # symlinks can require elevation on Windows; downloading remains valid
    if linked:
        print(f"reused {linked} release dependency entries for HIL", flush=True)


def validate_hil_stock_ble_guard() -> None:
    """Require the synced HIL guard to precede every stock BLE side effect."""
    source = FIRMWARE / "src/platform/esp32/main-esp32.cpp"
    expected = (
        "void setBluetoothEnable(bool enable)\n"
        "{\n"
        "#ifdef ADVUI_HIL\n"
        "    (void)enable;\n"
        "    return; // advui-inject-hil-no-stock-ble: no BLE advertising or PhoneAPI in release HIL\n"
        "#endif\n"
    )
    try:
        body = source.read_text(encoding="utf-8")
    except OSError as exc:
        raise HilError(f"cannot validate synced stock-BLE HIL guard: {exc}") from exc
    if body.count("advui-inject-hil-no-stock-ble") != 1 or body.count(expected) != 1:
        raise HilError("synced stock-BLE HIL guard is missing, duplicated, or misplaced")


def build(release: bool = False) -> None:
    run_checked(["bash", "scripts/sync-overlay.sh"])
    validate_hil_stock_ble_guard()
    env_name = RELEASE_ENV if release else HIL_ENV
    if not release:
        reuse_release_dependencies()
    run_checked([find_pio(), "run", "-e", env_name], FIRMWARE)


def flash(
    fixture: dict[str, object], release: bool = False, image_override: Path | None = None
) -> dict[str, object]:
    if image_override is not None and not release:
        raise HilError("an external app image may only be used for production restore")
    device = resolve_role(fixture, "dut")
    env_name = RELEASE_ENV if release else HIL_ENV
    print(
        f"target role=dut kind={device.get('kind', '?')} identity=verified",
        flush=True,
    )
    # Never use `pio -t upload` here. The pioarduino hybrid builder launches a
    # child `pio run` without forwarding --upload-port, which silently falls
    # back to auto-detection and can flash another connected ESP32. Resolve the
    # stable USB identity, confirm it again at chip level, and invoke esptool on
    # that exact path. Only the app partition is touched; fixture configuration
    # and identity in NVS/LittleFS remain intact.
    expected_mac = str(device["usb_serial"])
    actual_mac = chip_mac(str(device["port"]))
    if actual_mac != expected_mac:
        raise HilError("chip identity did not match the fixture before flash")
    live = wait_for_usb_serial(expected_mac)
    image = validate_app_image(image_override) if image_override is not None else firmware_image(env_name)
    command = find_esptool() + [
        "--chip", "esp32s3", "--port", str(live["port"]), "--baud", "921600",
        "write-flash", "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "8MB",
        "0x10000", str(image),
    ]
    run_checked(command, redact_output=True)
    verified = wait_for_usb_serial(expected_mac)
    run_checked(
        find_esptool() + [
            "--chip", "esp32s3", "--port", str(verified["port"]),
            "verify-flash", "0x10000", str(image),
        ],
        redact_output=True,
    )
    live = kick_device(expected_mac)
    print("application USB ready: identity verified", flush=True)
    return {**device, **live, "image": str(image), "chip_mac": actual_mac}


def validate_filesystem_backup(path: Path, layout: FlashLayout) -> Path:
    """Accept only the owner-only, exact-size raw LittleFS recovery image."""
    if path.is_symlink() or not path.is_file():
        raise HilError("restorable filesystem backup is missing or unsafe")
    resolved = path.resolve()
    if stat.S_IMODE(resolved.stat().st_mode) & 0o077:
        raise HilError("restorable filesystem backup has unsafe permissions")
    if resolved.stat().st_size != layout.filesystem_size:
        raise HilError("restorable filesystem backup has an invalid size")
    return resolved


def filesystem_backup_for(configuration_backup: Path) -> Path:
    return configuration_backup.parent / "filesystem-before.bin"


def capture_filesystem_backup(
    fixture: dict[str, object], destination: Path, layout: FlashLayout
) -> tuple[Path, str]:
    """Read the complete production LittleFS before HIL changes any flash bytes."""
    if destination.exists() or destination.is_symlink():
        raise HilError("filesystem backup destination already exists")
    if not destination.parent.is_dir() or destination.parent.is_symlink():
        raise HilError("filesystem backup parent must be an existing real directory")
    parent_stat = destination.parent.stat()
    if parent_stat.st_uid != os.getuid() or stat.S_IMODE(parent_stat.st_mode) & 0o077:
        raise HilError("filesystem backup parent is not owner-only")

    device = resolve_role(fixture, "dut")
    expected_mac = str(device["usb_serial"])
    if chip_mac(str(device["port"])) != expected_mac:
        raise HilError("chip identity did not match the fixture before filesystem backup")
    live = wait_for_usb_serial(expected_mac)
    partial = destination.with_name(destination.name + ".partial")
    if partial.exists() or partial.is_symlink():
        raise HilError("filesystem backup partial destination already exists")
    try:
        run_checked(
            find_esptool() + [
                "--chip", "esp32s3", "--port", str(live["port"]), "--baud", "921600",
                "read-flash", hex(layout.filesystem_offset), hex(layout.filesystem_size), str(partial),
            ],
            redact_output=True,
        )
        os.chmod(partial, 0o600)
        with partial.open("rb") as stream:
            os.fsync(stream.fileno())
        validate_filesystem_backup(partial, layout)
        os.replace(partial, destination)
        backup = validate_filesystem_backup(destination, layout)
        digest = hashlib.sha256(backup.read_bytes()).hexdigest()
        wait_for_usb_serial(expected_mac, timeout=30)
        return backup, digest
    except Exception:
        partial.unlink(missing_ok=True)
        raise


def restore_production_state(
    fixture: dict[str, object], release_image: Path, factory_image: Path,
    filesystem_backup: Path,
) -> dict[str, object]:
    """Atomically write and verify the exact production app and saved LittleFS."""
    app = validate_app_image(release_image)
    _, layout = validate_factory_image(factory_image, app)
    filesystem = validate_filesystem_backup(filesystem_backup, layout)
    device = resolve_role(fixture, "dut")
    expected_mac = str(device["usb_serial"])
    if chip_mac(str(device["port"])) != expected_mac:
        raise HilError("chip identity did not match the fixture before production restore")
    live = wait_for_usb_serial(expected_mac)
    common = [
        "--chip", "esp32s3", "--port", str(live["port"]), "--after", "no-reset"
    ]
    run_checked(
        find_esptool() + common + [
            "--baud", "921600", "write-flash", "--flash-mode", "dio", "--flash-freq", "80m",
            "--flash-size", "8MB", hex(layout.app_offset), str(app),
            hex(layout.filesystem_offset), str(filesystem),
        ],
        redact_output=True,
    )
    verified = wait_for_usb_serial(expected_mac)
    verify_common = ["--chip", "esp32s3", "--port", str(verified["port"]), "verify-flash"]
    run_checked(
        find_esptool() + verify_common + [
            hex(layout.app_offset), str(app), hex(layout.filesystem_offset), str(filesystem),
        ],
        redact_output=True,
    )
    live = kick_device(expected_mac)
    print("production app and filesystem restored: identity verified", flush=True)
    return {**device, **live, "chip_mac": expected_mac}


def capture_config_fingerprint(
    fixture: dict[str, object], temporary_root: Path, label: str, preserve: Path | None = None
) -> bytes:
    """Export a stable DUT config fingerprint and optionally retain one private backup."""
    if not re.fullmatch(r"[A-Za-z0-9_-]+", label):
        raise HilError("unsafe configuration export label")
    environment = dict(os.environ)
    environment["MESHTASTIC_CLI"] = find_meshtastic_cli()
    # Match backup-node.sh's field-proven retry policy. A self-hosted runner can
    # tune either value downward, but the outer timeout remains an absolute cap.
    environment.setdefault("MESHTASTIC_BACKUP_ATTEMPTS", "4")
    environment.setdefault("MESHTASTIC_BACKUP_TIMEOUT", "90")
    previous: bytes | None = None
    for sample in range(1, 4):
        device = resolve_role(fixture, "dut")
        sample_port = str(device["port"])
        destination = temporary_root / f"{label}-{sample}"
        try:
            try:
                result = subprocess.run(
                    [
                        "bash",
                        str(ROOT / "scripts/backup-node.sh"),
                        "--port",
                        sample_port,
                        "--out",
                        str(destination),
                    ],
                    cwd=ROOT,
                    env=environment,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                    timeout=480,
                )
            except subprocess.TimeoutExpired as exc:
                raise HilError("verified node configuration export timed out") from exc
            except OSError as exc:
                raise HilError("verified node configuration export could not start") from exc

            verified = resolve_role(fixture, "dut")
            if str(verified["port"]) != sample_port:
                raise HilError("identity-bound DUT port changed during configuration export")

            exports = sorted(destination.glob("config-*.yaml"))
            if result.returncode != 0 or len(exports) != 1:
                raise HilError("verified node configuration export failed")
            exported = exports[0]
            try:
                mode = stat.S_IMODE(exported.stat().st_mode)
                payload = exported.read_bytes()
            except OSError as exc:
                raise HilError("verified node configuration export could not be read") from exc
            if mode & 0o077:
                raise HilError("verified node configuration export has unsafe permissions")
            if not payload:
                raise HilError("verified node configuration export is empty")
        finally:
            if destination.is_symlink():
                raise HilError("configuration export directory became a symlink")
            try:
                shutil.rmtree(destination)
            except FileNotFoundError:
                pass
            except OSError as exc:
                raise HilError("private configuration export cleanup failed") from exc

        digest = hashlib.sha256(stable_config_payload(payload)).digest()
        if digest == previous:
            if preserve is not None:
                flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
                if hasattr(os, "O_NOFOLLOW"):
                    flags |= os.O_NOFOLLOW
                try:
                    descriptor = os.open(preserve, flags, 0o600)
                except OSError as exc:
                    raise HilError("configuration backup destination is not exclusive") from exc
                try:
                    with os.fdopen(descriptor, "wb") as output:
                        output.write(payload)
                        output.flush()
                        os.fsync(output.fileno())
                    os.chmod(preserve, 0o600)
                    validate_config_backup(preserve)
                except Exception:
                    preserve.unlink(missing_ok=True)
                    raise
            return digest
        previous = digest

    raise HilError("verified node configuration export was not stable")


def prepare_config_backup_destination(artifacts: Path, requested: Path | None) -> Path:
    """Choose a private, non-overwriting backup path before hardware is touched."""
    github_actions = os.environ.get("GITHUB_ACTIONS", "").lower() == "true"
    if github_actions and requested is None:
        raise HilError("GitHub Actions HIL requires an external configuration backup destination")

    if requested is None:
        parent = artifacts / "private"
        if parent.is_symlink():
            raise HilError("local configuration backup directory is a symlink")
        if parent.exists():
            if not parent.is_dir() or parent.stat().st_uid != os.getuid():
                raise HilError("local configuration backup directory is unsafe")
        else:
            parent.mkdir(parents=True, mode=0o700)
        os.chmod(parent, 0o700)
        destination = parent / "config-before.yaml"
    else:
        destination = requested
        if not destination.is_absolute() or destination.suffix != ".yaml":
            raise HilError("configuration backup destination must be an absolute .yaml path")
        parent = destination.parent
        if not parent.is_dir() or parent.is_symlink():
            raise HilError("configuration backup parent must be an existing real directory")
        parent_stat = parent.stat()
        if parent_stat.st_uid != os.getuid() or stat.S_IMODE(parent_stat.st_mode) & 0o077:
            raise HilError("configuration backup parent is not owner-only")

    resolved = destination.resolve(strict=False)
    if github_actions:
        for variable in ("GITHUB_WORKSPACE", "RUNNER_TEMP"):
            value = os.environ.get(variable)
            if not value:
                raise HilError(f"{variable} is required to validate the recovery vault")
            unsafe_root = Path(value).resolve()
            if resolved == unsafe_root or unsafe_root in resolved.parents:
                raise HilError("configuration backup destination is inside an ephemeral job directory")

    if destination.exists() or destination.is_symlink():
        raise HilError("configuration backup destination already exists")
    return destination


def configuration_temporary_directory(prefix: str) -> tempfile.TemporaryDirectory[str]:
    """Keep CI's transient private exports inside the runner-managed temp root."""
    parent: Path | None = None
    if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
        value = os.environ.get("RUNNER_TEMP")
        if not value:
            raise HilError("RUNNER_TEMP is required for private configuration exports")
        parent = Path(value)
        if not parent.is_absolute() or parent.is_symlink() or not parent.is_dir():
            raise HilError("RUNNER_TEMP is not a safe configuration export root")
        if parent.stat().st_uid != os.getuid():
            raise HilError("RUNNER_TEMP is not owned by the HIL runner")
    return tempfile.TemporaryDirectory(prefix=prefix, dir=parent)


def flash_marker_for(configuration_backup: Path) -> Path:
    return configuration_backup.parent / "hil-flash-started"


def flash_marker_payload(
    configuration_backup: Path, release_image: Path, factory_image: Path,
    filesystem_backup: Path,
) -> bytes:
    backup = validate_config_backup(configuration_backup)
    image = validate_app_image(release_image)
    factory, layout = validate_factory_image(factory_image, image)
    filesystem = validate_filesystem_backup(filesystem_backup, layout)
    expected_filesystem = filesystem_backup_for(backup).resolve(strict=False)
    if filesystem != expected_filesystem:
        raise HilError("filesystem backup is outside the configuration recovery vault")
    payload = {
        "schema": 3,
        "backup": backup.name,
        "sha256": hashlib.sha256(backup.read_bytes()).hexdigest(),
        "release_app_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "release_app_size": image.stat().st_size,
        "release_factory_sha256": hashlib.sha256(factory.read_bytes()).hexdigest(),
        "partition_table_sha256": layout.partition_table_sha256,
        "filesystem_backup": filesystem.name,
        "filesystem_sha256": hashlib.sha256(filesystem.read_bytes()).hexdigest(),
        "filesystem_offset": layout.filesystem_offset,
        "filesystem_size": layout.filesystem_size,
    }
    return (json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n").encode()


def write_flash_marker(
    configuration_backup: Path, release_image: Path, factory_image: Path,
    filesystem_backup: Path,
) -> Path:
    backup = validate_config_backup(configuration_backup)
    image = validate_app_image(release_image)
    marker = flash_marker_for(backup)
    payload = flash_marker_payload(backup, image, factory_image, filesystem_backup)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(marker, flags, 0o600)
    except OSError as exc:
        raise HilError("HIL flash marker destination is not exclusive") from exc
    with os.fdopen(descriptor, "wb") as output:
        output.write(payload)
        output.flush()
        os.fsync(output.fileno())
    return marker


def validate_flash_marker(
    marker: Path, configuration_backup: Path, release_image: Path, factory_image: Path,
    filesystem_backup: Path,
) -> Path:
    backup = validate_config_backup(configuration_backup)
    image = validate_app_image(release_image)
    expected = flash_marker_for(backup)
    if marker.is_symlink() or not marker.is_file():
        raise HilError("valid HIL flash marker is missing")
    try:
        resolved = marker.resolve(strict=True)
        resolved_expected = expected.resolve(strict=True)
    except OSError as exc:
        raise HilError("valid HIL flash marker is missing") from exc
    if resolved != resolved_expected:
        raise HilError("valid HIL flash marker is missing")
    if stat.S_IMODE(resolved.stat().st_mode) & 0o077:
        raise HilError("HIL flash marker has unsafe permissions")
    if resolved.read_bytes() != flash_marker_payload(
        backup, image, factory_image, filesystem_backup
    ):
        raise HilError("HIL flash marker is invalid")
    return resolved


def validate_config_backup(path: Path) -> Path:
    if path.is_symlink() or not path.is_file():
        raise HilError("restorable node configuration backup is missing or unsafe")
    resolved = path.resolve()
    if stat.S_IMODE(resolved.stat().st_mode) & 0o077:
        raise HilError("restorable node configuration backup has unsafe permissions")
    payload = resolved.read_bytes()
    if not 1 <= len(payload) <= 2_000_000:
        raise HilError("restorable node configuration backup has an invalid size")
    required = (b"channel_url:", b"\nconfig:\n", b"\nmodule_config:\n", b"\nowner:")
    if any(token not in b"\n" + payload for token in required) or not re.search(
        rb"(?m)^\s+privateKey:\s+\S{16,}\s*$", payload
    ):
        raise HilError("restorable node configuration backup is incomplete")
    return resolved


def restore_config_backup(fixture: dict[str, object], backup: Path) -> None:
    """Apply a previously validated full export to the identity-bound DUT only."""
    backup = validate_config_backup(backup)
    device = resolve_role(fixture, "dut")
    expected_mac = str(device["usb_serial"])
    live = wait_for_usb_serial(expected_mac)
    cli = find_meshtastic_cli()
    result = subprocess.run(
        [cli, "--port", str(live["port"]), "--configure", str(backup)],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=300,
        check=False,
    )
    if result.returncode != 0:
        raise HilError("node configuration recovery command failed")
    time.sleep(4)
    wait_for_usb_serial(expected_mac, timeout=30)


def recover_config_if_needed(fixture: dict[str, object], backup: Path, temporary_root: Path) -> bool:
    """Restore config only after two stable exports prove a real drift."""
    backup = validate_config_backup(backup)
    expected = hashlib.sha256(stable_config_payload(backup.read_bytes())).digest()
    current = capture_config_fingerprint(fixture, temporary_root, "recovery-check")
    if current == expected:
        return False
    restore_config_backup(fixture, backup)
    repaired = capture_config_fingerprint(fixture, temporary_root, "recovery-verify")
    if repaired != expected:
        raise HilError("node configuration still differs after recovery")
    return True


def stable_config_payload(payload: bytes) -> bytes:
    """Remove Meshtastic's live position from an otherwise exact config export.

    ``--export-config`` places the latest GPS position in a top-level ``location``
    mapping. It can move while the node configuration remains unchanged, so it
    is neither restorable configuration nor a safe input to a pre/post HIL
    fingerprint. Keep every other byte exact, including identity, channel,
    config, module_config and owner fields.
    """
    retained: list[bytes] = []
    skipping_location = False
    for line in payload.splitlines(keepends=True):
        content = line.rstrip(b"\r\n")
        top_level = bool(content) and content[:1] not in (b" ", b"\t", b"#")
        if top_level:
            if content == b"location:" or content.startswith(b"location: "):
                skipping_location = True
                continue
            skipping_location = False
        if not skipping_location:
            retained.append(line)
    return b"".join(retained)


@dataclass
class Case:
    name: str
    status: str
    seconds: float
    message: str = ""
    data: dict[str, object] | None = None


class Report:
    def __init__(self, suite: str):
        self.suite = suite
        self.started = datetime.now(timezone.utc).isoformat()
        self.cases: list[Case] = []
        self._protected_values: set[str] = set()

    def protect_node_id(self, value: str | None) -> None:
        candidate = str(value or "").strip()
        if re.fullmatch(r"[0-9a-fA-F]{8}", candidate) and int(candidate, 16) != 0:
            self._protected_values.add(candidate)

    def redact(self, value: str) -> str:
        result = redact_lab_details(value)
        for protected in self._protected_values:
            result = re.sub(re.escape(protected), "<dut-node>", result, flags=re.IGNORECASE)
        return result

    def sanitize(self, value: object) -> object:
        if isinstance(value, str):
            return self.redact(value)
        if isinstance(value, dict):
            return {key: self.sanitize(item) for key, item in value.items()}
        if isinstance(value, list):
            return [self.sanitize(item) for item in value]
        if isinstance(value, tuple):
            return tuple(self.sanitize(item) for item in value)
        return value

    def check(self, name: str, fn: Callable[[], dict[str, object] | None]) -> None:
        started = time.monotonic()
        fatal: UnexpectedReboot | None = None
        try:
            data = fn()
            case = Case(name, "passed", time.monotonic() - started, data=data)
            print(f"PASS {name}", flush=True)
        except Exception as exc:  # every failed assertion belongs in the evidence report
            message = self.redact(str(exc))
            case = Case(name, "failed", time.monotonic() - started, message)
            print(f"FAIL {name}: {message}", flush=True)
            if isinstance(exc, UnexpectedReboot):
                fatal = exc
        self.cases.append(case)
        if fatal is not None:
            raise fatal

    @property
    def failed(self) -> int:
        return sum(case.status == "failed" for case in self.cases)

    def write(self, directory: Path, metadata: dict[str, object]) -> None:
        directory.mkdir(parents=True, exist_ok=True)
        payload = {
            "suite": self.suite,
            "started": self.started,
            "metadata": metadata,
            "summary": {"tests": len(self.cases), "failures": self.failed},
            "cases": [asdict(case) for case in self.cases],
        }
        report_path = directory / "report.json"
        report_path.write_text(json.dumps(self.sanitize(payload), indent=2) + "\n")
        os.chmod(report_path, 0o600)

        testsuite = ET.Element(
            "testsuite", name=self.suite, tests=str(len(self.cases)), failures=str(self.failed)
        )
        for case in self.cases:
            node = ET.SubElement(testsuite, "testcase", name=case.name, time=f"{case.seconds:.3f}")
            if case.status == "failed":
                ET.SubElement(node, "failure", message=case.message).text = case.message
        junit_path = directory / "junit.xml"
        ET.ElementTree(testsuite).write(junit_path, encoding="utf-8", xml_declaration=True)
        os.chmod(junit_path, 0o600)


def dut_evidence(device: dict[str, object]) -> dict[str, object]:
    """Return useful HIL metadata without publishing the protected fixture identity."""
    evidence: dict[str, object] = {
        "role": "dut",
        "kind": device.get("kind", "unknown"),
        "identity_verified": True,
    }
    for name in ("expected_region", "expected_tx_power"):
        if name in device:
            evidence[name] = device[name]
    return evidence


class HilSession:
    def __init__(self, port: str):
        serial, _ = require_serial()
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 115200
        self.serial.timeout = 0.25
        self.serial.write_timeout = 3
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.expected_boot: str | None = None

    def close(self) -> None:
        self.serial.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def send(self, data: bytes) -> None:
        self.serial.write(data)
        self.serial.flush()

    def line_until(self, prefix: str, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.serial.readline().decode("ascii", "replace").strip()
            # Firmware logs and HIL replies share native USB CDC. A log write
            # can race the reply and prefix it on the same physical line; the
            # protocol marker is still unambiguous, so retain the marker and
            # everything after it instead of discarding a completed command.
            marker = line.find(prefix)
            if marker >= 0:
                return line[marker:]
        raise HilError(f"timeout waiting for {prefix}")

    def exchange(self, request: bytes, prefix: str, timeout: float = 4, attempts: int = 4) -> str:
        last_error = "no response"
        for attempt in range(attempts):
            self.serial.reset_input_buffer()
            self.send(request)
            try:
                return self.line_until(prefix, timeout)
            except HilError as exc:
                last_error = str(exc)
                # Native USB CDC occasionally loses a reply while the UI thread
                # is finishing a render/config update. Immediate retries only
                # deepen that backlog and can turn a transient loss into a task
                # watchdog, so give the single-threaded endpoint one bounded tick.
                if attempt + 1 < attempts:
                    time.sleep(min(0.15 * (2**attempt), 0.6))
        raise HilError(f"{prefix} not received after {attempts} attempts: {last_error}")

    def query(self, timeout: float = 12) -> dict[str, str]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.send(b"Q")
            try:
                state = parse_fields(self.line_until("@@STATE", 1.2), "@@STATE")
                stats = parse_fields(self.line_until("@@STATS", 1.2), "@@STATS")
                last = parse_fields(self.line_until("@@LAST", 1.2), "@@LAST")
                companion = parse_fields(self.line_until("@@COMP", 1.2), "@@COMP")
                companion_heap = parse_fields(self.line_until("@@CHEAP", 1.2), "@@CHEAP")
            except HilError:
                continue
            result = {**companion_heap, **companion, **last, **stats, **state}
            boot = result.get("boot")
            if not boot:
                raise HilError(f"HIL state omitted its boot nonce: {result}")
            if self.expected_boot is None:
                self.expected_boot = boot
            elif boot != self.expected_boot:
                raise UnexpectedReboot(
                    f"device rebooted unexpectedly: boot {self.expected_boot}->{boot}, "
                    f"reset_reason={result.get('reset', 'unknown')}"
                )
            return result
        raise HilError("HIL protocol did not answer; is the HIL image running?")

    def wait_state(self, expected: dict[str, str], timeout: float = 8) -> dict[str, str]:
        deadline = time.monotonic() + timeout
        last: dict[str, str] = {}
        while time.monotonic() < deadline:
            try:
                last = self.query(timeout=2)
            except UnexpectedReboot:
                raise
            except HilError:
                time.sleep(0.2)
                continue
            if all(last.get(key) == value for key, value in expected.items()):
                return last
            time.sleep(0.2)
        raise HilError(f"state mismatch: expected {expected}, got {last}")

    def home(self) -> dict[str, str]:
        for _ in range(3):
            self.send(b"X")
            try:
                self.line_until("@@ACK home", 2)
                return self.wait_state({"mode": "chats"})
            except UnexpectedReboot:
                raise
            except HilError:
                continue
        raise HilError("timeout waiting for @@ACK home")

    def key(self, token: str, expected: dict[str, str]) -> dict[str, str]:
        raw = token.encode("latin1")
        if len(raw) != 1:
            raise HilError(f"key token must be one byte: {token!r}")
        self.send(b"K" + raw)
        return self.wait_state(expected)

    def inject(self, frame: bytes, *, expect_ok: bool = True) -> dict[str, str]:
        if not frame or len(frame) > 512:
            raise HilError(f"injected FromRadio frame must be 1..512 bytes, got {len(frame)}")
        response = parse_fields(
            self.exchange(b"I" + struct.pack("<H", len(frame)) + frame, "@@INJECT"),
            "@@INJECT",
        )
        expected = "1" if expect_ok else "0"
        if response.get("ok") != expected:
            raise HilError(f"unexpected injection result: {response}")
        return response

    def inject_companion(self, frame: bytes, *, expect_ok: bool = True) -> dict[str, str]:
        if not frame or len(frame) > 512:
            raise HilError(f"companion FromRadio frame must be 1..512 bytes, got {len(frame)}")
        response = parse_fields(
            self.exchange(b"J" + struct.pack("<H", len(frame)) + frame, "@@CINJECT"),
            "@@CINJECT",
        )
        expected = "1" if expect_ok else "0"
        if response.get("ok") != expected:
            raise HilError(f"unexpected companion injection result: {response}")
        # BLE/GATT cannot deliver back-to-back config envelopes at native USB
        # speed. Keep the fixture faster than radio while still allowing the
        # production run loop to finish the tick that acknowledged this frame.
        time.sleep(0.01)
        return response

    def fault_next_send(self, error: int) -> dict[str, str]:
        if not 0 <= error <= 255:
            raise HilError(f"send fault must fit one byte: {error}")
        return parse_fields(self.exchange(b"F" + bytes((error,)), "@@FAULT"), "@@FAULT")

    def clear(self) -> dict[str, str]:
        self.exchange(b"E", "@@ACK clear")
        return self.wait_state({"messages": "0", "reactions": "0"})

    def persist(self) -> dict[str, str]:
        result = parse_fields(self.exchange(b"P", "@@ACK persist"), "@@ACK persist")
        require_fields(result, {"ok": "1"})
        return result

    def seed_history(self) -> None:
        self.exchange(b"H", "@@ACK history")

    def test_seen_saturation(self) -> dict[str, str]:
        return parse_fields(self.exchange(b"V", "@@SEEN", timeout=12), "@@SEEN")

    def reaction_state(self) -> dict[str, str]:
        return parse_fields(self.exchange(b"R", "@@REACT"), "@@REACT")

    def memory_state(self) -> dict[str, str]:
        return parse_fields(self.exchange(b"T", "@@MEM"), "@@MEM")

    def release_companion_state(self) -> dict[str, str]:
        result = parse_fields(self.exchange(b"Y", "@@BRELEASE"), "@@BRELEASE")
        require_fields(result, {"ok": "1"})
        return result

    def load_legacy_store(self) -> dict[str, str]:
        result = parse_fields(self.exchange(b"M", "@@LEGACY"), "@@LEGACY")
        require_fields(result, {"ok": "1", "messages": "1", "reactions": "1"})
        return result

    def reboot(self) -> None:
        self.serial.reset_input_buffer()
        self.send(b"B")
        # Native USB can disappear before the final ACK reaches the host. The
        # caller proves the restart with a changed per-boot nonce, so ACK loss
        # is not treated as command failure and cannot tempt us to reboot twice.
        try:
            self.line_until("@@ACK reboot", 3)
        except HilError:
            pass
        self.expected_boot = None

    def frame(self, expected_mode: str) -> dict[str, str]:
        self.serial.reset_input_buffer()
        last_error = "no response"
        for _ in range(3):
            self.send(b"C")
            try:
                frame = parse_fields(self.line_until("@@FRAME", 4), "@@FRAME")
                if frame.get("mode") != expected_mode:
                    raise HilError(f"frame mode is {frame.get('mode')}, expected {expected_mode}")
                if frame.get("w") != "240" or frame.get("h") != "135" or frame.get("bytes") != "32400":
                    raise HilError(f"unexpected framebuffer geometry: {frame}")
                if int(frame.get("colors", "0")) < 2:
                    raise HilError(f"framebuffer looks blank: {frame}")
                return frame
            except HilError as exc:
                last_error = str(exc)
        raise HilError(f"frame capture failed after 3 attempts: {last_error}")

    def demo_frames(self, directory: Path, timeout: float) -> list[dict[str, object]]:
        directory.mkdir(parents=True, exist_ok=True)
        self.serial.reset_input_buffer()
        self.send(b"S")
        deadline = time.monotonic() + timeout
        current: tuple[str, int, int] | None = None
        rows: list[str] = []
        frames: list[dict[str, object]] = []
        transport_error: DemoCaptureTransportError | None = None
        while time.monotonic() < deadline:
            line = self.serial.readline().decode("ascii", "replace").strip()
            if line.startswith("@@SHOT "):
                parts = line.split()
                if len(parts) != 4:
                    raise HilError(f"malformed screenshot header: {line}")
                current = (parts[1], int(parts[2]), int(parts[3]))
                rows = []
            elif line == "@@END" and current:
                name, width, height = current
                if len(rows) != height:
                    if transport_error is None:
                        transport_error = DemoCaptureTransportError(
                            f"{name}: received {len(rows)}/{height} rows"
                        )
                    current = None
                    # Consume the remaining matrix through @@DONE so the
                    # firmware reaches its transactional reboot. The visual
                    # suite can then retry the whole deterministic dump once.
                    continue
                pixels = b"".join(bytes.fromhex(row) for row in rows)
                if len(pixels) != width * height or len(set(pixels)) < 2:
                    raise HilError(f"{name}: blank or truncated framebuffer")
                (directory / f"{name}.rgb332").write_bytes(pixels)
                write_png(directory / f"{name}.png", pixels, width, height)
                frame = {
                    "name": name,
                    "w": width,
                    "h": height,
                    "sha256": hashlib.sha256(pixels).hexdigest(),
                    "colors": len(set(pixels)),
                }
                frames.append(frame)
                print(f"FRAME {name} {len(frames)}/{len(EXPECTED_DEMO_FRAMES)}", flush=True)
                current = None
                # The firmware reboots immediately after the last frame. Do
                # not make a lossy final @@DONE line the only completion proof.
                if [str(item["name"]) for item in frames] == EXPECTED_DEMO_FRAMES:
                    break
            elif line == "@@DONE":
                break
            elif current:
                _, width, _ = current
                if len(line) == width * 2 and re.fullmatch(r"[0-9a-f]+", line):
                    rows.append(line)
        else:
            raise HilError("demo capture timed out")
        if transport_error is not None:
            raise transport_error
        names = [str(frame["name"]) for frame in frames]
        if names != EXPECTED_DEMO_FRAMES:
            raise HilError(f"demo frame set/order mismatch: {names}")
        stress = next(frame for frame in frames if frame["name"] == "stress")
        if int(stress["colors"]) < 4:
            raise HilError(f"stress frame lost rendered content: {stress}")
        return frames


def write_png(path: Path, pixels: bytes, width: int, height: int) -> None:
    try:
        from PIL import Image
    except ImportError:
        return
    rgb = bytearray(width * height * 3)
    for index, value in enumerate(pixels):
        rgb[index * 3:index * 3 + 3] = bytes(
            ((value >> 5) * 255 // 7, ((value >> 2) & 7) * 255 // 7, (value & 3) * 255 // 3)
        )
    image = Image.frombytes("RGB", (width, height), bytes(rgb))
    image.resize((width * 3, height * 3), Image.Resampling.NEAREST).save(path)


def require_fields(state: dict[str, str], expected: dict[str, str]) -> dict[str, object]:
    wrong = {key: (state.get(key), value) for key, value in expected.items() if state.get(key) != value}
    if wrong:
        raise HilError(f"field mismatch: {wrong}")
    return state


def require_heap_fields(
    state: dict[str, str], fields: tuple[str, ...], minimum: int = 12_000
) -> dict[str, int]:
    values = {name: int(state.get(name, "0")) for name in fields}
    if min(values.values()) < minimum:
        raise HilError(f"unsafe heap headroom (minimum {minimum}): {state}")
    return values


def require_heap_headroom(state: dict[str, str], minimum: int = 12_000) -> dict[str, int]:
    """Gate a clean phase on both current and lifetime minimum free heap."""
    return require_heap_fields(state, ("heap", "min_heap"), minimum)


def require_companion_ingress_heap(
    state: dict[str, str], minimum: int = 12_000, retained_tolerance: int = 512
) -> dict[str, int]:
    """Gate sustained companion-ingress retention, not asynchronous heap noise."""
    values = require_heap_fields(state, ("heap", "ingress_first", "ingress_pre", "ingress_last"), minimum)
    # FreeRTOS logging/native-USB work can allocate and free around an individual
    # decoder call, so summing per-call deltas and gating the largest transient
    # misclassified harmless 2 KiB task activity as a decoder leak on hardware.
    # The first/last envelope spans the complete 66-node burst and catches the
    # retained growth that matters, with a small allocator-alignment allowance.
    if values["ingress_last"] + retained_tolerance < values["ingress_first"]:
        raise HilError(f"companion ingress retained heap across the burst: {state}")
    return values


def require_scoped_heap_headroom(
    state: dict[str, str],
    current_fields: tuple[str, ...],
    watermark_before: str,
    watermark_after: str,
    minimum: int = 12_000,
) -> dict[str, int]:
    """Gate one feature without charging it for an older unrelated heap trough."""
    values = require_heap_fields(state, current_fields, minimum)
    before = int(state.get(watermark_before, "0"))
    after = int(state.get(watermark_after, "0"))
    values[watermark_before] = before
    values[watermark_after] = after
    # The ESP watermark is monotonic for the whole boot. It describes this
    # feature only when it moved while the feature was running; an unchanged
    # older low belongs to a previous display/engine phase.
    if after < before and after < minimum:
        raise HilError(f"unsafe scoped heap headroom (minimum {minimum}): {state}")
    return values


def smoke(fixture: dict[str, object], artifacts: Path) -> Report:
    dut = resolve_role(fixture, "dut")
    report = Report("cardputer-adv-hil-smoke")
    metadata = {"dut": dut_evidence(dut), "fixture_schema": fixture["schema"]}
    digests: list[str] = []
    try:
        with HilSession(str(dut["port"])) as session:
            state_box: dict[str, dict[str, str]] = {}

            def boot_case():
                state = session.wait_state({"splash": "1"}, timeout=30)
                report.protect_node_id(state.get("node"))
                state_box["boot"] = state
                require_usb_wifi_node_identity(dut, state)
                expected_radio: dict[str, str] = {}
                if "expected_region" in dut:
                    expected_radio["rf_region"] = str(REGION_CODES[str(dut["expected_region"])])
                if "expected_tx_power" in dut:
                    expected_radio["rf_power"] = str(dut["expected_tx_power"])
                require_fields(
                    state,
                    {
                        "v": "2", "backend": "onboard", "keyboard": "1", "canvas": "1", "screen": "1",
                        "rx_drop": "0", "tx_drop": "0", "radio_tx": "0",
                        **expected_radio,
                    },
                )
                require_heap_headroom(state)
                return state

            report.check("boot/protocol/peripherals", boot_case)

            def home_case():
                state = session.home()
                frame = session.frame("chats")
                digests.append(frame["fnv"])
                return {"state": state, "frame": frame}

            report.check("ui/home", home_case)

            def navigation_case():
                states = [session.key("\t", {"mode": "nodes"})]
                frame = session.frame("nodes"); digests.append(frame["fnv"])
                states.append(session.key("z", {"mode": "picker", "query": "1"}))
                frame = session.frame("picker"); digests.append(frame["fnv"])
                states.append(session.key("^", {"mode": "chats"}))
                return {"states": states}

            report.check("ui/nodes/search/back", navigation_case)

            def settings_case():
                states = [session.key("!", {"mode": "settings", "set_section": "-1", "set_sel": "0"})]
                states.append(session.key("~", {"mode": "settings", "set_section": "0"}))
                frame = session.frame("settings"); digests.append(frame["fnv"])
                states.append(session.key("^", {"mode": "settings", "set_section": "-1"}))
                return {"states": states}

            report.check("ui/settings/section", settings_case)

            def network_pages_case():
                session.key("D", {"mode": "settings", "set_sel": "1"})
                session.key("D", {"mode": "settings", "set_sel": "2"})
                wifi = session.key("~", {"mode": "netpage", "net_page": "0"})
                frame_wifi = session.frame("netpage"); digests.append(frame_wifi["fnv"])
                session.key("^", {"mode": "settings", "set_sel": "2"})
                session.key("D", {"mode": "settings", "set_sel": "3"})
                mqtt = session.key("~", {"mode": "netpage", "net_page": "1"})
                frame_mqtt = session.frame("netpage"); digests.append(frame_mqtt["fnv"])
                session.key("^", {"mode": "settings", "set_sel": "3"})
                return {"wifi": wifi, "mqtt": mqtt, "frames": [frame_wifi, frame_mqtt]}

            report.check("ui/wifi/mqtt/read-only", network_pages_case)

            def about_case():
                session.key("!", {"mode": "settings", "set_sel": "0"})
                for selected in range(1, 7):
                    session.key("D", {"mode": "settings", "set_sel": str(selected)})
                state = session.key("~", {"mode": "settings", "set_section": "3"})
                frame = session.frame("settings"); digests.append(frame["fnv"])
                session.key("^", {"mode": "settings", "set_section": "-1"})
                session.home()
                return {"state": state, "frame": frame}

            report.check("ui/about", about_case)

            def distinct_frames_case():
                unique = len(set(digests))
                if unique < 5:
                    raise HilError(f"only {unique}/{len(digests)} distinct framebuffer digests")
                return {"digests": digests, "unique": unique}

            report.check("display/distinct-renders", distinct_frames_case)
    except Exception as exc:
        message = report.redact(str(exc))
        if isinstance(exc, UnexpectedReboot):
            print(f"ABORT session after unexpected reboot: {message}", flush=True)
        else:
            report.cases.append(Case("session", "failed", 0, message))
            print(f"FAIL session: {message}", flush=True)
    report.write(artifacts, metadata)
    return report


def message_flow(fixture: dict[str, object], artifacts: Path) -> Report:
    """Exercise the real FromRadio handler without radiating a mesh packet."""
    dut = resolve_role(fixture, "dut")
    report = Report("cardputer-adv-hil-message-flow")
    metadata = {
        "dut": dut_evidence(dut),
        "fixture_schema": fixture["schema"],
        "ingress": "protobuf/FromRadio",
    }
    source = 0x000BEEF1
    colliding_source = 0x000BEEF2
    broadcast = 0xFFFFFFFF
    session: HilSession | None = None
    expected_after_persist: dict[str, str] = {}
    boot_before_persist = ""
    try:
        session = HilSession(str(dut["port"]))

        def clean_start_case():
            state = session.clear()
            report.protect_node_id(state.get("node"))
            require_fields(
                state,
                {"v": "2", "messages": "0", "reactions": "0", "incoming": "0", "buffers": "1"},
            )
            if int(state.get("node", "0"), 16) == 0:
                raise HilError(f"device did not expose its node id: {state}")
            # clear also initializes the on-demand companion queues/table. This
            # catches a buffer allocation that succeeds but leaves too little
            # heap for the later BLE/GATT task.
            require_heap_headroom(state)
            return state

        report.check("fixtures/isolated-clean-start", clean_start_case)
        initial = session.query()
        report.protect_node_id(initial.get("node"))
        me = int(initial.get("node", "0"), 16)

        def legacy_store_case():
            loaded = session.load_legacy_store()
            state = session.wait_state({"messages": "1", "reactions": "1"})
            reaction = require_fields(
                session.reaction_state(),
                {"react_exact": "0", "react_ambiguous": "1", "react_last_target": "00000000"},
            )
            cleared = session.clear()
            require_fields(cleared, {"messages": "0", "reactions": "0"})
            return {"load": loaded, "state": state, "reaction": reaction, "cleanup": cleared}

        def seen_saturation_case():
            result = session.test_seen_saturation()
            require_fields(result, {"v": "1", "ok": "1", "records": "2048", "found": "1"})
            if int(result.get("active", "0")) < 1:
                raise HilError(f"fresh sender was stored but not counted active: {result}")
            return result

        report.check("storage/saturated-seen-ledger-admits-fresh-sender", seen_saturation_case)

        def per_sender_packet_id_case():
            shared_id = 0x0C0111DE
            session.inject(make_text_frame(source, me, shared_id, "first sender"))
            session.inject(make_text_frame(colliding_source, me, shared_id, "second sender"))
            collided = require_fields(
                session.wait_state({"messages": "2", "incoming": "2"}),
                {"id": f"{shared_id:08x}", "from": f"{colliding_source:08x}"},
            )
            cleared = session.clear()
            require_fields(cleared, {"messages": "0", "incoming": "0", "reactions": "0"})
            return {"collision_retained": collided, "isolated_cleanup": cleared}

        report.check("ingress/packet-id-deduplication-is-per-sender", per_sender_packet_id_case)

        def incoming_case():
            text = "Привет из FromRadio 👋"
            session.inject(make_text_frame(source, me, 0x1001, text, rx_time=1_750_000_100))
            state = session.wait_state({"messages": "1", "unread": "1", "incoming": "1"})
            return require_fields(
                state,
                {
                    "id": "00001001", "from": f"{source:08x}", "to": f"{me:08x}",
                    "status": "0", "reply": "00000000", "text_len": str(len(text.encode("utf-8"))),
                    "text_fnv": fnv1a32(text.encode("utf-8")),
                },
            )

        report.check("ingress/incoming-unicode-dm", incoming_case)

        def duplicate_case():
            duplicate = session.inject(make_text_frame(source, me, 0x1001, "duplicate must be ignored"))
            require_fields(duplicate, {"changed": "0"})
            return require_fields(session.query(), {"messages": "1", "incoming": "1"})

        report.check("ingress/packet-id-deduplication", duplicate_case)

        def unrelated_payloads_case():
            before = session.query()
            responses = {
                "non_packet": session.inject(make_companion_my_info(0x00A11CE1)),
                "encrypted": session.inject(
                    make_encrypted_frame(source, me, 0x1007, b"not decoded application data")
                ),
                "other_port": session.inject(
                    make_fromradio_frame(source, me, 0x1008, b"position fixture", portnum=3)
                ),
            }
            for response in responses.values():
                require_fields(response, {"changed": "0"})
            after = session.query()
            unchanged = {
                key: before[key]
                for key in (
                    "messages", "hist", "reactions", "unread", "incoming",
                    "sending", "delivered", "failed",
                )
            }
            require_fields(after, unchanged)
            require_fields(responses["non_packet"], {"variant": "3"})
            require_fields(responses["encrypted"], {"variant": "2"})
            require_fields(responses["other_port"], {"variant": "2"})
            return {"before": unchanged, "after": after, "responses": responses}

        report.check("ingress/unrelated-and-encrypted-payloads-ignored", unrelated_payloads_case)

        def reply_case():
            session.inject(
                make_text_frame(source, me, 0x1002, "reply", reply_id=0x1001, rx_time=1_750_000_101)
            )
            return require_fields(
                session.wait_state({"messages": "2", "incoming": "2"}),
                {"id": "00001002", "reply": "00001001"},
            )

        report.check("ingress/reply-link", reply_case)

        def reaction_case():
            session.inject(make_text_frame(source, me, 0x1003, "👍", reply_id=0x1001, emoji=1))
            return require_fields(session.wait_state({"messages": "2", "reactions": "1"}), {"incoming": "2"})

        report.check("ingress/reaction-without-bubble", reaction_case)

        def phone_reaction_case():
            session.inject(make_text_frame(me, source, 0x1006, "❤️", reply_id=0x1001, emoji=1))
            return require_fields(
                session.wait_state({"messages": "2", "reactions": "2"}),
                {"incoming": "2"},
            )

        report.check("ingress/phone-self-reaction-mirrored", phone_reaction_case)

        def broadcast_case():
            session.inject(
                make_text_frame(
                    source,
                    broadcast,
                    0x1004,
                    "private fixture channel",
                    channel=2,
                    rx_time=1_750_000_050,
                )
            )
            return require_fields(
                session.wait_state({"messages": "3", "incoming": "3"}),
                {"id": "00001004", "to": "ffffffff", "ch": "2"},
            )

        report.check("ingress/channel-broadcast", broadcast_case)

        def ack_case():
            # Drive the actual chat/compose/send UI. The HIL RadioLib interface
            # releases the fully constructed packet instead of radiating it;
            # the routing result still enters through the production decoder.
            session.home()
            session.key("D", {"mode": "chats"})  # channel is newest; DM is the second conversation
            opened = session.key("~", {"mode": "node", "channel": "-1"})
            require_fields(opened, {"selected": f"{source:08x}"})
            session.key("o", {"mode": "compose", "compose": "1"})
            session.key("k", {"mode": "compose", "compose": "2"})
            session.fault_next_send(32)  # ERRNO_UNKNOWN: deterministic local TX FIFO rejection
            rejected = session.key(
                "~", {
                    "mode": "compose", "compose": "2", "send_error": "2",
                    "send_errno": "32", "messages": "3",
                }
            )
            state = session.key(
                "~", {
                    "mode": "node", "messages": "4", "sending": "1",
                    "send_error": "0", "send_errno": "34",
                }
            )
            request_id = int(state["id"], 16)
            if request_id == 0:
                raise HilError(f"UI send did not allocate a packet id: {state}")

            # Valid route-discovery control traffic, a routing result without a
            # request id, and malformed routing payload must all leave the
            # pending bubble pending. Only error_reason for our request is ACK.
            session.inject(make_routing_frame(source, me, request_id, 0, variant=1))
            route_request = require_fields(
                session.query(), {"sending": "1", "delivered": "0", "failed": "0"}
            )
            session.inject(make_routing_frame(source, me, 0, 0))
            no_request_id = require_fields(
                session.query(), {"sending": "1", "delivered": "0", "failed": "0"}
            )
            session.inject(
                make_fromradio_frame(source, me, 0, b"\xff", portnum=5, request_id=request_id)
            )
            malformed_routing = require_fields(
                session.query(), {"sending": "1", "delivered": "0", "failed": "0"}
            )
            session.inject(make_queue_status_frame(32, request_id))
            queue_rejected = require_fields(
                session.wait_state({"sending": "0", "delivered": "0", "failed": "1"}),
                {"err": "32"},
            )
            retried = session.key("~", {"mode": "node", "sending": "1", "failed": "0"})
            retry_id = int(retried["id"], 16)
            if retry_id == request_id:
                raise HilError(f"retry reused rejected packet id: {retried}")
            session.inject(make_routing_frame(source, me, retry_id, 0))
            result = require_fields(
                session.wait_state({"sending": "0", "delivered": "1"}), {"failed": "0", "radio_tx": "0"}
            )
            result["request_id"] = f"{retry_id:08x}"
            result["draft_preserved"] = rejected
            result["queue_rejected"] = queue_rejected
            result["ignored_before_ack"] = {
                "route_request": route_request,
                "missing_request_id": no_request_id,
                "malformed_routing": malformed_routing,
            }
            return result

        report.check("ui/send-routing-filters-and-positive-ack", ack_case)

        def nak_case():
            # A failed reply retains its local link and must put the same
            # reply_id back on the wire when ENTER retries it in place.
            session.inject(
                make_text_frame(me, source, 0x2002, "outgoing failed reply", reply_id=0x1001)
            )
            require_fields(session.wait_state({"messages": "5", "sending": "1"}), {"status": "1"})
            session.inject(make_routing_frame(source, me, 0x2002, 5))  # MAX_RETRANSMIT
            require_fields(
                session.wait_state({"sending": "0", "failed": "1"}),
                {"delivered": "1", "reply": "00001001"},
            )
            retried = require_fields(
                session.key("~", {"sending": "1", "failed": "0"}),
                {"tx_reply": "00001001", "reply": "00001001"},
            )
            retry_id = int(retried["id"], 16)
            if retry_id == 0x2002:
                raise HilError(f"reply retry reused rejected packet id: {retried}")
            session.inject(make_routing_frame(source, me, retry_id, 5))
            failed_again = require_fields(
                session.wait_state({"sending": "0", "failed": "1"}),
                {"delivered": "1", "reply": "00001001", "tx_reply": "00001001"},
            )
            return {"retried": retried, "failed_again": failed_again}

        report.check("ui/failed-reply-resend-preserves-wire-link", nak_case)

        def malformed_utf8_case():
            session.inject(make_text_frame(source, me, 0x1005, b"A\x80B"))
            sanitized = b"A?B"
            return require_fields(
                session.wait_state({"messages": "6", "incoming": "4"}),
                {"id": "00001005", "text_len": "3", "text_fnv": fnv1a32(sanitized)},
            )

        report.check("ingress/malformed-utf8-sanitized", malformed_utf8_case)

        def malformed_protobuf_case():
            before = session.query()
            response = session.inject(b"\x12\x01\xff", expect_ok=False)
            after = session.query()
            require_fields(after, {"messages": before["messages"], "reactions": before["reactions"]})
            return {"response": response, "before": before, "after": after}

        report.check("ingress/malformed-protobuf-rejected", malformed_protobuf_case)

        companion_me = 0x00A11CE1
        # Entry 63 remains in the bounded 64-slot mirror after entries 65 and
        # 66 evict the two oldest non-self fixtures.
        companion_peer = 0x00B0003F

        def companion_identity_case():
            session.inject_companion(make_companion_my_info(companion_me))
            return require_fields(
                session.query(), {"my": f"{companion_me:08x}", "nodes": "0", "seen": "0"}
            )

        report.check("companion/my-info", companion_identity_case)

        def companion_nodes_case():
            session.inject_companion(
                make_companion_node_info(companion_me, battery=87, hops=0, last_heard=1_700_000_000)
            )
            for index in range(1, 66):
                session.inject_companion(
                    make_companion_node_info(
                        0x00B00000 + index,
                        long_name=f"Fixture {index:02d}",
                        short_name=f"{index % 100:02d}",
                        hops=2,
                        last_heard=1_700_000_000 + index,
                        public_key=bytes(range(32)),
                    )
                )
            state = require_fields(
                session.query(),
                {
                    "nodes": "64", "seen": "66", "batt": "87",
                    "last": f"{companion_peer:08x}", "hops": "2", "key": "1",
                    "name_fnv": fnv1a32(b"Fixture 63"),
                },
            )
            require_companion_ingress_heap(state)
            return state

        report.check("companion/bounded-66-node-db-mirror", companion_nodes_case)

        def companion_config_case():
            channel_name = "ADV fixture"
            session.inject_companion(make_companion_channel(0, channel_name, role=1))
            session.inject_companion(make_companion_lora_config(preset=4, region=1, tx_power=26))
            session.inject_companion(make_companion_device_config(role=2))
            session.inject_companion(make_companion_config_complete(0x1234))
            return require_fields(
                session.query(),
                {
                    "done": "1", "channel_present": "1",
                    "channel_fnv": fnv1a32(channel_name.encode("utf-8")),
                    "lora": "1", "preset": "4", "region": "1", "tx": "26",
                    "device": "1", "role": "2",
                },
            )

        report.check("companion/channel-config-complete", companion_config_case)

        companion_resync_peer = 0x00C00001

        def companion_resync_case():
            # PhoneAPI may start a fresh config snapshot while the BLE link is
            # still alive. The second my_info must replace the old snapshot,
            # not merge it with stale nodes, channels or radio settings.
            session.inject_companion(make_companion_my_info(companion_me))
            require_fields(
                session.query(),
                {
                    "my": f"{companion_me:08x}", "nodes": "0", "seen": "0",
                    "done": "0", "batt": "-1", "channel_present": "0",
                    "lora": "0", "preset": "0", "device": "0",
                },
            )
            session.inject_companion(
                make_companion_node_info(
                    companion_me, long_name="Fixture radio", short_name="FR",
                    battery=88, hops=0, last_heard=1_700_001_000,
                )
            )
            resync_name = "Resync peer"
            session.inject_companion(
                make_companion_node_info(
                    companion_resync_peer, long_name=resync_name, short_name="RS",
                    hops=1, last_heard=1_700_001_001,
                )
            )
            channel_name = "ADV resync"
            session.inject_companion(make_companion_channel(0, channel_name, role=1))
            session.inject_companion(make_companion_lora_config(preset=3, region=1, tx_power=26))
            session.inject_companion(make_companion_device_config(role=1))
            session.inject_companion(make_companion_config_complete(0x1235))
            return require_fields(
                session.query(),
                {
                    "nodes": "2", "seen": "2", "done": "1", "batt": "88",
                    "last": f"{companion_resync_peer:08x}", "hops": "1",
                    "name_fnv": fnv1a32(resync_name.encode("utf-8")),
                    "channel_present": "1",
                    "channel_fnv": fnv1a32(channel_name.encode("utf-8")),
                    "lora": "1", "preset": "3", "region": "1", "tx": "26",
                    "device": "1", "role": "1",
                },
            )

        report.check("companion/repeated-config-stream-replaces-snapshot", companion_resync_case)

        def companion_packet_case():
            text = "companion queue ingress"
            session.inject_companion(
                make_text_frame(companion_resync_peer, me, 0x1010, text, rx_time=1_750_000_102)
            )
            state = session.wait_state({"messages": "7", "incoming": "5"})
            return require_fields(
                state,
                {
                    "id": "00001010", "from": f"{companion_resync_peer:08x}", "to": f"{me:08x}",
                    "text_len": str(len(text)), "text_fnv": fnv1a32(text.encode("utf-8")),
                    "rx_drop": "0",
                },
            )

        report.check("companion/packet-queue-to-ui", companion_packet_case)

        def release_companion_phase_case():
            before = session.query()
            require_fields(before, {"buffers": "1", "nodes": "2"})
            released = session.release_companion_state()
            if int(released.get("freed", "0")) < 6 * 1024:
                raise HilError(f"companion-only arena was not released: {released}")
            after = session.wait_state({"buffers": "0", "nodes": "0", "seen": "0"})
            return {"before_heap": before["heap"], "release": released, "after": after}

        report.check("fixtures/release-companion-state-before-onboard-storage", release_companion_phase_case)

        def pending_before_overflow_case():
            session.inject(make_text_frame(me, source, 0x2003, "late ack survives overflow"))
            return require_fields(
                session.wait_state({"messages": "8", "sending": "1"}),
                {"id": "00002003", "status": "1"},
            )

        report.check("storage/pending-message-before-overflow", pending_before_overflow_case)

        def history_overflow_case():
            # Eight existing messages plus 48 deterministic ghost DMs overflow
            # the 32-slot live ring by exactly 24 records. The one pending send
            # must remain live so a delayed ACK can still update it. Feed every
            # record through the real protobuf ingress on its own UI tick: the
            # old dev shortcut appended all 48 synchronously, creating a
            # filesystem-allocation burst that no six-frame radio/BLE queue can
            # produce and making the heap watermark describe the harness rather
            # than the firmware's reachable runtime behavior.
            checkpoints = []
            state: dict[str, str] = {}
            for index in range(1, 49):
                session.inject(
                    make_text_frame(
                        source,
                        me,
                        7000 + index,
                        f"hist test {index}/48",
                        rx_time=1_750_001_000 + index,
                    )
                )
                if index % 8 == 0:
                    checkpoint = session.memory_state()
                    checkpoints.append(
                        {
                            "after": index,
                            "heap": int(checkpoint.get("heap", "0")),
                            "min_heap": int(checkpoint.get("min_heap", "0")),
                            "hist": int(checkpoint.get("hist", "-1")),
                        }
                    )
                    require_heap_fields(checkpoint, ("heap",))
                    state = checkpoint
            state = require_fields(
                state,
                {"messages": "32", "hist": "24", "sending": "1", "incoming": "31"},
            )
            memory = state
            try:
                if int(memory.get("saves", "0")) < 1:
                    raise HilError(f"storage workload did not persist its live ring: {memory}")
                require_scoped_heap_headroom(
                    memory,
                    ("heap", "save_before", "save_after"),
                    "min_before",
                    "min_after",
                )
            except HilError as exc:
                raise HilError(f"{exc}; overflow checkpoints={checkpoints}; persistence={memory}") from exc
            return {"state": state, "checkpoints": checkpoints, "persistence": memory}

        report.check("storage/live-ring-to-flash-archive", history_overflow_case)

        def delayed_ack_after_overflow_case():
            session.inject(make_routing_frame(source, me, 0x2003, 0))
            return require_fields(
                session.wait_state({"sending": "0", "delivered": "1"}),
                {"hist": "24", "incoming": "31"},
            )

        report.check("storage/delayed-ack-after-overflow", delayed_ack_after_overflow_case)

        def delete_other_conversation_keeps_archived_reactions_case():
            # The reaction targets from the first cases now live in the archive.
            # Add a new live channel whose packet ID intentionally collides with
            # that archived DM. The same local user then reacts to both targets:
            # packet IDs are only per-sender unique, so these must be two distinct
            # reactions even though (reply_id, reactor) is identical. Erasing the
            # channel must remove only its scoped reaction and keep the DM pair.
            channel_source = source + 1
            session.inject(
                make_text_frame(
                    channel_source,
                    broadcast,
                    0x1001,
                    "temporary conversation",
                    channel=3,
                    rx_time=1_750_002_000,
                )
            )
            session.inject(
                make_text_frame(
                    me,
                    broadcast,
                    0x1009,
                    "🔥",
                    channel=3,
                    reply_id=0x1001,
                    emoji=1,
                )
            )
            before = session.wait_state({"messages": "32", "hist": "25", "reactions": "3"})
            before_reactions = require_fields(
                session.reaction_state(),
                {
                    "react_exact": "3", "react_ambiguous": "0",
                    "react_last_target": f"{channel_source:08x}",
                },
            )
            session.home()
            session.key("\x08", {"mode": "chats"})  # Del: arm deletion of the newest channel
            after = session.key("~", {"mode": "chats", "messages": "31", "reactions": "2"})
            after_reactions = require_fields(
                session.reaction_state(),
                {
                    "react_exact": "2", "react_ambiguous": "0",
                    "react_last_target": f"{source:08x}",
                },
            )
            return {
                "before": before, "before_reactions": before_reactions,
                "after": after, "after_reactions": after_reactions,
            }

        report.check(
            "storage/reactions-scoped-across-id-collision-and-delete",
            delete_other_conversation_keeps_archived_reactions_case,
        )

        def render_case():
            session.home()
            frame = session.frame("chats")
            if frame.get("fnv") == "00000000":
                raise HilError(f"message list produced an invalid frame digest: {frame}")
            return frame

        report.check("display/injected-conversation-render", render_case)

        def ui_preferences_case():
            # Exercise the real settings/picker/key path. HIL builds persist
            # these preferences in /advui_hil_ui.bin, never the production file.
            session.home()
            session.key("!", {"mode": "settings", "set_section": "-1", "set_sel": "0"})
            for selected in range(1, 5):
                session.key("D", {"mode": "settings", "set_sel": str(selected)})
            session.key("~", {"mode": "settings", "set_section": "2", "set_sel": "0"})

            # Sort: force the picker to its first item, then choose Last heard.
            session.key("D", {"mode": "settings", "set_sel": "1"})
            session.key("D", {"mode": "settings", "set_sel": "2"})
            session.key("~", {"mode": "picklist"})
            for _ in range(4):
                session.key("U", {"mode": "picklist"})
            session.key("D", {"mode": "picklist"})
            sorted_state = session.key("~", {"mode": "settings", "sort": "1"})

            # Names: likewise choose Short independently of stale HIL state.
            session.key("D", {"mode": "settings", "set_sel": "3"})
            session.key("~", {"mode": "picklist"})
            for _ in range(2):
                session.key("U", {"mode": "picklist"})
            session.key("D", {"mode": "picklist"})
            named_state = session.key("~", {"mode": "settings", "names": "1"})
            session.home()
            return {"sort": sorted_state, "names": named_state}

        report.check("settings/ui-preferences-atomic-save", ui_preferences_case)

        def persist_case():
            nonlocal expected_after_persist, boot_before_persist
            state = session.query()
            boot_before_persist = state["boot"]
            expected_after_persist = {
                key: state[key]
                for key in (
                    "messages", "hist", "reactions", "incoming", "delivered", "failed", "sort", "names"
                )
            }
            ack = session.persist()
            require_fields(ack, {"ok": "1", "messages": state["messages"]})
            session.reboot()
            return {"persisted": expected_after_persist, "ack": ack}

        report.check("storage/persist-before-reboot", persist_case)
        session.close()
        session = None
        time.sleep(3)

        def reboot_case():
            nonlocal session
            live = wait_for_usb_serial(str(dut["usb_serial"]), timeout=20)
            session = HilSession(str(live["port"]))
            restored = session.wait_state({"backend": "onboard"}, timeout=30)
            require_fields(restored, expected_after_persist)
            restored_reactions = require_fields(
                session.reaction_state(), {"react_exact": "2", "react_ambiguous": "0"}
            )
            if restored.get("boot") == boot_before_persist:
                raise HilError(f"reboot was not proven: boot nonce stayed {boot_before_persist}")
            return {"state": restored, "reactions": restored_reactions}

        report.check("storage/reboot-recovery", reboot_case)

        # This destructive migration fixture deliberately replaces the whole
        # isolated HIL message store and has a short-lived allocation burst.
        # Run it only after every steady-state/reboot assertion so its all-time
        # heap watermark cannot contaminate unrelated release gates.
        report.check("storage/legacy-avs4-reaction-migrates-conservatively", legacy_store_case)
    except Exception as exc:
        message = report.redact(str(exc))
        if isinstance(exc, UnexpectedReboot):
            print(f"ABORT session after unexpected reboot: {message}", flush=True)
        else:
            report.cases.append(Case("session", "failed", 0, message))
            print(f"FAIL session: {message}", flush=True)
    finally:
        if session is not None:
            try:
                # A reboot is fatal to the scenario, but the new boot still
                # needs its isolated HIL files removed before production is
                # restored. Accept it only for this cleanup transaction.
                session.expected_boot = None
                session.clear()
            except Exception as exc:
                message = report.redact(str(exc))
                report.cases.append(Case("fixtures/final-cleanup", "failed", 0, message))
                print(f"FAIL fixtures/final-cleanup: {message}", flush=True)
            session.close()
    report.write(artifacts, metadata)
    return report


def visual(fixture: dict[str, object], artifacts: Path, timeout: float) -> Report:
    dut = resolve_role(fixture, "dut")
    report = Report("cardputer-adv-hil-visual")
    metadata = {"dut": dut_evidence(dut), "fixture_schema": fixture["schema"]}
    frame_data: list[dict[str, object]] = []

    def capture_case():
        nonlocal frame_data
        live = dut
        restored: dict[str, str] = {}
        for attempt in range(2):
            capture_error: DemoCaptureTransportError | None = None
            with HilSession(str(live["port"])) as session:
                state = session.query()
                report.protect_node_id(state.get("node"))
                require_fields(state, {"backend": "onboard"})
                boot_before = state["boot"]
                try:
                    frame_data = session.demo_frames(artifacts / "frames", timeout)
                except DemoCaptureTransportError as exc:
                    capture_error = exc
            time.sleep(3)  # runDemoDump ends with a transactional reboot
            live = wait_for_usb_serial(str(dut["usb_serial"]), timeout=20)
            with HilSession(str(live["port"])) as session:
                restored = session.wait_state({"splash": "1", "backend": "onboard"}, timeout=30)
            if restored.get("boot") == boot_before:
                raise HilError(f"visual reboot was not proven: boot nonce stayed {boot_before}")
            if capture_error is None:
                break
            if attempt == 1:
                raise capture_error
            print(f"RETRY visual matrix after USB row loss: {capture_error}", flush=True)

        unique = len({str(frame["sha256"]) for frame in frame_data})
        if unique < 20:
            raise HilError(f"only {unique}/{len(frame_data)} visually distinct demo frames")
        hashes_path = artifacts / "visual-hashes.json"
        hashes_path.write_text(json.dumps(frame_data, indent=2) + "\n")
        os.chmod(hashes_path, 0o600)
        return {"frames": len(frame_data), "unique": unique, "post_reboot": restored}

    report.check("visual/demo-matrix", capture_case)
    report.write(artifacts, metadata)
    return report


def full_run(
    fixture: dict[str, object], artifacts: Path, timeout: float, skip_build: bool = False,
    release_image: Path | None = None, factory_image: Path | None = None,
    config_backup: Path | None = None, production_wifi: bool = False,
) -> int:
    """Run the complete safe suite and always put production firmware back."""
    if production_wifi:
        require_production_wifi_config(fixture)
        if release_image is None:
            raise HilError("production WiFi soak requires an exact --release-image")
    if release_image is not None and factory_image is None:
        raise HilError("an exact --release-image requires its matching --factory-image")
    exact_release = validate_app_image(release_image) if release_image is not None else None
    exact_factory = (
        validate_factory_image(factory_image, exact_release)[0]
        if factory_image is not None and exact_release is not None else None
    )
    if not skip_build:
        build(release=False)
        if exact_release is None:
            build(release=True)  # prove and stage recovery before touching hardware

    flash_attempted = False
    restored = False
    failures = 0
    failure_type: str | None = None
    restore_failure_type: str | None = None
    configuration_preserved: bool | None = None
    configuration_restored = False
    configuration_check_failure_type: str | None = None
    production_wifi_evidence: dict[str, object] | None = None
    production_wifi_before: ProductionWifiSnapshot | None = None
    production_wifi_failure_type: str | None = None
    production_wifi_skipped = False
    configuration_backup = prepare_config_backup_destination(artifacts, config_backup)
    filesystem_backup = filesystem_backup_for(configuration_backup)
    filesystem_captured = False
    filesystem_restored = False
    with configuration_temporary_directory("meshtastic-adv-hil-config-") as temporary:
        configuration_before: bytes | None = None
        staged_release: Path | None = None
        staged_factory: Path | None = None
        flash_layout: FlashLayout | None = None
        release_sha256: str | None = None
        factory_sha256: str | None = None
        try:
            # A build or another local task may clean firmware/.pio while the
            # several-minute physical suite is running. Recovery must not
            # depend on that mutable directory after the HIL app is flashed.
            staged_release, release_sha256 = stage_release_image(exact_release, Path(temporary))
            staged_factory, flash_layout, factory_sha256 = stage_factory_image(
                exact_factory, staged_release, Path(temporary)
            )
            if production_wifi:
                print("validating production WiFi NodeDB persistence floor", flush=True)
                production_wifi_before = production_wifi_ready_dump(
                    fixture,
                    enforce_configured_minimum=True,
                )
            try:
                configuration_before = capture_config_fingerprint(
                    fixture, Path(temporary), "before", preserve=configuration_backup
                )
            except Exception as exc:
                configuration_check_failure_type = type(exc).__name__
                raise

            if staged_release is None:
                raise HilError("production recovery image was not staged")
            if staged_factory is None or flash_layout is None:
                raise HilError("production partition layout was not staged")
            capture_filesystem_backup(
                fixture, filesystem_backup, flash_layout
            )
            filesystem_captured = True
            write_flash_marker(
                configuration_backup, staged_release, staged_factory, filesystem_backup
            )
            flash_attempted = True
            flash(fixture, release=False)
            before = smoke(fixture, artifacts / "smoke-before")
            failures += before.failed
            messages = message_flow(fixture, artifacts / "message-flow")
            failures += messages.failed
            matrix = visual(fixture, artifacts / "visual", timeout)
            failures += matrix.failed
            after = smoke(fixture, artifacts / "smoke-after")
            failures += after.failed
        except Exception as exc:
            # Keep evidence useful without copying exception text that could contain
            # fixture paths, identities or other lab-local details.
            failure_type = type(exc).__name__
            raise
        finally:
            restore_error: Exception | None = None
            configuration_error: Exception | None = None
            production_wifi_error: Exception | None = None
            if flash_attempted:
                try:
                    print("restoring production firmware", flush=True)
                    if staged_release is None:
                        raise HilError("production recovery image was not staged")
                    if staged_factory is None or not filesystem_captured:
                        raise HilError("production filesystem recovery image was not staged")
                    restore_production_state(
                        fixture, staged_release, staged_factory, filesystem_backup
                    )
                    restored = True
                    filesystem_restored = True
                except Exception as exc:
                    restore_failure_type = type(exc).__name__
                    restore_error = exc

                if restored and production_wifi:
                    if failure_type is None and failures == 0:
                        try:
                            print("soaking restored production WiFi PhoneAPI", flush=True)
                            production_wifi_evidence = production_wifi_soak(
                                fixture,
                            )
                        except Exception as exc:
                            production_wifi_failure_type = type(exc).__name__
                            production_wifi_error = exc
                    else:
                        production_wifi_skipped = True

                if restored:
                    try:
                        configuration_after = capture_config_fingerprint(
                            fixture, Path(temporary), "after"
                        )
                        configuration_preserved = configuration_before == configuration_after
                        if not configuration_preserved:
                            print("restoring verified node configuration backup", flush=True)
                            restore_config_backup(fixture, configuration_backup)
                            repaired = capture_config_fingerprint(
                                fixture, Path(temporary), "after-recovery"
                            )
                            configuration_restored = configuration_before == repaired
                            if not configuration_restored:
                                raise HilError("node configuration changed across HIL and recovery failed")
                            # Recovery protects the lab device; it must not turn a
                            # destructive firmware regression into a green release.
                            raise HilError("node configuration changed across HIL; verified backup restored")
                    except Exception as exc:
                        configuration_check_failure_type = type(exc).__name__
                        configuration_error = exc

            artifacts.mkdir(parents=True, exist_ok=True)
            summary_path = artifacts / "summary.json"
            summary = {
                "failures": failures,
                "failure_type": failure_type,
                "hil_flash_attempted": flash_attempted,
                "production_restored": restored,
                "filesystem_captured": filesystem_captured,
                "filesystem_restored": filesystem_restored,
                "restore_failure_type": restore_failure_type,
                "configuration_preserved": configuration_preserved,
                "configuration_restored": configuration_restored,
                "configuration_check_failure_type": configuration_check_failure_type,
                "production_wifi_requested": production_wifi,
                "production_wifi_validated": production_wifi_evidence is not None,
                "production_wifi_skipped_after_hil_failure": production_wifi_skipped,
                "production_wifi_failure_type": production_wifi_failure_type,
            }
            if production_wifi_before is not None:
                summary["production_wifi_before"] = {
                    "node_count": production_wifi_before.node_count,
                    "reboot_count": production_wifi_before.reboot_count,
                }
            if production_wifi_evidence is not None:
                summary["production_wifi"] = production_wifi_evidence
            if release_sha256 is not None:
                summary["release_image_sha256"] = release_sha256
            if factory_sha256 is not None:
                summary["release_factory_sha256"] = factory_sha256
            if flash_layout is not None:
                summary["partition_table_sha256"] = flash_layout.partition_table_sha256
                summary["filesystem_size"] = flash_layout.filesystem_size
            summary_path.write_text(json.dumps(summary, indent=2) + "\n")
            os.chmod(summary_path, 0o600)

            # Restoration and configuration integrity failures are safety failures;
            # surface them even when a behavioral assertion had already failed.
            if restore_error is not None:
                raise restore_error
            if configuration_error is not None:
                raise configuration_error
            if production_wifi_error is not None:
                raise production_wifi_error
    return failures


def default_artifacts(suite: str) -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return ROOT / "hil-artifacts" / f"{stamp}-{suite}"


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inv = commands.add_parser("inventory", help="list stable Espressif USB identities")
    inv.add_argument("--json", action="store_true")

    wifi_inv = commands.add_parser("wifi-inventory", help="probe only configured WiFi PhoneAPI endpoints")
    wifi_inv.add_argument("--fixture", type=Path, default=ROOT / "hil/fixture.local.json")
    wifi_inv.add_argument("--json", action="store_true")
    wifi_inv.add_argument("--timeout", type=float, default=1)

    init = commands.add_parser("init", help="write an ignored local fixture file")
    init.add_argument("--dut", required=True, help="Cardputer USB serial/MAC")
    init.add_argument("--protected-usb", action="append", default=[], help="USB serial/MAC that HIL must never open")
    init.add_argument("--wifi-peer", action="append", default=[], metavar="[NAME=]HOST",
                      help="read-only Meshtastic TCP endpoint; repeat for each node")
    init.add_argument("--output", type=Path, default=ROOT / "hil/fixture.local.json")

    commands.add_parser("build", help="build the HIL firmware")
    restore_build = commands.add_parser("build-release", help="build the production firmware")
    restore_build.set_defaults(release=True)

    for name, help_text in (("flash", "flash HIL firmware to the fixture DUT"),
                            ("restore", "flash production firmware back to the DUT")):
        cmd = commands.add_parser(name, help=help_text)
        cmd.add_argument("--fixture", type=Path, default=ROOT / "hil/fixture.local.json")
        if name == "restore":
            cmd.add_argument(
                "--image", type=Path,
                help="exact validated production app image (for cancellation recovery)",
            )
            cmd.add_argument(
                "--factory-image", type=Path,
                help="exact factory artifact supplying the validated partition layout",
            )
            cmd.add_argument(
                "--config-backup", type=Path,
                help="private full export to compare and restore only if drift is proven",
            )
            cmd.add_argument(
                "--filesystem-backup", type=Path,
                help="private raw LittleFS snapshot captured before HIL",
            )
            cmd.add_argument(
                "--flash-marker", type=Path,
                help="durable proof that HIL started after a verified backup",
            )
        cmd.set_defaults(release=name == "restore")

    smoke_cmd = commands.add_parser("smoke", help="run fast state/render HIL checks")
    smoke_cmd.add_argument("--fixture", type=Path, default=ROOT / "hil/fixture.local.json")
    smoke_cmd.add_argument("--artifacts", type=Path)

    visual_cmd = commands.add_parser("visual", help="capture and validate the complete demo screen matrix")
    visual_cmd.add_argument("--fixture", type=Path, default=ROOT / "hil/fixture.local.json")
    visual_cmd.add_argument("--artifacts", type=Path)
    visual_cmd.add_argument("--timeout", type=float, default=240)

    messages_cmd = commands.add_parser("messages", help="inject and validate real FromRadio message flows")
    messages_cmd.add_argument("--fixture", type=Path, default=ROOT / "hil/fixture.local.json")
    messages_cmd.add_argument("--artifacts", type=Path)

    run_cmd = commands.add_parser("run", help="build, test, reboot-check and restore production firmware")
    run_cmd.add_argument("--fixture", type=Path, default=ROOT / "hil/fixture.local.json")
    run_cmd.add_argument("--artifacts", type=Path)
    run_cmd.add_argument("--timeout", type=float, default=240)
    run_cmd.add_argument("--skip-build", action="store_true", help="use already-built HIL and release images")
    run_cmd.add_argument(
        "--release-image", type=Path,
        help="exact production app image to restore after HIL (the release artifact's app bytes)",
    )
    run_cmd.add_argument(
        "--factory-image", type=Path,
        help="matching factory artifact used to derive the production flash layout",
    )
    run_cmd.add_argument(
        "--config-backup", type=Path,
        help="exclusive private backup destination outside the CI checkout/temp directories",
    )
    run_cmd.add_argument(
        "--production-wifi", action="store_true",
        help="soak the restored exact production image with read-only full PhoneAPI config dumps",
    )
    return parser


def main() -> int:
    args = make_parser().parse_args()
    try:
        if args.command == "inventory":
            found = ports()
            if args.json:
                print(json.dumps(found, indent=2))
            else:
                for item in found:
                    print(f"{item['usb_serial']}  {item['port']}  {item['description']}")
            return 0
        if args.command == "wifi-inventory":
            fixture = load_fixture(args.fixture)
            found = wifi_inventory(fixture, args.timeout)
            if args.json:
                print(json.dumps(found, indent=2))
            else:
                for item in found:
                    status = "online" if item["online"] else "offline"
                    print(f"{status:7} {item.get('name', '-'):12} {item['host']}:{item['port']} {item['access']}")
            return 1 if any(not item["online"] for item in found) else 0
        if args.command == "init":
            wifi_peers = []
            for value in args.wifi_peer:
                name, separator, host = value.partition("=")
                if not separator:
                    host, name = name, name
                wifi_peers.append({"name": name, "host": host, "access": "read-only"})
            fixture = {
                "schema": 2,
                "devices": {
                    "dut": {"kind": "m5stack-cardputer-adv", "usb_serial": normalize_serial(args.dut)},
                },
                "protected_devices": [
                    {"kind": "protected-usb", "usb_serial": normalize_serial(value)}
                    for value in args.protected_usb
                ],
                "wifi_peers": wifi_peers,
            }
            args.output.parent.mkdir(parents=True, exist_ok=True)
            try:
                write_owner_only_text(args.output, json.dumps(fixture, indent=2) + "\n")
            except OSError as exc:
                raise HilError(f"cannot write owner-only fixture {args.output}: {exc}") from exc
            load_fixture(args.output)  # validate identities before accepting the fixture
            print(args.output)
            return 0
        if args.command in ("build", "build-release"):
            build(getattr(args, "release", False))
            return 0
        fixture = load_fixture(args.fixture)
        if args.command in ("flash", "restore"):
            image = validate_app_image(args.image) if args.command == "restore" and args.image else None
            recovery_arguments = (
                args.config_backup, args.filesystem_backup, args.factory_image, args.flash_marker
            ) if args.command == "restore" else ()
            if args.command == "restore" and any(recovery_arguments) and not all(recovery_arguments):
                raise HilError("all interrupted-HIL recovery inputs must be supplied together")
            if args.command == "restore" and args.config_backup:
                if image is None:
                    raise HilError("--image is required for interrupted-HIL recovery")
                validate_config_backup(args.config_backup)
                _, layout = validate_factory_image(args.factory_image, image)
                validate_filesystem_backup(args.filesystem_backup, layout)
                validate_flash_marker(
                    args.flash_marker, args.config_backup, image,
                    args.factory_image, args.filesystem_backup,
                )
                restore_production_state(
                    fixture, image, args.factory_image, args.filesystem_backup
                )
            else:
                flash(fixture, args.release, image_override=image)
            if args.command == "restore" and args.config_backup:
                with configuration_temporary_directory("meshtastic-adv-hil-recovery-") as temporary:
                    recovered = recover_config_if_needed(
                        fixture, args.config_backup, Path(temporary)
                    )
                print(f"configuration recovery: {'restored' if recovered else 'already exact'}")
            return 0
        if args.command == "smoke":
            artifacts = args.artifacts or default_artifacts("smoke")
            report = smoke(fixture, artifacts)
            print(f"artifacts: {artifacts}")
            return 1 if report.failed else 0
        if args.command == "visual":
            artifacts = args.artifacts or default_artifacts("visual")
            report = visual(fixture, artifacts, args.timeout)
            print(f"artifacts: {artifacts}")
            return 1 if report.failed else 0
        if args.command == "messages":
            artifacts = args.artifacts or default_artifacts("messages")
            report = message_flow(fixture, artifacts)
            print(f"artifacts: {artifacts}")
            return 1 if report.failed else 0
        if args.command == "run":
            artifacts = args.artifacts or default_artifacts("full")
            failures = full_run(
                fixture, artifacts, args.timeout, args.skip_build,
                release_image=args.release_image, factory_image=args.factory_image,
                config_backup=args.config_backup,
                production_wifi=args.production_wifi,
            )
            print(f"artifacts: {artifacts}")
            return 1 if failures else 0
    except (HilError, subprocess.CalledProcessError) as exc:
        print(f"HIL ERROR: {redact_lab_details(str(exc))}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
