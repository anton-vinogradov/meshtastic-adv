#pragma once
#include <cstdint>

namespace advui
{
// U+00C0..U+017F: Latin-1 letters and Latin Extended-A, always in flash.
// MSB-first 8x16 bitmap. Unavailable codepoints return zero without touching out.
int latinGlyph(uint32_t cp, uint8_t *out);
int latinGlyphWidth(uint32_t cp);
}
