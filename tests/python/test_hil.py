import hashlib
import importlib.util
import json
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("hil_runner", ROOT / "scripts/hil.py")
hil = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = hil
SPEC.loader.exec_module(hil)


class HilRunnerTests(unittest.TestCase):
    def test_init_writes_identity_fixture_owner_only(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "fixture.json"
            output.write_text("stale")
            output.chmod(0o644)
            with mock.patch.object(
                sys,
                "argv",
                ["hil.py", "init", "--dut", "02:11:22:33:44:55", "--output", str(output)],
            ):
                self.assertEqual(hil.main(), 0)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)

    def test_hil_firmware_forces_live_radio_tx_off(self):
        variant = (ROOT / "overlay/variants/esp32s3/m5stack_cardputer_adv_advui/platformio.ini").read_text()
        sync = (ROOT / "scripts/sync-overlay.sh").read_text()
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        self.assertIn("-D USERPREFS_LORA_TX_DISABLED=1", variant)
        self.assertIn("advui-inject-hil-rx-only", sync)
        healing = source.split("// Nameless-node healing.", 1)[1].split("// First-boot onboarding:", 1)[0]
        self.assertIn("#ifndef ADVUI_HIL", healing)
        self.assertIn("nodeInfoModule->sendOurNodeInfo", healing)

    def test_hil_release_does_not_delete_static_queue_interior_pointers(self):
        source = (ROOT / "overlay/src/advui/AdvBle.cpp").read_text()
        body = source.split("void bleHilReleaseState()", 1)[1].split("uint32_t bleHilIngressFirstHeap()", 1)[0]
        self.assertNotIn("vQueueDelete(", body)
        self.assertIn("free(g_buffers)", body)

    def test_hil_companion_config_stream_does_not_invent_ui_redraws(self):
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        body = source.split("void AdvUI::hilInjectCompanion", 1)[1].split(
            "void AdvUI::hilClearData", 1
        )[0]
        self.assertIn("bool uiChanged = false", body)
        self.assertIn("if (uiChanged)\n            uiDirty = true", body)
        self.assertNotIn("}\n        uiDirty = true;", body)

    def test_ignored_and_duplicate_ingress_do_not_dirty_the_display(self):
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        ingress = source.split("bool AdvUI::handleFromRadio", 1)[1].split("struct SendResult", 1)[0]
        self.assertIn("const bool changed = addMsg", ingress)
        self.assertIn("if (changed && unread)", ingress)
        self.assertIn("return changed", ingress)
        self.assertIn("uiDirty = handleFromRadio(fr) || uiDirty", source)
        runner = (ROOT / "scripts/hil.py").read_text()
        self.assertIn('require_fields(duplicate, {"changed": "0"})', runner)

    def test_name_editor_enforces_wire_byte_limits_before_owner_broadcast(self):
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        editor = source.split("if (mode == MODE_SETNAME)", 1)[1].split("if (mode == MODE_BTPIN)", 1)[0]
        self.assertIn("translitFeed(nameBuf, nameLen, maxLen + 1", editor)
        apply_name = source.split("bool AdvUI::applyName()", 1)[1].split("void AdvUI::extInit()", 1)[0]
        self.assertIn("utf8CopyValid(owner.long_name", apply_name)
        self.assertIn("utf8CopyValid(owner.short_name", apply_name)
        self.assertIn("utf8CopyValid(ch.settings.name", apply_name)

    def test_send_failures_preserve_compose_and_consume_queue_status(self):
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        compose = source.split("if (mode == MODE_COMPOSE)", 1)[1].split("// Home: recent conversations", 1)[0]
        self.assertIn("preserve both the draft and its reply target", compose)
        self.assertIn("sendFailure != SendFailure::NONE", compose)
        ingress = source.split("bool AdvUI::handleFromRadio", 1)[1].split("static SendResult sendTextPacket", 1)[0]
        self.assertIn("meshtastic_FromRadio_queueStatus_tag", ingress)
        self.assertIn("ackMsg(status.mesh_packet_id, MSG_FAILED", ingress)
        companion = (ROOT / "overlay/src/advui/AdvBle.cpp").read_text()
        self.assertIn("case meshtastic_FromRadio_queueStatus_tag:", companion)
        self.assertLess(source.index("if (injected != ERRNO_OK)"), source.index("service->sendToMesh"))
        self.assertIn("emulate successful queue admission", source)
        self.assertIn("g_hilLastSendError = ERRNO_DISABLED", source)

    def test_reply_render_and_resend_are_scoped(self):
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        render = source.split("if (replyId) { // a reply", 1)[1].split("if (chatAnchorMsgIdx", 1)[0]
        self.assertIn("for (int q = i - 1; q >= 0; q--)", render)
        self.assertIn("srcs[q].m->id == replyId", render)
        self.assertNotIn("g_arch[q].id == replyId", render)
        resend = source.split("// resend the newest failed DM in place", 1)[1].split("} else if (tab)", 1)[0]
        self.assertIn("sendTextPacket(selectedNum, m.text, 0, g_msgReply[idx])", resend)
        self.assertIn("g_hilLastTxReply = p->decoded.reply_id", source)
        self.assertNotIn("g_hilLastTxReply = replyId", source)
        runner = (ROOT / "scripts/hil.py").read_text()
        self.assertIn('"tx_reply": "00001001"', runner)

    def test_settings_cover_engine_regions_roles_and_intentional_power(self):
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        regions = source.split("const EnumOpt kRegionOpts[]", 1)[1].split("const EnumOpt kPresetOpts[]", 1)[0]
        for value in range(27):
            self.assertRegex(regions, rf'\{{"[A-Z0-9_]+",\s*{value}\}}')
        roles = source.split("const EnumOpt kRoleOpts2[]", 1)[1].split("const EnumOpt kHopOpts[]", 1)[0]
        for value in range(13):
            self.assertRegex(roles, rf'\{{"[A-Za-z &]+",\s*{value}\}}')
        power = source.split("const EnumOpt kPowerOpts[]", 1)[1].split("const EnumOpt kRebroadOpts[]", 1)[0]
        self.assertIn('{"26 dBm", 26}', power)
        self.assertIn("return -1; // never let an unknown current value", source)
        open_setting = source.split("void AdvUI::openSetting(int item)", 1)[1].split("void AdvUI::handleKey", 1)[0]
        self.assertIn("if (index < 0)", open_setting)
        self.assertIn("refusing %s picker for unsupported current value", open_setting)

    def test_long_settings_editor_keeps_cursor_visible(self):
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        editor = source.split("void AdvUI::drawSetName()", 1)[1].split("bool AdvUI::applyName()", 1)[0]
        self.assertIn("char field[sizeof(nameBuf) + 2]", editor)
        self.assertIn("while (shown[0] && g->textWidth(shown) > 228)", editor)
        self.assertIn("shown += utf8Decode(shown).bytes", editor)

    def test_advisory_epoch_write_cannot_overlap_a_message_burst(self):
        source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        epoch = source.split("void epochNote()", 1)[1].split("uint32_t savedDateEpoch()", 1)[0]
        self.assertNotIn("SafeFile f(", epoch)
        self.assertIn("FSCom.open(kEpochPath, FILE_O_WRITE)", epoch)
        self.assertIn("saved == now", epoch)
        scheduler = source.split("static uint32_t lastEpochNoteMs", 1)[1].split("clockSeen", 1)[0]
        self.assertIn("if (!g_msgsDirty &&", scheduler)

    def test_parse_fields(self):
        parsed = hil.parse_fields("@@STATE v=1 mode=chats heap=123", "@@STATE")
        self.assertEqual(parsed, {"v": "1", "mode": "chats", "heap": "123"})

    def test_hil_marker_survives_interleaved_log_prefix(self):
        session = object.__new__(hil.HilSession)
        session.serial = mock.Mock()
        session.serial.readline.return_value = b"DEBUG partial write @@ACK clear\n"
        self.assertEqual(session.line_until("@@ACK clear", 0.1), "@@ACK clear")

    def test_exchange_backs_off_after_a_lost_usb_reply(self):
        session = object.__new__(hil.HilSession)
        session.serial = mock.Mock()
        session.send = mock.Mock()
        session.line_until = mock.Mock(side_effect=[hil.HilError("lost"), "@@ACK clear"])
        with mock.patch.object(hil.time, "sleep") as sleep:
            self.assertEqual(session.exchange(b"E", "@@ACK clear", attempts=2), "@@ACK clear")
        sleep.assert_called_once_with(0.15)
        self.assertEqual(session.send.call_count, 2)

    def test_demo_row_loss_is_drained_and_classified_for_retry(self):
        session = object.__new__(hil.HilSession)
        session.serial = mock.Mock()
        session.serial.readline.side_effect = [
            b"@@SHOT splash 2 2\n",
            b"0001\n",
            b"@@END\n",
            b"@@DONE\n",
        ]
        session.send = mock.Mock()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(hil.DemoCaptureTransportError, "received 1/2 rows"):
                session.demo_frames(Path(directory), 1)
        self.assertEqual(session.serial.readline.call_count, 4)

    def test_visual_retries_one_transport_loss_after_a_proven_reboot(self):
        capture_one = mock.MagicMock()
        capture_one.query.return_value = {"node": "00000001", "backend": "onboard", "boot": "one"}
        capture_one.demo_frames.side_effect = hil.DemoCaptureTransportError("lost row")
        verify_one = mock.MagicMock()
        verify_one.wait_state.return_value = {"boot": "two"}
        capture_two = mock.MagicMock()
        capture_two.query.return_value = {"node": "00000001", "backend": "onboard", "boot": "two"}
        frames = [
            {"name": name, "sha256": f"{index:064x}", "colors": 4}
            for index, name in enumerate(hil.EXPECTED_DEMO_FRAMES, 1)
        ]
        capture_two.demo_frames.return_value = frames
        verify_two = mock.MagicMock()
        verify_two.wait_state.return_value = {"boot": "three"}
        sessions = [capture_one, verify_one, capture_two, verify_two]
        for session in sessions:
            session.__enter__.return_value = session

        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(
                    hil,
                    "resolve_role",
                    return_value={"port": "/dev/dut", "usb_serial": "02:11:22:33:44:55"},
                ),
                mock.patch.object(hil, "HilSession", side_effect=sessions),
                mock.patch.object(hil, "wait_for_usb_serial", return_value={"port": "/dev/dut"}),
                mock.patch.object(hil.time, "sleep"),
            ):
                report = hil.visual({"schema": 2}, Path(directory), 1)

        self.assertEqual(report.failed, 0)
        capture_one.demo_frames.assert_called_once()
        capture_two.demo_frames.assert_called_once()

    def test_query_keeps_selected_and_companion_channel_fields_distinct(self):
        session = object.__new__(hil.HilSession)
        session.serial = mock.Mock()
        session.expected_boot = None
        session.serial.readline.side_effect = [
            b"@@STATE v=2 boot=12345678 channel=-1\n",
            b"@@STATS v=2 heap=13000\n",
            b"@@LAST v=2 ch=0\n",
            b"@@COMP my=00000001 channel_present=1\n",
            b"@@CHEAP buffers=1 ingress_first=0 ingress_last=0 ingress_count=0 ingress_pre=0 ingress_direct=0 ingress_max=0\n",
        ]
        state = session.query(timeout=0.1)
        self.assertEqual(state["channel"], "-1")
        self.assertEqual(state["channel_present"], "1")

    def test_query_fails_immediately_when_device_reboots_unexpectedly(self):
        session = object.__new__(hil.HilSession)
        session.expected_boot = "11111111"
        session.send = mock.Mock()
        session.line_until = mock.Mock(
            side_effect=[
                "@@STATE v=2 boot=22222222 reset=7 channel=-1",
                "@@STATS v=2 heap=13000",
                "@@LAST v=2 ch=0",
                "@@COMP my=00000001 channel_present=0",
                "@@CHEAP buffers=1 ingress_first=0 ingress_last=0 ingress_count=0 ingress_pre=0 ingress_direct=0 ingress_max=0",
            ]
        )
        with self.assertRaisesRegex(hil.UnexpectedReboot, "reset_reason=7"):
            session.query(timeout=0.1)

    def test_reaction_state_is_an_on_demand_short_exchange(self):
        session = object.__new__(hil.HilSession)
        session.exchange = mock.Mock(
            return_value="@@REACT v=1 react_exact=2 react_ambiguous=0 react_last_target=00000002"
        )
        state = session.reaction_state()
        self.assertEqual(state["react_exact"], "2")
        self.assertEqual(state["react_last_target"], "00000002")
        session.exchange.assert_called_once_with(b"R", "@@REACT")

    def test_companion_release_requires_positive_firmware_ack(self):
        session = object.__new__(hil.HilSession)
        session.exchange = mock.Mock(return_value="@@BRELEASE v=1 ok=0 freed=0")
        with self.assertRaisesRegex(hil.HilError, "field mismatch"):
            session.release_companion_state()

    def test_legacy_store_requires_loaded_message_and_reaction(self):
        session = object.__new__(hil.HilSession)
        session.exchange = mock.Mock(return_value="@@LEGACY v=1 ok=1 messages=1 reactions=1")
        self.assertEqual(session.load_legacy_store()["ok"], "1")
        session.exchange.assert_called_once_with(b"M", "@@LEGACY")

    def test_destructive_legacy_fixture_runs_after_steady_state_gates(self):
        source = (ROOT / "scripts/hil.py").read_text()
        body = source.split("def message_flow(", 1)[1].split("\ndef visual(", 1)[0]
        archive_gate = body.index('report.check("storage/live-ring-to-flash-archive"')
        reboot_gate = body.index('report.check("storage/reboot-recovery"')
        legacy_fixture = body.index(
            'report.check("storage/legacy-avs4-reaction-migrates-conservatively"'
        )
        self.assertLess(archive_gate, legacy_fixture)
        self.assertLess(reboot_gate, legacy_fixture)

    def test_report_records_then_propagates_unexpected_reboot(self):
        report = hil.Report("reboot")
        with mock.patch("builtins.print"), self.assertRaises(hil.UnexpectedReboot):
            report.check("case", lambda: (_ for _ in ()).throw(hil.UnexpectedReboot("reset")))
        self.assertEqual(report.failed, 1)

    def test_normalize_serial(self):
        self.assertEqual(hil.normalize_serial("02-11-22-33-44-55"), "02:11:22:33:44:55")

    def test_public_hil_evidence_redacts_usb_identity_and_port(self):
        text = "MAC: 02:11:22:33:44:55 Serial port /dev/cu.usbmodem1101"
        redacted = hil.redact_lab_details(text)
        self.assertEqual(redacted, "MAC: <usb-identity> Serial port <serial-port>")
        evidence = hil.dut_evidence(
            {
                "kind": "cardputer",
                "usb_serial": "02:11:22:33:44:55",
                "port": "/dev/cu.usbmodem1101",
                "expected_region": "US",
                "expected_tx_power": 26,
            }
        )
        self.assertEqual(
            evidence,
            {
                "role": "dut",
                "kind": "cardputer",
                "identity_verified": True,
                "expected_region": "US",
                "expected_tx_power": 26,
            },
        )

    def test_public_hil_report_redacts_dut_node_but_keeps_synthetic_nodes(self):
        report = hil.Report("privacy")
        report.protect_node_id("1234abcd")
        report.cases.append(
            hil.Case(
                "identity",
                "passed",
                0,
                data={"node": "1234abcd", "to": "1234ABCD", "fixture_source": "000beef1"},
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            report.write(Path(directory), {"note": "dut !1234abcd"})
            payload = (Path(directory) / "report.json").read_text()
        self.assertNotIn("1234abcd", payload.lower())
        self.assertIn("<dut-node>", payload)
        self.assertIn("000beef1", payload)

    def test_config_fingerprint_uses_exact_dut_and_private_temporary_export(self):
        device = {"port": "/dev/cu.usbmodem1101", "usb_serial": "02:11:22:33:44:55"}

        def export(command, **kwargs):
            destination = Path(command[command.index("--out") + 1])
            destination.mkdir(parents=True)
            config = destination / "config-fixture.yaml"
            config.write_text("complete fixture")
            config.chmod(0o600)
            self.assertEqual(kwargs["env"]["MESHTASTIC_BACKUP_ATTEMPTS"], "4")
            self.assertEqual(kwargs["env"]["MESHTASTIC_BACKUP_TIMEOUT"], "90")
            self.assertEqual(kwargs["timeout"], 480)
            return mock.Mock(returncode=0)

        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(hil, "resolve_role", return_value=device),
                mock.patch.dict(hil.os.environ, {}, clear=True),
                mock.patch.object(hil.subprocess, "run", side_effect=export) as run,
            ):
                digest = hil.capture_config_fingerprint({}, Path(directory), "before")

        self.assertEqual(len(digest), 32)
        self.assertEqual(run.call_count, 2)
        destinations = [
            call.args[0][call.args[0].index("--out") + 1]
            for call in run.call_args_list
        ]
        self.assertEqual(len(set(destinations)), 2)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--port") + 1], device["port"])
        self.assertIs(run.call_args.kwargs["stdout"], hil.subprocess.DEVNULL)
        self.assertIs(run.call_args.kwargs["stderr"], hil.subprocess.DEVNULL)

    def test_config_fingerprint_requires_consecutive_matching_exports(self):
        payloads = iter((b"first", b"second", b"second"))

        def export(command, **_kwargs):
            destination = Path(command[command.index("--out") + 1])
            destination.mkdir(parents=True)
            config = destination / "config-fixture.yaml"
            config.write_bytes(next(payloads))
            config.chmod(0o600)
            return mock.Mock(returncode=0)

        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(hil, "resolve_role", return_value={"port": "/dev/dut"}),
                mock.patch.object(hil.subprocess, "run", side_effect=export) as run,
            ):
                digest = hil.capture_config_fingerprint({}, Path(directory), "before")

        self.assertEqual(digest, hashlib.sha256(b"second").digest())
        self.assertEqual(run.call_count, 3)

    def test_config_fingerprint_rejects_three_different_exports(self):
        payloads = iter((b"first", b"second", b"third"))

        def export(command, **_kwargs):
            destination = Path(command[command.index("--out") + 1])
            destination.mkdir(parents=True)
            config = destination / "config-fixture.yaml"
            config.write_bytes(next(payloads))
            config.chmod(0o600)
            return mock.Mock(returncode=0)

        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(hil, "resolve_role", return_value={"port": "/dev/dut"}),
                mock.patch.object(hil.subprocess, "run", side_effect=export),
            ):
                with self.assertRaisesRegex(hil.HilError, "was not stable"):
                    hil.capture_config_fingerprint({}, Path(directory), "after")

    def test_config_fingerprint_ignores_only_live_top_level_location(self):
        first = b"""channel_url: secret\nconfig:\n  lora:\n    region: US\nlocation:\n  lat: 1.0\n  lon: 2.0\n  alt: 3\nmodule_config:\n  mqtt:\n    enabled: false\nowner: fixture\n"""
        moved = first.replace(
            b"location:\n  lat: 1.0\n  lon: 2.0\n  alt: 3\n",
            b"location:\n  lat: 4.0\n  lon: 5.0\n  alt: 6\n",
        )
        changed_config = moved.replace(b"region: US", b"region: EU_868")
        nested_location = moved.replace(
            b"    enabled: false", b"    enabled: false\n    location: configured"
        )

        baseline = hil.stable_config_payload(first)
        self.assertEqual(baseline, hil.stable_config_payload(moved))
        self.assertNotEqual(baseline, hil.stable_config_payload(changed_config))
        self.assertNotEqual(baseline, hil.stable_config_payload(nested_location))
        self.assertNotIn(b"\nlocation:\n", b"\n" + baseline)

    def test_fromradio_text_fixture_matches_wire_schema(self):
        frame = hil.make_text_frame(0x01020304, 0xA0B0C0D0, 0x11223344, b"hi", channel=2, reply_id=9)
        self.assertEqual(
            frame.hex(),
            "121e0d0403020115d0c0b0a01802220b0801120268693d090000003544332211",
        )

    def test_routing_none_keeps_explicit_oneof_tag(self):
        frame = hil.make_routing_frame(1, 2, 0x11223344, 0)
        self.assertIn(bytes.fromhex("12021800"), frame)
        self.assertIn(bytes.fromhex("3544332211"), frame)

    def test_routing_route_discovery_is_not_encoded_as_an_ack(self):
        frame = hil.make_routing_frame(1, 2, 0x11223344, 0, variant=1)
        self.assertIn(bytes.fromhex("12020a00"), frame)
        self.assertNotIn(bytes.fromhex("1800"), frame)
        with self.assertRaises(hil.HilError):
            hil.make_routing_frame(1, 2, 3, 0, variant=4)

    def test_queue_status_fixture_matches_fromradio_schema(self):
        frame = hil.make_queue_status_frame(32, 0x11223344)
        self.assertEqual(frame.hex(), "5a0c08201000181020c4e6888901")

    def test_encrypted_fixture_uses_meshpacket_encrypted_oneof(self):
        frame = hil.make_encrypted_frame(1, 2, 3, b"ciphertext", channel=4)
        self.assertIn(bytes.fromhex("2a0a63697068657274657874"), frame)
        self.assertNotIn(bytes.fromhex("220a63697068657274657874"), frame)

    def test_text_fixture_can_pin_receive_time(self):
        frame = hil.make_text_frame(1, 2, 3, "x", rx_time=0x11223344)
        self.assertIn(bytes.fromhex("3d44332211"), frame)

    def test_companion_config_stream_fixtures_match_wire_schema(self):
        self.assertEqual(hil.make_companion_my_info(0x1234).hex(), "1a0308b424")
        self.assertEqual(hil.make_companion_config_complete(0x1234).hex(), "38b424")
        node = hil.make_companion_node_info(
            7, long_name="Node", short_name="N", hops=2, battery=87, public_key=bytes(32)
        )
        self.assertTrue(node.startswith(bytes.fromhex("22")))
        self.assertIn(b"Node", node)
        self.assertIn(bytes.fromhex("32020857"), node)

    def test_protobuf_builder_rejects_out_of_range_fixed32(self):
        with self.assertRaises(hil.HilError):
            hil.make_text_frame(0x1_0000_0000, 1, 1, "bad")

    def test_fnv_matches_firmware_vector(self):
        self.assertEqual(hil.fnv1a32(b"A?B"), "5e7e7f91")

    def test_heap_headroom_gate(self):
        self.assertEqual(
            hil.require_heap_headroom({"heap": "13000", "min_heap": "12000"}),
            {"heap": 13000, "min_heap": 12000},
        )
        with self.assertRaises(hil.HilError):
            hil.require_heap_headroom({"heap": "11999", "min_heap": "12000"})

    def test_feature_heap_gate_uses_named_current_samples(self):
        self.assertEqual(
            hil.require_heap_fields(
                {"heap": "13000", "before": "12500", "min_heap": "1000"},
                ("heap", "before"),
            ),
            {"heap": 13000, "before": 12500},
        )
        with self.assertRaises(hil.HilError):
            hil.require_heap_fields({"heap": "13000", "before": "11999"}, ("heap", "before"))

    def test_companion_heap_gate_uses_sustained_burst_envelope(self):
        physical = {
            "heap": "47412",
            "ingress_first": "47276",
            "ingress_pre": "47412",
            "ingress_last": "47412",
            "ingress_direct": "-2328",
            "ingress_max": "2328",
        }
        self.assertEqual(hil.require_companion_ingress_heap(physical)["ingress_last"], 47412)
        with self.assertRaises(hil.HilError):
            hil.require_companion_ingress_heap({**physical, "ingress_last": "46763"})

    def test_scoped_heap_gate_ignores_only_an_older_lifetime_low(self):
        polluted = {
            "heap": "15000", "save_before": "14000", "save_after": "13000",
            "min_before": "2500", "min_after": "2500",
        }
        values = hil.require_scoped_heap_headroom(
            polluted, ("heap", "save_before", "save_after"), "min_before", "min_after"
        )
        self.assertEqual(values["min_after"], 2500)

        new_trough = {**polluted, "min_before": "13000", "min_after": "11000"}
        with self.assertRaises(hil.HilError):
            hil.require_scoped_heap_headroom(
                new_trough, ("heap", "save_before", "save_after"), "min_before", "min_after"
            )

    def test_persist_rejects_failed_device_commit(self):
        session = object.__new__(hil.HilSession)
        session.exchange = mock.Mock(return_value="@@ACK persist ok=0 messages=12")
        with self.assertRaisesRegex(hil.HilError, "field mismatch"):
            session.persist()

    def test_fixture_validation(self):
        fixture = {
            "schema": 1,
            "devices": {
                "dut": {"usb_serial": "AA-BB-CC-DD-EE-FF"},
                "peer": {"usb_serial": "11:22:33:44:55:66"},
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            path.write_text(json.dumps(fixture))
            loaded = hil.load_fixture(path)
        self.assertEqual(loaded["devices"]["dut"]["usb_serial"], "AA:BB:CC:DD:EE:FF")

    def test_same_device_is_rejected(self):
        fixture = {
            "schema": 1,
            "devices": {
                "dut": {"usb_serial": "AA:BB"},
                "peer": {"usb_serial": "AA:BB"},
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            path.write_text(json.dumps(fixture))
            with self.assertRaises(hil.HilError):
                hil.load_fixture(path)

    def test_schema_two_defaults_wifi_to_read_only(self):
        fixture = {
            "schema": 2,
            "devices": {"dut": {"usb_serial": "AA:BB"}},
            "protected_devices": [{"usb_serial": "CC:DD"}],
            "wifi_peers": [{"name": "node-a", "host": "192.0.2.1"}],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            path.write_text(json.dumps(fixture))
            loaded = hil.load_fixture(path)
        self.assertEqual(loaded["wifi_peers"][0]["access"], "read-only")

    def test_fixture_normalizes_expected_us_profile(self):
        fixture = {
            "schema": 2,
            "devices": {
                "dut": {
                    "usb_serial": "AA:BB",
                    "expected_region": "us",
                    "expected_tx_power": 26,
                }
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            path.write_text(json.dumps(fixture))
            loaded = hil.load_fixture(path)
        self.assertEqual(loaded["devices"]["dut"]["expected_region"], "US")

    def test_schema_two_rejects_unknown_access_policy(self):
        fixture = {
            "schema": 2,
            "devices": {"dut": {"usb_serial": "AA:BB"}},
            "wifi_peers": [{"host": "192.0.2.1", "access": "write"}],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            path.write_text(json.dumps(fixture))
            with self.assertRaises(hil.HilError):
                hil.load_fixture(path)

    def test_full_run_attempts_restore_after_hil_flash_error(self):
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory) / "artifacts"
            with (
                mock.patch.object(hil, "stage_release_image", return_value=(Path("/staged.bin"), "a" * 64)),
                mock.patch.object(hil, "capture_config_fingerprint", return_value=b"same"),
                mock.patch.object(
                    hil, "flash", side_effect=[hil.HilError("upload failed"), {"role": "dut"}]
                ) as flash,
            ):
                with self.assertRaises(hil.HilError):
                    hil.full_run({"schema": 1}, artifacts, timeout=1, skip_build=True)
            self.assertEqual([call.kwargs["release"] for call in flash.call_args_list], [False, True])
            summary = json.loads((artifacts / "summary.json").read_text())
            self.assertTrue(summary["hil_flash_attempted"])
            self.assertTrue(summary["production_restored"])
            self.assertTrue(summary["configuration_preserved"])
            self.assertEqual(summary["failure_type"], "HilError")
            self.assertIsNone(summary["restore_failure_type"])
            self.assertIsNone(summary["configuration_check_failure_type"])

    def test_full_run_records_test_and_restore_failure_types(self):
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory) / "artifacts"
            with (
                mock.patch.object(hil, "stage_release_image", return_value=(Path("/staged.bin"), "b" * 64)),
                mock.patch.object(hil, "capture_config_fingerprint", return_value=b"before"),
                mock.patch.object(
                    hil,
                    "flash",
                    side_effect=[hil.HilError("upload failed"), hil.HilError("restore failed")],
                ),
            ):
                with self.assertRaisesRegex(hil.HilError, "restore failed"):
                    hil.full_run({"schema": 1}, artifacts, timeout=1, skip_build=True)
            summary = json.loads((artifacts / "summary.json").read_text())
            self.assertTrue(summary["hil_flash_attempted"])
            self.assertFalse(summary["production_restored"])
            self.assertIsNone(summary["configuration_preserved"])
            self.assertEqual(summary["failure_type"], "HilError")
            self.assertEqual(summary["restore_failure_type"], "HilError")

    def test_full_run_fails_when_restored_node_configuration_drifted(self):
        report = mock.Mock(failed=0)
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory) / "artifacts"
            with (
                mock.patch.object(hil, "stage_release_image", return_value=(Path("/staged.bin"), "c" * 64)),
                mock.patch.object(
                    hil, "capture_config_fingerprint", side_effect=[b"before", b"after"]
                ),
                mock.patch.object(hil, "flash", return_value={"role": "dut"}),
                mock.patch.object(hil, "smoke", return_value=report),
                mock.patch.object(hil, "message_flow", return_value=report),
                mock.patch.object(hil, "visual", return_value=report),
            ):
                with self.assertRaisesRegex(hil.HilError, "configuration changed"):
                    hil.full_run({"schema": 1}, artifacts, timeout=1, skip_build=True)

            summary = json.loads((artifacts / "summary.json").read_text())
            self.assertTrue(summary["hil_flash_attempted"])
            self.assertTrue(summary["production_restored"])
            self.assertFalse(summary["configuration_preserved"])
            self.assertEqual(summary["configuration_check_failure_type"], "HilError")
            self.assertNotIn("configuration_sha", summary)
            self.assertNotIn("configuration_path", summary)

    def test_full_run_does_not_flash_without_verified_configuration_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory) / "artifacts"
            with (
                mock.patch.object(hil, "stage_release_image", return_value=(Path("/staged.bin"), "d" * 64)),
                mock.patch.object(
                    hil, "capture_config_fingerprint", side_effect=hil.HilError("backup failed")
                ),
                mock.patch.object(hil, "flash") as flash,
            ):
                with self.assertRaisesRegex(hil.HilError, "backup failed"):
                    hil.full_run({"schema": 1}, artifacts, timeout=1, skip_build=True)

            flash.assert_not_called()
            summary = json.loads((artifacts / "summary.json").read_text())
            self.assertFalse(summary["hil_flash_attempted"])
            self.assertFalse(summary["production_restored"])
            self.assertIsNone(summary["configuration_preserved"])
            self.assertEqual(summary["configuration_check_failure_type"], "HilError")

    def test_external_release_image_requires_valid_esp_app(self):
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / "not-an-app.bin"
            bad.write_bytes(b"wrong")
            with self.assertRaisesRegex(hil.HilError, "invalid ESP magic"):
                hil.validate_app_image(bad)

    def test_release_image_is_staged_outside_mutable_build_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "firmware.bin"
            source.write_bytes(b"\xe9" + b"\0" * (64 * 1024 - 1))
            staging = root / "private"
            staging.mkdir()
            staged, digest = hil.stage_release_image(source, staging)
            source.unlink()
            self.assertEqual(staged.read_bytes()[:1], b"\xe9")
            self.assertEqual(digest, hil.hashlib.sha256(staged.read_bytes()).hexdigest())
            self.assertEqual(staged.stat().st_mode & 0o777, 0o600)

    def test_chip_identity_uses_esptool_5_command_spelling(self):
        with (
            mock.patch.object(hil, "find_esptool", return_value=["esptool"]),
            mock.patch.object(
                hil,
                "run_captured",
                return_value="Connecting...\nMAC: 02:11:22:33:44:55\n",
            ) as run,
        ):
            self.assertEqual(hil.chip_mac("/dev/cardputer"), "02:11:22:33:44:55")
        command = run.call_args.args[0]
        self.assertIn("read-mac", command)
        self.assertNotIn("read_mac", command)

    def test_esptool_prefers_the_hil_interpreters_pinned_module(self):
        with (
            mock.patch.dict(hil.os.environ, {}, clear=True),
            mock.patch.object(hil.importlib.util, "find_spec", return_value=object()),
            mock.patch.object(hil.shutil, "which") as which,
        ):
            self.assertEqual(hil.find_esptool(), [hil.sys.executable, "-m", "esptool"])
        which.assert_not_called()

    def test_flash_uses_esptool_with_explicit_verified_port(self):
        device = {
            "role": "dut",
            "kind": "cardputer",
            "usb_serial": "02:11:22:33:44:55",
            "port": "/dev/cu.usbmodem1101",
        }
        image = Path("/tmp/cardputer-release.bin")
        with (
            mock.patch.object(hil, "resolve_role", return_value=device),
            mock.patch.object(hil, "chip_mac", return_value=device["usb_serial"]),
            mock.patch.object(hil, "wait_for_usb_serial", return_value=device),
            mock.patch.object(hil, "firmware_image", return_value=image),
            mock.patch.object(hil, "find_esptool", return_value=["esptool"]),
            mock.patch.object(hil, "run_checked") as run,
            mock.patch.object(hil, "kick_device", return_value=device),
        ):
            result = hil.flash({"devices": {"dut": device}}, release=True)

        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(len(commands), 2)
        self.assertIn("write-flash", commands[0])
        self.assertIn("verify-flash", commands[1])
        self.assertIn(device["port"], commands[0])
        self.assertNotIn("pio", commands[0])
        self.assertEqual(result["chip_mac"], device["usb_serial"])

    def test_flash_stops_before_write_on_chip_mac_mismatch(self):
        device = {
            "role": "dut",
            "usb_serial": "02:11:22:33:44:55",
            "port": "/dev/cu.usbmodem1101",
        }
        with (
            mock.patch.object(hil, "resolve_role", return_value=device),
            mock.patch.object(hil, "chip_mac", return_value="02:AA:BB:CC:DD:EE"),
            mock.patch.object(hil, "run_checked") as run,
        ):
            with self.assertRaises(hil.HilError):
                hil.flash({"devices": {"dut": device}}, release=True)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
