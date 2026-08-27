#include "AdvStorage.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace advui
{

bool checkedRecordRegion(size_t count, size_t recordBytes, size_t tailCount, size_t tailBytes, size_t available,
                         size_t *recordsTotal, bool allowTrailing)
{
    if (!recordBytes || !tailBytes || count > std::numeric_limits<size_t>::max() / recordBytes ||
        tailCount > std::numeric_limits<size_t>::max() / tailBytes)
        return false;
    const size_t head = count * recordBytes;
    const size_t tail = tailCount * tailBytes;
    if (head > std::numeric_limits<size_t>::max() - tail)
        return false;
    const size_t total = head + tail;
    if (total > available || (!allowTrailing && total != available))
        return false;
    if (recordsTotal)
        *recordsTotal = total;
    return true;
}

bool checkedFixedRecordFile(size_t fileBytes, size_t headerBytes, size_t recordBytes, size_t *recordCount)
{
    if (!recordBytes || fileBytes < headerBytes)
        return false;
    const size_t payload = fileBytes - headerBytes;
    if (payload % recordBytes)
        return false;
    if (recordCount)
        *recordCount = payload / recordBytes;
    return true;
}

bool validUtcOffsetMinutes(int32_t minutes)
{
    return minutes >= -12 * 60 && minutes <= 14 * 60;
}

bool deadlineReached(uint32_t now, uint32_t deadline)
{
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t armDeadline(uint32_t now, uint32_t delay)
{
    const uint32_t deadline = now + delay;
    return deadline ? deadline : 1U;
}

bool validBleAddress(const char *address)
{
    if (!address)
        return false;
    for (size_t i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (address[i] != ':')
                return false;
        } else {
            const char c = address[i];
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex)
                return false;
        }
    }
    return address[17] == '\0';
}

bool parseFrequencyOverride(const char *text, float *mhz)
{
    if (!text || !mhz)
        return false;
    if (!text[0]) {
        *mhz = 0.0f;
        return true;
    }
    bool digit = false, dot = false;
    for (const char *p = text; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            digit = true;
        } else if (*p == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digit)
        return false;
    char *end = nullptr;
    const float value = strtof(text, &end);
    if (!end || *end || !std::isfinite(value) || (value != 0.0f && (value < 150.0f || value > 960.0f)))
        return false;
    *mhz = value;
    return true;
}

bool parseClockHm(const char *text, int *hour, int *minute)
{
    if (!text || !hour || !minute)
        return false;
    const size_t len = strlen(text);
    int hh = 0, mm = 0;
    if (len == 4 && strspn(text, "0123456789") == 4) {
        hh = (text[0] - '0') * 10 + text[1] - '0';
        mm = (text[2] - '0') * 10 + text[3] - '0';
    } else {
        const char *colon = strchr(text, ':');
        if (!colon || strchr(colon + 1, ':'))
            return false;
        const size_t left = (size_t)(colon - text), right = len - left - 1;
        if (left < 1 || left > 2 || right < 1 || right > 2)
            return false;
        for (size_t i = 0; i < left; i++) {
            if (text[i] < '0' || text[i] > '9')
                return false;
            hh = hh * 10 + text[i] - '0';
        }
        for (size_t i = 1; i <= right; i++) {
            if (colon[i] < '0' || colon[i] > '9')
                return false;
            mm = mm * 10 + colon[i] - '0';
        }
    }
    if (hh > 23 || mm > 59)
        return false;
    *hour = hh;
    *minute = mm;
    return true;
}

bool validStoredEpoch(uint32_t epoch)
{
    return epoch >= 1600000000U && epoch <= 4102444800U;
}

void considerOldestSlot(OldestSlot *slots, size_t capacity, size_t *count, uint32_t position, uint32_t stamp)
{
    if (!slots || !count || !capacity)
        return;
    if (*count < capacity) {
        slots[(*count)++] = {position, stamp};
        return;
    }
    size_t newest = 0;
    for (size_t i = 1; i < capacity; i++)
        if (slots[i].stamp > slots[newest].stamp ||
            (slots[i].stamp == slots[newest].stamp && slots[i].position > slots[newest].position))
            newest = i;
    if (stamp < slots[newest].stamp || (stamp == slots[newest].stamp && position < slots[newest].position))
        slots[newest] = {position, stamp};
}

} // namespace advui
