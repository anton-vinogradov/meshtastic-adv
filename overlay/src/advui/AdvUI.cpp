#include "AdvUI.h"
#include "CyrillicFont.h"
#include "FSCommon.h"
#include "PowerStatus.h"
#include "configuration.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/mesh-pb-constants.h"
#include "modules/NodeInfoModule.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern uint32_t rebootAtMsec; // main.cpp: set to a future millis() to schedule a reboot

namespace advui
{

namespace
{

// LovyanGFX wrapper for the embedded Cyrillic font — used for message text.
const lgfx::U8g2font cyrFont(u8g2_font_9x15_t_cyrillic);

const char *nodeName(const meshtastic_NodeInfoLite *n)
{
    return n->long_name[0] ? n->long_name : n->short_name;
}

bool isFav(const meshtastic_NodeInfoLite *n)
{
    return n->bitfield & NODEINFO_BITFIELD_IS_FAVORITE_MASK;
}

// Case-insensitive substring match; empty needle matches everything.
bool ciContains(const char *hay, const char *needle)
{
    if (!needle || !needle[0])
        return true;
    for (const char *p = hay; *p; ++p) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

// Incoming text messages, kept in a small RAM ring (oldest overwritten). Enough
// to show a node's recent thread and to know who we've talked to for sorting.
enum MsgStatus : uint8_t { MSG_IN = 0, MSG_SENDING = 1, MSG_DELIVERED = 2, MSG_FAILED = 3 };

struct Msg {
    uint32_t from;
    uint32_t to; // 0xFFFFFFFF = broadcast/channel, else a DM to that node
    uint32_t rxTime;
    uint32_t id;     // our packet id (outgoing only), to match the delivery ACK
    uint8_t status;  // MsgStatus
    uint8_t err;     // routing error code when status == MSG_FAILED
    bool read;
    char text[160];
};

// Short names for meshtastic_Routing_Error, shown next to a failed message.
const char *errName(uint8_t e)
{
    switch (e) {
    case 1:
        return "no route";
    case 2:
        return "nak";
    case 3:
        return "timeout";
    case 4:
        return "no iface";
    case 5:
        return "no ack";
    case 6:
        return "no chan";
    case 7:
        return "too big";
    case 8:
        return "no resp";
    case 9:
        return "duty lim";
    case 12:
        return "pki fail";
    case 13:
        return "no pubkey";
    default:
        return "fail";
    }
}
constexpr int kMaxMsgs = 32;
constexpr int kNumSettings = 6; // Name, Short, Region, Preset, Frequency, Channel
Msg g_msgs[kMaxMsgs];
int g_msgCount = 0;         // populated slots (grows to kMaxMsgs)
int g_msgNext = 0;          // next write slot (ring head)
bool g_msgsDirty = false;   // ring changed since the last flash save
uint32_t g_lastSaveMs = 0;  // when we last wrote the ring to flash

void addMsg(uint32_t from, uint32_t to, uint32_t rxTime, bool unread, const char *text, uint32_t id, uint8_t status)
{
    Msg &m = g_msgs[g_msgNext];
    m.from = from;
    m.to = to;
    m.rxTime = rxTime;
    m.id = id;
    m.status = status;
    m.err = 0;
    m.read = !unread;
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = 0;
    g_msgNext = (g_msgNext + 1) % kMaxMsgs;
    if (g_msgCount < kMaxMsgs)
        g_msgCount++;
    g_msgsDirty = true;
}

// Mark the sent message whose id matches an incoming ACK/routing response.
void ackMsg(uint32_t reqId, uint8_t status, uint8_t err)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].id == reqId && g_msgs[i].id != 0) {
            g_msgs[i].status = status;
            g_msgs[i].err = err;
            g_msgsDirty = true;
            return;
        }
}

int unreadCount()
{
    int c = 0;
    for (int i = 0; i < g_msgCount; i++)
        if (!g_msgs[i].read)
            c++;
    return c;
}

void markReadFrom(uint32_t nodeNum)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].from == nodeNum && !g_msgs[i].read) {
            g_msgs[i].read = true;
            g_msgsDirty = true;
        }
}

// True if there's an unread message from this node (drives the envelope marker).
bool hasUnreadFrom(uint32_t nodeNum)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].from == nodeNum && !g_msgs[i].read)
            return true;
    return false;
}

// Persist the message ring (with delivery status + read flags) to flash, so a
// reboot keeps the conversation. Written debounced from runOnce.
constexpr uint32_t kMsgMagic = 0x41565331; // "AVS1" — bump if Msg layout changes
const char *kMsgPath = "/advui_msgs.bin";

void saveMsgs()
{
    auto f = FSCom.open(kMsgPath, FILE_O_WRITE);
    if (!f)
        return;
    uint32_t magic = kMsgMagic;
    int32_t cnt = g_msgCount, nxt = g_msgNext;
    f.write((const uint8_t *)&magic, sizeof(magic));
    f.write((const uint8_t *)&cnt, sizeof(cnt));
    f.write((const uint8_t *)&nxt, sizeof(nxt));
    f.write((const uint8_t *)g_msgs, sizeof(g_msgs));
    f.close();
}

void loadMsgs()
{
    auto f = FSCom.open(kMsgPath, FILE_O_READ);
    if (!f)
        return;
    uint32_t magic = 0;
    int32_t cnt = 0, nxt = 0;
    f.read((uint8_t *)&magic, sizeof(magic));
    f.read((uint8_t *)&cnt, sizeof(cnt));
    f.read((uint8_t *)&nxt, sizeof(nxt));
    size_t n = f.read((uint8_t *)g_msgs, sizeof(g_msgs));
    f.close();
    if (magic != kMsgMagic || n != sizeof(g_msgs) || cnt < 0 || cnt > kMaxMsgs || nxt < 0 || nxt >= kMaxMsgs)
        return;
    g_msgCount = cnt;
    g_msgNext = nxt;
    // an outgoing message still "sending" before the reboot can't get its ACK now
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].status == MSG_SENDING)
            g_msgs[i].status = MSG_FAILED;
}

// Default node ordering: favourites first, then nodes we have a conversation with,
// then everyone else by hop distance (nearest first; unknown hops last). Ties are
// broken by most-recently-heard.
bool nodeLess(uint16_t a, uint16_t b)
{
    const meshtastic_NodeInfoLite *na = nodeDB->getMeshNodeByIndex(a);
    const meshtastic_NodeInfoLite *nb = nodeDB->getMeshNodeByIndex(b);
    auto bucket = [](const meshtastic_NodeInfoLite *n) { return hasUnreadFrom(n->num) ? 0 : isFav(n) ? 1 : 2; };
    int ba = bucket(na), bb = bucket(nb);
    if (ba != bb)
        return ba < bb;
    int ha = na->has_hops_away ? na->hops_away : 255;
    int hb = nb->has_hops_away ? nb->hops_away : 255;
    if (ha != hb)
        return ha < hb;
    return na->last_heard > nb->last_heard;
}

// Trim s in place until it fits within budget px in the current font.
void fitWidth(lgfx::LGFXBase *g, char *s, int budget)
{
    while (s[0] && g->textWidth(s) > budget)
        s[strlen(s) - 1] = 0;
}

// SNR -> 0..4 signal bars. 0 means no direct SNR (node heard only via relays).
int sigLevel(const meshtastic_NodeInfoLite *n)
{
    float snr = n->snr; // live SNR is in the float field; snr_q4 is the on-disk form, zeroed in RAM
    if (snr == 0)
        return 0; // no direct SNR (heard only via relays / never directly)
    if (snr >= 5)
        return 4;
    if (snr >= 0)
        return 3;
    if (snr >= -7)
        return 2;
    return 1;
}

const char *roleTag(const meshtastic_NodeInfoLite *n)
{
    switch (n->role) {
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
        return "RTR";
    case meshtastic_Config_DeviceConfig_Role_REPEATER:
        return "RPT";
    case meshtastic_Config_DeviceConfig_Role_TRACKER:
        return "TRK";
    case meshtastic_Config_DeviceConfig_Role_SENSOR:
        return "SEN";
    case meshtastic_Config_DeviceConfig_Role_TAK:
        return "TAK";
    default:
        return "CLI";
    }
}

const char *regionName(meshtastic_Config_LoRaConfig_RegionCode r)
{
    switch (r) {
    case meshtastic_Config_LoRaConfig_RegionCode_UNSET:
        return "UNSET";
    case meshtastic_Config_LoRaConfig_RegionCode_US:
        return "US";
    case meshtastic_Config_LoRaConfig_RegionCode_EU_868:
        return "EU_868";
    case meshtastic_Config_LoRaConfig_RegionCode_RU:
        return "RU";
    default: {
        static char buf[8];
        snprintf(buf, sizeof(buf), "R%d", (int)r);
        return buf;
    }
    }
}

const char *presetName(meshtastic_Config_LoRaConfig_ModemPreset p)
{
    switch (p) {
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST:
        return "LongFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW:
        return "LongSlow";
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE:
        return "LongMod";
    case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW:
        return "MedSlow";
    case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST:
        return "MediumFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW:
        return "ShortSlow";
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST:
        return "ShortFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO:
        return "ShortTurbo";
    default:
        return "custom";
    }
}

// Pickable enum values for the settings list-pickers (name + raw enum value).
struct EnumOpt {
    const char *name;
    int value;
};
const EnumOpt kRegionOpts[] = {{"UNSET", 0},   {"US", 1},      {"EU_433", 2}, {"EU_868", 3}, {"CN", 4},
                               {"JP", 5},       {"ANZ", 6},     {"KR", 7},     {"TW", 8},     {"RU", 9},
                               {"IN", 10},      {"NZ_865", 11}, {"TH", 12},    {"UA_433", 14},
                               {"UA_868", 15},  {"KZ_433", 23}, {"KZ_863", 24}};
const EnumOpt kPresetOpts[] = {{"LongFast", 0},  {"LongSlow", 1},  {"LongMod", 7},    {"MedSlow", 3},
                               {"MediumFast", 4}, {"ShortSlow", 5}, {"ShortFast", 6},  {"ShortTurbo", 8}};
constexpr int kRegionCount = sizeof(kRegionOpts) / sizeof(kRegionOpts[0]);
constexpr int kPresetCount = sizeof(kPresetOpts) / sizeof(kPresetOpts[0]);

int optIndex(const EnumOpt *opts, int cnt, int value)
{
    for (int i = 0; i < cnt; i++)
        if (opts[i].value == value)
            return i;
    return 0;
}

// Text-editor (MODE_SETNAME) targets: 0 long name, 1 short name, 2 frequency, 3 channel.
unsigned editMax(int t)
{
    return t == 1 ? 4 : t == 2 ? 9 : t == 3 ? 11 : 24;
}
const char *editTitle(int t)
{
    return t == 1 ? "Set short name" : t == 2 ? "Set frequency (MHz)" : t == 3 ? "Set channel name" : "Set long name";
}

// The node row (list and picker): proportional name on the left, then a compact
// Font0 meta cluster in FIXED columns so rows line up — signal bars, hops (arrow,
// not "h", so it can't be mistaken for the age), last-heard age, and role.
void drawNodeRow(lgfx::LGFXBase *g, const meshtastic_NodeInfoLite *n, int y, bool self)
{
    const int rowH = 18, metaY = y + 5, cw = 6;
    const int xBars = 150;  // signal bars, left edge
    const int xHopsR = 186; // hops, right edge
    const int xAgeR = 212;  // age, right edge
    const int xRole = 216;  // role, left edge

    const int nbars = 4, barW = 3, barGap = 1;
    int level = self ? -1 : sigLevel(n);
    int barBase = y + rowH - 3;
    const int bh[4] = {4, 7, 10, 13};
    const uint16_t barCol[4] = {0xF800, 0xFD20, 0xFFE0, 0x07E0}; // red -> orange -> yellow -> green
    for (int i = 0; i < nbars; i++) {
        uint16_t c = (level > 0 && i < level) ? barCol[i] : 0x4208; // dim gray for empty/relayed
        g->fillRect(xBars + i * (barW + barGap), barBase - bh[i], barW, bh[i], c);
    }

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);

    char hbuf[6];
    if (n->has_hops_away)
        snprintf(hbuf, sizeof(hbuf), "\x1a%u", (unsigned)n->hops_away); // 0x1A = right arrow
    else
        strcpy(hbuf, "\x1a?");
    g->setTextColor(0x9CD3); // light blue
    g->setCursor(xHopsR - (int)strlen(hbuf) * cw, metaY);
    g->print(hbuf);

    char abuf[6];
    uint16_t acol;
    if (!n->last_heard) {
        strcpy(abuf, "?");
        acol = 0x630C;
    } else {
        uint32_t s = sinceLastSeen(n);
        if (s > 100u * 86400u) {
            strcpy(abuf, "?");
            acol = 0x630C;
        } else if (s < 60) {
            strcpy(abuf, "now");
            acol = 0x07E0; // green: fresh
        } else if (s < 3600) {
            snprintf(abuf, sizeof(abuf), "%um", (unsigned)(s / 60));
            acol = s < 300 ? 0x07E0 : 0xC618;
        } else if (s < 86400) {
            snprintf(abuf, sizeof(abuf), "%uh", (unsigned)(s / 3600));
            acol = 0x8410;
        } else {
            snprintf(abuf, sizeof(abuf), "%ud", (unsigned)(s / 86400));
            acol = 0x630C; // dim: stale
        }
    }
    g->setTextColor(acol);
    g->setCursor(xAgeR - (int)strlen(abuf) * cw, metaY);
    g->print(abuf);

    g->setTextColor(0x8410); // gray
    g->setCursor(xRole, metaY);
    g->print(roleTag(n));

    // envelope marker for a node with unread messages, before the name
    int nameX = 4;
    if (!self && hasUnreadFrom(n->num)) {
        int ex = 4, ey = y + 5;
        g->drawRect(ex, ey, 11, 8, 0xF800); // red envelope
        g->drawLine(ex, ey, ex + 5, ey + 4, 0xF800);
        g->drawLine(ex + 10, ey, ex + 5, ey + 4, 0xF800);
        nameX = 18;
    }

    bool fav = isFav(n);
    char nm[28];
    const char *name = nodeName(n);
    if (name[0])
        snprintf(nm, sizeof(nm), "%s", name);
    else
        snprintf(nm, sizeof(nm), "!%08x", (unsigned)n->num);

    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(1);
    fitWidth(g, nm, xBars - 6 - nameX);
    g->setTextColor(fav ? 0xFFE0 : 0xFFFF); // favourite = yellow
    g->setCursor(nameX, y + 2);
    g->print(nm);
}

void drawFooter(lgfx::LGFXBase *g, const char *hint)
{
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x630c); // dim gray
    g->setCursor(4, 126);
    g->print(hint);
}

// Delivery-status glyph for an outgoing message, drawn at the right of its line.
void drawMsgStatus(lgfx::LGFXBase *g, int x, int y, uint8_t status)
{
    if (status == MSG_SENDING) {
        g->fillCircle(x + 6, y + 6, 2, 0xFFE0); // yellow dot: in flight
    } else if (status == MSG_DELIVERED) {
        g->drawLine(x + 2, y + 6, x + 5, y + 9, 0x07E0); // green check: acked
        g->drawLine(x + 5, y + 9, x + 12, y + 2, 0x07E0);
    } else if (status == MSG_FAILED) {
        g->drawLine(x + 3, y + 2, x + 11, y + 10, 0xF800); // red x: failed
        g->drawLine(x + 11, y + 2, x + 3, y + 10, 0xF800);
    }
}

} // namespace

AdvUI::AdvUI() : concurrency::OSThread("advui") {}

void AdvUI::initHardware()
{
    pinMode(38, OUTPUT); // display power/backlight rail — steady HIGH, not PWM
    digitalWrite(38, HIGH);

    bool ok = display.init();
    display.setRotation(1); // landscape 240x135
    display.fillScreen(0x0000);

    // 8-bit (rgb332) frame buffer: 32KB instead of 64KB. The full 16-bit buffer
    // starved the internal DMA pool so PKI crypto (esp-aes) couldn't allocate and
    // DMs failed to send. Colours coarsen slightly but stay recognisable.
    canvas.setColorDepth(8);
    haveCanvas = (canvas.createSprite(display.width(), display.height()) != nullptr);
    LOG_INFO("advui: UI up init=%d %dx%d canvas=%d", (int)ok, display.width(), display.height(),
             (int)haveCanvas);

    api.begin();
    kb.begin();
    LOG_INFO("advui: keyboard ready");

    loadMsgs(); // restore the saved conversation from flash
    bootMs = millis();
    drawSplash(); // branded splash instead of black while the mesh comes up
}

void AdvUI::drawSplash()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(2);
    g->setTextColor(0xFFFF);
    const char *t1 = "Meshtastic";
    g->setCursor((240 - g->textWidth(t1)) / 2, 34);
    g->print(t1);

    g->setTextColor(0x07FF); // cyan
    const char *t2 = "ADV";
    g->setCursor((240 - g->textWidth(t2)) / 2, 66);
    g->print(t2);

    g->drawFastHLine(70, 98, 100, 0x045F); // thin accent divider

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x8410);
    const char *t3 = "Cardputer";
    g->setCursor((240 - g->textWidth(t3)) / 2, 112);
    g->print(t3);

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// Picks incoming text messages out of the FromRadio stream and files them in the
// ring. Everything else (config, node DB, telemetry, ...) is ignored here.
void AdvUI::handleFromRadio(const meshtastic_FromRadio &fr)
{
    if (fr.which_payload_variant != meshtastic_FromRadio_packet_tag)
        return;
    const meshtastic_MeshPacket &p = fr.packet;
    if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag)
        return;

    // A routing response carrying our request id is the ACK/NAK for a sent message.
    // Decode it: error_reason NONE = delivered, anything else (no route, no ack,
    // max retransmit, ...) = failed. Route request/reply variants aren't acks.
    if (p.decoded.portnum == meshtastic_PortNum_ROUTING_APP && p.decoded.request_id) {
        meshtastic_Routing routing = meshtastic_Routing_init_zero;
        bool ok = pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_Routing_msg, &routing);
        if (ok && routing.which_variant == meshtastic_Routing_error_reason_tag) {
            bool delivered = routing.error_reason == meshtastic_Routing_Error_NONE;
            ackMsg(p.decoded.request_id, delivered ? MSG_DELIVERED : MSG_FAILED, (uint8_t)routing.error_reason);
        }
        return;
    }

    if (p.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return;

    char text[160];
    size_t n = p.decoded.payload.size;
    if (n > sizeof(text) - 1)
        n = sizeof(text) - 1;
    memcpy(text, p.decoded.payload.bytes, n);
    text[n] = 0;

    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;
    bool unread = (p.to == me && p.from != me); // a DM addressed to us (not a broadcast, not our echo)
    addMsg(p.from, p.to, p.rx_time, unread, text, 0, MSG_IN);
}

// Sends a text DM to a node and adds it to our own thread immediately (status
// "sending"); the delivery ACK later flips it to "delivered" via ackMsg().
void AdvUI::sendMessage(uint32_t to, const char *text)
{
    if (!router || !service)
        return;
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = to;
    p->want_ack = true;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.dest = to;
    size_t n = strlen(text);
    if (n > sizeof(p->decoded.payload.bytes))
        n = sizeof(p->decoded.payload.bytes);
    memcpy(p->decoded.payload.bytes, text, n);
    p->decoded.payload.size = n;

    // A DM to a key-capable node must be PKI-encrypted (stock forces this); without
    // it the recipient on a modern mesh won't accept/ack the message.
    meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNode(to) : nullptr;
    if (node && node->public_key.size == 32) {
        p->pki_encrypted = true;
        p->channel = 0;
    }

    uint32_t id = p->id;
    service->sendToMesh(p, RX_SRC_LOCAL, false);

    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;
    addMsg(me, to, 0, false, text, id, MSG_SENDING);
}

// Fills out[] with node-DB indices matching query (all if query is null/empty),
// in the default sorted order. Returns the count (capped at max).
int AdvUI::buildNodeList(uint16_t *out, int max, const char *query)
{
    int count = 0;
    if (!nodeDB)
        return 0;
    uint32_t me = nodeDB->getNodeNum();
    size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total && count < max; i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node)
            continue;
        if (node->num == me)
            continue; // don't list ourselves — you don't message yourself
        if (query && query[0]) {
            const char *name = nodeName(node);
            if (!ciContains(name[0] ? name : "", query))
                continue;
        }
        out[count++] = (uint16_t)i;
    }
    std::sort(out, out + count, nodeLess);
    return count;
}

void AdvUI::drawNodeList()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    size_t total = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;

    g->setFont(&lgfx::fonts::Font0); // header stays on the compact bitmap font
    g->setTextSize(1);

    char bbuf[10];
    uint16_t bcol;
    if (powerStatus && powerStatus->getHasBattery()) {
        int pct = powerStatus->getBatteryChargePercent();
        if (pct > 100)
            pct = 100;
        snprintf(bbuf, sizeof(bbuf), "%d%%%s", pct, powerStatus->getIsCharging() ? "+" : "");
        bcol = pct > 50 ? 0x07E0 : (pct > 20 ? 0xFFE0 : 0xF800);
    } else {
        snprintf(bbuf, sizeof(bbuf), "USB");
        bcol = 0x9CD3;
    }

    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    g->printf("%u nodes", (unsigned)total);

    int bw = g->textWidth(bbuf);
    g->setTextColor(bcol);
    g->setCursor(238 - bw, 3);
    g->print(bbuf);

    int uc = unreadCount();
    if (uc > 0) {
        char ub[12];
        snprintf(ub, sizeof(ub), "%d new", uc);
        int tw = g->textWidth(ub);
        int px = 238 - bw - 8 - (tw + 6);
        g->fillRoundRect(px, 2, tw + 6, 10, 2, 0xF800); // red unread badge
        g->setTextColor(0xFFFF);
        g->setCursor(px + 3, 3);
        g->print(ub);
    }

    g->drawFastHLine(0, 13, 240, 0x39C7);

    rebuildFiltered(); // fills filtered[]/filteredCount (empty query here), clamps sel
    const int top = 15, rowH = 18, maxRows = (124 - top) / rowH;
    if (sel < scrollTop)
        scrollTop = sel;
    if (sel >= scrollTop + maxRows)
        scrollTop = sel - maxRows + 1;
    if (scrollTop < 0)
        scrollTop = 0;

    int y = top;
    for (int r = 0; r < maxRows; r++) {
        int idx = scrollTop + r;
        if (idx >= filteredCount)
            break;
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(filtered[idx]);
        if (!node)
            continue;
        if (idx == sel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945); // cursor highlight
        drawNodeRow(g, node, y, node->num == me);
        y += rowH;
    }

    drawFooter(g, "up/dn  </>fav  ENTER open  type find");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::rebuildFiltered()
{
    filteredCount = buildNodeList(filtered, kMaxFiltered, queryLen ? query : nullptr);
    if (sel >= filteredCount)
        sel = filteredCount ? filteredCount - 1 : 0;
    if (sel < 0)
        sel = 0;
}

void AdvUI::drawPicker()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    rebuildFiltered();

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);

    g->setTextSize(1);
    g->setTextColor(0xFFE0); // yellow
    g->setCursor(4, 3);
    g->printf("> %s_", query);
    g->drawFastHLine(0, 13, 240, 0x39C7);

    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;
    const int rowH = 18, top = 16, maxRows = (124 - top) / rowH;

    // Keep the selection on screen.
    if (sel < scrollTop)
        scrollTop = sel;
    if (sel >= scrollTop + maxRows)
        scrollTop = sel - maxRows + 1;
    if (scrollTop < 0)
        scrollTop = 0;

    int y = top;
    for (int r = 0; r < maxRows; r++) {
        int idx = scrollTop + r;
        if (idx >= filteredCount)
            break;
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(filtered[idx]);
        if (!node)
            continue;
        if (idx == sel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945); // selection highlight
        drawNodeRow(g, node, y, node->num == me);
        y += rowH;
    }

    drawFooter(g, "up/dn  </>fav  ENTER open  ESC back");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::drawNode()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNode(selectedNum) : nullptr;
    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;

    markReadFrom(selectedNum); // opening the thread clears its unread

    // header: the node's row (name + signal + hops + age + role), like the contact list
    if (node) {
        drawNodeRow(g, node, 2, false);
    } else {
        g->setFont(&lgfx::fonts::Font0);
        g->setTextSize(1);
        g->setTextColor(0x8410);
        g->setCursor(4, 6);
        g->printf("!%08x  (left DB)", (unsigned)selectedNum);
    }
    g->drawFastHLine(0, 21, 240, 0x39C7);

    // message thread (chronological, most recent at the bottom)
    const int fy0 = 24, lh = 17;
    int bottom = (mode == MODE_COMPOSE) ? 112 : 122; // leave room for the compose bar
    int maxLines = (bottom - fy0) / lh;
    int matched[kMaxMsgs], mc = 0;
    for (int i = 0; i < g_msgCount; i++) {
        int idx = (g_msgCount == kMaxMsgs) ? (g_msgNext + i) % kMaxMsgs : i;
        if (g_msgs[idx].from == selectedNum || g_msgs[idx].to == selectedNum)
            matched[mc++] = idx;
    }
    if (mc == 0) {
        g->setTextColor(0x630C);
        g->setCursor(4, fy0);
        g->print("(no messages yet)");
    } else {
        g->setFont(&cyrFont); // Cyrillic-capable, so Russian message text renders
        g->setTextSize(1);
        int start = mc > maxLines ? mc - maxLines : 0;
        int y = fy0;
        for (int i = start; i < mc; i++) {
            Msg &m = g_msgs[matched[i]];
            bool out = (m.from == me);
            char line[180];
            snprintf(line, sizeof(line), "%s%s", out ? "> " : "< ", m.text);
            bool failed = out && m.status == MSG_FAILED;
            fitWidth(g, line, failed ? 150 : (out ? 214 : 232));
            g->setTextColor(out ? 0x07FF : 0xFFFF); // outgoing cyan, incoming white
            g->setCursor(4, y);
            g->print(line);
            if (failed) {
                g->setFont(&lgfx::fonts::Font0); // red error name at the right
                g->setTextSize(1);
                g->setTextColor(0xF800);
                const char *en = errName(m.err);
                g->setCursor(238 - g->textWidth(en), y + 4);
                g->print(en);
                g->setFont(&cyrFont); // restore for the next line
            } else if (out) {
                drawMsgStatus(g, 220, y, m.status);
            }
            y += lh;
        }
    }

    if (mode == MODE_COMPOSE) {
        g->drawFastHLine(0, 114, 240, 0x39C7);
        g->setFont(&cyrFont);
        g->setTextSize(1);
        g->setTextColor(0xFFE0); // yellow
        char cbuf[210];
        snprintf(cbuf, sizeof(cbuf), "%s_", msgBuf);
        char *shown = cbuf; // drop leading chars so the cursor stays visible
        while (*shown && g->textWidth(shown) > 232)
            shown++;
        g->setCursor(4, 120);
        g->print(shown);
    } else {
        drawFooter(g, "type to reply   ESC: back");
    }

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::drawSetName()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    g->print(editTitle(editTarget));
    g->drawFastHLine(0, 13, 240, 0x39C7);

    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(1);
    g->setTextColor(0xFFFF);
    char field[27];
    snprintf(field, sizeof(field), "%s_", nameBuf);
    g->setCursor(6, 44);
    g->print(field);

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x8410); // gray
    g->setCursor(6, 74);
    g->printf("%u / %u", (unsigned)nameLen, editMax(editTarget));

    drawFooter(g, "type   ENTER save   ESC cancel");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// Writes the edited name into the engine's owner and broadcasts it — the same
// path AdminModule::handleSetOwner takes, but driven from our UI.
bool AdvUI::applyName()
{
    if (editTarget == 2) { // frequency (MHz) -> override_frequency; radio restart to apply
        config.lora.override_frequency = strtof(nameBuf, nullptr);
        if (nodeDB)
            nodeDB->saveToDisk(SEGMENT_CONFIG);
        rebootAtMsec = millis() + 1500;
        mode = MODE_REBOOT;
        return true;
    }
    if (editTarget == 3) { // primary channel name; radio restart to apply
        meshtastic_Channel ch = channels.getByIndex(0);
        strncpy(ch.settings.name, nameBuf, sizeof(ch.settings.name));
        ch.settings.name[sizeof(ch.settings.name) - 1] = 0;
        channels.setChannel(ch);
        channels.onConfigChanged();
        if (nodeDB)
            nodeDB->saveToDisk(SEGMENT_CHANNELS);
        rebootAtMsec = millis() + 1500;
        mode = MODE_REBOOT;
        return true;
    }

    // name (long / short): applied live, no reboot
    if (editTarget == 1) {
        strncpy(owner.short_name, nameBuf, sizeof(owner.short_name));
        owner.short_name[sizeof(owner.short_name) - 1] = 0;
    } else {
        strncpy(owner.long_name, nameBuf, sizeof(owner.long_name));
        owner.long_name[sizeof(owner.long_name) - 1] = 0;
    }
    snprintf(owner.id, sizeof(owner.id), "!%08x", (unsigned)(nodeDB ? nodeDB->getNodeNum() : 0));
    if (service)
        service->reloadOwner(true); // update local node DB + broadcast NodeInfo
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE); // persist across reboots
    return false;
}

void AdvUI::drawSettings()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    g->print("Settings");
    g->drawFastHLine(0, 13, 240, 0x39C7);

    const char *labels[kNumSettings] = {"Name", "Short", "Region", "Preset", "Frequency", "Channel"};
    char vals[kNumSettings][24];
    snprintf(vals[0], sizeof(vals[0]), "%s", owner.long_name[0] ? owner.long_name : "(unset)");
    snprintf(vals[1], sizeof(vals[1]), "%s", owner.short_name[0] ? owner.short_name : "(unset)");
    snprintf(vals[2], sizeof(vals[2]), "%s", regionName(config.lora.region));
    snprintf(vals[3], sizeof(vals[3]), "%s", config.lora.use_preset ? presetName(config.lora.modem_preset) : "custom");
    if (config.lora.override_frequency > 0)
        snprintf(vals[4], sizeof(vals[4]), "%.3f", (double)config.lora.override_frequency);
    else
        strcpy(vals[4], "auto");
    snprintf(vals[5], sizeof(vals[5]), "%s", channels.getName(0));

    const int rowH = 18, top = 15;
    for (int i = 0; i < kNumSettings; i++) {
        int y = top + i * rowH;
        if (i == setSel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945); // selection highlight

        g->setFont(&lgfx::fonts::FreeSansBold9pt7b); // same size as the contact list
        g->setTextSize(1);
        g->setTextColor(0xFFFF);
        g->setCursor(6, y + 1);
        g->print(labels[i]);
        int lw = g->textWidth(labels[i]);

        fitWidth(g, vals[i], 230 - (6 + lw));
        g->setTextColor(0x9CD3);
        g->setCursor(236 - g->textWidth(vals[i]), y + 1);
        g->print(vals[i]);
    }

    drawFooter(g, "up/dn   ENTER edit   ESC back");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::drawPickList()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    const EnumOpt *opts = pickTarget == 0 ? kRegionOpts : kPresetOpts;
    int cnt = pickTarget == 0 ? kRegionCount : kPresetCount;

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    g->print(pickTarget == 0 ? "Region" : "Preset");
    g->drawFastHLine(0, 13, 240, 0x39C7);

    const int rowH = 18, top = 15, maxRows = (124 - top) / rowH;
    if (pickSel < pickScroll)
        pickScroll = pickSel;
    if (pickSel >= pickScroll + maxRows)
        pickScroll = pickSel - maxRows + 1;
    if (pickScroll < 0)
        pickScroll = 0;

    int y = top;
    for (int r = 0; r < maxRows; r++) {
        int i = pickScroll + r;
        if (i >= cnt)
            break;
        if (i == pickSel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945);
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextSize(1);
        g->setTextColor(0xFFFF);
        g->setCursor(8, y + 1);
        g->print(opts[i].name);
        y += rowH;
    }

    drawFooter(g, "up/dn   ENTER select   ESC back");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::drawReboot()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(1);
    g->setTextColor(0xFFE0); // yellow
    const char *t1 = "Applying settings";
    g->setCursor((240 - g->textWidth(t1)) / 2, 50);
    g->print(t1);
    g->setTextColor(0x8410);
    const char *t2 = "rebooting...";
    g->setCursor((240 - g->textWidth(t2)) / 2, 74);
    g->print(t2);

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// Applies a LoRa setting (0 = region, 1 = preset), persists it, and schedules the
// reboot the radio needs to re-init on the new parameters.
void AdvUI::applyLoRa(int target, int value)
{
    if (target == 0) {
        config.lora.region = (meshtastic_Config_LoRaConfig_RegionCode)value;
    } else {
        config.lora.use_preset = true;
        config.lora.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)value;
    }
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CONFIG);
    rebootAtMsec = millis() + 1500;
    mode = MODE_REBOOT;
}

void AdvUI::handleKey(char ch)
{
    unsigned char c = (unsigned char)ch;
    bool esc = c == 0x1b;   // ESC
    bool bksp = c == 0x08;  // BSP
    bool enter = c == 0x0d; // SELECT
    bool up = c == 0xb5;    // UP
    bool down = c == 0xb6;  // DOWN
    bool left = c == 0xb4;  // LEFT  -> favourite
    bool right = c == 0xb7; // RIGHT -> unfavourite
    bool printable = c >= 0x20 && c < 0x7f;

    if (c == AdvKeyboard::kLongEsc) { // long-press ESC opens settings from anywhere
        setSel = 0;
        mode = MODE_SETTINGS;
        return;
    }

    if (mode == MODE_SETNAME) {
        unsigned maxLen = editMax(editTarget);
        bool numeric = (editTarget == 2);
        if (esc) {
            mode = nameReturn;
        } else if (enter) {
            bool rebooting = nameLen && applyName();
            if (!rebooting)
                mode = nameReturn;
        } else if (bksp) {
            if (nameLen)
                nameBuf[--nameLen] = 0;
        } else if (printable && nameLen < maxLen && (!numeric || (c >= '0' && c <= '9') || c == '.')) {
            nameBuf[nameLen++] = c;
            nameBuf[nameLen] = 0;
        }
        return;
    }

    if (mode == MODE_SETTINGS) {
        if (esc) {
            mode = MODE_NODES;
        } else if (up) {
            if (setSel > 0)
                setSel--;
        } else if (down) {
            if (setSel < kNumSettings - 1)
                setSel++;
        } else if (enter && setSel <= 1) { // 0 = long name, 1 = short name
            editTarget = setSel;
            const char *cur = (setSel == 1) ? owner.short_name : owner.long_name;
            strncpy(nameBuf, cur, sizeof(nameBuf));
            nameBuf[sizeof(nameBuf) - 1] = 0;
            nameLen = strlen(nameBuf);
            nameReturn = MODE_SETTINGS;
            mode = MODE_SETNAME;
        } else if (enter && setSel == 2) { // Region
            pickTarget = 0;
            pickSel = optIndex(kRegionOpts, kRegionCount, (int)config.lora.region);
            pickScroll = 0;
            mode = MODE_PICKLIST;
        } else if (enter && setSel == 3) { // Preset
            pickTarget = 1;
            pickSel = optIndex(kPresetOpts, kPresetCount, (int)config.lora.modem_preset);
            pickScroll = 0;
            mode = MODE_PICKLIST;
        } else if (enter && setSel == 4) { // Frequency (MHz)
            editTarget = 2;
            if (config.lora.override_frequency > 0)
                snprintf(nameBuf, sizeof(nameBuf), "%.3f", (double)config.lora.override_frequency);
            else
                nameBuf[0] = 0;
            nameLen = strlen(nameBuf);
            nameReturn = MODE_SETTINGS;
            mode = MODE_SETNAME;
        } else if (enter && setSel == 5) { // Channel name
            editTarget = 3;
            strncpy(nameBuf, channels.getByIndex(0).settings.name, sizeof(nameBuf));
            nameBuf[sizeof(nameBuf) - 1] = 0;
            nameLen = strlen(nameBuf);
            nameReturn = MODE_SETTINGS;
            mode = MODE_SETNAME;
        }
        return;
    }

    if (mode == MODE_PICKLIST) {
        int cnt = pickTarget == 0 ? kRegionCount : kPresetCount;
        if (esc) {
            mode = MODE_SETTINGS;
        } else if (up) {
            if (pickSel > 0)
                pickSel--;
        } else if (down) {
            if (pickSel < cnt - 1)
                pickSel++;
        } else if (enter) {
            const EnumOpt *opts = pickTarget == 0 ? kRegionOpts : kPresetOpts;
            applyLoRa(pickTarget, opts[pickSel].value); // sets config, saves, schedules reboot
        }
        return;
    }

    if (mode == MODE_REBOOT)
        return; // rebooting shortly; ignore input

    if (mode == MODE_NODE) {
        if (esc || bksp) {
            mode = nodeReturn;
        } else if (printable) {
            msgBuf[0] = c; // start composing a reply to this node
            msgBuf[1] = 0;
            msgLen = 1;
            mode = MODE_COMPOSE;
        }
        return;
    }

    if (mode == MODE_COMPOSE) {
        if (esc) {
            mode = MODE_NODE;
        } else if (enter) {
            if (msgLen)
                sendMessage(selectedNum, msgBuf);
            msgLen = 0;
            msgBuf[0] = 0;
            mode = MODE_NODE;
        } else if (bksp) {
            if (msgLen)
                msgBuf[--msgLen] = 0;
        } else if (printable && msgLen < sizeof(msgBuf) - 1) {
            msgBuf[msgLen++] = c;
            msgBuf[msgLen] = 0;
        }
        return;
    }

    // Home node list: navigable directly (cursor + scroll); typing opens the filter.
    if (mode == MODE_NODES) {
        rebuildFiltered();
        if (up) {
            if (sel > 0)
                sel--;
            return;
        }
        if (down) {
            if (sel < filteredCount - 1)
                sel++;
            return;
        }
        if (enter) {
            if (sel < filteredCount) {
                meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNodeByIndex(filtered[sel]) : nullptr;
                if (node) {
                    selectedNum = node->num;
                    nodeReturn = MODE_NODES;
                    mode = MODE_NODE;
                }
            }
            return;
        }
        if (left || right) { // favourite / unfavourite the selected node (persists in nodeDB)
            if (sel < filteredCount && nodeDB) {
                meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(filtered[sel]);
                if (node)
                    nodeDB->set_favorite(left, node->num);
            }
            return;
        }
        if (!printable)
            return; // short ESC / anything else: nothing at the root
        // a printable char: switch to the filter picker and let it consume the char below
        mode = MODE_PICKER;
        queryLen = 0;
        query[0] = 0;
        sel = 0;
        scrollTop = 0;
        // fall through to MODE_PICKER handling
    }

    // MODE_PICKER
    if (esc) {
        mode = MODE_NODES;
        queryLen = 0;
        query[0] = 0;
        sel = 0;
        scrollTop = 0;
        return;
    }

    rebuildFiltered();

    if (up) {
        if (sel > 0)
            sel--;
        return;
    }
    if (down) {
        if (sel < filteredCount - 1)
            sel++;
        return;
    }
    if (enter) {
        if (sel < filteredCount) {
            meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNodeByIndex(filtered[sel]) : nullptr;
            if (node) {
                selectedNum = node->num;
                nodeReturn = MODE_PICKER;
                mode = MODE_NODE;
            }
        }
        return;
    }
    if (left || right) { // favourite / unfavourite the selected node
        if (sel < filteredCount && nodeDB) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(filtered[sel]);
            if (node)
                nodeDB->set_favorite(left, node->num);
        }
        return;
    }
    if (bksp) {
        if (queryLen)
            query[--queryLen] = 0;
        sel = 0;
        scrollTop = 0;
        return;
    }
    if (printable && queryLen < sizeof(query) - 1) {
        query[queryLen++] = c;
        query[queryLen] = 0;
        sel = 0;
        scrollTop = 0;
    }
}

int32_t AdvUI::runOnce()
{
    if (!inited) {
        initHardware();
        inited = true;
    }

    while (api.available() && api.getFromRadio(fromRadioBuf) > 0) {
        handleFromRadio(api.lastFromRadio()); // pick out incoming text messages
    }

    kb.setNavKeys(mode != MODE_SETNAME && mode != MODE_COMPOSE); // symbols while typing, arrows otherwise
    kb.trigger();
    bool keyDuringSplash = false;
    while (kb.hasEvent()) {
        char ch = kb.dequeueEvent();
        if (splashDone)
            handleKey(ch);
        else
            keyDuringSplash = true; // any key dismisses the splash early
        kb.trigger(); // re-read the FIFO as we drain (matches the stock poll loop)
    }
    kb.clearInt(); // re-arm the TCA8418 interrupt, else it stops reporting after the first event

    if (!splashDone && (keyDuringSplash || millis() - bootMs > 2000))
        splashDone = true;

    // Announce ourselves early so neighbours see us right away — stock waits ~30s.
    if (splashDone && !announced && nodeInfoModule) {
        nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, false);
        announced = true;
    }

    // Persist the conversation, debounced, so we don't hammer the flash.
    if (g_msgsDirty && millis() - g_lastSaveMs > 3000) {
        saveMsgs();
        g_msgsDirty = false;
        g_lastSaveMs = millis();
    }

    if (!splashDone)
        drawSplash();
    else if (mode == MODE_PICKER)
        drawPicker();
    else if (mode == MODE_NODE || mode == MODE_COMPOSE)
        drawNode();
    else if (mode == MODE_SETNAME)
        drawSetName();
    else if (mode == MODE_SETTINGS)
        drawSettings();
    else if (mode == MODE_PICKLIST)
        drawPickList();
    else if (mode == MODE_REBOOT)
        drawReboot();
    else
        drawNodeList();

    return splashDone ? 200 : 80; // 5 Hz normally; snappier while the splash is up
}

// Created once from an injected call in main.cpp (after setupModules); the
// OSThread base then self-schedules, so no main-loop edits are needed.
void advuiSetup()
{
    static AdvUI advUI;
}

} // namespace advui
