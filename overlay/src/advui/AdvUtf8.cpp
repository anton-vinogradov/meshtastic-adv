#include "AdvUtf8.h"

#include <cstring>

namespace advui
{

namespace
{

bool continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

} // namespace

Utf8Rune utf8Decode(const char *s)
{
    if (!s || !s[0])
        return {0, 0, true};

    const unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80)
        return {c0, 1, true};

    // Lead-byte ranges below exclude overlong encodings, UTF-16 surrogates,
    // and codepoints beyond U+10FFFF before looking at continuation bytes.
    if (c0 >= 0xC2 && c0 <= 0xDF) {
        const unsigned char c1 = (unsigned char)s[1];
        if (s[1] && continuation(c1))
            return {uint32_t((c0 & 0x1F) << 6) | uint32_t(c1 & 0x3F), 2, true};
    } else if (c0 >= 0xE0 && c0 <= 0xEF) {
        const unsigned char c1 = (unsigned char)s[1];
        if (!s[1])
            return {'?', 1, false};
        const unsigned char c2 = (unsigned char)s[2];
        const bool scalar = (c0 != 0xE0 || c1 >= 0xA0) && (c0 != 0xED || c1 <= 0x9F);
        if (s[2] && continuation(c1) && continuation(c2) && scalar)
            return {uint32_t((c0 & 0x0F) << 12) | uint32_t((c1 & 0x3F) << 6) | uint32_t(c2 & 0x3F), 3,
                    true};
    } else if (c0 >= 0xF0 && c0 <= 0xF4) {
        const unsigned char c1 = (unsigned char)s[1];
        if (!s[1])
            return {'?', 1, false};
        const unsigned char c2 = (unsigned char)s[2];
        if (!s[2])
            return {'?', 1, false};
        const unsigned char c3 = (unsigned char)s[3];
        const bool scalar = (c0 != 0xF0 || c1 >= 0x90) && (c0 != 0xF4 || c1 <= 0x8F);
        if (s[3] && continuation(c1) && continuation(c2) && continuation(c3) && scalar)
            return {uint32_t((c0 & 0x07) << 18) | uint32_t((c1 & 0x3F) << 12) |
                        uint32_t((c2 & 0x3F) << 6) | uint32_t(c3 & 0x3F),
                    4, true};
    }

    return {'?', 1, false};
}

size_t utf8CopyValid(char *out, size_t outCap, const char *s)
{
    if (!out || outCap == 0)
        return 0;
    if (!s) {
        out[0] = 0;
        return 0;
    }

    size_t read = 0, written = 0;
    while (s[read]) {
        Utf8Rune r = utf8Decode(s + read);
        const size_t need = r.valid ? r.bytes : 1;
        if (written + need >= outCap)
            break;
        if (r.valid)
            memcpy(out + written, s + read, r.bytes);
        else
            out[written] = '?';
        read += r.bytes;
        written += need;
    }
    out[written] = 0;
    return written;
}

void utf8Sanitize(char *s, size_t cap)
{
    if (!s || cap == 0)
        return;
    s[cap - 1] = 0;
    size_t read = 0, written = 0;
    while (s[read]) {
        Utf8Rune r = utf8Decode(s + read);
        const size_t need = r.valid ? r.bytes : 1;
        if (written + need >= cap)
            break;
        if (r.valid) {
            if (written != read)
                memmove(s + written, s + read, r.bytes);
        } else {
            s[written] = '?';
        }
        read += r.bytes;
        written += need;
    }
    s[written] = 0;
}

bool utf8TrimLast(char *s)
{
    if (!s || !s[0])
        return false;
    size_t pos = 0, last = 0;
    while (s[pos]) {
        last = pos;
        Utf8Rune r = utf8Decode(s + pos);
        pos += r.bytes;
    }
    s[last] = 0;
    return true;
}

} // namespace advui
