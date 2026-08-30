import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ProfileSyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = (ROOT / "overlay/src/advui/AdvProfile.cpp").read_text()
        cls.ui = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()
        cls.patch = (ROOT / "overlay/patches/nodedb-profile-sync.patch").read_text()
        cls.sync = (ROOT / "scripts/sync-overlay.sh").read_text()

    def test_profile_is_device_bound_versioned_and_rotated(self):
        for token in (
            'kArchiveMagic = 0x31505641',
            'kArchiveVersion = 1',
            'ESP.getEfuseMac()',
            '"profile-a.bin", "profile-b.bin", "profile-c.bin"',
            'header.deviceId != deviceId()',
            'header.generation == 0',
            'header.headerCrc != headerCrc(header)',
            'header.bodyCrc != crc32Final(body)',
            '(entry.flags & ~kRequired)',
            'file.position() == header.totalSize',
            'latest.generation == UINT32_MAX',
        ):
            self.assertIn(token, self.profile)

    def test_profile_contains_settings_but_not_messages_or_node_cache(self):
        for path in (
            "/prefs/device.proto",
            "/prefs/config.proto",
            "/prefs/module.proto",
            "/prefs/channels.proto",
            "/prefs/uiconfig.proto",
            "/advui_ui.bin",
            "/advui_radio.bin",
        ):
            self.assertIn(f'"{path}"', self.profile)
        for excluded in (
            "/prefs/nodes.proto",
            "/advui_msgs.bin",
            "/advui_hist.bin",
            "/advui_seen.bin",
            "/advui_epoch.bin",
            "/advui_bleatt",
        ):
            self.assertNotIn(excluded, self.profile)

    def test_clean_install_never_overwrites_unavailable_or_corrupt_backup(self):
        restore = self.profile.split("RestoreAttempt attemptRestore()", 1)[1].split(
            "bool syncProfile()", 1
        )[0]
        self.assertIn("if (!mountSd(&owned))", restore)
        self.assertIn("RestoreResult::Unavailable", restore)
        self.assertIn("anySlot ? RestoreResult::Corrupt", restore)
        self.assertIn("writeInternalMarker(0) ? RestoreResult::Initialized", restore)
        self.assertLess(
            restore.index("if (!newestArchive"),
            restore.index("writeInternalMarker(0)"),
        )
        tick = self.profile.split("void advProfileSyncTick()", 1)[1]
        self.assertLess(tick.index("if (g_restoreNeeded)"), tick.index("if (!g_dirty.load"))
        self.assertIn("refusing to overwrite them", tick)

    def test_restore_stages_and_rolls_back_before_marker_commit(self):
        restore = self.profile.split("bool restoreArchive", 1)[1].split(
            "RestoreAttempt attemptRestore", 1
        )[0]
        self.assertIn('restorePath(staged, sizeof(staged), kSpecs[i], "restore")', restore)
        self.assertIn('restorePath(rollback, sizeof(rollback), kSpecs[i], "rollback")', restore)
        self.assertIn("committed = committed && writeInternalMarker(header.generation)", restore)
        self.assertLess(restore.index("stageEntry("), restore.index("writeInternalMarker("))
        self.assertIn("FSCom.rename(rollback, kSpecs[i].path)", restore)

    def test_every_settings_writer_marks_profile_dirty(self):
        self.assertIn("advProfileMarkDirty();", self.patch)
        self.assertIn("advuiFavouriteChanged(nodeId, is_favorite);", self.patch)
        self.assertIn("nodedb-profile-sync.patch", self.sync)
        self.assertIn("advProfileMarkDirty();", self.ui)
        self.assertIn("advProfileRestoreIfNeeded();", self.ui)
        self.assertIn("advProfileSyncTick();", self.ui)

    def test_sd_profile_and_phoneapi_config_streams_are_serialized(self):
        for token in (
            "g_apiConfigCount",
            "g_profileStorageBusy",
            "ProfileStorageGuard",
            "advProfileApiConfigStart",
            "advProfileApiConfigEnd",
            "g_apiQuietSince",
        ):
            self.assertIn(token, self.profile)
        for token in (
            "advui::advProfileApiConfigStart()",
            "advui::advProfileApiConfigEnd()",
            "advProfileConfigActive",
        ):
            self.assertIn(token, self.patch + (ROOT / "overlay/patches/phoneapi-profile-gate.patch").read_text())
        self.assertIn("phoneapi-profile-gate.patch", self.sync)

    def test_success_is_not_throttled_and_concurrent_dirty_state_survives(self):
        mark = self.profile.split("void advProfileMarkDirty()", 1)[1].split(
            "void advProfileApiConfigStart()", 1
        )[0]
        self.assertLess(mark.index("g_dirtySince.store"), mark.index("g_dirty.store"))
        tick = self.profile.split("void advProfileSyncTick()", 1)[1]
        self.assertLess(tick.index("g_dirty.exchange(false"), tick.index("syncProfile()"))
        success = tick.split("if (syncProfile())", 1)[1].split(
            "} catch (const std::bad_alloc &)", 1
        )[0]
        self.assertIn("g_lastSyncAttemptMs = 0", success)
        self.assertIn("g_dirty.store(true", success)
        failure = tick.split("catch (const std::bad_alloc &)", 1)[1]
        self.assertIn("g_dirty.store(true", failure)
        self.assertIn("g_lastSyncAttemptMs = millis()", failure)

    def test_portable_ui_config_owns_all_adv_preferences_and_favourites(self):
        save = self.ui.split("bool saveUiCfg()", 1)[1].split("void persistUiCfg()", 1)[0]
        for field in (
            "g_nodeSort",
            "g_battRest",
            "g_nameShort",
            "g_extRot",
            "g_favChannels",
            "g_ruMode",
            "g_utcOffsetMin",
            "g_screenOffSec",
            "g_favNodeCount",
            "g_favNodes[i]",
        ):
            self.assertIn(field, save)
        self.assertIn("constexpr int kMaxFavNodes = 150", self.ui)
        self.assertIn("if (!g_uiCfgPortable)", self.ui)
        self.assertIn("migrateDbFavourites();", self.ui)

    def test_favourite_repeat_follows_the_original_identity(self):
        selection = self.ui.split("void AdvUI::favSelectedEntry", 1)[1].split(
            "// Collects one entry", 1
        )[0]
        self.assertLess(selection.index("const uint32_t node"), selection.index("favEntry(sel, on)"))
        self.assertLess(selection.index("favEntry(sel, on)"), selection.index("rebuildFiltered()"))
        self.assertIn("nodeNumAt(i) == node", selection)
        hil = (ROOT / "scripts/hil.py").read_text()
        self.assertIn("ui/favourite-key-repeat-keeps-single-target", hil)
        self.assertGreaterEqual(hil.count('session.key("<", {"mode": "nodes", "fav_nodes": "1"})'), 2)
        self.assertGreaterEqual(hil.count('session.key(">", {"mode": "nodes", "fav_nodes": "0"'), 2)


if __name__ == "__main__":
    unittest.main()
