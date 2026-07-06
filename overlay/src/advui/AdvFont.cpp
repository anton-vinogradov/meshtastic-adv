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
constexpr size_t kFontSize = 4 + 65536UL * 33;

File g_font;
bool g_ready = false;
const uint8_t *g_map = nullptr; // memory-mapped "font" flash partition (fast path)
char g_state[24] = "off";       // what Settings shows: flash / sd / off / error tag

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
    if (!g_ready || cp > 0xFFFF)
        return nullptr;
    for (int i = 0; i < kCacheN; i++)
        if (g_cache[i].cp == cp)
            return &g_cache[i];

    uint8_t rec[33];
    {
        concurrency::LockGuard guard(spiLock);
        if (!g_font.seek(4 + (uint32_t)cp * 33) || g_font.read(rec, sizeof(rec)) != (int)sizeof(rec))
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
    } else if (part->size < kFontSize) {
        snprintf(g_state, sizeof(g_state), "part too small");
    } else {
        const void *ptr = nullptr;
        esp_partition_mmap_handle_t h;
        esp_err_t err = esp_partition_mmap(part, 0, kFontSize, ESP_PARTITION_MMAP_DATA, &ptr, &h);
        if (err != ESP_OK) {
            snprintf(g_state, sizeof(g_state), "mmap 0x%x", (unsigned)err);
        } else if (memcmp(ptr, "AUF1", 4) != 0) {
            snprintf(g_state, sizeof(g_state), "not flashed");
            esp_partition_munmap(h);
        } else {
            g_map = (const uint8_t *)ptr;
            g_ready = true;
            snprintf(g_state, sizeof(g_state), "flash");
            LOG_INFO("advui: unicode font mapped from the flash partition");
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
    if (!g_font || g_font.size() < kFontSize || g_font.read((uint8_t *)magic, 4) != 4 || strcmp(magic, "AUF1") != 0) {
        LOG_INFO("advui: %s missing/invalid, unicode font off", kFontPath);
        if (g_font)
            g_font.close();
        return;
    }
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
        if (cp > 0xFFFF)
            return 0;
        const uint8_t *rec = g_map + 4 + (uint32_t)cp * 33;
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
        if (cp > 0xFFFF)
            return 0;
        uint8_t w = g_map[4 + (uint32_t)cp * 33];
        return w == 2 ? 16 : w == 1 ? 8 : 0;
    }
    CacheEnt *e = lookup(cp);
    return e ? e->w : 0;
}

} // namespace advui
