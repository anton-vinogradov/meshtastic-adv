#pragma once

namespace advui
{

// Once the bounded mirror is full, a live node_info for an absent node can be
// either a genuinely new identity or an existing DB entry that was evicted.
// Treat the completed config stream as the authoritative total rather than
// letting repeated live updates inflate it without bound.
constexpr bool shouldIncrementNodesSeen(bool configDone, int slot, int nodeCount, int capacity)
{
    // Duplicate transport delivery must be idempotent during the initial
    // config stream too. Once the mirror is full, only that stream can prove
    // an absent identity is another DB entry; live updates stay conservative.
    return slot < 0 && (!configDone || nodeCount < capacity);
}

} // namespace advui
