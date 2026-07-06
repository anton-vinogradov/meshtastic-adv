#pragma once

#include "mesh/generated/meshtastic/channel.pb.h"
#include "mesh/generated/meshtastic/config.pb.h"
#include <cstddef>
#include <cstdint>

namespace advui
{

// BLE central for the companion mode: the Cardputer is a client to another
// Meshtastic node's stock BLE API (the same GATT service the phone app uses).
// The BLE central/observer roles are restored via our custom_sdkconfig.

struct BleScanHit {
    char name[24];
    char addr[18];
    uint8_t type; // BLE address type — needed to connect (public vs random static)
    int rssi;
};
constexpr int kMaxScanHits = 8;
extern BleScanHit g_scanHits[kMaxScanHits];
extern int g_scanCount;
extern bool g_scanning;
extern bool g_bleUnsupported;

void bleCompanionInit();
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
extern volatile BleLinkState g_linkState;
extern volatile bool g_pinRequested;   // host task wants a passkey -> UI opens the PIN screen
extern volatile uint32_t g_linkRxPkts; // FromRadio packets drained so far
extern volatile uint32_t g_linkMyNode; // my_info.my_node_num from the config stream (0 = not seen)
extern char g_linkErr[28];             // short reason when g_linkState == BLE_FAILED

void bleConnectAsync(const char *addr, uint8_t addrType); // one-shot task: connect + discover + start config
void bleDisconnect();
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
constexpr int kMaxCompNodes = 128;
extern CompNode g_compNodes[kMaxCompNodes];
extern volatile int g_compNodeCount;
// Full channel objects from the config stream. Kept whole — the PSK included —
// so a rename can round-trip the channel back via set_channel without wiping
// the key (the same reason the phone can edit channels).
extern meshtastic_Channel g_compChans[8];
extern volatile bool g_linkConfigDone; // config download finished
extern volatile int g_compPreset;      // the node's LoRa modem preset (blank primary shows it)
extern meshtastic_Config_LoRaConfig g_compLora; // the node's full LoRa config (remote admin edits it)
extern volatile bool g_compLoraValid;
extern volatile int g_linkNodeBatt;    // the radio node's battery % (-1 unknown)
extern volatile int g_linkRssi;        // BLE link RSSI in dBm (0 unknown)

// Mesh packets (messages/acks) hand off pump -> UI as raw FromRadio bytes; the UI
// decodes and runs its normal handleFromRadio pipeline.
bool bleNextPacket(uint8_t *buf, uint16_t *len); // pop one, false when empty (UI thread)
bool bleQueueToRadio(const uint8_t *buf, uint16_t len); // UI thread enqueues, pump writes

} // namespace advui
