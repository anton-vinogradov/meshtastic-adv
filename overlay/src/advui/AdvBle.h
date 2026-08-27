#pragma once

#include "mesh/generated/meshtastic/channel.pb.h"
#include "mesh/generated/meshtastic/config.pb.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace advui
{

// BLE central for the companion mode: the Cardputer is a client to another
// Meshtastic node's stock BLE API (the same GATT service the phone app uses).
// The BLE central/observer roles come from 2.7's sdkconfig.defaults or are
// restored by our custom_sdkconfig on the newer pioarduino line.

struct BleScanHit {
    char name[24];
    char addr[18];
    uint8_t type; // BLE address type — needed to connect (public vs random static)
    int rssi;
};
constexpr int kMaxScanHits = 8;
extern bool g_bleUnsupported;

// Copy scan results under the BLE/UI state lock. The GAP callback writes on the
// NimBLE host task, so UI code must not read the exported test fixtures live.
int bleScanSnapshot(BleScanHit *out, int max, bool *scanning);
bool bleScanHit(int index, BleScanHit *out);
void bleSetScanSnapshot(const BleScanHit *hits, int count, bool scanning);

// Allocates the fixed companion-only buffers and starts the BLE stack. False is
// a clean low-memory failure; local-LoRa mode never pays for these buffers.
bool bleCompanionInit();
void bleScanStart();
void bleScanStop();

// --- Link (A3) ------------------------------------------------------------------
enum BleLinkState : uint8_t {
    BLE_IDLE = 0,
    BLE_CONNECTING,
    BLE_PAIRING,  // waiting for the PIN shown on the node's screen
    BLE_CONNECTED,
    BLE_FAILED
};
extern std::atomic<BleLinkState> g_linkState;
extern std::atomic<bool> g_pinRequested;   // host task wants a passkey -> UI opens the PIN screen
extern std::atomic<uint32_t> g_linkRxPkts; // FromRadio packets drained so far
extern std::atomic<uint32_t> g_linkMyNode; // my_info.my_node_num from the config stream (0 = not seen)
constexpr size_t kBleLinkErrorSize = 28;
void bleCopyLinkError(char *out, size_t cap);

// Screen-off power trim: stretch the peripheral advertising interval to ~1 s
// (a dark device in a pocket doesn't need 100 ms discoverability); false
// restores the stack defaults. No-op while BLE is down or a phone is connected.
void bleAdvSlow(bool slow);

void bleConnectAsync(const char *addr, uint8_t addrType); // one-shot task: connect + discover + start config
bool bleDisconnect(); // waits for the transport task to release its GATT objects
void bleSubmitPin(uint32_t pin); // answer the pairing passkey prompt
void bleCancelPin();             // give up on pairing
void blePump();                  // drain FromRadio (call from the UI thread)
bool bleWriteToRadio(const uint8_t *buf, size_t len);

// --- Companion mesh state (A4) --------------------------------------------------
// Filled by the pump task from the FromRadio config stream; read by the UI thread.
struct CompNode {
    uint32_t num;
    uint32_t lastHeard;
    float snr;
    uint8_t hops;
    bool hasKey; // node advertises a PKI public key -> DMs must set pki_encrypted
    char shortName[5];
    char longName[24];
};
constexpr int kMaxCompNodes = 64; // synced-node table; 64 is plenty and saves ~3 KB of
                                   // static RAM this no-PSRAM board can't spare (see v0.3.2 heap notes)
extern std::atomic<int> g_compNodeCount;
// Stable size of the linked node's DB: the config stream sends each entry once
// per sync (reset on my_info, counted through config_complete). Before the
// mirror fills, live node_info for a new identity can also grow it. Once all 64
// slots are occupied, the next sync is authoritative because an absent live
// identity may merely have been evicted from the mirror.
extern std::atomic<int> g_compNodesSeen;
// Full channel objects from the config stream. Kept whole — the PSK included —
// so a rename can round-trip the channel back via set_channel without wiping
// the key (the same reason the phone can edit channels).
extern std::atomic<bool> g_linkConfigDone; // config download finished
extern std::atomic<int> g_compPreset;      // the node's LoRa modem preset (blank primary shows it)
extern std::atomic<int> g_linkNodeBatt;    // the radio node's battery % (-1 unknown)
extern std::atomic<int> g_linkRssi;        // BLE link RSSI in dBm (0 unknown)

// Companion snapshots cross from the BLE pump task to the UI task. Copy them
// through these helpers so protobuf structs and node names cannot tear mid-frame.
bool bleCopyCompNodeAt(int index, CompNode *out);
bool bleCopyCompNode(uint32_t num, CompNode *out);
bool bleUpdateCompNodeNames(uint32_t num, const char *longName, const char *shortName);
bool bleCopyCompChannel(int index, meshtastic_Channel *out);
bool bleUpdateCompChannel(int index, const meshtastic_Channel &channel);
bool bleCopyCompLora(meshtastic_Config_LoRaConfig *out);
void bleUpdateCompLora(const meshtastic_Config_LoRaConfig &config);
bool bleCopyCompDevice(meshtastic_Config_DeviceConfig *out);
void bleUpdateCompDevice(const meshtastic_Config_DeviceConfig &config);

// Mesh packets (messages/acks) hand off pump -> UI as raw FromRadio bytes; the UI
// decodes and runs its normal handleFromRadio pipeline.
constexpr size_t kBleFrameMax = 512;
struct BleFrame {
    uint16_t len;
    uint8_t data[kBleFrameMax];
};
bool bleNextPacket(BleFrame *frame); // pop one, false when empty (UI thread)
bool bleQueueToRadio(const uint8_t *buf, uint16_t len); // UI thread enqueues, pump writes
uint32_t bleRxDrops(); // queue-full/oversize counters, reset on every connection
uint32_t bleTxDrops();

#ifdef ADVUI_HIL
// Feed captured/synthetic stock PhoneAPI responses through the exact companion
// decoder/state router without opening BLE or transmitting anything.
bool bleHilInjectFromRadio(const uint8_t *bytes, uint16_t len);
void bleHilClearState();
void bleHilReleaseState(); // model companion->onboard reboot between isolated HIL phases
uint32_t bleHilIngressFirstHeap();
uint32_t bleHilIngressLastHeap();
uint32_t bleHilIngressCount();
uint32_t bleHilIngressLastBeforeHeap();
int32_t bleHilIngressDirectDelta();
uint32_t bleHilIngressMaxDrop();
bool bleHilBuffersReady();
#endif

} // namespace advui
