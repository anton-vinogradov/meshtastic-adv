#!/usr/bin/env python3
"""State-preserving two-node Meshtastic RF smoke test on a private channel."""

from __future__ import annotations

import argparse
import base64
import contextlib
import hashlib
import json
import os
import re
import secrets
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

import hil


CHANNEL_NAME = "ADV-HIL"
CHANNEL_INDEX = 2
# Fields that must agree for two radios to share the same over-the-air profile.
# Everything else is device policy or hardware tuning and must remain local:
# FEM/LNA mode, frequency calibration, boosted gain, MQTT flags, etc. Tx power
# follows the explicitly selected shared RF profile together with its region.
RF_PROFILE_FIELDS = (
    "use_preset",
    "modem_preset",
    "bandwidth",
    "spread_factor",
    "coding_rate",
    "region",
    "tx_power",
    "channel_num",
    "override_frequency",
)


class RfHilError(RuntimeError):
    pass


def parse_yaml(text: str, source: str) -> object:
    try:
        import yaml
    except ModuleNotFoundError as exc:
        raise RfHilError("PyYAML is required for RF HIL; install hil/requirements.txt") from exc
    try:
        return yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise RfHilError(f"invalid YAML from {source}: {exc}") from exc


@contextlib.contextmanager
def quiet_phoneapi():
    """Keep noisy library worker threads out of unattended HIL output."""
    with open(os.devnull, "w", encoding="utf-8") as sink:
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            yield


def find_cli() -> str:
    candidates = [
        os.environ.get("MESHTASTIC_CLI"),
        str(Path.home() / ".pio-core-venv/bin/meshtastic"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise RfHilError("meshtastic CLI not found; set MESHTASTIC_CLI")


def has_key(value: object, wanted: str) -> bool:
    if isinstance(value, dict):
        return wanted in value or any(has_key(child, wanted) for child in value.values())
    if isinstance(value, list):
        return any(has_key(child, wanted) for child in value)
    return False


def parse_channel_url(url: str):
    from meshtastic.protobuf import apponly_pb2

    encoded = url.split("#")[-1]
    encoded += "=" * (-len(encoded) % 4)
    channel_set = apponly_pb2.ChannelSet()
    channel_set.ParseFromString(base64.urlsafe_b64decode(encoded))
    return channel_set


def load_export(path: Path) -> tuple[dict[str, object], object]:
    try:
        text = path.read_text()
    except OSError as exc:
        raise RfHilError(f"cannot read backup {path}: {exc}") from exc
    data = parse_yaml(text, str(path))
    if not isinstance(data, dict) or not has_key(data, "privateKey"):
        raise RfHilError(f"backup is not restorable (privateKey missing): {path}")
    url = data.get("channel_url")
    if not isinstance(url, str):
        raise RfHilError(f"backup has no channel_url: {path}")
    return data, parse_channel_url(url)


def channel_digest(channel_set: object) -> str:
    return hashlib.sha256(channel_set.SerializeToString()).hexdigest()


def safe_channel_summary(channel_set: object) -> dict[str, object]:
    from google.protobuf.json_format import MessageToDict

    return {
        "active": len(channel_set.settings),
        "channels": [
            {
                "index": index,
                "name": settings.name,
                "psk_sha256": hashlib.sha256(bytes(settings.psk)).hexdigest(),
                "uplink_enabled": settings.uplink_enabled,
                "downlink_enabled": settings.downlink_enabled,
                "position_precision": settings.module_settings.position_precision,
                "is_muted": settings.module_settings.is_muted,
            }
            for index, settings in enumerate(channel_set.settings)
        ],
        "lora": MessageToDict(channel_set.lora_config, preserving_proto_field_name=True),
    }


def latest_backup(root: Path, name: str) -> Path:
    matches = sorted((root / name).glob("config-*.yaml"))
    if not matches:
        raise RfHilError(f"no config backup for {name} below {root}")
    return matches[-1]


def safe_fixture_name(peer: dict[str, object]) -> str:
    name = str(peer.get("name") or peer["host"])
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", name):
        raise RfHilError(f"unsafe RF fixture name: {name!r}")
    return name


def run_cli(host: str, arguments: list[str], timeout: float) -> tuple[bool, str]:
    command = [find_cli(), "--host", host, *arguments]
    try:
        result = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        return result.returncode == 0, result.stdout
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout.decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        return False, output


def export_live(host: str, attempts: int = 4) -> tuple[dict[str, object], object]:
    for attempt in range(attempts):
        ok, output = run_cli(host, ["--export-config"], timeout=95)
        if not ok or "privateKey:" not in output:
            if attempt + 1 < attempts:
                time.sleep(3)
            continue
        try:
            data = parse_yaml(output, host)
        except RfHilError:
            if attempt + 1 < attempts:
                time.sleep(3)
            continue
        if isinstance(data, dict) and isinstance(data.get("channel_url"), str):
            return data, parse_channel_url(data["channel_url"])
    # Never include CLI output here: a partial export can contain private keys.
    raise RfHilError(f"could not verify live configuration at {host} after {attempts} attempts")


def capture_backup(peer: dict[str, object], root: Path, attempts: int = 4) -> Path:
    """Capture a fresh, restorable export immediately before any RF mutation."""
    name = safe_fixture_name(peer)
    host = str(peer["host"])
    directory = root / name
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(directory, 0o700)
    for attempt in range(attempts):
        ok, output = run_cli(host, ["--export-config"], timeout=95)
        if ok and "privateKey:" in output:
            try:
                data = parse_yaml(output, host)
                url = data.get("channel_url") if isinstance(data, dict) else None
                if not isinstance(url, str):
                    raise RfHilError("channel_url missing")
                parse_channel_url(url)
            except Exception:
                pass
            else:
                stamp = datetime.now().strftime("%H%M%S-%f")
                path = directory / f"config-{stamp}.yaml"
                hil.write_owner_only_text(path, output)
                return path
        if attempt + 1 < attempts:
            time.sleep(3)
    # Never include a partial export here: it may already contain private keys.
    raise RfHilError(f"could not capture a restorable backup from {host} after {attempts} attempts")


def apply_rf_profile(target: object, canonical: object) -> None:
    """Copy only settings required for RF interoperability."""
    for field in RF_PROFILE_FIELDS:
        setattr(target, field, getattr(canonical, field))


def select_profile(
    peers: list[dict[str, object]], originals: dict[str, object], requested: str | None
) -> tuple[str, object]:
    """Choose an RF profile without silently resolving a mixed-region fixture."""
    names = [str(peer.get("name") or peer["host"]) for peer in peers]
    if requested:
        if requested not in originals:
            raise RfHilError(f"unknown --profile-from {requested!r}; choose one of: {', '.join(names)}")
        return requested, originals[requested]
    regions = {int(originals[name].lora_config.region) for name in names}
    if len(regions) != 1:
        raise RfHilError("fixture backups use different LoRa regions; choose explicitly with --profile-from NAME")
    return names[0], originals[names[0]]


def validate_expected_profile(
    channel_set: object, expected_region: str | None, expected_tx_power: int | None
) -> tuple[str, int]:
    """Fail before RF mutation if the authoritative profile drifted."""
    lora = channel_set.lora_config
    region_field = lora.DESCRIPTOR.fields_by_name["region"]
    region_value = region_field.enum_type.values_by_number.get(int(lora.region))
    if region_value is None:
        raise RfHilError(f"authoritative profile has unknown region value {int(lora.region)}")
    actual_region = str(region_value.name).upper()
    actual_tx_power = int(lora.tx_power)
    if expected_region and actual_region != expected_region.strip().upper():
        raise RfHilError(
            f"authoritative RF profile drifted: region is {actual_region}, expected "
            f"{expected_region.strip().upper()}"
        )
    if expected_tx_power is not None and actual_tx_power != expected_tx_power:
        raise RfHilError(
            f"authoritative RF profile drifted: tx_power is {actual_tx_power}, expected "
            f"{expected_tx_power}"
        )
    return actual_region, actual_tx_power


def lora_difference(actual: object, expected: object) -> str:
    """Describe the first non-secret LoRa field mismatch."""
    for field in expected.DESCRIPTOR.fields:
        left, right = getattr(actual, field.name), getattr(expected, field.name)
        if left != right:
            return f"{field.name}: read {left!r}, expected {right!r}"
    return "protobuf representation differs"


def private_add_url(original: object, canonical: object, key: bytes) -> tuple[str, object]:
    from meshtastic.protobuf import apponly_pb2

    channel_set = apponly_pb2.ChannelSet()
    settings = channel_set.settings.add()
    settings.name = CHANNEL_NAME
    settings.psk = key
    channel_set.lora_config.CopyFrom(original.lora_config)
    apply_rf_profile(channel_set.lora_config, canonical.lora_config)
    encoded = base64.urlsafe_b64encode(channel_set.SerializeToString()).decode("ascii").rstrip("=")
    expected_lora = type(channel_set.lora_config)()
    expected_lora.CopyFrom(channel_set.lora_config)
    return f"https://meshtastic.org/e/?add=true#{encoded}", expected_lora


def verify_prepared(channel_set: object, original: object, expected_lora: object, key: bytes) -> None:
    if len(original.settings) != CHANNEL_INDEX:
        raise RfHilError(
            f"safe private index {CHANNEL_INDEX} unavailable: backup has {len(original.settings)} active channels"
        )
    if len(channel_set.settings) != CHANNEL_INDEX + 1:
        raise RfHilError(f"expected {CHANNEL_INDEX + 1} active channels, got {len(channel_set.settings)}")
    for index in range(CHANNEL_INDEX):
        if channel_set.settings[index].SerializeToString() != original.settings[index].SerializeToString():
            raise RfHilError(f"existing channel {index} changed while preparing RF HIL")
    test = channel_set.settings[CHANNEL_INDEX]
    if test.name != CHANNEL_NAME or bytes(test.psk) != key:
        raise RfHilError("private HIL channel name/key mismatch")
    if channel_set.lora_config.SerializeToString() != expected_lora.SerializeToString():
        raise RfHilError(
            "LoRa settings did not converge to the private fixture configuration "
            f"({lora_difference(channel_set.lora_config, expected_lora)})"
        )


def open_tcp_interface(host: str, attempts: int = 4):
    """Open a real PhoneAPI session, allowing the previous client to drain."""
    from meshtastic.tcp_interface import TCPInterface

    last_type = "connection error"
    for attempt in range(attempts):
        try:
            return TCPInterface(hostname=host, timeout=30)
        except Exception as exc:
            last_type = type(exc).__name__
            if attempt + 1 < attempts:
                time.sleep(4)
    raise RfHilError(f"PhoneAPI session failed at {host} after {attempts} attempts ({last_type})")


def close_tcp_interface(interface: object) -> None:
    try:
        interface.close()
    except Exception:
        pass
    # Meshtastic WiFi nodes accept one PhoneAPI client. Give the firmware time
    # to observe EOF before another CLI/library client connects.
    time.sleep(3)


def prepare_peer(
    peer: dict[str, object], url: str, original: object, expected_lora: object, key: bytes
) -> dict[str, object]:
    host = str(peer["host"])
    # The CLI can time out after the node has already committed and rebooted, so
    # success is decided by a fresh read-back, never by process exit alone.
    last_error = "private channel was not prepared"
    for attempt in range(3):
        if attempt:
            # An add-only command can be ignored if firmware retained stale
            # settings in a disabled slot. The guarded cleaner refuses any
            # active-channel drift and erases only our reserved final slot.
            clear_test_channel(host, original)
        run_cli(host, ["--ch-add-url", url, "--wait-to-disconnect", "10"], timeout=90)
        time.sleep(3)
        _, live = export_live(host)
        try:
            verify_prepared(live, original, expected_lora, key)
        except RfHilError as exc:
            last_error = str(exc)
            # Retry only when channels 0/1 are still byte-identical and no
            # trailing active channel exists. Every other drift is unsafe.
            if needs_test_channel_cleanup(live, original):
                raise
            continue
        time.sleep(3)
        return {
            "name": peer.get("name", host),
            "channel_digest": channel_digest(live),
            "region": int(live.lora_config.region),
        }
    raise RfHilError(f"could not prepare private channel at {host}: {last_error}")


def needs_test_channel_cleanup(live: object, original: object) -> bool:
    """Return whether the sole trailing channel is our private HIL channel.

    Refuse to mutate anything if the live channel layout has drifted in any
    other way.  LoRa settings are intentionally ignored here because the
    subsequent full backup restore owns those settings.
    """
    if (
        len(live.settings) == len(original.settings)
        and all(
            current.SerializeToString() == saved.SerializeToString()
            for current, saved in zip(live.settings, original.settings)
        )
    ):
        return False
    if len(original.settings) != CHANNEL_INDEX:
        raise RfHilError(
            f"cleanup expects {CHANNEL_INDEX} backed-up channels, got {len(original.settings)}"
        )
    if len(live.settings) != CHANNEL_INDEX + 1:
        raise RfHilError(
            f"cleanup refused: expected {CHANNEL_INDEX + 1} live channels, got {len(live.settings)}"
        )
    for index in range(CHANNEL_INDEX):
        if live.settings[index].SerializeToString() != original.settings[index].SerializeToString():
            raise RfHilError(f"cleanup refused: existing channel {index} differs from backup")
    if live.settings[CHANNEL_INDEX].name != CHANNEL_NAME:
        raise RfHilError(
            f"cleanup refused: channel {CHANNEL_INDEX} is not the reserved {CHANNEL_NAME} channel"
        )
    return True


def clear_test_channel(host: str, original: object) -> None:
    from meshtastic.protobuf import channel_pb2

    _, live = export_live(host)
    needs_test_channel_cleanup(live, original)
    time.sleep(3)

    # `--ch-del` queues writes for slots 2..7 and can close TCP before they are
    # flushed. `--ch-disable` is one write, but retains the test name and PSK in
    # a disabled slot, causing the next add-only URL to be ignored. Clear the
    # final slot in one guarded protobuf write, wait for the queue, then inspect
    # that disabled slot directly so no private test key remains at rest.
    with quiet_phoneapi():
        interface = open_tcp_interface(host)
        try:
            channel = interface.localNode.channels[CHANNEL_INDEX]
            allowed_roles = (
                channel_pb2.Channel.Role.SECONDARY,
                channel_pb2.Channel.Role.DISABLED,
            )
            if channel.role not in allowed_roles:
                raise RfHilError(f"cleanup refused: unexpected channel {CHANNEL_INDEX} role at {host}")
            if channel.settings.name not in ("", CHANNEL_NAME):
                raise RfHilError(f"cleanup refused: channel {CHANNEL_INDEX} is owned by another user at {host}")
            channel.Clear()
            channel.index = CHANNEL_INDEX
            channel.role = channel_pb2.Channel.Role.DISABLED
            interface.localNode.writeChannel(CHANNEL_INDEX)
            time.sleep(10)
        except RfHilError:
            raise
        except Exception as exc:
            raise RfHilError(f"could not clear reserved HIL channel at {host} ({type(exc).__name__})") from exc
        finally:
            close_tcp_interface(interface)

    _, cleaned = export_live(host)
    if len(cleaned.settings) != len(original.settings):
        raise RfHilError(f"reserved HIL channel is still active at {host}")
    for index in range(CHANNEL_INDEX):
        if cleaned.settings[index].SerializeToString() != original.settings[index].SerializeToString():
            raise RfHilError(f"channel {index} changed while cleaning {host}")

    time.sleep(3)
    with quiet_phoneapi():
        interface = open_tcp_interface(host)
        try:
            channel = interface.localNode.channels[CHANNEL_INDEX]
            if channel.role != channel_pb2.Channel.Role.DISABLED or channel.settings.ListFields():
                raise RfHilError(f"reserved HIL channel data remains in disabled slot at {host}")
        except RfHilError:
            raise
        except Exception as exc:
            raise RfHilError(f"could not verify cleared HIL slot at {host} ({type(exc).__name__})") from exc
        finally:
            close_tcp_interface(interface)


def restore_peer(peer: dict[str, object], backup: Path, original: object) -> dict[str, object]:
    host = str(peer["host"])
    last_error = "restore did not run"
    for _ in range(3):
        # setURL/--configure overwrites the channels present in the URL but does
        # not disable trailing channels. Disable only our guarded temporary
        # index first, then restore every original setting from the export.
        try:
            clear_test_channel(host, original)
        except RfHilError as exc:
            last_error = str(exc)
            continue
        run_cli(host, ["--configure", str(backup)], timeout=150)
        try:
            time.sleep(4)
            _, live = export_live(host)
            if live.SerializeToString() == original.SerializeToString():
                return {"name": peer.get("name", host), "restored": True, "digest": channel_digest(live)}
            last_error = "channel/LoRa fingerprint differs from backup"
        except RfHilError as exc:
            last_error = str(exc)
    return {"name": peer.get("name", host), "restored": False, "error": last_error}


def send_directed_private(interface: object, token: str, destination_id: str):
    return interface.sendText(
        token,
        destinationId=destination_id,
        wantAck=True,
        channelIndex=CHANNEL_INDEX,
        hopLimit=1,
    )


def is_private_delivery(packet: dict[str, object], source_num: int, token: str) -> bool:
    decoded = packet.get("decoded") or {}
    return bool(
        decoded.get("portnum") == "TEXT_MESSAGE_APP"
        and int(packet.get("from", 0)) == source_num
        and int(packet.get("channel", 0)) == CHANNEL_INDEX
        and decoded.get("text") == token
    )


def is_explicit_routing_ack(packet: dict[str, object], destination_num: int, request_id: int | None) -> bool:
    candidate = routing_ack_request_id(packet, destination_num)
    return request_id is not None and candidate == request_id


def routing_ack_request_id(packet: dict[str, object], destination_num: int) -> int | None:
    decoded = packet.get("decoded") or {}
    routing = decoded.get("routing") or {}
    if not (
        decoded.get("portnum") == "ROUTING_APP"
        and int(packet.get("from", 0)) == destination_num
        and routing.get("errorReason") == "NONE"
    ):
        return None
    request_id = int(decoded.get("requestId", 0))
    return request_id or None


class RoutingAckTracker:
    """Match an ACK even if the callback wins the sendText return-value race."""

    def __init__(self, destination_num: int):
        self.destination_num = destination_num
        self.request_id: int | None = None
        self.early_request_ids: set[int] = set()
        self.acknowledged = threading.Event()
        self.lock = threading.Lock()

    def observe(self, packet: dict[str, object]) -> None:
        candidate = routing_ack_request_id(packet, self.destination_num)
        if candidate is None:
            return
        with self.lock:
            if self.request_id == candidate:
                self.acknowledged.set()
            elif self.request_id is None:
                self.early_request_ids.add(candidate)

    def expect(self, request_id: int) -> None:
        with self.lock:
            self.request_id = request_id
            if request_id in self.early_request_ids:
                self.acknowledged.set()


def directed_private_ack(
    source: str,
    destination: str,
    source_id: str,
    destination_id: str,
    token: str,
    timeout: float,
) -> None:
    from pubsub import pub

    with quiet_phoneapi():
        destination_interface = open_tcp_interface(destination)
        source_interface = None
        source_num = int(source_id[-8:], 16)
        destination_num = int(destination_id[-8:], 16)
        tracker = RoutingAckTracker(destination_num)
        delivered = threading.Event()

        def on_receive(packet, interface) -> None:
            if interface is source_interface:
                tracker.observe(packet)
            elif interface is destination_interface and is_private_delivery(packet, source_num, token):
                delivered.set()

        pub.subscribe(on_receive, "meshtastic.receive")
        try:
            source_interface = open_tcp_interface(source)
            sent = send_directed_private(source_interface, token, destination_id)
            tracker.expect(int(sent.id))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if delivered.is_set() and tracker.acknowledged.is_set():
                    return
                time.sleep(0.05)
            missing = []
            if not delivered.is_set():
                missing.append("private delivery")
            if not tracker.acknowledged.is_set():
                missing.append("explicit routing ACK")
            raise RfHilError(f"missing {' and '.join(missing)} from {destination_id}")
        except RfHilError:
            raise
        except Exception as exc:
            raise RfHilError(f"private RF transaction failed at {source} ({type(exc).__name__})") from exc
        finally:
            pub.unsubscribe(on_receive, "meshtastic.receive")
            if source_interface is not None:
                close_tcp_interface(source_interface)
            close_tcp_interface(destination_interface)


def rf_exchange(peers: list[dict[str, object]], timeout: float) -> list[dict[str, object]]:
    results = []
    for source, destination in ((0, 1), (1, 0)):
        last_error = "RF transaction did not run"
        attempts = 0
        for attempts in range(1, 4):
            token = f"[ADV-HIL] {secrets.token_hex(6)}"
            try:
                directed_private_ack(
                    str(peers[source]["host"]),
                    str(peers[destination]["host"]),
                    str(peers[source]["node_id"]),
                    str(peers[destination]["node_id"]),
                    token,
                    timeout,
                )
                break
            except RfHilError as exc:
                last_error = str(exc)
                if attempts < 3:
                    time.sleep(5)
        else:
            raise RfHilError(
                f"private-channel RF failure: {peers[source].get('name')} -> "
                f"{peers[destination].get('name')} after 3 attempts ({last_error})"
            )
        results.append(
            {
                "from": peers[source].get("name"),
                "to": peers[destination].get("name"),
                "channel": CHANNEL_INDEX,
                "hop_limit": 1,
                "attempts": attempts,
                "received": True,
                "routing_ack": True,
            }
        )
    return results


def report_cases(
    report: dict[str, object], peers: list[dict[str, object]]
) -> list[tuple[str, bool, str]]:
    """Convert the secret-free RF report into stable CI testcase records."""
    names = [str(peer.get("name") or peer["host"]) for peer in peers]
    default_error = str(report.get("error") or report.get("restore_error") or "stage did not complete")
    cases: list[tuple[str, bool, str]] = []

    if report.get("suite") != "meshtastic-private-rf-restore":
        prepared = {
            str(item.get("name")): item
            for item in report.get("prepared", [])
            if isinstance(item, dict)
        }
        for name in names:
            passed = name in prepared
            cases.append((f"prepare/{name}", passed, "" if passed else default_error))

        exchange = {
            (str(item.get("from")), str(item.get("to"))): item
            for item in report.get("exchange", [])
            if isinstance(item, dict)
        }
        for source, destination in ((names[0], names[1]), (names[1], names[0])):
            item = exchange.get((source, destination), {})
            passed = bool(item.get("received") and item.get("routing_ack"))
            cases.append((f"rf/{source}-to-{destination}", passed, "" if passed else default_error))

    restored = {
        str(item.get("name")): item
        for item in report.get("restored", [])
        if isinstance(item, dict)
    }
    for name in names:
        item = restored.get(name, {})
        passed = bool(item.get("restored"))
        message = "" if passed else str(item.get("error") or default_error)
        cases.append((f"restore/{name}", passed, message))
    return cases


def redact_peer_details(value: str, peers: list[dict[str, object]]) -> str:
    """Remove stable lab addresses from evidence that may be uploaded by CI."""
    result = hil.redact_lab_details(value)
    for field, replacement in (("host", "<peer-host>"), ("node_id", "<peer-node>")):
        for peer in peers:
            candidate = str(peer.get(field) or "").strip()
            if candidate:
                result = re.sub(re.escape(candidate), replacement, result, flags=re.IGNORECASE)
    return result


def sanitize_report(value: object, peers: list[dict[str, object]]) -> object:
    """Recursively redact lab identities without mutating the live report."""
    if isinstance(value, str):
        return redact_peer_details(value, peers)
    if isinstance(value, dict):
        return {key: sanitize_report(item, peers) for key, item in value.items()}
    if isinstance(value, list):
        return [sanitize_report(item, peers) for item in value]
    if isinstance(value, tuple):
        return tuple(sanitize_report(item, peers) for item in value)
    return value


def write_report(report: dict[str, object], peers: list[dict[str, object]], artifacts: Path) -> None:
    artifacts.mkdir(parents=True, exist_ok=True)
    cases = report_cases(report, peers)
    failures = sum(not passed for _, passed, _ in cases)
    report["summary"] = {"tests": len(cases), "failures": failures}

    json_path = artifacts / "report.json"
    safe_report = sanitize_report(report, peers)
    hil.write_owner_only_text(json_path, json.dumps(safe_report, indent=2) + "\n")

    testsuite = ET.Element(
        "testsuite",
        name=redact_peer_details(str(report["suite"]), peers),
        tests=str(len(cases)),
        failures=str(failures),
    )
    for name, passed, message in cases:
        safe_name = redact_peer_details(name, peers)
        safe_message = redact_peer_details(message, peers)
        testcase = ET.SubElement(testsuite, "testcase", name=safe_name)
        if not passed:
            ET.SubElement(testcase, "failure", message=safe_message).text = safe_message
    junit_path = artifacts / "junit.xml"
    xml = ET.tostring(testsuite, encoding="unicode", xml_declaration=True)
    hil.write_owner_only_text(junit_path, xml)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=hil.ROOT / "hil/fixture.local.json")
    parser.add_argument("--backup-root", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--profile-from", help="fixture name whose over-the-air profile is authoritative")
    parser.add_argument("--expect-region", help="required region name for the authoritative profile")
    parser.add_argument("--expect-tx-power", type=int, help="required dBm for the authoritative profile")
    parser.add_argument(
        "--capture-backups", action="store_true",
        help="capture fresh restorable exports below --backup-root before preparing RF",
    )
    parser.add_argument("--reverse-first", action="store_true", help="test the second fixture as RF source first")
    parser.add_argument("--restore-only", action="store_true", help="remove channel 2 and restore backups")
    args = parser.parse_args()

    artifacts = args.artifacts or hil.ROOT / "hil-artifacts" / f"{datetime.now():%Y%m%d-%H%M%S}-rf"
    artifacts.mkdir(parents=True, exist_ok=True)
    fixture = hil.load_fixture(args.fixture)
    peers = [peer for peer in fixture.get("wifi_peers", []) if peer.get("access") == "test"]
    if len(peers) != 2:
        raise RfHilError(f"RF smoke requires exactly two access=test WiFi peers, got {len(peers)}")

    if args.capture_backups:
        args.backup_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.chmod(args.backup_root, 0o700)
        for peer in peers:
            capture_backup(peer, args.backup_root)

    backups: dict[str, Path] = {}
    originals: dict[str, object] = {}
    backup_hashes: dict[str, str] = {}
    for peer in peers:
        name = safe_fixture_name(peer)
        backup = latest_backup(args.backup_root, name)
        _, original = load_export(backup)
        backups[name] = backup
        originals[name] = original
        backup_hashes[name] = hashlib.sha256(backup.read_bytes()).hexdigest()

    if args.restore_only:
        restored = []
        for peer in peers:
            name = safe_fixture_name(peer)
            restored.append(restore_peer(peer, backups[name], originals[name]))
        report = {
            "suite": "meshtastic-private-rf-restore",
            "started": datetime.now(timezone.utc).isoformat(),
            "backups": backup_hashes,
            "restored": restored,
            "status": "passed" if all(item.get("restored") for item in restored) else "failed",
        }
        write_report(report, peers, artifacts)
        print(f"RF RESTORE {str(report['status']).upper()}: {artifacts / 'report.json'}")
        return 0 if report["status"] == "passed" else 1

    profile_name, canonical = select_profile(peers, originals, args.profile_from)
    profile_region, profile_tx_power = validate_expected_profile(
        canonical, args.expect_region, args.expect_tx_power
    )
    key = secrets.token_bytes(32)
    report: dict[str, object] = {
        "suite": "meshtastic-private-rf-smoke",
        "started": datetime.now(timezone.utc).isoformat(),
        "channel": {
            "name": CHANNEL_NAME,
            "index": CHANNEL_INDEX,
            "hop_limit": 1,
            "profile_from": profile_name,
            "region": profile_region,
            "tx_power": profile_tx_power,
        },
        "backups": backup_hashes,
        "prepared": [],
        "exchange": [],
        "restored": [],
        "status": "failed",
    }
    failure: Exception | None = None
    try:
        for peer in peers:
            name = safe_fixture_name(peer)
            url, expected_lora = private_add_url(originals[name], canonical, key)
            report["prepared"].append(prepare_peer(peer, url, originals[name], expected_lora, key))
        exchange_peers = list(reversed(peers)) if args.reverse_first else peers
        report["exchange"] = rf_exchange(exchange_peers, args.timeout)
        report["status"] = "passed"
    except Exception as exc:
        failure = exc
        report["error"] = str(exc)
    finally:
        for peer in peers:
            name = safe_fixture_name(peer)
            report["restored"].append(restore_peer(peer, backups[name], originals[name]))
        if not all(item.get("restored") for item in report["restored"]):
            report["status"] = "failed"
            report["restore_error"] = "one or more fixtures did not match their backup after restore"
        write_report(report, peers, artifacts)

    print(f"RF HIL {str(report['status']).upper()}: {artifacts / 'report.json'}")
    if failure:
        print(f"RF HIL ERROR: {failure}", file=sys.stderr)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RfHilError, hil.HilError) as exc:
        print(f"RF HIL ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
