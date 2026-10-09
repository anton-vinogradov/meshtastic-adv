"""Test actual UI glyph/measure/wrap/name paths without an SD or font partition."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()


def function(signature):
    start = source.index(signature + "\n{")
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


signatures = [
    "bool textNeedsUnicode(const char *s)",
    "int labelWidth(lgfx::LGFXBase *g, const char *s)",
    "void printLabel(lgfx::LGFXBase *g, int x, int y, const char *s, uint16_t color)",
    "void fitWidth(lgfx::LGFXBase *g, char *s, int budget)",
    "int bitmapGlyphWidth(uint32_t cp)",
    "int bitmapGlyph(uint32_t cp, uint8_t *out)",
    "int wrapLine(lgfx::LGFXBase *g, const char *s, int maxW, char *out, int outCap)",
    "bool flashFontCovers(uint32_t cp)",
    "bool cpInvisible(uint32_t cp)",
    "int lineWidthEmotes(lgfx::LGFXBase *g, const char *s)",
    "int printLineEmotes(lgfx::LGFXBase *g, int x, int y, const char *s, uint16_t color, int emojiDy)",
    "void sanitizeDisplay(char *s)",
    "const char *visibleFieldTail(lgfx::LGFXBase *g, const char *s, bool unicode, int maxWidth)",
]
subprocess.run([os.environ.get("PYTHON", "python3"), str(ROOT / "scripts/mklatinfont.py"), "--check"], check=True)
blob = (ROOT / "docs/unifont.bin").read_bytes()
sample = [0xC4, 0xC5, 0xD6, 0xE4, 0xE5, 0xF6]
expected = ",\n".join("{" + ",".join(str(n) for n in blob[5 + cp * 33:21 + cp * 33]) + "}" for cp in sample)
# Execute the real fixed-size value copies, not a duplicate of their intended
# behavior. These buffers precede pixel clipping in settings and network pages.
settings_copy = source.split("void AdvUI::drawSettings()", 1)[1].split(
    "        char vbuf[24];\n", 1)[1].split("        fitWidth", 1)[0]
network_copy = source.split("void AdvUI::drawNetPage()", 1)[1].split(
    "                v[n] = 0;\n            } else {\n", 1)[1].split("\n            }", 1)[0]
fixture = r'''
#include "AdvLatinFont.h"
#include "AdvUtf8.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace advui;
int cyrFont = 1, asciiFont = 0;
namespace graphics { struct Emote { int width, height; const uint8_t *bitmap; }; }
int emoteMatch(const char *, const graphics::Emote ** = nullptr) { return 0; }
bool fullFont = false;
int sdReads = 0;
bool sdFontReady() { return fullFont; }
int sdGlyphWidth(uint32_t cp) { return fullFont && cp == 0x4E00 ? 16 : 0; }
int sdGlyph(uint32_t cp, uint8_t *out) {
    ++sdReads;
    int width = sdGlyphWidth(cp);
    if (width) memset(out, 0xff, 32);
    return width;
}
namespace lgfx {
struct LGFXBase {
    const int *font = &asciiFont;
    std::vector<std::array<uint8_t,16>> bitmaps;
    std::string printed;
    int x = 0, y = 0;
    void setFont(const int *f) { font = f; }
    void setTextSize(int) {}
    void setTextColor(uint16_t) {}
    void setCursor(int cx, int cy) { x = cx; y = cy; }
    int textWidth(const char *s) {
        int width = 0;
        while (*s) {
            auto rune = utf8Decode(s);
            // Deliberately wrong native width for missing Nordic/wide glyphs.
            width += rune.codepoint < 0x80 ? (font == &cyrFont ? 9 : 7) : 4;
            s += rune.bytes;
        }
        return width;
    }
    void print(const char *s) { printed += s; }
    void drawXBitmap(int, int, const uint8_t *, int, int, uint16_t) { assert(false); }
    void drawBitmap(int, int, const uint8_t *bits, int width, int height, uint16_t) {
        assert((width == 8 || width == 16) && height == 16);
        std::array<uint8_t,16> copy;
        std::copy(bits, bits + 16, copy.begin());
        bitmaps.push_back(copy);
    }
};
}
PROTOTYPES
FUNCTIONS
const uint8_t expected[6][16] = {EXPECTED};
std::string settingsValueCopy(const char *src) {
    const char *rowVal[] = {src};
    int i = 0;
    char vbuf[24];
SETTINGS_VALUE_COPY
    return vbuf;
}
std::string networkValueCopy(const char *t) {
    char v[26];
NETWORK_VALUE_COPY
    return v;
}
void requireValidUtf8(const std::string &s) {
    const char *p = s.c_str();
    while (*p) {
        auto rune = utf8Decode(p);
        assert(rune.valid);
        p += rune.bytes;
    }
}
int main() {
    std::string longNordic;
    for (int i = 0; i < 20; ++i)
        longNordic += u8"Ä";
    auto setting = settingsValueCopy(longNordic.c_str());
    auto network = networkValueCopy(longNordic.c_str());
    requireValidUtf8(setting);
    requireValidUtf8(network);
    assert(setting.size() == 22 && network.size() == 24);
    std::string longAscii(30, 'x');
    assert(settingsValueCopy(longAscii.c_str()).size() == 23);
    assert(networkValueCopy(longAscii.c_str()).size() == 25);
    assert(settingsValueCopy("").empty() && networkValueCopy("").empty());
    uint8_t bits[32];
    for (uint32_t cp = 0xC0; cp <= 0x17F; ++cp) {
        memset(bits, 0xa5, sizeof(bits));
        assert(latinGlyphWidth(cp) == 8);
        assert(latinGlyph(cp, bits) == 8);
        assert(std::any_of(bits, bits + 16, [](uint8_t b) { return b != 0; }));
        assert(std::all_of(bits + 16, bits + 32, [](uint8_t b) { return b == 0xa5; }));
    }
    for (uint32_t cp : {0U, 0xBFU, 0x180U, 0x400U, 0xFFFFU, 0xFFFFFFFFU}) {
        memset(bits, 0xa5, sizeof(bits));
        assert(latinGlyphWidth(cp) == 0 && latinGlyph(cp, bits) == 0);
        assert(std::all_of(bits, bits + 32, [](uint8_t b) { return b == 0xa5; }));
    }
    assert(latinGlyph(0xC4, nullptr) == 0);
    for (bool available : {false, true}) {
        fullFont = available;
        lgfx::LGFXBase g;
        const char *nordic = u8"ÄÅÖäåö";
        assert(lineWidthEmotes(&g, nordic) == 54);
        sdReads = 0;
        assert(printLineEmotes(&g, 10, 2, nordic, 0xffff, 0) == 64);
        assert(sdReads == 0 && g.printed.empty() && g.bitmaps.size() == 6);
        for (size_t i = 0; i < 6; ++i)
            assert(memcmp(g.bitmaps[i].data(), expected[i], 16) == 0);
        char compact[32] = u8"ÄÅÖäåö";
        sanitizeDisplay(compact);
        assert(strcmp(compact, nordic) == 0);
        char name[32] = u8"ÄÅÖäåö";
        fitWidth(&g, name, 27);
        assert(strcmp(name, u8"ÄÅÖ") == 0 && labelWidth(&g, name) == 27);
        g.bitmaps.clear();
        printLabel(&g, 4, 2, name, 0xffff);
        assert(g.bitmaps.size() == 3);
        char wrapped[32];
        int consumed = wrapLine(&g, nordic, 27, wrapped, sizeof(wrapped));
        assert(consumed == 6 && strcmp(wrapped, u8"ÄÅÖ") == 0);
        consumed = wrapLine(&g, nordic + consumed, 27, wrapped, sizeof(wrapped));
        assert(consumed == 6 && strcmp(wrapped, u8"äåö") == 0);
        assert(strcmp(visibleFieldTail(&g, u8"ÄÅÖäåö_", true, 27), u8"åö_") == 0);
        std::string longAscii(64, 'x');
        longAscii += '_';
        const char *tail = visibleFieldTail(&g, longAscii.c_str(), false, 228);
        assert(g.textWidth(tail) <= 228 && strlen(tail) == 25 && tail[24] == '_');
        for (int capacity = 3; capacity <= 12; ++capacity) {
            wrapLine(&g, nordic, 200, wrapped, capacity);
            assert(utf8Decode(wrapped).valid);
            assert(strlen(wrapped) < static_cast<size_t>(capacity));
            assert(strlen(wrapped) % 2 == 0);
        }
        g.setFont(&asciiFont);
        char ascii[32] = "ABC";
        fitWidth(&g, ascii, 21);
        assert(g.font == &asciiFont && strcmp(ascii, "ABC") == 0);
        g.printed.clear();
        printLabel(&g, 0, 0, ascii, 0xffff);
        assert(g.printed == "ABC");
    }
    fullFont = true;
    lgfx::LGFXBase wide;
    assert(lineWidthEmotes(&wide, u8"一一") == 34);
    char wrapped[32];
    assert(wrapLine(&wide, u8"一一", 17, wrapped, sizeof(wrapped)) == 3);
    assert(strcmp(wrapped, u8"一") == 0);
    fullFont = false;
    char mixed[32] = u8"Åґ一Ö";
    sanitizeDisplay(mixed);
    assert(strcmp(mixed, u8"ÅґÖ") == 0);
    puts("Embedded Latin bitmap, label, width, wrap and compact-name tests: OK");
}
'''.replace("PROTOTYPES", "\n".join(signature.replace("int emojiDy)", "int emojiDy = 0)") + ";"
                                  for signature in signatures)).replace(
    "FUNCTIONS", "\n\n".join(function(signature) for signature in signatures)).replace("EXPECTED", expected).replace(
    "SETTINGS_VALUE_COPY", settings_copy).replace("NETWORK_VALUE_COPY", network_copy)
with tempfile.TemporaryDirectory(prefix="adv-latin-rendering-") as directory:
    binary = Path(directory) / "test-latin-rendering"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(ROOT / "overlay/src/advui"),
                    str(ROOT / "overlay/src/advui/AdvLatinFont.cpp"), str(ROOT / "overlay/src/advui/AdvUtf8.cpp"),
                    "-x", "c++", "-", "-o", str(binary)], input=fixture, text=True, check=True)
    subprocess.run([str(binary)], check=True)
