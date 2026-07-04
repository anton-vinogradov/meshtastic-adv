#pragma once

#include <cstdint>

// u8g2 9x15 bitmap font covering ASCII + Cyrillic, so received Russian message
// text renders (the GFX/Font0 fonts are ASCII-only). Data lives in
// CyrillicFont.cpp; wrap it with lgfx::U8g2font to use as a LovyanGFX font.
extern const uint8_t u8g2_font_9x15_t_cyrillic[];
