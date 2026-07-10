#include "AdvFont.h"
#include "SPILock.h"
#include "configuration.h"
#include <SD.h>
#include <SPI.h>
#include <cstdlib>
#include <cstring>
#include <esp_partition.h>

namespace advui
{

namespace
{

// Cardputer ADV TF slot: same SPI bus as the LoRa cap (SCK 40 / MISO 39 /
// MOSI 14), its own CS on G12. Every touch of the bus goes under spiLock so
// SD traffic never interleaves with a radio transaction.
constexpr int kSdCs = 12;
constexpr uint32_t kSdHz = 16000000;
const char *kFontPath = "/unifont.bin";
// AUF1 = the BMP only; AUF2 appends the supplementary emoji blocks
// (U+1F000..U+1FBFF) as records 65536.. — see scripts/mkunifont.py.
constexpr uint32_t kSmpBase = 0x1F000, kSmpEnd = 0x1FBFF;
constexpr size_t kFontSizeV1 = 4 + 65536UL * 33;
constexpr size_t kFontSizeV2 = 4 + (65536UL + (kSmpEnd - kSmpBase + 1)) * 33;

File g_font;
bool g_ready = false;
bool g_v2 = false;              // blob has the supplementary emoji records
const uint8_t *g_map = nullptr; // memory-mapped "font" flash partition (fast path)
char g_state[24] = "off";       // what Settings shows: flash / sd / off / error tag

// Byte offset of a codepoint's record, or -1 when the blob can't have it.
int32_t recOff(uint32_t cp)
{
    if (cp <= 0xFFFF)
        return 4 + (int32_t)cp * 33;
    if (g_v2 && cp >= kSmpBase && cp <= kSmpEnd)
        return 4 + (int32_t)(65536UL + cp - kSmpBase) * 33;
    return -1;
}

// A small LRU-ish glyph cache (round-robin eviction): allocated only when the
// font is actually present, so cardless setups pay nothing. Misses cost one
// 33-byte SD read; absent glyphs are cached too (width 0) so tofu doesn't
// re-hit the card on every frame.
struct CacheEnt {
    uint32_t cp;
    uint8_t w; // pixel width: 0 none, 8 halfwidth, 16 fullwidth
    uint8_t bits[32];
};
constexpr int kCacheN = 96;
CacheEnt *g_cache = nullptr;
int g_hand = 0;

CacheEnt *lookup(uint32_t cp) // SD path only; the mapped partition needs no cache
{
    int32_t off = recOff(cp);
    if (!g_ready || off < 0)
        return nullptr;
    for (int i = 0; i < kCacheN; i++)
        if (g_cache[i].cp == cp)
            return &g_cache[i];

    uint8_t rec[33];
    {
        concurrency::LockGuard guard(spiLock);
        if (!g_font.seek((uint32_t)off) || g_font.read(rec, sizeof(rec)) != (int)sizeof(rec))
            return nullptr;
    }
    CacheEnt &e = g_cache[g_hand];
    g_hand = (g_hand + 1) % kCacheN;
    e.cp = cp;
    e.w = rec[0] == 1 ? 8 : rec[0] == 2 ? 16 : 0;
    memcpy(e.bits, rec + 1, sizeof(e.bits));
    return &e;
}

} // namespace

void sdFontInit()
{
    // First choice: the "font" flash partition the web installer writes —
    // zero user action, and glyph reads are plain memory-mapped loads.
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "font");
    if (!part) {
        snprintf(g_state, sizeof(g_state), "no partition");
    } else if (part->size < kFontSizeV1) {
        snprintf(g_state, sizeof(g_state), "part too small");
    } else {
        // Peek the magic first: it decides how much we map (V2 adds the emoji records).
        char magic[4] = {0};
        bool v2 = esp_partition_read(part, 0, magic, 4) == ESP_OK && memcmp(magic, "AUF2", 4) == 0;
        bool v1 = !v2 && memcmp(magic, "AUF1", 4) == 0;
        size_t need = v2 ? kFontSizeV2 : kFontSizeV1;
        const void *ptr = nullptr;
        esp_partition_mmap_handle_t h;
        esp_err_t err;
        if (!v1 && !v2) {
            snprintf(g_state, sizeof(g_state), "not flashed");
        } else if (part->size < need) {
            snprintf(g_state, sizeof(g_state), "part too small");
        } else if ((err = esp_partition_mmap(part, 0, need, ESP_PARTITION_MMAP_DATA, &ptr, &h)) != ESP_OK) {
            snprintf(g_state, sizeof(g_state), "mmap 0x%x", (unsigned)err);
        } else {
            g_map = (const uint8_t *)ptr;
            g_v2 = v2;
            g_ready = true;
            snprintf(g_state, sizeof(g_state), "flash");
            LOG_INFO("advui: unicode font mapped from the flash partition (%s)", v2 ? "AUF2" : "AUF1");
            return;
        }
    }

    // Fallback: /unifont.bin on the SD card (Launcher installs, bare esptool
    // flashes — anyone whose flashing path didn't write the partition).
    concurrency::LockGuard guard(spiLock);
    if (!SD.begin(kSdCs, SPI, kSdHz)) {
        LOG_INFO("advui: no SD card, unicode font off");
        return;
    }
    g_font = SD.open(kFontPath, FILE_READ);
    char magic[5] = {0};
    bool ok = g_font && g_font.read((uint8_t *)magic, 4) == 4;
    bool v2 = ok && strcmp(magic, "AUF2") == 0;
    ok = ok && (v2 ? g_font.size() >= kFontSizeV2 : (strcmp(magic, "AUF1") == 0 && g_font.size() >= kFontSizeV1));
    if (!ok) {
        LOG_INFO("advui: %s missing/invalid, unicode font off", kFontPath);
        if (g_font)
            g_font.close();
        return;
    }
    g_v2 = v2;
    g_cache = (CacheEnt *)calloc(kCacheN, sizeof(CacheEnt));
    if (!g_cache) {
        g_font.close();
        return;
    }
    for (int i = 0; i < kCacheN; i++)
        g_cache[i].cp = 0xFFFFFFFF;
    g_ready = true;
    snprintf(g_state, sizeof(g_state), "sd");
    LOG_INFO("advui: unicode font ready (%s)", kFontPath);
}

const char *sdFontState()
{
    return g_state;
}

bool sdFontReady()
{
    return g_ready;
}

int sdGlyph(uint32_t cp, uint8_t *out)
{
    if (g_map) {
        int32_t off = recOff(cp);
        if (off < 0)
            return 0;
        const uint8_t *rec = g_map + off;
        if (!rec[0])
            return 0;
        memcpy(out, rec + 1, 32);
        return rec[0] == 2 ? 16 : 8;
    }
    CacheEnt *e = lookup(cp);
    if (!e || !e->w)
        return 0;
    memcpy(out, e->bits, 32);
    return e->w;
}

int sdGlyphWidth(uint32_t cp)
{
    if (g_map) {
        int32_t off = recOff(cp);
        if (off < 0)
            return 0;
        uint8_t w = g_map[off];
        return w == 2 ? 16 : w == 1 ? 8 : 0;
    }
    CacheEnt *e = lookup(cp);
    return e ? e->w : 0;
}

} // namespace advui
