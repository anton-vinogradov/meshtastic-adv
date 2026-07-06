#pragma once

#include <cstdint>

namespace advui
{

// Full-Unicode (BMP) glyphs streamed from /unifont.bin on the SD card — the
// "AUF1" repack of GNU Unifont (see scripts/mkunifont.py): 65536 fixed 33-byte
// records, so a glyph is one seek at 4 + cp * 33. Probed once at startup;
// with no card or no file everything here is a cheap no-op and the UI keeps
// its flash-font behaviour.

void sdFontInit(); // probe the card + file once (call after the SPI bus is up)
bool sdFontReady();

// Copies the 16-row bitmap into out (MSB-first rows: halfwidth 1 byte/row in
// out[0..15], fullwidth 2 bytes/row in out[0..31]) and returns the pixel width
// (8 or 16); 0 when the glyph — or the whole font — is unavailable.
int sdGlyph(uint32_t cp, uint8_t *out);
int sdGlyphWidth(uint32_t cp); // measuring only: cache-backed, no bitmap copy
const char *sdFontState();     // "flash" / "sd" / "off" / an error tag, for Settings

} // namespace advui
