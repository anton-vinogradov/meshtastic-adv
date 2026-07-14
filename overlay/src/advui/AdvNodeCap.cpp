#include "FSCommon.h"
#include <Esp.h>
#include <cstring>

// Injected as MAX_NUM_NODES via build_flags (see the variant platformio.ini).
//
// In companion mode the UI reads mesh state from the linked node over BLE, so
// the local hot store shrinks to a token size: the ~22 KB a 200-node
// NodeInfoLite vector costs is exactly the DRAM the BLE central + SMP pairing
// need. 32 slots still fit our own entry plus any favourites, so switching
// back to onboard LoRa keeps them (the rest of the mesh is re-learned off the
// air).
//
// Local mode does NOT follow the stock flash-size tiering: this board has 16 MB
// flash but no PSRAM, so DRAM — not flash — is the ceiling. The stock 250-node
// tier is a ~40 KB NodeInfoLite vector, and that is exactly what tips a
// WiFi+HTTPS build into out-of-memory reboots on a busy mesh. Cap the hot store
// at 150 (matches WARM_NODE_COUNT, so each evicted node keeps exactly one warm
// slot and DMs to it still decrypt); the tail lives in the flash warm tier.
//
// Runs from NodeDB's constructor — after fsInit(), long before the UI loads
// its radio config — so it reads /advui_radio.bin (AVR1) directly.
int advui_max_num_nodes()
{
    static int cached = 0;
    if (cached)
        return cached;

    bool companion = false;
    auto f = FSCom.open("/advui_radio.bin", FILE_O_READ);
    if (f) {
        uint8_t hdr[5] = {0};
        if (f.read(hdr, sizeof(hdr)) == sizeof(hdr)) {
            uint32_t magic;
            memcpy(&magic, hdr, sizeof(magic));
            companion = magic == 0x41565231 /* AVR1 */ && hdr[4] == 1;
        }
        f.close();
    }

    if (companion) {
        cached = 32;
    } else {
        uint32_t flashMb = ESP.getFlashChipSize() / (1024 * 1024);
        cached = flashMb >= 7 ? 150 : 100;
    }
    return cached;
}
