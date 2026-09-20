#pragma once

#include <cstddef>
#include <cstdint>
#include <strings.h>

namespace advui
{

struct NodeOrder {
    uint32_t num;
    uint32_t lastHeard;
    uint8_t hops;
    bool unread;
    bool favourite;
};

// Identical ordering for local and companion snapshots. Names are supplied at
// comparison time so moving a snapshot never leaves self-referential pointers.
inline bool nodeOrderLess(const NodeOrder &a, const char *nameA, const NodeOrder &b, const char *nameB, int mode)
{
    if (a.unread != b.unread)
        return a.unread;
    if (mode == 2) {
        const int order = strcasecmp(nameA, nameB);
        if (order)
            return order < 0;
    } else {
        if (mode == 0 && a.favourite != b.favourite)
            return a.favourite;
        if (mode != 1 && a.hops != b.hops)
            return a.hops < b.hops;
        if (a.lastHeard != b.lastHeard)
            return a.lastHeard > b.lastHeard;
    }
    return a.num < b.num;
}

template <typename T, typename Less> void insertSortedBounded(T *items, int &count, int capacity, T item, Less less)
{
    if (!items || capacity <= 0)
        return;
    int position = count;
    while (position > 0 && less(item, items[position - 1]))
        position--;
    if (position >= capacity)
        return;
    if (count < capacity)
        count++;
    for (int move = count - 1; move > position; move--)
        items[move] = items[move - 1];
    items[position] = item;
}

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
