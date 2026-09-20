#pragma once

#include "AdvStorage.h"
#include <cstddef>
#include <cstdint>

namespace advui
{

// Caller supplies synchronization. Fixed capacity is also the admission limit:
// failures cannot overflow a separate result queue and silently disappear.
template <size_t Capacity> struct PendingSends {
    struct Entry { uint32_t id, deadline; uint8_t error; } entries[Capacity];

    bool add(uint32_t id, uint32_t now, uint32_t timeoutMs)
    {
        if (!id)
            return false;
        for (const auto &entry : entries)
            if (entry.id == id)
                return false;
        for (auto &entry : entries)
            if (!entry.id) {
                entry = {id, armDeadline(now, timeoutMs), 0};
                return true;
            }
        return false;
    }
    void forget(uint32_t id)
    {
        for (auto &entry : entries)
            if (entry.id == id)
                entry = {};
    }
    void fail(uint32_t id, uint8_t error)
    {
        for (auto &entry : entries)
            if (entry.id && (!id || entry.id == id))
                entry.error = error;
    }
    bool popFailure(uint32_t now, uint8_t timeoutError, uint32_t *id, uint8_t *error)
    {
        for (auto &entry : entries) {
            if (entry.id && (entry.error || deadlineReached(now, entry.deadline))) {
                *id = entry.id;
                *error = entry.error ? entry.error : timeoutError;
                entry = {};
                return true;
            }
        }
        return false;
    }
};

} // namespace advui
