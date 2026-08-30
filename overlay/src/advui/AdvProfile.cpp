#include "AdvProfile.h"

#include "AdvFont.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "Throttle.h"
#include "configuration.h"
#include <ErriezCRC32.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <esp_system.h>
#include <new>

namespace advui
{

#ifndef ADVUI_HIL
namespace
{

constexpr int kSdCs = 12;
constexpr uint32_t kSdHz = 16000000;
constexpr uint32_t kArchiveMagic = 0x31505641; // "AVP1"
constexpr uint16_t kArchiveVersion = 1;
constexpr uint32_t kMarkerMagic = 0x314D5641; // "AVM1"
constexpr uint16_t kMarkerVersion = 1;
constexpr uint32_t kQuietMs = 8000;
constexpr uint32_t kRetryMs = 5 * 60 * 1000UL;
constexpr size_t kBufferSize = 512;
const char *kMarkerPath = "/advui_profile_v1";
const char *kMarkerTmpPath = "/advui_profile_v1.tmp";

struct __attribute__((packed)) ArchiveHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t entryCount;
    uint64_t deviceId;
    uint32_t generation;
    uint32_t totalSize;
    uint32_t sourceCrc;
    uint32_t bodyCrc;
    uint32_t headerCrc;
};

struct __attribute__((packed)) WireEntry {
    uint8_t id;
    uint8_t flags;
    uint16_t reserved;
    uint32_t length;
    uint32_t crc;
};

struct __attribute__((packed)) InternalMarker {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t deviceId;
    uint32_t generation;
    uint32_t crc;
};

struct EntrySpec {
    uint8_t id;
    const char *path;
    uint32_t maxSize;
    bool required;
};

constexpr uint8_t kRequired = 1;
const EntrySpec kSpecs[] = {
    {1, "/prefs/device.proto", 64 * 1024, true},
    {2, "/prefs/config.proto", 64 * 1024, true},
    {3, "/prefs/module.proto", 64 * 1024, true},
    {4, "/prefs/channels.proto", 16 * 1024, true},
    {5, "/prefs/uiconfig.proto", 16 * 1024, false},
    {6, "/advui_ui.bin", 1024, true},
    {7, "/advui_radio.bin", 256, false},
};
constexpr size_t kSpecCount = sizeof(kSpecs) / sizeof(kSpecs[0]);
constexpr const char *kSlotNames[] = {"profile-a.bin", "profile-b.bin", "profile-c.bin"};
constexpr size_t kSlotCount = sizeof(kSlotNames) / sizeof(kSlotNames[0]);

struct EntryState {
    const EntrySpec *spec;
    uint32_t length;
    uint32_t crc;
};

std::atomic<bool> g_dirty{false};
std::atomic<uint32_t> g_dirtySince{0};
std::atomic<uint32_t> g_apiConfigCount{0};
std::atomic<uint32_t> g_apiQuietSince{0};
std::atomic<bool> g_profileStorageBusy{false};
bool g_restoreNeeded = false;
bool g_started = false;
uint32_t g_lastRestoreAttemptMs = 0;
uint32_t g_lastSyncAttemptMs = 0;

// A config stream announces itself before it allocates its large protobuf
// buffers and waits for an already-running profile transaction. Conversely,
// profile work claims this guard and then rechecks the stream count. That
// closes both races without holding a Meshtastic mutex across SD I/O.
class ProfileStorageGuard
{
  public:
    ProfileStorageGuard()
    {
        if (g_apiConfigCount.load(std::memory_order_acquire))
            return;
        bool expected = false;
        if (!g_profileStorageBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;
        if (g_apiConfigCount.load(std::memory_order_acquire)) {
            g_profileStorageBusy.store(false, std::memory_order_release);
            return;
        }
        active = true;
    }

    ~ProfileStorageGuard()
    {
        if (active)
            g_profileStorageBusy.store(false, std::memory_order_release);
    }

    explicit operator bool() const { return active; }

  private:
    bool active = false;
};

enum class RestoreResult : uint8_t {
    Unavailable,
    Initialized,
    Corrupt,
    Failed,
    Restored,
};

struct RestoreAttempt {
    RestoreResult result;
    uint32_t generation;
};

uint64_t deviceId()
{
    return ESP.getEfuseMac() & 0x0000FFFFFFFFFFFFULL;
}

uint32_t markerCrc(InternalMarker marker)
{
    marker.crc = 0;
    return crc32Buffer(&marker, sizeof(marker));
}

uint32_t headerCrc(ArchiveHeader header)
{
    header.headerCrc = 0;
    return crc32Buffer(&header, sizeof(header));
}

bool readExact(File &file, void *buffer, size_t size)
{
    return file.read(static_cast<uint8_t *>(buffer), size) == static_cast<int>(size);
}

bool writeExact(File &file, const void *buffer, size_t size)
{
    return file.write(static_cast<const uint8_t *>(buffer), size) == size;
}

const EntrySpec *findSpec(uint8_t id)
{
    for (const EntrySpec &spec : kSpecs)
        if (spec.id == id)
            return &spec;
    return nullptr;
}

bool readInternalMarker(InternalMarker *out)
{
    auto file = FSCom.open(kMarkerPath, FILE_READ);
    InternalMarker marker = {};
    const bool ok = file && readExact(file, &marker, sizeof(marker)) && marker.magic == kMarkerMagic &&
                    marker.version == kMarkerVersion && marker.deviceId == deviceId() && marker.crc == markerCrc(marker);
    if (file)
        file.close();
    if (ok && out)
        *out = marker;
    return ok;
}

bool writeInternalMarker(uint32_t generation)
{
    InternalMarker marker = {kMarkerMagic, kMarkerVersion, 0, deviceId(), generation, 0};
    marker.crc = markerCrc(marker);
    FSCom.remove(kMarkerTmpPath);
    auto file = FSCom.open(kMarkerTmpPath, FILE_WRITE);
    const bool written = file && writeExact(file, &marker, sizeof(marker));
    if (file) {
        file.flush();
        file.close();
    }
    if (!written) {
        FSCom.remove(kMarkerTmpPath);
        return false;
    }
    FSCom.remove(kMarkerPath);
    if (!FSCom.rename(kMarkerTmpPath, kMarkerPath)) {
        FSCom.remove(kMarkerTmpPath);
        return false;
    }
    return true;
}

void profileDirectory(char *out, size_t size)
{
    snprintf(out, size, "/meshtastic-adv/%012llx", static_cast<unsigned long long>(deviceId()));
}

void profilePath(char *out, size_t size, const char *name)
{
    char directory[48];
    profileDirectory(directory, sizeof(directory));
    snprintf(out, size, "%s/%s", directory, name);
}

bool mountSd(bool *owned)
{
    *owned = !sdFontUsesSd();
    if (!*owned || SD.begin(kSdCs, SPI, kSdHz))
        return true;
    SD.end();
    return false;
}

void unmountSd(bool owned)
{
    if (owned)
        SD.end();
}

bool ensureProfileDirectory()
{
    char directory[48];
    profileDirectory(directory, sizeof(directory));
    return (SD.exists("/meshtastic-adv") || SD.mkdir("/meshtastic-adv")) &&
           (SD.exists(directory) || SD.mkdir(directory));
}

bool hashFile(fs::FS &filesystem, const char *path, uint32_t maxSize, uint32_t *length, uint32_t *crc)
{
    auto file = filesystem.open(path, FILE_READ);
    if (!file)
        return false;
    const size_t size = file.size();
    if (size > maxSize || size > UINT32_MAX) {
        file.close();
        return false;
    }
    uint8_t buffer[kBufferSize];
    uint32_t running = CRC32_INITIAL;
    size_t consumed = 0;
    while (consumed < size) {
        const size_t wanted = std::min(sizeof(buffer), size - consumed);
        const int got = file.read(buffer, wanted);
        if (got != static_cast<int>(wanted)) {
            file.close();
            return false;
        }
        running = crc32Update(buffer, wanted, running);
        consumed += wanted;
    }
    file.close();
    *length = static_cast<uint32_t>(size);
    *crc = crc32Final(running);
    return true;
}

bool collectEntryStates(EntryState *states, uint32_t *sourceCrc)
{
    uint32_t running = CRC32_INITIAL;
    for (size_t i = 0; i < kSpecCount; i++) {
        states[i] = {&kSpecs[i], 0, 0};
        const bool exists = hashFile(FSCom, kSpecs[i].path, kSpecs[i].maxSize, &states[i].length, &states[i].crc);
        if (!exists && kSpecs[i].required)
            return false;
        const WireEntry entry = {kSpecs[i].id, static_cast<uint8_t>(kSpecs[i].required ? kRequired : 0), 0,
                                 states[i].length, states[i].crc};
        running = crc32Update(&entry, sizeof(entry), running);
    }
    *sourceCrc = crc32Final(running);
    return true;
}

bool validateArchive(fs::FS &filesystem, const char *path, ArchiveHeader *result, bool requireDevice)
{
    auto file = filesystem.open(path, FILE_READ);
    ArchiveHeader header = {};
    if (!file || !readExact(file, &header, sizeof(header)) || header.magic != kArchiveMagic ||
        header.version != kArchiveVersion || header.entryCount != kSpecCount ||
        header.generation == 0 || (requireDevice && header.deviceId != deviceId()) ||
        header.headerCrc != headerCrc(header) ||
        header.totalSize != file.size()) {
        if (file)
            file.close();
        return false;
    }

    bool seen[kSpecCount] = {};
    uint32_t body = CRC32_INITIAL;
    uint8_t buffer[kBufferSize];
    for (size_t i = 0; i < header.entryCount; i++) {
        WireEntry entry = {};
        if (!readExact(file, &entry, sizeof(entry))) {
            file.close();
            return false;
        }
        body = crc32Update(&entry, sizeof(entry), body);
        const EntrySpec *spec = findSpec(entry.id);
        const size_t index = spec ? static_cast<size_t>(spec - kSpecs) : kSpecCount;
        if (!spec || seen[index] || (entry.flags & ~kRequired) || entry.reserved || entry.length > spec->maxSize ||
            (spec->required && !(entry.flags & kRequired)) || (!spec->required && (entry.flags & kRequired))) {
            file.close();
            return false;
        }
        seen[index] = true;
        uint32_t itemCrc = CRC32_INITIAL;
        uint32_t remaining = entry.length;
        while (remaining) {
            const size_t wanted = std::min(sizeof(buffer), static_cast<size_t>(remaining));
            if (!readExact(file, buffer, wanted)) {
                file.close();
                return false;
            }
            itemCrc = crc32Update(buffer, wanted, itemCrc);
            body = crc32Update(buffer, wanted, body);
            remaining -= wanted;
        }
        if (entry.crc != crc32Final(itemCrc)) {
            file.close();
            return false;
        }
    }
    const bool exactEnd = file.position() == header.totalSize;
    file.close();
    if (!exactEnd)
        return false;
    for (bool value : seen)
        if (!value)
            return false;
    if (header.bodyCrc != crc32Final(body))
        return false;
    if (result)
        *result = header;
    return true;
}

bool newestArchive(char *path, size_t pathSize, ArchiveHeader *header)
{
    bool found = false;
    ArchiveHeader newest = {};
    char candidate[80];
    for (const char *slot : kSlotNames) {
        profilePath(candidate, sizeof(candidate), slot);
        ArchiveHeader current = {};
        if (validateArchive(SD, candidate, &current, true) && (!found || current.generation > newest.generation)) {
            found = true;
            newest = current;
            snprintf(path, pathSize, "%s", candidate);
        }
    }
    if (found && header)
        *header = newest;
    return found;
}

bool copyInternalToArchive(File &archive, const EntryState &state, uint32_t *bodyCrc)
{
    const WireEntry entry = {state.spec->id, static_cast<uint8_t>(state.spec->required ? kRequired : 0), 0,
                             state.length, state.crc};
    if (!writeExact(archive, &entry, sizeof(entry)))
        return false;
    *bodyCrc = crc32Update(&entry, sizeof(entry), *bodyCrc);
    if (!state.length)
        return true;
    auto source = FSCom.open(state.spec->path, FILE_READ);
    if (!source)
        return false;
    uint8_t buffer[kBufferSize];
    uint32_t remaining = state.length;
    while (remaining) {
        const size_t wanted = std::min(sizeof(buffer), static_cast<size_t>(remaining));
        if (!readExact(source, buffer, wanted) || !writeExact(archive, buffer, wanted)) {
            source.close();
            return false;
        }
        *bodyCrc = crc32Update(buffer, wanted, *bodyCrc);
        remaining -= wanted;
    }
    source.close();
    return true;
}

bool writeArchive(const EntryState *states, uint32_t sourceCrc, uint32_t generation)
{
    if (!ensureProfileDirectory())
        return false;
    char tmp[80];
    profilePath(tmp, sizeof(tmp), "profile.tmp");
    SD.remove(tmp);
    auto archive = SD.open(tmp, FILE_WRITE);
    ArchiveHeader header = {kArchiveMagic, kArchiveVersion, static_cast<uint16_t>(kSpecCount), deviceId(), generation,
                            0, sourceCrc, 0, 0};
    bool ok = archive && writeExact(archive, &header, sizeof(header));
    uint32_t bodyCrc = CRC32_INITIAL;
    for (size_t i = 0; ok && i < kSpecCount; i++)
        ok = copyInternalToArchive(archive, states[i], &bodyCrc);
    if (ok) {
        header.totalSize = archive.position();
        header.bodyCrc = crc32Final(bodyCrc);
        header.headerCrc = headerCrc(header);
        ok = archive.seek(0) && writeExact(archive, &header, sizeof(header));
    }
    if (archive) {
        archive.flush();
        archive.close();
    }
    ArchiveHeader verified = {};
    ok = ok && validateArchive(SD, tmp, &verified, true) && verified.sourceCrc == sourceCrc;
    if (!ok) {
        SD.remove(tmp);
        return false;
    }

    size_t targetIndex = 0;
    uint32_t oldestGeneration = UINT32_MAX;
    char candidate[80];
    for (size_t i = 0; i < kSlotCount; i++) {
        profilePath(candidate, sizeof(candidate), kSlotNames[i]);
        ArchiveHeader current = {};
        if (!validateArchive(SD, candidate, &current, true)) {
            targetIndex = i;
            oldestGeneration = 0;
            break;
        }
        if (current.generation < oldestGeneration) {
            oldestGeneration = current.generation;
            targetIndex = i;
        }
    }
    char target[80];
    profilePath(target, sizeof(target), kSlotNames[targetIndex]);
    SD.remove(target);
    if (!SD.rename(tmp, target)) {
        SD.remove(tmp);
        return false;
    }
    return validateArchive(SD, target, nullptr, true);
}

void restorePath(char *out, size_t size, const EntrySpec &spec, const char *suffix)
{
    snprintf(out, size, "%s.%s", spec.path, suffix);
}

void cleanRestoreArtifacts()
{
    char path[96];
    for (const EntrySpec &spec : kSpecs) {
        restorePath(path, sizeof(path), spec, "restore");
        FSCom.remove(path);
        restorePath(path, sizeof(path), spec, "rollback");
        FSCom.remove(path);
    }
}

bool stageEntry(File &archive, const WireEntry &entry, const EntrySpec &spec)
{
    char tmp[96];
    restorePath(tmp, sizeof(tmp), spec, "restore");
    FSCom.remove(tmp);
    auto target = FSCom.open(tmp, FILE_WRITE);
    if (!target)
        return false;
    uint8_t buffer[kBufferSize];
    uint32_t remaining = entry.length;
    bool ok = true;
    while (remaining && ok) {
        const size_t wanted = std::min(sizeof(buffer), static_cast<size_t>(remaining));
        ok = readExact(archive, buffer, wanted) && writeExact(target, buffer, wanted);
        remaining -= ok ? wanted : 0;
    }
    target.flush();
    target.close();
    uint32_t length = 0, crc = 0;
    ok = ok && hashFile(FSCom, tmp, spec.maxSize, &length, &crc) && length == entry.length && crc == entry.crc;
    if (!ok) {
        FSCom.remove(tmp);
        return false;
    }
    return true;
}

bool restoreArchive(const char *path, const ArchiveHeader &header)
{
    auto archive = SD.open(path, FILE_READ);
    if (!archive || !archive.seek(sizeof(ArchiveHeader)))
        return false;
    cleanRestoreArtifacts();
    bool present[kSpecCount] = {};
    for (size_t i = 0; i < header.entryCount; i++) {
        WireEntry entry = {};
        if (!readExact(archive, &entry, sizeof(entry))) {
            archive.close();
            cleanRestoreArtifacts();
            return false;
        }
        const EntrySpec *spec = findSpec(entry.id);
        if (!spec) {
            archive.close();
            cleanRestoreArtifacts();
            return false;
        }
        if (!entry.length) {
            if (spec->required) {
                archive.close();
                cleanRestoreArtifacts();
                return false;
            }
            continue;
        }
        if (!stageEntry(archive, entry, *spec)) {
            archive.close();
            cleanRestoreArtifacts();
            return false;
        }
        present[static_cast<size_t>(spec - kSpecs)] = true;
    }
    archive.close();

    bool hadOriginal[kSpecCount] = {};
    char staged[96], rollback[96];
    for (size_t i = 0; i < kSpecCount; i++) {
        restorePath(rollback, sizeof(rollback), kSpecs[i], "rollback");
        FSCom.remove(rollback);
        hadOriginal[i] = FSCom.exists(kSpecs[i].path);
        if (hadOriginal[i] && !FSCom.rename(kSpecs[i].path, rollback)) {
            for (size_t j = 0; j < i; j++) {
                if (!hadOriginal[j])
                    continue;
                restorePath(rollback, sizeof(rollback), kSpecs[j], "rollback");
                FSCom.rename(rollback, kSpecs[j].path);
            }
            cleanRestoreArtifacts();
            return false;
        }
    }

    bool committed = true;
    for (size_t i = 0; i < kSpecCount && committed; i++) {
        if (!present[i])
            continue;
        restorePath(staged, sizeof(staged), kSpecs[i], "restore");
        committed = FSCom.rename(staged, kSpecs[i].path);
    }
    committed = committed && writeInternalMarker(header.generation);
    if (!committed) {
        for (size_t i = 0; i < kSpecCount; i++) {
            FSCom.remove(kSpecs[i].path);
            if (!hadOriginal[i])
                continue;
            restorePath(rollback, sizeof(rollback), kSpecs[i], "rollback");
            FSCom.rename(rollback, kSpecs[i].path);
        }
        FSCom.remove(kMarkerPath);
        cleanRestoreArtifacts();
        return false;
    }
    cleanRestoreArtifacts();
    return true;
}

RestoreAttempt attemptRestore()
{
    concurrency::LockGuard guard(spiLock);
    bool owned = false;
    if (!mountSd(&owned))
        return {RestoreResult::Unavailable, 0};

    bool anySlot = false;
    char candidate[80];
    for (const char *slot : kSlotNames) {
        profilePath(candidate, sizeof(candidate), slot);
        anySlot = anySlot || SD.exists(candidate);
    }

    char path[80];
    ArchiveHeader header = {};
    if (!newestArchive(path, sizeof(path), &header)) {
        const RestoreResult result = anySlot ? RestoreResult::Corrupt
                                             : (writeInternalMarker(0) ? RestoreResult::Initialized : RestoreResult::Failed);
        unmountSd(owned);
        return {result, 0};
    }
    const bool restored = restoreArchive(path, header);
    unmountSd(owned);
    return {restored ? RestoreResult::Restored : RestoreResult::Failed, header.generation};
}

bool syncProfile()
{
    concurrency::LockGuard guard(spiLock);
    EntryState states[kSpecCount];
    uint32_t sourceCrc = 0;
    if (!collectEntryStates(states, &sourceCrc)) {
        LOG_WARN("advui: SD profile deferred; internal settings are incomplete");
        return false;
    }
    bool owned = false;
    if (!mountSd(&owned))
        return false;
    char latestPath[80];
    ArchiveHeader latest = {};
    const bool haveLatest = newestArchive(latestPath, sizeof(latestPath), &latest);
    if (haveLatest && latest.sourceCrc == sourceCrc) {
        unmountSd(owned);
        return true;
    }
    if (haveLatest && latest.generation == UINT32_MAX) {
        LOG_ERROR("advui: SD profile generation exhausted; refusing to overwrite the backup");
        unmountSd(owned);
        return false;
    }
    const uint32_t generation = haveLatest ? latest.generation + 1 : 1;
    bool ok = writeArchive(states, sourceCrc, generation);
    if (ok)
        ok = writeInternalMarker(generation);
    unmountSd(owned);
    if (ok)
        LOG_INFO("advui: settings profile synchronized to SD generation %u", (unsigned)generation);
    return ok;
}

} // namespace
#endif

void advProfileCaptureBootState()
{
#ifndef ADVUI_HIL
    try {
        concurrency::LockGuard guard(spiLock);
        InternalMarker marker = {};
        g_restoreNeeded = !readInternalMarker(&marker);
    } catch (const std::bad_alloc &) {
        g_restoreNeeded = true;
    }
#endif
}

void advProfileRestoreIfNeeded()
{
#ifndef ADVUI_HIL
    g_started = true;
    if (!g_restoreNeeded) {
        advProfileMarkDirty();
        return;
    }
    ProfileStorageGuard storage;
    if (!storage)
        return;
    g_lastRestoreAttemptMs = millis();
    try {
        const RestoreAttempt attempt = attemptRestore();
        if (attempt.result == RestoreResult::Restored) {
            g_restoreNeeded = false;
            LOG_INFO("advui: settings profile restored automatically from SD generation %u",
                     (unsigned)attempt.generation);
            delay(100);
            ESP.restart();
            return;
        }
        if (attempt.result == RestoreResult::Initialized) {
            g_restoreNeeded = false;
            advProfileMarkDirty();
            return;
        }
        if (attempt.result == RestoreResult::Corrupt)
            LOG_ERROR("advui: SD settings profiles are corrupt; refusing to overwrite them");
        else if (attempt.result == RestoreResult::Failed)
            LOG_ERROR("advui: automatic SD settings restore failed; will retry without overwriting the profile");
    } catch (const std::bad_alloc &) {
        LOG_WARN("advui: SD settings restore deferred after heap allocation failure");
    }
#endif
}

void advProfileMarkDirty()
{
#ifndef ADVUI_HIL
    g_dirtySince.store(millis(), std::memory_order_relaxed);
    // Publish the timestamp and the already-committed settings file together.
    // The sync tick consumes only this particular dirty generation; another
    // writer that arrives while SD is being streamed must leave the flag set.
    g_dirty.store(true, std::memory_order_release);
#endif
}

void advProfileApiConfigStart()
{
#ifndef ADVUI_HIL
    g_apiConfigCount.fetch_add(1, std::memory_order_acq_rel);
    g_apiQuietSince.store(millis(), std::memory_order_relaxed);
    // If profile I/O won the race, let its small atomic transaction finish
    // before PhoneAPI begins the much larger config/node stream.
    while (g_profileStorageBusy.load(std::memory_order_acquire))
        delay(1);
#endif
}

void advProfileApiConfigEnd()
{
#ifndef ADVUI_HIL
    const uint32_t previous = g_apiConfigCount.fetch_sub(1, std::memory_order_acq_rel);
    if (!previous)
        g_apiConfigCount.store(0, std::memory_order_release);
    g_apiQuietSince.store(millis(), std::memory_order_relaxed);
#endif
}

void advProfileSyncTick()
{
#ifndef ADVUI_HIL
    if (!g_started)
        return;
    if (g_restoreNeeded) {
        if (g_lastRestoreAttemptMs && Throttle::isWithinTimespanMs(g_lastRestoreAttemptMs, kRetryMs))
            return;
        ProfileStorageGuard storage;
        if (!storage)
            return;
        g_lastRestoreAttemptMs = millis();
        try {
            const RestoreAttempt attempt = attemptRestore();
            if (attempt.result == RestoreResult::Restored) {
                g_restoreNeeded = false;
                LOG_INFO("advui: settings profile restored automatically from SD generation %u",
                         (unsigned)attempt.generation);
                delay(100);
                ESP.restart();
                return;
            }
            if (attempt.result == RestoreResult::Initialized) {
                g_restoreNeeded = false;
                advProfileMarkDirty();
            } else if (attempt.result == RestoreResult::Corrupt) {
                LOG_ERROR("advui: SD settings profiles are corrupt; refusing to overwrite them");
            } else if (attempt.result == RestoreResult::Failed) {
                LOG_ERROR("advui: automatic SD settings restore failed; will retry without overwriting the profile");
            }
        } catch (const std::bad_alloc &) {
            LOG_WARN("advui: SD settings restore deferred after heap allocation failure");
        }
        return;
    }
    if (!g_dirty.load(std::memory_order_acquire))
        return;
    const uint32_t dirtySince = g_dirtySince.load(std::memory_order_relaxed);
    const uint32_t apiQuietSince = g_apiQuietSince.load(std::memory_order_relaxed);
    if (Throttle::isWithinTimespanMs(dirtySince, kQuietMs) ||
        g_apiConfigCount.load(std::memory_order_acquire) ||
        (apiQuietSince && Throttle::isWithinTimespanMs(apiQuietSince, kQuietMs)) ||
        (g_lastSyncAttemptMs && Throttle::isWithinTimespanMs(g_lastSyncAttemptMs, kRetryMs)))
        return;
    ProfileStorageGuard storage;
    if (!storage)
        return;
    // Consume the generation that made this attempt due before opening files.
    // A concurrent writer publishes a fresh `true` and therefore schedules a
    // follow-up generation instead of being erased by a successful old copy.
    g_dirty.exchange(false, std::memory_order_acq_rel);
    try {
        if (syncProfile()) {
            // kRetryMs is a failure backoff, not a minimum interval between
            // successful profiles. A new setting should still sync after the
            // documented quiet window even when the previous save was recent.
            g_lastSyncAttemptMs = 0;
        } else {
            g_dirty.store(true, std::memory_order_release);
            g_lastSyncAttemptMs = millis();
        }
    } catch (const std::bad_alloc &) {
        g_dirty.store(true, std::memory_order_release);
        g_lastSyncAttemptMs = millis();
        LOG_WARN("advui: SD profile sync deferred after heap allocation failure");
    }
#endif
}

} // namespace advui
