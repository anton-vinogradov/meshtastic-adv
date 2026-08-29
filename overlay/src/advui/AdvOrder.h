#pragma once

#include <cstddef>

namespace advui
{

// Move matching entries to the front without heap allocation while preserving
// the relative order of both groups. The channel picker has at most eight
// entries, so bounded insertion moves are safer than std::stable_sort's
// optional temporary heap buffer on a fragmented ESP32.
template <typename T, typename Predicate> void stableMoveMatchingFirst(T *items, size_t count, Predicate matches)
{
    if (!items)
        return;
    size_t front = 0;
    for (size_t index = 0; index < count; index++) {
        if (!matches(items[index]))
            continue;
        const T item = items[index];
        for (size_t move = index; move > front; move--)
            items[move] = items[move - 1];
        items[front++] = item;
    }
}

} // namespace advui
