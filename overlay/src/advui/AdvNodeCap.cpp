#include "FSCommon.h"
#include "AdvStorage.h"
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
// Local mode does NOT follow the stock flash-size tiering: this board has 8 MB
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

#ifdef ADVUI_HIL
    // The release HIL exercises the companion arena and BLE ingress while the
    // production config remains untouched. Model the reachable companion boot
    // profile here: a real companion reads mode=1 before NodeDB construction
    // and therefore allocates 32 local slots, never the onboard 150-slot DB
    // plus the companion arena at the same time.
    cached = 32;
    return cached;
#endif

    bool companion = false;
    auto f = FSCom.open("/advui_radio.bin", FILE_O_READ);
    if (f) {
        // AVR1 is either the original 47-byte record or the current 48-byte
        // record with a BLE address type. Match the UI loader's full-record
        // validation: a valid-looking five-byte prefix of a torn file must not
        // shrink the onboard node database for the whole boot.
        constexpr size_t kLegacySize = 4 + 1 + 18 + 24;
        constexpr size_t kCurrentSize = kLegacySize + 1;
        uint8_t record[kCurrentSize] = {0};
        const size_t size = f.size();
        if ((size == kLegacySize || size == kCurrentSize) && f.read(record, size) == size) {
            uint32_t magic;
            memcpy(&magic, record, sizeof(magic));
            const char *address = reinterpret_cast<const char *>(record + 5);
            const uint8_t peerType = size == kCurrentSize ? record[kLegacySize] : 0;
            const bool peerOk = !address[0] || (advui::validBleAddress(address) && peerType <= 3);
            companion = magic == 0x41565231 /* AVR1 */ && record[4] == 1 && peerOk;
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
