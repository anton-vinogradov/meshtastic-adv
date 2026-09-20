// Run the production SD transaction code against a fault-injecting filesystem.
#include "../../overlay/src/advui/AdvProfile.cpp"
namespace advui { bool sdFontUsesSd() { return profileTestFontMounted; } }
using namespace advui;

static void seedInternal(uint8_t value)
{
    for (const auto &spec : kSpecs) {
        if (!spec.required)
            continue;
        auto file = FSCom.open(spec.path, FILE_WRITE);
        assert(file.write(&value, 1) == 1);
    }
}

static void reset()
{
    FSCom = {};
    SD = {};
    g_restoreNeeded = g_started = false;
    g_dirty = false;
    g_apiConfigCount = g_apiQuietSince = 0;
    g_profileStorageBusy = false;
    g_lastSyncAttemptMs = g_lastRestoreAttemptMs = 0;
    profileTestNow = 100000;
    profileTestFontMounted = false;
}

int main()
{
    reset();
    seedInternal(42);
    assert(syncProfile());
    FSCom.remove(kMarkerPath);
    seedInternal(0); // defaults after reinstall
    SD.readFault = true;
    assert(attemptRestore().result == RestoreResult::Unavailable);
    assert(!readInternalMarker(nullptr));
    assert(!syncProfile());
    SD.readFault = false;
    SD.failReadOpen = true; // stat succeeds, but File cannot be allocated/opened
    assert(attemptRestore().result == RestoreResult::Unavailable);
    assert(!syncProfile() && !readInternalMarker(nullptr));
    SD.failReadOpen = false;
    SD.shortReads = true;
    assert(attemptRestore().result == RestoreResult::Unavailable);
    assert(!syncProfile() && !readInternalMarker(nullptr));
    SD.shortReads = false;
    assert(attemptRestore().result == RestoreResult::Restored);
    uint8_t restored = 0;
    auto file = FSCom.open(kSpecs[0].path, FILE_READ);
    assert(file.read(&restored, 1) == 1 && restored == 42);

    reset();
    assert(attemptRestore().result == RestoreResult::Initialized);
    assert(readInternalMarker(nullptr));

    reset();
    seedInternal(42);
    assert(writeInternalMarker(1));
    advProfileCaptureBootState();
    advProfileRestoreIfNeeded();
    profileTestNow += kQuietMs + 1;
    SD.throwOnOpen = true;
    advProfileSyncTick();
    assert(!SD.mounted && SD.ends == 1);
    assert(g_dirty && g_lastSyncAttemptMs == millis());

    reset();
    SD.throwOnBegin = true;
    try { attemptRestore(); assert(false); } catch (const std::bad_alloc &) {}
    assert(!SD.mounted && SD.ends == 1); // partial mount is owned before begin

    reset();
    profileTestFontMounted = true;
    SD.mounted = true;
    assert(attemptRestore().result == RestoreResult::Initialized);
    assert(SD.mounted && SD.ends == 0); // do not tear down a font-owned mount

    reset();
    seedInternal(42);
    assert(writeInternalMarker(0));
    FSCom.failMarkerRename = true;
    assert(!syncProfile());
    InternalMarker marker = {};
    assert(readInternalMarker(&marker) && marker.generation == 0);
    FSCom.failMarkerRename = false;
    assert(syncProfile());
    assert(readInternalMarker(&marker) && marker.generation == 1);
    // Even if the marker disappears independently, unchanged content repairs it.
    FSCom.remove(kMarkerPath);
    assert(syncProfile());
    assert(readInternalMarker(&marker) && marker.generation == 1);
    std::puts("profile transaction fault tests: OK");
}
