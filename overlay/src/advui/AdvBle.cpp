#include "AdvBle.h"
#include "BluetoothCommon.h" // MESH_SERVICE_UUID + characteristic UUIDs
#include "DebugConfiguration.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/mesh-pb-constants.h"
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <cstdio>
#include <cstring>
#include <host/ble_gap.h>
#include <host/ble_hs_adv.h>

namespace advui
{

BleScanHit g_scanHits[kMaxScanHits];
int g_scanCount = 0;
bool g_scanning = false;
bool g_bleUnsupported = false; // roles are compiled in via our custom_sdkconfig

volatile BleLinkState g_linkState = BLE_IDLE;
volatile bool g_pinRequested = false;
volatile uint32_t g_linkRxPkts = 0;
volatile uint32_t g_linkMyNode = 0;
char g_linkErr[28] = {0};

CompNode g_compNodes[kMaxCompNodes];
volatile int g_compNodeCount = 0;
meshtastic_Channel g_compChans[8];
volatile bool g_linkConfigDone = false;
volatile int g_compPreset = 0;
meshtastic_Config_LoRaConfig g_compLora = meshtastic_Config_LoRaConfig_init_default;
volatile bool g_compLoraValid = false;
volatile int g_linkNodeBatt = -1;
volatile int g_linkRssi = 0;

namespace
{
// pump -> UI: raw FromRadio frames (mesh packets only)
constexpr int kRxRing = 6, kRxSlot = 512;
uint8_t g_rxRing[kRxRing][kRxSlot];
uint16_t g_rxLen[kRxRing];
volatile int g_rxHead = 0, g_rxTail = 0; // head = writer (pump), tail = reader (UI)
// UI -> pump: encoded ToRadio frames
constexpr int kTxRing = 4, kTxSlot = 300;
uint8_t g_txRing[kTxRing][kTxSlot];
uint16_t g_txLen[kTxRing];
volatile int g_txHead = 0, g_txTail = 0;
} // namespace

bool bleNextPacket(uint8_t *buf, uint16_t *len)
{
    if (g_rxTail == g_rxHead)
        return false;
    int t = g_rxTail;
    memcpy(buf, g_rxRing[t], g_rxLen[t]);
    *len = g_rxLen[t];
    g_rxTail = (t + 1) % kRxRing;
    return true;
}

bool bleQueueToRadio(const uint8_t *buf, uint16_t len)
{
    int h = g_txHead, next = (h + 1) % kTxRing;
    if (next == g_txTail || len > kTxSlot)
        return false; // full
    memcpy(g_txRing[h], buf, len);
    g_txLen[h] = len;
    g_txHead = next;
    return true;
}

namespace
{

bool g_bleInited = false;
BLEClient *g_client = nullptr;
BLERemoteCharacteristic *g_toRadio = nullptr;
BLERemoteCharacteristic *g_fromRadio = nullptr;
BLERemoteCharacteristic *g_fromNum = nullptr;
volatile bool g_fromNumPing = false; // notify fired: FromRadio has data to drain
volatile bool g_pinReady = false;
volatile uint32_t g_pinValue = 0;
char g_connAddr[18] = {0};
uint8_t g_connType = 0;

void fail(const char *why)
{
    snprintf(g_linkErr, sizeof(g_linkErr), "%s", why);
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

    int slot = -1;
    for (int i = 0; i < g_scanCount; i++)
        if (!strcmp(g_scanHits[i].addr, addr))
            slot = i;
    if (slot < 0) {
        if (!mesh || g_scanCount >= kMaxScanHits)
            return 0; // only track Meshtastic advertisers
        slot = g_scanCount++;
        BleScanHit &h = g_scanHits[slot];
        memcpy(h.addr, addr, sizeof(h.addr));
        h.type = ev->disc.addr.type;
        h.name[0] = 0;
        LOG_INFO("advui: ble hit %s type=%d", addr, ev->disc.addr.type);
    }
    BleScanHit &h = g_scanHits[slot];
    h.rssi = ev->disc.rssi;
    if (f.name && f.name_len && !h.name[0]) { // name usually rides in the scan response
        int n = f.name_len < (int)sizeof(h.name) - 1 ? f.name_len : (int)sizeof(h.name) - 1;
        memcpy(h.name, f.name, n);
        h.name[n] = 0;
    }
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
        return g_pinReady ? g_pinValue : 0;
    }
    bool onSecurityRequest() override { return true; }
    bool onConfirmPIN(uint32_t) override { return true; }
    bool onAuthorizationRequest(uint16_t, uint16_t, bool) override { return true; }
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
    g_linkRxPkts = 0;
    g_linkMyNode = 0;
    g_linkConfigDone = false; // a fresh connect re-streams the config

    BLEDevice::setSecurityCallbacks(&g_secCb);
    BLESecurity::setCapability(ESP_IO_CAP_IN);          // node shows the PIN, we enter it
    BLESecurity::setAuthenticationMode(true, true, true); // bond + MITM + secure connections

    if (!g_client)
        g_client = BLEDevice::createClient();
    else if (g_client->isConnected())
        g_client->disconnect();

    if (!g_client->connect(BLEAddress(String(g_connAddr), g_connType), g_connType, 15000)) {
        fail("connect timeout");
        return;
    }
    g_client->setMTU(517);

    // Pair FIRST (Meshtastic characteristics require encryption). This blocks the
    // connect task while the user types the PIN — the UI thread stays free.
    if (!g_client->secureConnection()) {
        g_client->disconnect();
        fail("pairing failed");
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
        g_fromNum->registerForNotify(fromNumNotify);

    g_linkState = BLE_CONNECTED;
    g_fromNumPing = true; // drain whatever is queued
    LOG_INFO("advui: ble link up to %s", g_connAddr);
    startConfig();
}

// Routes one decoded FromRadio frame into the companion state (pump task).
void routeFromRadio(const uint8_t *bytes, uint16_t len)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_default;
    if (!pb_decode_from_bytes(bytes, len, &meshtastic_FromRadio_msg, &fr))
        return;
    switch (fr.which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        g_linkMyNode = fr.my_info.my_node_num;
        break;
    case meshtastic_FromRadio_node_info_tag: {
        const meshtastic_NodeInfo &ni = fr.node_info;
        int slot = -1;
        for (int i = 0; i < g_compNodeCount; i++)
            if (g_compNodes[i].num == ni.num)
                slot = i;
        if (slot < 0 && g_compNodeCount < kMaxCompNodes)
            slot = g_compNodeCount++;
        if (slot < 0) { // table full: evict the longest-silent node
            uint32_t oldest = 0xFFFFFFFF;
            for (int i = 0; i < g_compNodeCount; i++)
                if (g_compNodes[i].lastHeard < oldest && g_compNodes[i].num != g_linkMyNode) {
                    oldest = g_compNodes[i].lastHeard;
                    slot = i;
                }
        }
        if (slot >= 0) {
            CompNode &c = g_compNodes[slot];
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
        if (fr.channel.index >= 0 && fr.channel.index < 8)
            g_compChans[fr.channel.index] = fr.channel;
        break;
    case meshtastic_FromRadio_config_tag:
        if (fr.config.which_payload_variant == meshtastic_Config_lora_tag) {
            g_compPreset = (int)fr.config.payload_variant.lora.modem_preset;
            g_compLora = fr.config.payload_variant.lora; // full blob: remote admin round-trips it
            g_compLoraValid = true;
        }
        break;
    case meshtastic_FromRadio_config_complete_id_tag:
        g_linkConfigDone = true;
        LOG_INFO("advui: companion config complete, %d nodes, preset=%d", (int)g_compNodeCount, (int)g_compPreset);
        break;
    case meshtastic_FromRadio_packet_tag: { // mesh traffic: hand off to the UI pipeline
        int h = g_rxHead, next = (h + 1) % kRxRing;
        if (next != g_rxTail && len <= kRxSlot) {
            memcpy(g_rxRing[h], bytes, len);
            g_rxLen[h] = len;
            g_rxHead = next;
        }
        break;
    }
    default:
        break;
    }
}

// After the link is up this task becomes the transport pump: it owns all blocking
// GATT I/O so the UI thread never stalls on BLE.
void pumpLoop()
{
    uint32_t lastRssiMs = 0;
    while (g_linkState == BLE_CONNECTED && g_client && g_client->isConnected()) {
        if (millis() - lastRssiMs > 3000) { // refresh the link RSSI for the status page
            lastRssiMs = millis();
            g_linkRssi = g_client->getRssi();
        }
        while (g_txTail != g_txHead && g_toRadio) { // outbound first: sends feel instant
            int t = g_txTail;
            bool ok = g_toRadio->writeValue(g_txRing[t], g_txLen[t], true);
            LOG_INFO("advui: toRadio write %s (%u bytes)", ok ? "ok" : "FAILED", (unsigned)g_txLen[t]);
            g_txTail = (t + 1) % kTxRing;
        }
        if (g_fromNumPing) {
            g_fromNumPing = false;
            int n = 0;
            for (; n < 64 && g_fromRadio; n++) {
                String v = g_fromRadio->readValue();
                if (v.length() == 0)
                    break;
                g_linkRxPkts = g_linkRxPkts + 1;
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
    connectBlocking();
    if (g_linkState == BLE_CONNECTED)
        pumpLoop();
    vTaskDelete(nullptr);
}

} // namespace

void bleCompanionInit()
{
    if (g_bleInited)
        return;
    BLEDevice::init("advui"); // no-op if the stock peripheral initialised the stack already
    g_bleInited = true;
}

void bleScanStart()
{
    bleCompanionInit();
    if (g_scanning)
        return;
    g_scanCount = 0;
    struct ble_gap_disc_params p = {};
    p.passive = 0; // active: names ride in scan responses
    p.itvl = 160;  // 100 ms (0.625 ms units)
    p.window = 128; // 80 ms
    p.filter_duplicates = 0; // we dedupe ourselves; controller cache is only 10 deep
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 8000, &p, scanGapEvent, nullptr);
    g_scanning = rc == 0;
    LOG_INFO("advui: ble scan %s (rc=%d)", g_scanning ? "started" : "FAILED", rc);
}

void bleScanStop()
{
    ble_gap_disc_cancel();
    g_scanning = false;
}

void bleConnectAsync(const char *addr, uint8_t addrType)
{
    bleCompanionInit();
    if (g_linkState == BLE_CONNECTING || g_linkState == BLE_PAIRING)
        return; // one attempt at a time
    if (g_linkState == BLE_CONNECTED)
        bleDisconnect(); // R while connected = a clean reconnect, not a second client
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
    fail("no mem for link task");
}

void bleDisconnect()
{
    if (g_client && g_client->isConnected())
        g_client->disconnect();
    g_toRadio = g_fromRadio = g_fromNum = nullptr;
    g_linkState = BLE_IDLE;
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
    if (g_linkState != BLE_CONNECTED || !g_toRadio)
        return false;
    return g_toRadio->writeValue(const_cast<uint8_t *>(buf), len, true);
}

// The transport pump lives in the link task (see pumpLoop) — the UI thread must
// never issue blocking GATT reads. Kept as a no-op for the runOnce call site.
void blePump() {}

} // namespace advui
