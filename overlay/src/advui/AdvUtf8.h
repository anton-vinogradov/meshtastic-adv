#pragma once

#include <cstddef>
#include <cstdint>

namespace advui
{

struct Utf8Rune {
    uint32_t codepoint;
    uint8_t bytes;
    bool valid;
};

// Decode one NUL-terminated UTF-8 codepoint. Invalid input consumes one byte,
// which guarantees callers always make progress without reading past the NUL.
Utf8Rune utf8Decode(const char *s);

// Copy a string into a byte-limited destination without splitting codepoints.
// Invalid input bytes become '?' so malformed radio/persistence data never
// reaches the renderer. Returns the number of output bytes (excluding NUL).
size_t utf8CopyValid(char *out, size_t outCap, const char *s);

// Repair malformed UTF-8 in place and guarantee a trailing NUL within cap.
void utf8Sanitize(char *s, size_t cap);

// Remove the final complete codepoint. Returns false for an already-empty string.
bool utf8TrimLast(char *s);

} // namespace advui
