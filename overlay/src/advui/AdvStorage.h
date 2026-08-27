#pragma once

#include <cstddef>
#include <cstdint>

namespace advui
{

// Validate a counted record region without overflowing size_t. `available`
// includes both record groups and may be larger only when allowTrailing is true.
bool checkedRecordRegion(size_t count, size_t recordBytes, size_t tailCount, size_t tailBytes, size_t available,
                         size_t *recordsTotal, bool allowTrailing = false);

// Validate a file made of a fixed header followed by whole fixed-size records.
// A torn final append must be rejected instead of silently shifting every later record.
bool checkedFixedRecordFile(size_t fileBytes, size_t headerBytes, size_t recordBytes, size_t *recordCount);

// Civil time-zone offsets currently range from UTC-12 through UTC+14. Keeping
// persisted values in that envelope also makes seconds conversion overflow-safe.
bool validUtcOffsetMinutes(int32_t minutes);

// Rollover-safe test for a short millis()-based deadline. Zero remains the
// conventional "not armed" sentinel.
bool deadlineReached(uint32_t now, uint32_t deadline);
uint32_t armDeadline(uint32_t now, uint32_t delay);

// Rollover-safe quiet-time debounce for a flash write.
constexpr bool dirtyWriteDue(uint32_t now, uint32_t lastChange, uint32_t quietDelay)
{
    return now - lastChange >= quietDelay;
}

// Canonical six-octet Bluetooth address (xx:xx:xx:xx:xx:xx). Persisted radio
// state is untrusted input after a torn/corrupt filesystem write.
bool validBleAddress(const char *address);

// Strict parsers for the on-device numeric editors. They reject prefixes with
// ignored trailing characters; an empty/zero frequency intentionally means
// auto, otherwise the Cardputer ADV's SX1262 range is 150..960 MHz.
bool parseFrequencyOverride(const char *text, float *mhz);
bool parseClockHm(const char *text, int *hour, int *minute);

// Supported persisted wall-clock range: Sep 2020 through 2100-01-01. Values
// outside it are corruption (or post-uint32 rollover), not useful timestamps.
bool validStoredEpoch(uint32_t epoch);

// Keep a bounded set of the oldest replaceable flash-record slots. This lets a
// saturated recency ledger admit fresh identities without allocating in the UI
// loop or rewriting the whole file.
struct OldestSlot {
    uint32_t position;
    uint32_t stamp;
};
void considerOldestSlot(OldestSlot *slots, size_t capacity, size_t *count, uint32_t position, uint32_t stamp);

} // namespace advui
