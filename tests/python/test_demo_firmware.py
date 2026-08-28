import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class DemoFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        cls.story = cls.source.split("// --- Usage-scenario frames", 1)[1].split(
            'Serial.println("@@DONE")', 1
        )[0]

    def test_promotional_story_uses_synthetic_identity_boundary(self):
        self.assertIn("constexpr uint32_t kDemoMe", self.source)
        self.assertIn("constexpr uint32_t kDemoPeer", self.source)
        self.assertIn('nodeSetNames(&g_demoMeNode, "Field Kit", "ME")', self.source)
        self.assertIn('nodeSetNames(&g_demoPeerNode, "TrailOps", "TOP")', self.source)
        self.assertIn("beginDemoIdentity();", self.story)
        self.assertIn("g_radioCompanion = false;", self.story)
        self.assertIn("g_favChannels = 0;", self.story)
        self.assertIn("histExit();", self.story)
        self.assertIn("sendFailure = SendFailure::NONE;", self.story)
        self.assertIn("confirmDel = false;", self.story)
        self.assertNotIn("nodeAt(", self.story)
        self.assertNotIn("nodeDB", self.story)
        self.assertNotIn("channels.", self.story)

        demo_favourite_branch = self.source.split(
            "Promotional frames must not inherit even visual metadata", 1
        )[1].split("// time (right, dim)", 1)[0]
        self.assertIn("if (g_demoActive)", demo_favourite_branch)
        self.assertLess(
            demo_favourite_branch.index("if (g_demoActive)"),
            demo_favourite_branch.index("nodeDB->isFavorite"),
        )

    def test_promotional_story_has_exact_ordered_frame_set(self):
        frames = re.findall(r'screenshot\("(a\d{2})"\)', self.story)
        self.assertEqual(frames, [f"a{index:02d}" for index in range(1, 20)])

    def test_promotional_story_covers_messenger_flow(self):
        for token in (
            "MODE_CHATS",
            "firstUnreadIdx()",
            "MODE_COMPOSE",
            "MODE_EMOJI",
            "MSG_SENDING",
            "ackMsg(",
            "MSG_IN",
            "reactStrip = true",
            "addReaction(",
            "pendingReplyId = 17",
        ):
            self.assertIn(token, self.story)

    def test_promotional_story_cannot_transmit_or_persist(self):
        for forbidden in (
            "sendTextPacket(",
            "sendMessage(",
            "sendReaction(",
            "saveMsgs(",
            "saveToDisk(",
            "FSCom.",
        ):
            self.assertNotIn(forbidden, self.story)

    def test_hil_blocks_every_physical_ble_companion_egress(self):
        ble = (ROOT / "overlay/src/advui/AdvBle.cpp").read_text()
        for signature in (
            "bool bleQueueToRadio(",
            "bool bleCompanionInit(",
            "void bleScanStart(",
            "void bleConnectAsync(",
            "bool bleWriteToRadio(",
        ):
            body = ble.split(signature, 1)[1].split("\n}", 1)[0]
            self.assertIn("#ifdef ADVUI_HIL", body, signature)

        send = self.source.split("static SendResult sendTextPacket", 1)[1].split(
            "// Remote admin over the companion link", 1
        )[0]
        self.assertIn("if (g_radioCompanion)", send)
        self.assertIn("#ifdef ADVUI_HIL", send)
        self.assertIn("ERRNO_DISABLED", send)

        admin = self.source.split("static bool sendAdminToNode", 1)[1].split(
            "// Sends a text DM", 1
        )[0]
        self.assertIn("#ifdef ADVUI_HIL", admin)
        self.assertIn("return false;", admin)

    def test_hil_never_starts_the_stock_ble_phone_api(self):
        sync = (ROOT / "scripts/sync-overlay.sh").read_text()
        guard = (
            "void setBluetoothEnable\\(bool enable\\)\\n\\{\\n)}{$1#ifdef ADVUI_HIL\n"
            "    (void)enable;\n"
            "    return; // advui-inject-hil-no-stock-ble: no BLE advertising or PhoneAPI in release HIL\n"
            "#endif\n"
        )
        self.assertEqual(sync.count("advui-inject-hil-no-stock-ble"), 3)
        self.assertIn(guard, sync)
        self.assertIn(
            'test "$(grep -c \'advui-inject-hil-no-stock-ble\' "$ESP32_MAIN" || true)" -eq 1',
            sync,
        )

        hil = (ROOT / "scripts/hil.py").read_text()
        validator = hil.split("def validate_hil_stock_ble_guard", 1)[1].split(
            "def build", 1
        )[0]
        self.assertIn('body.count("advui-inject-hil-no-stock-ble") != 1', validator)
        self.assertIn("body.count(expected) != 1", validator)
        build = hil.split("def build", 1)[1].split("def flash", 1)[0]
        self.assertLess(
            build.index("validate_hil_stock_ble_guard()"), build.index("find_pio()")
        )

    def test_promotional_frames_override_live_battery_clock_and_network_status(self):
        battery = self.source.split("void batteryText", 1)[1].split(
            "bool batteryCharging()", 1
        )[0]
        row = self.source.split("void drawNodeRow", 1)[1].split("void drawFooter", 1)[0]
        network = self.source.split("void AdvUI::drawNetPage()", 1)[1].split(
            "// Companion: scan for Meshtastic nodes", 1
        )[0]
        self.assertIn("if (g_demoActive)", battery)
        self.assertIn('snprintf(out, cap, "87%%")', battery)
        self.assertIn("if (g_demoActive)", row)
        self.assertIn('strcpy(abuf, "now")', row)
        self.assertIn("if (demoNetActive())", network)
        self.assertNotIn("g_demoNetActive", network)
        self.assertIn("#ifdef ADVUI_SCREENSHOT\n    return g_demoNetActive;", self.source)
        self.assertIn('strcpy(ln, "192.0.2.10  -55dBm")', network)


if __name__ == "__main__":
    unittest.main()
