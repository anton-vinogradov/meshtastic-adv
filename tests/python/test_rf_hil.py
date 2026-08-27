import importlib.util
import json
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]

HIL_SPEC = importlib.util.spec_from_file_location("hil", ROOT / "scripts/hil.py")
hil = importlib.util.module_from_spec(HIL_SPEC)
sys.modules[HIL_SPEC.name] = hil
HIL_SPEC.loader.exec_module(hil)

RF_SPEC = importlib.util.spec_from_file_location("rf_hil_runner", ROOT / "scripts/rf_hil.py")
rf_hil = importlib.util.module_from_spec(RF_SPEC)
sys.modules[RF_SPEC.name] = rf_hil
RF_SPEC.loader.exec_module(rf_hil)


class FakeChannel:
    def __init__(self, name, fingerprint):
        self.name = name
        self.fingerprint = fingerprint

    def SerializeToString(self):
        return self.fingerprint.encode()


class FakeChannelSet:
    def __init__(self, *channels):
        self.settings = list(channels)


class RfHilCleanupTests(unittest.TestCase):
    def setUp(self):
        self.original = FakeChannelSet(
            FakeChannel("MediumFast", "primary"),
            FakeChannel("FerretClub", "secondary"),
        )

    def test_cleanup_is_not_needed_for_original_channels(self):
        live = FakeChannelSet(
            FakeChannel("MediumFast", "primary"),
            FakeChannel("FerretClub", "secondary"),
        )
        self.assertFalse(rf_hil.needs_test_channel_cleanup(live, self.original))

    def test_fixture_name_cannot_escape_backup_root(self):
        with self.assertRaises(rf_hil.RfHilError):
            rf_hil.safe_fixture_name({"name": "../outside", "host": "192.0.2.1"})

    def test_capture_backup_writes_only_valid_restorable_export(self):
        export = "privateKey: secret\nchannel_url: https://meshtastic.org/e/#fixture\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with (
                mock.patch.object(rf_hil, "run_cli", return_value=(True, export)),
                mock.patch.object(rf_hil, "parse_yaml", return_value={"channel_url": "fixture"}),
                mock.patch.object(rf_hil, "parse_channel_url", return_value=object()),
            ):
                path = rf_hil.capture_backup({"name": "lab-us", "host": "192.0.2.1"}, root)
            self.assertEqual(path.read_text(), export)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_rf_profile_merge_preserves_device_specific_lora_settings(self):
        local_values = {field: index for index, field in enumerate(rf_hil.RF_PROFILE_FIELDS)}
        canonical_values = {field: index + 100 for index, field in enumerate(rf_hil.RF_PROFILE_FIELDS)}
        local_values["tx_power"] = 26
        canonical_values["tx_power"] = 20
        local = SimpleNamespace(**local_values, fem_lna_mode=0, frequency_offset=1.25)
        canonical = SimpleNamespace(**canonical_values, fem_lna_mode=2, frequency_offset=-0.5)

        rf_hil.apply_rf_profile(local, canonical)

        for field in rf_hil.RF_PROFILE_FIELDS:
            self.assertEqual(getattr(local, field), getattr(canonical, field))
        self.assertEqual(local.tx_power, 20)
        self.assertEqual(local.fem_lna_mode, 0)
        self.assertEqual(local.frequency_offset, 1.25)

    def test_lora_difference_names_only_the_safe_field(self):
        descriptor = SimpleNamespace(fields=[SimpleNamespace(name="tx_power")])
        actual = SimpleNamespace(DESCRIPTOR=descriptor, tx_power=20)
        expected = SimpleNamespace(DESCRIPTOR=descriptor, tx_power=26)
        self.assertEqual(rf_hil.lora_difference(actual, expected), "tx_power: read 20, expected 26")

    def test_mixed_regions_require_explicit_profile_source(self):
        peers = [{"name": "ru", "host": "192.0.2.1"}, {"name": "us", "host": "192.0.2.2"}]
        originals = {
            "ru": SimpleNamespace(lora_config=SimpleNamespace(region=9)),
            "us": SimpleNamespace(lora_config=SimpleNamespace(region=1)),
        }
        with self.assertRaisesRegex(rf_hil.RfHilError, "--profile-from"):
            rf_hil.select_profile(peers, originals, None)
        name, selected = rf_hil.select_profile(peers, originals, "us")
        self.assertEqual(name, "us")
        self.assertIs(selected, originals["us"])

    def test_expected_profile_rejects_region_or_power_drift(self):
        region_value = SimpleNamespace(name="US")
        enum_type = SimpleNamespace(values_by_number={1: region_value})
        descriptor = SimpleNamespace(
            fields_by_name={"region": SimpleNamespace(enum_type=enum_type)}
        )
        profile = SimpleNamespace(
            lora_config=SimpleNamespace(DESCRIPTOR=descriptor, region=1, tx_power=26)
        )
        self.assertEqual(rf_hil.validate_expected_profile(profile, "US", 26), ("US", 26))
        with self.assertRaisesRegex(rf_hil.RfHilError, "region is US, expected RU"):
            rf_hil.validate_expected_profile(profile, "RU", 26)
        with self.assertRaisesRegex(rf_hil.RfHilError, "tx_power is 26, expected 20"):
            rf_hil.validate_expected_profile(profile, "US", 20)

    def test_cleanup_accepts_only_reserved_trailing_channel(self):
        live = FakeChannelSet(
            FakeChannel("MediumFast", "primary"),
            FakeChannel("FerretClub", "secondary"),
            FakeChannel(rf_hil.CHANNEL_NAME, "private-test"),
        )
        self.assertTrue(rf_hil.needs_test_channel_cleanup(live, self.original))

    def test_cleanup_rejects_changed_existing_channel(self):
        live = FakeChannelSet(
            FakeChannel("MediumFast", "changed"),
            FakeChannel("FerretClub", "secondary"),
            FakeChannel(rf_hil.CHANNEL_NAME, "private-test"),
        )
        with self.assertRaises(rf_hil.RfHilError):
            rf_hil.needs_test_channel_cleanup(live, self.original)

    def test_cleanup_rejects_unowned_trailing_channel(self):
        live = FakeChannelSet(
            FakeChannel("MediumFast", "primary"),
            FakeChannel("FerretClub", "secondary"),
            FakeChannel("someone-elses-channel", "private-test"),
        )
        with self.assertRaises(rf_hil.RfHilError):
            rf_hil.needs_test_channel_cleanup(live, self.original)

    def test_prepare_keeps_tcp_open_for_async_channel_writes(self):
        peer = {"name": "fixture", "host": "192.0.2.10", "node_id": "!10203040"}
        with (
            mock.patch.object(rf_hil, "run_cli", return_value=(True, "")) as run,
            mock.patch.object(rf_hil.time, "sleep"),
            mock.patch.object(
                rf_hil,
                "export_live",
                return_value=({}, SimpleNamespace(lora_config=SimpleNamespace(region=1))),
            ),
            mock.patch.object(rf_hil, "verify_prepared"),
            mock.patch.object(rf_hil, "channel_digest", return_value="digest"),
        ):
            result = rf_hil.prepare_peer(peer, "private-url", object(), object(), b"key")
        self.assertEqual(
            run.call_args.args[1],
            ["--ch-add-url", "private-url", "--wait-to-disconnect", "10"],
        )
        self.assertNotIn("host", result)
        self.assertNotIn("node_id", result)

    def test_prepare_retries_only_after_guarded_cleanup(self):
        peer = {"name": "fixture", "host": "192.0.2.10"}
        live = FakeChannelSet(
            FakeChannel("MediumFast", "primary"),
            FakeChannel("FerretClub", "secondary"),
        )
        live.lora_config = SimpleNamespace(region=1)
        with (
            mock.patch.object(rf_hil, "run_cli", return_value=(True, "")) as run,
            mock.patch.object(rf_hil.time, "sleep"),
            mock.patch.object(rf_hil, "export_live", return_value=({}, live)),
            mock.patch.object(
                rf_hil,
                "verify_prepared",
                side_effect=[rf_hil.RfHilError("not committed"), None],
            ),
            mock.patch.object(rf_hil, "clear_test_channel") as clear,
            mock.patch.object(rf_hil, "channel_digest", return_value="digest"),
        ):
            rf_hil.prepare_peer(peer, "private-url", self.original, object(), b"key")
        self.assertEqual(run.call_count, 2)
        clear.assert_called_once_with(peer["host"], self.original)

    def test_private_send_is_pinned_to_test_channel_and_one_hop(self):
        interface = mock.Mock()
        rf_hil.send_directed_private(interface, "opaque-token", "!12345678")
        interface.sendText.assert_called_once_with(
            "opaque-token",
            destinationId="!12345678",
            wantAck=True,
            channelIndex=rf_hil.CHANNEL_INDEX,
            hopLimit=1,
        )

    def test_private_delivery_requires_source_token_and_test_channel(self):
        packet = {
            "from": int("10203040", 16),
            "channel": rf_hil.CHANNEL_INDEX,
            "decoded": {"portnum": "TEXT_MESSAGE_APP", "text": "opaque-token"},
        }
        self.assertTrue(rf_hil.is_private_delivery(packet, int("10203040", 16), "opaque-token"))
        self.assertFalse(rf_hil.is_private_delivery(packet, int("50607080", 16), "opaque-token"))
        self.assertFalse(rf_hil.is_private_delivery(packet, int("10203040", 16), "other-token"))
        packet["channel"] = 0
        self.assertFalse(rf_hil.is_private_delivery(packet, int("10203040", 16), "opaque-token"))

    def test_explicit_routing_ack_uses_decoded_request_id(self):
        packet = {
            "from": int("50607080", 16),
            "decoded": {
                "portnum": "ROUTING_APP",
                "requestId": 1234,
                "routing": {"errorReason": "NONE"},
            },
        }
        self.assertTrue(
            rf_hil.is_explicit_routing_ack(packet, int("50607080", 16), 1234)
        )
        self.assertFalse(
            rf_hil.is_explicit_routing_ack(packet, int("10203040", 16), 1234)
        )

    def test_early_routing_ack_is_matched_after_send_returns(self):
        tracker = rf_hil.RoutingAckTracker(int("50607080", 16))
        tracker.observe(
            {
                "from": int("50607080", 16),
                "decoded": {
                    "portnum": "ROUTING_APP",
                    "requestId": 1234,
                    "routing": {"errorReason": "NONE"},
                },
            }
        )
        self.assertFalse(tracker.acknowledged.is_set())
        tracker.expect(1234)
        self.assertTrue(tracker.acknowledged.is_set())

    def test_rf_exchange_retries_a_lost_ack(self):
        peers = [
            {"name": "fca", "host": "192.0.2.10", "node_id": "!10203040"},
            {"name": "fc1", "host": "192.0.2.11", "node_id": "!50607080"},
        ]
        with (
            mock.patch.object(
                rf_hil,
                "directed_private_ack",
                side_effect=[rf_hil.RfHilError("lost"), None, None],
            ) as exchange,
            mock.patch.object(rf_hil.time, "sleep"),
        ):
            result = rf_hil.rf_exchange(peers, timeout=1)
        self.assertEqual(exchange.call_count, 3)
        self.assertEqual([item["attempts"] for item in result], [2, 1])

    def test_report_writes_secret_free_json_and_junit(self):
        peers = [
            {"name": "fca", "host": "192.0.2.10", "node_id": "!10203040"},
            {"name": "fc1", "host": "192.0.2.11", "node_id": "!50607080"},
        ]
        report = {
            "suite": "meshtastic-private-rf-smoke",
            "prepared": [{"name": "fca"}, {"name": "fc1"}],
            "exchange": [
                {"from": "fca", "to": "fc1", "received": True, "routing_ack": True},
                {"from": "fc1", "to": "fca", "received": True, "routing_ack": True},
            ],
            "restored": [
                {"name": "fca", "restored": True},
                {
                    "name": "fc1",
                    "restored": False,
                    "error": "restore at 192.0.2.11 for !50607080 failed",
                },
            ],
            "diagnostic": {
                "host": "192.0.2.10",
                "node_id": "!10203040",
                "error": "connection to 192.0.2.11 for !50607080 failed",
            },
            "status": "passed",
        }
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory)
            rf_hil.write_report(report, peers, artifacts)
            json_text = (artifacts / "report.json").read_text()
            junit_text = (artifacts / "junit.xml").read_text()
            payload = json.loads(json_text)
            suite = ET.fromstring(junit_text)
        self.assertEqual(payload["summary"], {"tests": 6, "failures": 1})
        self.assertEqual(suite.attrib["tests"], "6")
        self.assertEqual(suite.attrib["failures"], "1")
        for private_value in ("192.0.2.10", "192.0.2.11", "!10203040", "!50607080"):
            self.assertNotIn(private_value.lower(), json_text.lower())
            self.assertNotIn(private_value.lower(), junit_text.lower())
        self.assertEqual(payload["diagnostic"]["host"], "<peer-host>")
        self.assertEqual(payload["diagnostic"]["node_id"], "<peer-node>")
        failure = suite.find("./testcase[@name='restore/fc1']/failure")
        self.assertIsNotNone(failure)
        self.assertIn("<peer-host>", failure.attrib["message"])
        self.assertIn("<peer-node>", failure.attrib["message"])


if __name__ == "__main__":
    unittest.main()
