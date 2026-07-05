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
// air). Local mode keeps the stock flash-size tiering.
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
        cached = flashMb >= 15 ? 250 : flashMb >= 7 ? 200 : 100;
    }
    return cached;
}
