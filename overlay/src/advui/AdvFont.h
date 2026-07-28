#pragma once

#include <cstdint>

namespace advui
{

// Full-Unicode (BMP) glyphs streamed from /unifont.bin on the SD card — the
// "AUF1" repack of GNU Unifont (see scripts/mkunifont.py): 65536 fixed 33-byte
// records, so a glyph is one seek at 4 + cp * 33. Probed once at startup;
// with no card or no file everything here is a cheap no-op and the UI keeps
// its flash-font behaviour.

void sdFontInit(); // map the flash partition if present (call after the SPI bus is up)
bool sdFontReady();

// With no font partition (M5Launcher installs, bare esptool flashes) the card
// can still carry the font — but mounting it costs ~32 KB of heap, which on
// this board is most of the free memory: WiFi's web server and MQTT stop being
// able to allocate. So it is opt-in, from Settings > Device > Font.
bool sdFontOffered();     // no partition, but the SD path is available
void sdFontLoadFromSd();  // mount the card and stream glyphs from it
void sdFontUnload();      // release the card and its memory again

// Copies the 16-row bitmap into out (MSB-first rows: halfwidth 1 byte/row in
// out[0..15], fullwidth 2 bytes/row in out[0..31]) and returns the pixel width
// (8 or 16); 0 when the glyph — or the whole font — is unavailable.
int sdGlyph(uint32_t cp, uint8_t *out);
int sdGlyphWidth(uint32_t cp); // measuring only: cache-backed, no bitmap copy
const char *sdFontState();     // "flash" / "sd" / "off" / an error tag, for Settings

} // namespace advui
