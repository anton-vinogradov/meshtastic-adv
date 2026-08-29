#include "AdvBle.h"
#include "AdvNodeCount.h"
#include "AdvStorage.h"
#include "BluetoothCommon.h" // MESH_SERVICE_UUID + characteristic UUIDs
#include "DebugConfiguration.h"
#include "Throttle.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/mesh-pb-constants.h"
#include <Esp.h>
#if __has_include(<BLEDevice.h>)
#include <BLEDevice.h>
#include <BLESecurity.h>
#define ADVUI_NIMBLE_14 0
#else
// Meshtastic 2.7 uses the external NimBLE-Arduino 1.4 compatibility API;
// NimBLEDevice.h supplies the traditional BLE* aliases used below.
#include <NimBLEDevice.h>
#define ADVUI_NIMBLE_14 1
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#if ADVUI_NIMBLE_14
#include <nimble/nimble/host/include/host/ble_gap.h>
#include <nimble/nimble/host/include/host/ble_hs_adv.h>
#else
#include <host/ble_gap.h>
#include <host/ble_hs_adv.h>
#endif

namespace advui
{

bool g_bleUnsupported = false; // roles are compiled in via our custom_sdkconfig

std::atomic<BleLinkState> g_linkState{BLE_IDLE};
std::atomic<bool> g_pinRequested{false};
std::atomic<uint32_t> g_linkRxPkts{0};
std::atomic<uint32_t> g_linkMyNode{0};

std::atomic<int> g_compNodeCount{0};
std::atomic<int> g_compNodesSeen{0};
std::atomic<bool> g_linkConfigDone{false};
std::atomic<int> g_compPreset{0};
std::atomic<int> g_linkNodeBatt{-1};
std::atomic<int> g_linkRssi{0};

namespace
{
int g_scanCount = 0;
bool g_scanning = false;
#ifndef ADVUI_HIL
bool g_bleInitFailed = false;
#endif
char g_linkErr[kBleLinkErrorSize] = {0};
std::atomic<bool> g_compLoraValid{false};
std::atomic<bool> g_compDeviceValid{false};

portMUX_TYPE g_stateMux = portMUX_INITIALIZER_UNLOCKED;

// These buffers are exclusive to companion mode. Keeping them in .bss charged
// every local-LoRa boot about 8 KiB for a feature that was never started. One
// early, zeroed allocation preserves the fixed-capacity/no-fragmentation design
// in companion mode while returning that memory to the normal firmware heap.
constexpr int kRxDepth = 6;
constexpr int kTxDepth = 4, kTxSlot = 300;
struct TxFrame {
    uint16_t len;
    uint8_t data[kTxSlot];
};
struct CompanionBuffers {
    BleScanHit scanHits[kMaxScanHits];
    CompNode nodes[kMaxCompNodes];
    meshtastic_Channel channels[8];
    meshtastic_Config_LoRaConfig lora;
    meshtastic_Config_DeviceConfig device;
    StaticQueue_t rxQueueState;
    uint8_t rxQueueStorage[kRxDepth * sizeof(BleFrame)];
    StaticQueue_t txQueueState;
    uint8_t txQueueStorage[kTxDepth * sizeof(TxFrame)];
};
static_assert(sizeof(CompanionBuffers) <= 9 * 1024, "companion buffers exceeded their memory envelope");
CompanionBuffers *g_buffers = nullptr;
QueueHandle_t g_rxQueue = nullptr;
QueueHandle_t g_txQueue = nullptr;

bool initBuffers()
{
    if (g_buffers)
        return true;
    g_buffers = static_cast<CompanionBuffers *>(calloc(1, sizeof(CompanionBuffers)));
    if (!g_buffers) {
        LOG_ERROR("advui: no memory for %u-byte companion buffers", (unsigned)sizeof(CompanionBuffers));
        return false;
    }
    return true;
}

class StateGuard
{
  public:
    StateGuard() { portENTER_CRITICAL(&g_stateMux); }
    ~StateGuard() { portEXIT_CRITICAL(&g_stateMux); }
};

std::atomic<uint32_t> g_rxDrops{0}, g_txDrops{0};

bool initQueues()
{
    if (!initBuffers())
        return false;
    if (!g_rxQueue)
        g_rxQueue = xQueueCreateStatic(kRxDepth, sizeof(BleFrame), g_buffers->rxQueueStorage,
                                       &g_buffers->rxQueueState);
    if (!g_txQueue)
        g_txQueue = xQueueCreateStatic(kTxDepth, sizeof(TxFrame), g_buffers->txQueueStorage,
                                       &g_buffers->txQueueState);
    return g_rxQueue && g_txQueue;
}
} // namespace

int bleScanSnapshot(BleScanHit *out, int max, bool *scanning)
{
    StateGuard guard;
    if (scanning)
        *scanning = g_scanning;
    const int count = g_buffers && max > 0 ? std::min(g_scanCount, max) : 0;
    if (out && count)
        memcpy(out, g_buffers->scanHits, (size_t)count * sizeof(BleScanHit));
    return count;
}

bool bleScanHit(int index, BleScanHit *out)
{
    if (!out || !g_buffers)
        return false;
    StateGuard guard;
    if (index < 0 || index >= g_scanCount)
        return false;
    *out = g_buffers->scanHits[index];
    return true;
}

void bleSetScanSnapshot(const BleScanHit *hits, int count, bool scanning)
{
    if (!initBuffers())
        return;
    StateGuard guard;
    count = hits ? std::max(0, std::min(count, kMaxScanHits)) : 0;
    g_scanCount = count;
    if (hits && count)
        memcpy(g_buffers->scanHits, hits, (size_t)count * sizeof(BleScanHit));
    g_scanning = scanning;
}

void bleCopyLinkError(char *out, size_t cap)
{
    if (!out || !cap)
        return;
    StateGuard guard;
    snprintf(out, cap, "%s", g_linkErr);
}

bool bleCopyCompNodeAt(int index, CompNode *out)
{
    if (!out || !g_buffers)
        return false;
    StateGuard guard;
    const int count = g_compNodeCount.load(std::memory_order_relaxed);
    if (index < 0 || index >= count)
        return false;
    *out = g_buffers->nodes[index];
    return true;
}

bool bleCopyCompNode(uint32_t num, CompNode *out)
{
    if (!out || !g_buffers)
        return false;
    StateGuard guard;
    const int count = g_compNodeCount.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++)
        if (g_buffers->nodes[i].num == num) {
            *out = g_buffers->nodes[i];
            return true;
        }
    return false;
}

bool bleUpdateCompNodeNames(uint32_t num, const char *longName, const char *shortName)
{
    if (!g_buffers)
        return false;
    StateGuard guard;
    const int count = g_compNodeCount.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++)
        if (g_buffers->nodes[i].num == num) {
            snprintf(g_buffers->nodes[i].longName, sizeof(g_buffers->nodes[i].longName), "%s",
                     longName ? longName : "");
            snprintf(g_buffers->nodes[i].shortName, sizeof(g_buffers->nodes[i].shortName), "%s",
                     shortName ? shortName : "");
            return true;
        }
    return false;
}

bool bleCopyCompChannel(int index, meshtastic_Channel *out)
{
    if (!out || !g_buffers || index < 0 || index >= 8)
        return false;
    StateGuard guard;
    *out = g_buffers->channels[index];
    return true;
}

bool bleUpdateCompChannel(int index, const meshtastic_Channel &channel)
{
    if (index < 0 || index >= 8 || !initBuffers())
        return false;
    StateGuard guard;
    g_buffers->channels[index] = channel;
    return true;
}

bool bleCopyCompLora(meshtastic_Config_LoRaConfig *out)
{
    if (!out || !g_buffers)
        return false;
    StateGuard guard;
    if (!g_compLoraValid.load(std::memory_order_relaxed))
        return false;
    *out = g_buffers->lora;
    return true;
}

void bleUpdateCompLora(const meshtastic_Config_LoRaConfig &config)
{
    if (!initBuffers())
        return;
    StateGuard guard;
    g_buffers->lora = config;
    g_compPreset.store((int)config.modem_preset, std::memory_order_relaxed);
    g_compLoraValid.store(true, std::memory_order_release);
}

bool bleCopyCompDevice(meshtastic_Config_DeviceConfig *out)
{
    if (!out || !g_buffers)
        return false;
    StateGuard guard;
    if (!g_compDeviceValid.load(std::memory_order_relaxed))
        return false;
    *out = g_buffers->device;
    return true;
}

void bleUpdateCompDevice(const meshtastic_Config_DeviceConfig &config)
{
    if (!initBuffers())
        return;
    StateGuard guard;
    g_buffers->device = config;
    g_compDeviceValid.store(true, std::memory_order_release);
}

bool bleNextPacket(BleFrame *frame)
{
    return frame && g_rxQueue && xQueueReceive(g_rxQueue, frame, 0) == pdTRUE;
}

bool bleQueueToRadio(const uint8_t *buf, uint16_t len)
{
#ifdef ADVUI_HIL
    // USB HIL may exercise the companion decoder, but it must never enqueue a
    // real ToRadio frame.  Otherwise a persisted companion selection could
    // reach a bonded node and make that node transmit into the live mesh.
    (void)buf;
    (void)len;
    g_txDrops.fetch_add(1);
    return false;
#else
    if (!g_txQueue || !buf || len > kTxSlot) {
        g_txDrops.fetch_add(1);
        return false;
    }
    TxFrame frame{len, {0}};
    memcpy(frame.data, buf, len);
    if (xQueueSend(g_txQueue, &frame, 0) == pdTRUE)
        return true;
    g_txDrops.fetch_add(1);
    return false;
#endif
}

uint32_t bleRxDrops() { return g_rxDrops.load(); }
uint32_t bleTxDrops() { return g_txDrops.load(); }

namespace
{

bool g_bleInited = false;
BLEClient *g_client = nullptr;
BLERemoteCharacteristic *g_toRadio = nullptr;
BLERemoteCharacteristic *g_fromRadio = nullptr;
BLERemoteCharacteristic *g_fromNum = nullptr;
std::atomic<bool> g_fromNumPing{false}; // notify fired: FromRadio has data to drain
std::atomic<bool> g_pinReady{false};
std::atomic<uint32_t> g_pinValue{0};
std::atomic<bool> g_connectTaskRunning{false};
std::atomic<bool> g_cancelRequested{false};
char g_connAddr[18] = {0};
uint8_t g_connType = 0;

void resetCompanionState()
{
    const bool haveBuffers = initQueues();
    {
        StateGuard guard;
        g_compNodeCount.store(0, std::memory_order_relaxed);
        g_compNodesSeen.store(0, std::memory_order_relaxed);
        if (haveBuffers) {
            memset(g_buffers->nodes, 0, sizeof(g_buffers->nodes));
            memset(g_buffers->channels, 0, sizeof(g_buffers->channels));
        }
        g_compPreset.store(0, std::memory_order_relaxed);
        if (haveBuffers)
            g_buffers->lora = meshtastic_Config_LoRaConfig_init_default;
        g_compLoraValid.store(false, std::memory_order_relaxed);
        const meshtastic_Config_DeviceConfig freshDevice = meshtastic_Config_DeviceConfig_init_default;
        if (haveBuffers)
            g_buffers->device = freshDevice;
        g_compDeviceValid.store(false, std::memory_order_relaxed);
        g_linkConfigDone.store(false, std::memory_order_relaxed);
    }
    g_linkRxPkts.store(0);
    g_linkMyNode.store(0);
    g_linkNodeBatt.store(-1);
    g_linkRssi.store(0);
    g_rxDrops.store(0);
    g_txDrops.store(0);
    if (g_rxQueue)
        xQueueReset(g_rxQueue);
    if (g_txQueue)
        xQueueReset(g_txQueue);
}

// my_info is the boundary of an authoritative PhoneAPI config snapshot, not
// just an identity update. A node can request another snapshot without the BLE
// transport disconnecting, so retaining the previous mirror here would leave
// removed nodes/channels visible and make the DB-size counter ignore entries it
// already knew. Keep live packet queues intact, but replace all snapshot state.
void beginCompanionConfigStream(uint32_t myNode)
{
    StateGuard guard;
    memset(g_buffers->nodes, 0, sizeof(g_buffers->nodes));
    memset(g_buffers->channels, 0, sizeof(g_buffers->channels));
    const meshtastic_Config_LoRaConfig freshLora = meshtastic_Config_LoRaConfig_init_default;
    const meshtastic_Config_DeviceConfig freshDevice = meshtastic_Config_DeviceConfig_init_default;
    g_buffers->lora = freshLora;
    g_buffers->device = freshDevice;
    g_compNodeCount.store(0, std::memory_order_relaxed);
    g_compNodesSeen.store(0, std::memory_order_relaxed);
    g_compPreset.store(0, std::memory_order_relaxed);
    g_compLoraValid.store(false, std::memory_order_relaxed);
    g_compDeviceValid.store(false, std::memory_order_relaxed);
    g_linkConfigDone.store(false, std::memory_order_relaxed);
    g_linkMyNode.store(myNode, std::memory_order_relaxed);
    g_linkNodeBatt.store(-1, std::memory_order_relaxed);
}

void fail(const char *why)
{
    {
        StateGuard guard;
        snprintf(g_linkErr, sizeof(g_linkErr), "%s", why);
    }
    g_linkState = BLE_FAILED;
    LOG_INFO("advui: ble link failed: %s", why);
}

// Raw NimBLE scan: the Arduino BLEScan wrapper heap-copies every advertiser it
// hears (strings + vectors, unbounded) and OOM-aborted in a dense city radio
// environment. ble_gap_disc + a fixed hit array allocates nothing.
const ble_uuid128_t kMeshSvcUuid =
    BLE_UUID128_INIT(0xfd, 0xea, 0x73, 0xe2, 0xca, 0x5d, 0xa8, 0x9f, 0x1f, 0x46, 0xa8, 0x15, 0x18, 0xb2, 0xa1, 0x6b);

void fmtAddr(const uint8_t *val, char *out) // NimBLE keeps the 6 bytes little-endian
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", val[5], val[4], val[3], val[2], val[1], val[0]);
}

int scanGapEvent(struct ble_gap_event *ev, void *)
{
    if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        StateGuard guard;
        g_scanning = false;
        return 0;
    }
    if (ev->type != BLE_GAP_EVENT_DISC)
        return 0;

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) != 0)
        return 0;

    bool mesh = false;
    for (int i = 0; i < f.num_uuids128; i++)
        if (ble_uuid_cmp(&f.uuids128[i].u, &kMeshSvcUuid.u) == 0)
            mesh = true;

    char addr[18];
    fmtAddr(ev->disc.addr.val, addr);

    bool added = false;
    {
        StateGuard guard;
        int slot = -1;
        for (int i = 0; i < g_scanCount; i++)
            if (!strcmp(g_buffers->scanHits[i].addr, addr))
                slot = i;
        if (slot < 0) {
            if (!mesh || g_scanCount >= kMaxScanHits)
                return 0; // only track Meshtastic advertisers
            slot = g_scanCount++;
            BleScanHit &h = g_buffers->scanHits[slot];
            memcpy(h.addr, addr, sizeof(h.addr));
            h.type = ev->disc.addr.type;
            h.name[0] = 0;
            added = true;
        }
        BleScanHit &h = g_buffers->scanHits[slot];
        h.rssi = ev->disc.rssi;
        if (f.name && f.name_len && !h.name[0]) { // name usually rides in the scan response
            int n = f.name_len < (int)sizeof(h.name) - 1 ? f.name_len : (int)sizeof(h.name) - 1;
            memcpy(h.name, f.name, n);
            h.name[n] = 0;
        }
    }
    if (added)
        LOG_INFO("advui: ble hit %s type=%d", addr, ev->disc.addr.type);
    return 0;
}

// Pairing: the node displays a PIN, we type it. This runs on the NimBLE host task;
// it flags the UI to open the PIN screen and waits (SMP allows ~30s).
class AdvSecCb : public BLESecurityCallbacks
{
    uint32_t onPassKeyRequest() override
    {
        LOG_INFO("advui: ble passkey requested");
        g_pinReady = false;
        g_pinRequested = true;
        g_linkState = BLE_PAIRING;
        for (int i = 0; i < 280 && !g_pinReady; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        g_pinRequested = false;
        return g_pinReady.load() ? g_pinValue.load() : 0U;
    }
    bool onSecurityRequest() override { return true; }
    bool onConfirmPIN(uint32_t) override { return true; }
#if ADVUI_NIMBLE_14
    void onPassKeyNotify(uint32_t) override {}
    void onAuthenticationComplete(ble_gap_conn_desc *) override {}
#else
    bool onAuthorizationRequest(uint16_t, uint16_t, bool) override { return true; }
#endif
};
AdvSecCb g_secCb;

void fromNumNotify(BLERemoteCharacteristic *, uint8_t *, size_t, bool)
{
    g_fromNumPing = true;
}

// Kick the node into the config download (what the phone does on connect).
void startConfig()
{
    meshtastic_ToRadio t = meshtastic_ToRadio_init_default;
    t.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    t.want_config_id = (uint32_t)millis() | 1;
    uint8_t buf[64];
    size_t len = pb_encode_to_bytes(buf, sizeof(buf), &meshtastic_ToRadio_msg, &t);
    if (len)
        bleWriteToRadio(buf, len);
}

// Blocking connect + GATT discovery; runs in its own one-shot task so the UI
// thread stays free to show the PIN screen mid-pairing.
void connectBlocking()
{
    bleScanStop();
    g_linkState = BLE_CONNECTING;
    resetCompanionState(); // a fresh peer must never inherit the old snapshot/queues

    BLEDevice::setSecurityCallbacks(&g_secCb);
#if ADVUI_NIMBLE_14
    BLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY); // node shows the PIN, we enter it
    BLEDevice::setSecurityAuth(true, true, true);          // bond + MITM + secure connections
#else
    BLESecurity::setCapability(ESP_IO_CAP_IN);          // node shows the PIN, we enter it
    BLESecurity::setAuthenticationMode(true, true, true); // bond + MITM + secure connections
#endif

    if (!g_client)
        g_client = BLEDevice::createClient();
    else if (g_client->isConnected())
        g_client->disconnect();
    if (!g_client) {
        fail("no mem for BLE client");
        return;
    }

#if ADVUI_NIMBLE_14
    BLEDevice::setMTU(517);
    g_client->setConnectTimeout(15);
    uint8_t peerOctets[6];
    if (!parseBleAddress(g_connAddr, peerOctets)) {
        fail("bad BLE address");
        return;
    }
    const BLEAddress peer(peerOctets, g_connType);
    const bool connected = g_client->connect(peer, true);
#else
    const bool connected = g_client->connect(BLEAddress(String(g_connAddr), g_connType), g_connType, 15000);
#endif
    if (!connected) {
        fail("connect timeout");
        return;
    }
    if (g_cancelRequested.load()) {
        g_client->disconnect();
        g_linkState = BLE_IDLE;
        return;
    }
#if !ADVUI_NIMBLE_14
    g_client->setMTU(517);
#endif

    // Pair FIRST (Meshtastic characteristics require encryption). This blocks the
    // connect task while the user types the PIN — the UI thread stays free.
    if (!g_client->secureConnection()) {
        g_client->disconnect();
        fail("pairing failed");
        return;
    }
    if (g_cancelRequested.load()) {
        g_client->disconnect();
        g_linkState = BLE_IDLE;
        return;
    }

    BLERemoteService *svc = g_client->getService(BLEUUID(MESH_SERVICE_UUID));
    if (!svc) {
        g_client->disconnect();
        fail("no mesh service");
        return;
    }
    g_toRadio = svc->getCharacteristic(BLEUUID(TORADIO_UUID));
    g_fromRadio = svc->getCharacteristic(BLEUUID(FROMRADIO_UUID));
    g_fromNum = svc->getCharacteristic(BLEUUID(FROMNUM_UUID));
    if (!g_toRadio || !g_fromRadio) {
        g_client->disconnect();
        fail("no radio chars");
        return;
    }
    if (g_fromNum && g_fromNum->canNotify())
#if ADVUI_NIMBLE_14
    g_fromNum->subscribe(true, fromNumNotify);
#else
    g_fromNum->registerForNotify(fromNumNotify);
#endif

    g_linkState = BLE_CONNECTED;
    g_fromNumPing = true; // drain whatever is queued
    LOG_INFO("advui: ble link up to %s", g_connAddr);
    startConfig();
}

// Routes one decoded FromRadio frame into the companion state (pump task).
bool routeFromRadio(const uint8_t *bytes, uint16_t len)
{
    if (!initQueues())
        return false;
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_default;
    if (!pb_decode_from_bytes(bytes, len, &meshtastic_FromRadio_msg, &fr))
        return false;
    switch (fr.which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        beginCompanionConfigStream(fr.my_info.my_node_num);
        break;
    case meshtastic_FromRadio_node_info_tag: {
        const meshtastic_NodeInfo &ni = fr.node_info;
        StateGuard guard;
        const int nodeCount = g_compNodeCount.load(std::memory_order_relaxed);
        int slot = -1;
        for (int i = 0; i < nodeCount; i++)
            if (g_buffers->nodes[i].num == ni.num)
                slot = i;
        const bool alreadyMirrored = slot >= 0;
        // Config stream: one node_info per DB entry, so counting the stream gives
        // the true DB size even past our mirror. Once a full mirror starts
        // evicting entries, absence no longer proves a live update is a new node.
        if (shouldIncrementNodesSeen(g_linkConfigDone.load(std::memory_order_relaxed), slot, nodeCount,
                                     kMaxCompNodes))
            g_compNodesSeen.fetch_add(1, std::memory_order_relaxed);
        if (slot < 0 && nodeCount < kMaxCompNodes) {
            slot = nodeCount;
            g_compNodeCount.store(nodeCount + 1, std::memory_order_relaxed);
        }
        if (slot < 0) { // table full: evict the longest-silent node
            uint32_t oldest = 0xFFFFFFFF;
            for (int i = 0; i < nodeCount; i++)
                if (g_buffers->nodes[i].lastHeard < oldest && g_buffers->nodes[i].num != g_linkMyNode) {
                    oldest = g_buffers->nodes[i].lastHeard;
                    slot = i;
                }
        }
        if (slot >= 0) {
            CompNode &c = g_buffers->nodes[slot];
            // A table-full insertion reuses another node's slot. Partial live
            // NodeInfo packets are allowed to omit user, so clear the evicted
            // identity before filling the fields that are actually present.
            // For an existing identity, preserve its last known user block.
            if (!alreadyMirrored)
                c = {};
            c.num = ni.num;
            c.lastHeard = ni.last_heard;
            c.snr = ni.snr;
            c.hops = ni.has_hops_away ? ni.hops_away : 255;
            if (ni.has_user) {
                snprintf(c.shortName, sizeof(c.shortName), "%s", ni.user.short_name);
                snprintf(c.longName, sizeof(c.longName), "%s", ni.user.long_name);
                c.hasKey = ni.user.public_key.size == 32;
            }
        }
        if (ni.num == g_linkMyNode && ni.has_device_metrics && ni.device_metrics.has_battery_level)
            g_linkNodeBatt = ni.device_metrics.battery_level; // the radio node's own battery
        break;
    }
    case meshtastic_FromRadio_channel_tag:
        if (fr.channel.index >= 0 && fr.channel.index < 8) {
            StateGuard guard;
            g_buffers->channels[fr.channel.index] = fr.channel;
        }
        break;
    case meshtastic_FromRadio_config_tag:
        if (fr.config.which_payload_variant == meshtastic_Config_lora_tag) {
            bleUpdateCompLora(fr.config.payload_variant.lora);
        } else if (fr.config.which_payload_variant == meshtastic_Config_device_tag) {
            bleUpdateCompDevice(fr.config.payload_variant.device);
        }
        break;
    case meshtastic_FromRadio_config_complete_id_tag:
        g_linkConfigDone = true;
        LOG_INFO("advui: companion config complete, %d nodes, preset=%d", (int)g_compNodeCount, (int)g_compPreset);
        break;
    case meshtastic_FromRadio_packet_tag:
    case meshtastic_FromRadio_queueStatus_tag: { // mesh traffic and TX admission results -> UI pipeline
        BleFrame frame = {};
        if (len <= sizeof(frame.data)) {
            frame.len = len;
            memcpy(frame.data, bytes, len);
        }
        if (len > sizeof(frame.data) || !g_rxQueue || xQueueSend(g_rxQueue, &frame, 0) != pdTRUE) {
            const uint32_t drops = g_rxDrops.fetch_add(1) + 1;
            if (drops == 1 || (drops & (drops - 1)) == 0)
                LOG_WARN("advui: BLE RX queue dropped %u frame(s)", (unsigned)drops);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

// After the link is up this task becomes the transport pump: it owns all blocking
// GATT I/O so the UI thread never stalls on BLE.
void pumpLoop()
{
    uint32_t lastRssiMs = 0;
    while (!g_cancelRequested.load() && g_linkState == BLE_CONNECTED && g_client && g_client->isConnected()) {
        if (!Throttle::isWithinTimespanMs(lastRssiMs, 3000)) { // refresh link RSSI for the status page
            lastRssiMs = millis();
            g_linkRssi = g_client->getRssi();
        }
        TxFrame tx;
        while (g_toRadio && g_txQueue && xQueueReceive(g_txQueue, &tx, 0) == pdTRUE) { // outbound first
            bool ok = g_toRadio->writeValue(tx.data, tx.len, true);
            LOG_INFO("advui: toRadio write %s (%u bytes)", ok ? "ok" : "FAILED", (unsigned)tx.len);
            if (!ok)
                g_txDrops.fetch_add(1);
        }
        if (g_fromNumPing.exchange(false)) {
            int n = 0;
            for (; n < 64 && g_fromRadio; n++) {
                auto v = g_fromRadio->readValue();
                if (v.length() == 0)
                    break;
                g_linkRxPkts.fetch_add(1);
                routeFromRadio((const uint8_t *)v.c_str(), v.length());
            }
            if (n == 64) // hit the burst cap: the queue may still hold more — keep draining
                g_fromNumPing = true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (g_linkState == BLE_CONNECTED)
        fail("link dropped");
}

void connectTask(void *)
{
    try {
        connectBlocking();
        if (g_linkState == BLE_CONNECTED)
            pumpLoop();
    } catch (const std::bad_alloc &) {
        // NimBLE 1.4 builds its client/GATT graph with throwing new/vector
        // operations. Heap pressure must fail the optional companion link, not
        // terminate the firmware and reboot the whole messenger.
        if (g_client && g_client->isConnected())
            g_client->disconnect();
        fail("no mem for BLE link");
    }
    // The worker owns the discovered services/characteristics. Publish their
    // release before another worker or a new scan is allowed to start.
    g_toRadio = g_fromRadio = g_fromNum = nullptr;
#if ADVUI_NIMBLE_14
    // connect(..., true) deletes the old graph only after a successful new
    // connection. Releasing here prevents failed reconnects from retaining the
    // previous services while a fresh 9-16 KiB worker stack is allocated.
    if (g_client)
        g_client->deleteServices();
#endif
    resetCompanionState();
    g_connectTaskRunning.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

} // namespace

#ifdef ADVUI_HIL
namespace
{
uint32_t g_hilIngressFirstHeap = 0;
uint32_t g_hilIngressLastHeap = 0;
uint32_t g_hilIngressCount = 0;
uint32_t g_hilIngressLastBeforeHeap = 0;
int32_t g_hilIngressDirectDelta = 0;
uint32_t g_hilIngressMaxDrop = 0;
} // namespace

bool bleHilInjectFromRadio(const uint8_t *bytes, uint16_t len)
{
    if (!bytes || !len || !initQueues())
        return false;
    const uint32_t before = ESP.getFreeHeap();
    if (!g_hilIngressCount)
        g_hilIngressFirstHeap = before;
    const bool ok = routeFromRadio(bytes, len);
    const uint32_t after = ESP.getFreeHeap();
    g_hilIngressLastBeforeHeap = before;
    g_hilIngressLastHeap = after;
    g_hilIngressDirectDelta += static_cast<int32_t>(after) - static_cast<int32_t>(before);
    if (before > after)
        g_hilIngressMaxDrop = std::max(g_hilIngressMaxDrop, before - after);
    g_hilIngressCount++;
    return ok;
}

void bleHilClearState()
{
    resetCompanionState();
    g_hilIngressFirstHeap = 0;
    g_hilIngressLastHeap = 0;
    g_hilIngressCount = 0;
    g_hilIngressLastBeforeHeap = 0;
    g_hilIngressDirectDelta = 0;
    g_hilIngressMaxDrop = 0;
}

void bleHilReleaseState()
{
    // Production changes radio backend only through a reboot. The combined HIL
    // process exercises both backends in one boot, so explicitly tear down the
    // companion-only arena before measuring the onboard storage phase instead
    // of charging LittleFS for an impossible co-resident configuration.
    resetCompanionState();
    // Both queues were created with xQueueCreateStatic: their control blocks and
    // storage live inside g_buffers. vQueueDelete would hand an interior pointer
    // to the heap allocator on this FreeRTOS build and corrupt its accounting.
    // No worker exists in USB HIL, so invalidating the handles before freeing the
    // single owning arena is the complete teardown.
    g_rxQueue = nullptr;
    g_txQueue = nullptr;
    free(g_buffers);
    g_buffers = nullptr;
    g_hilIngressFirstHeap = 0;
    g_hilIngressLastHeap = 0;
    g_hilIngressCount = 0;
    g_hilIngressLastBeforeHeap = 0;
    g_hilIngressDirectDelta = 0;
    g_hilIngressMaxDrop = 0;
}

uint32_t bleHilIngressFirstHeap() { return g_hilIngressFirstHeap; }
uint32_t bleHilIngressLastHeap() { return g_hilIngressLastHeap; }
uint32_t bleHilIngressCount() { return g_hilIngressCount; }
uint32_t bleHilIngressLastBeforeHeap() { return g_hilIngressLastBeforeHeap; }
int32_t bleHilIngressDirectDelta() { return g_hilIngressDirectDelta; }
uint32_t bleHilIngressMaxDrop() { return g_hilIngressMaxDrop; }
bool bleHilBuffersReady() { return g_buffers && g_rxQueue && g_txQueue; }
#endif

void bleAdvSlow(bool slow)
{
    static bool slowed = false;
    if (slow == slowed)
        return; // idempotent: don't churn advertising on every wake
    if (!BLEDevice::getInitialized())
        return; // BLE is off (e.g. WiFi mode): nothing to trim
    BLEAdvertising *adv = BLEDevice::getAdvertising();
    if (!adv)
        return;
    bool was = adv->isAdvertising();
    if (was)
        adv->stop();
    // Units of 0.625 ms: ~1.0-1.2 s asleep, 0 = the stack's fast defaults. Set
    // this even while a phone is connected (was=false), so advertising resumes
    // slowly if that phone disconnects while the screen remains dark.
    adv->setMinInterval(slow ? 1600 : 0);
    adv->setMaxInterval(slow ? 1920 : 0);
    if (was)
        adv->start(0);
    slowed = slow;
    LOG_INFO("advui: ble advertising %s", slow ? "slowed for sleep" : "restored");
}

bool bleCompanionInit()
{
#ifdef ADVUI_HIL
    // HIL ingress calls initQueues() directly.  Never initialise the physical
    // BLE client stack in a test image: scan/connect/write are all fail-closed.
    g_bleUnsupported = true;
    return false;
#else
    if (g_bleInitFailed)
        return false; // a partial NimBLE init is not safe to repeat this boot
    if (!initQueues()) {
        fail("no mem for companion");
        return false;
    }
    if (g_bleInited)
        return true;
    try {
        BLEDevice::init("advui"); // no-op if the stock peripheral initialised the stack already
        g_bleInited = true;
        return true;
    } catch (const std::bad_alloc &) {
        // NimBLE creates its host/controller state with throwing STL
        // allocations before the protected connect worker exists.
        g_bleInitFailed = true;
        // No worker can exist yet. The queues are static objects inside this
        // single arena, so invalidate their handles and return its ~9 KB too.
        g_rxQueue = nullptr;
        g_txQueue = nullptr;
        free(g_buffers);
        g_buffers = nullptr;
        fail("no mem for BLE init");
        return false;
    }
#endif
}

void bleScanStart()
{
#ifdef ADVUI_HIL
    g_linkState = BLE_FAILED;
    return;
#else
    if (!bleCompanionInit())
        return;
    bool scanning = false;
    bleScanSnapshot(nullptr, 0, &scanning);
    if (scanning)
        return;
    {
        StateGuard guard;
        g_scanCount = 0;
    }
    struct ble_gap_disc_params p = {};
    p.passive = 0; // active: names ride in scan responses
    p.itvl = 160;  // 100 ms (0.625 ms units)
    p.window = 128; // 80 ms
    p.filter_duplicates = 0; // we dedupe ourselves; controller cache is only 10 deep
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 8000, &p, scanGapEvent, nullptr);
    {
        StateGuard guard;
        g_scanning = rc == 0;
    }
    LOG_INFO("advui: ble scan %s (rc=%d)", rc == 0 ? "started" : "FAILED", rc);
#endif
}

void bleScanStop()
{
    ble_gap_disc_cancel();
    StateGuard guard;
    g_scanning = false;
}

void bleConnectAsync(const char *addr, uint8_t addrType)
{
#ifdef ADVUI_HIL
    (void)addr;
    (void)addrType;
    g_linkState = BLE_FAILED;
    return;
#else
    if (!bleCompanionInit())
        return;
    if (!validBleAddress(addr) || addrType > 3) {
        fail("bad BLE address");
        return;
    }
    if (g_linkState == BLE_CONNECTING || g_linkState == BLE_PAIRING)
        return; // one attempt at a time
    if (g_linkState == BLE_CONNECTED && !bleDisconnect())
        return; // R while connected = a clean reconnect, not a second client
    bool expected = false;
    if (!g_connectTaskRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return; // the previous worker is still releasing its GATT objects
    g_cancelRequested.store(false);
    snprintf(g_connAddr, sizeof(g_connAddr), "%s", addr);
    g_connType = addrType;
    LOG_INFO("advui: connect %s type=%d heap=%u", g_connAddr, g_connType, (unsigned)ESP.getFreeHeap());
    // GATT+SMP want a roomy stack, but the no-PSRAM heap may not have it — step down.
    static const uint32_t stacks[] = {16384, 12288, 9216};
    for (uint32_t st : stacks)
        if (xTaskCreate(connectTask, "advuiBle", st, nullptr, 2, nullptr) == pdPASS) {
            LOG_INFO("advui: connect task started, stack=%u", (unsigned)st);
            return;
        }
    g_connectTaskRunning.store(false, std::memory_order_release);
    fail("no mem for link task");
#endif
}

bool bleDisconnect()
{
    // Make pumpLoop stop before touching the client. BLEClient::disconnect()
    // then wakes any in-flight GATT wait; the worker performs all pointer/reset
    // cleanup itself, avoiding a use-after-free during reconnect/forget.
    g_cancelRequested.store(true);
    g_pinValue.store(0);
    g_pinReady.store(true); // release a pairing callback waiting for UI input
    g_linkState = BLE_IDLE;
    if (g_client && g_client->isConnected())
        g_client->disconnect();
    for (int i = 0; i < 200 && g_connectTaskRunning.load(std::memory_order_acquire); i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (g_connectTaskRunning.load(std::memory_order_acquire)) {
        fail("link task busy");
        return false;
    }
    resetCompanionState(); // also covers idle/failed states with no worker
    return true;
}

void bleSubmitPin(uint32_t pin)
{
    g_pinValue = pin;
    g_pinReady = true;
}

void bleCancelPin()
{
    g_pinValue = 0;
    g_pinReady = true; // returns 0 -> pairing fails cleanly
}

bool bleWriteToRadio(const uint8_t *buf, size_t len)
{
#ifdef ADVUI_HIL
    (void)buf;
    (void)len;
    return false;
#else
    if (g_linkState != BLE_CONNECTED || !g_toRadio)
        return false;
    return g_toRadio->writeValue(const_cast<uint8_t *>(buf), len, true);
#endif
}

// The transport pump lives in the link task (see pumpLoop) — the UI thread must
// never issue blocking GATT reads. Kept as a no-op for the runOnce call site.
void blePump() {}

} // namespace advui
