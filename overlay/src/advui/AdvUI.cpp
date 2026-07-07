#include "AdvUI.h"
#include "AdvBle.h"
#include "AdvFont.h"
#include "BluetoothStatus.h"
#include "CyrillicFont.h"
#include "FSCommon.h"
#include "PowerStatus.h"
#include "configuration.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/admin.pb.h"
#include "mesh/mesh-pb-constants.h"
#include "gps/RTC.h" // getTime() for message timestamps
#include "graphics/emotes.h" // UTF-8 -> bitmap emoji table (forced in via EmotesData.cpp)
#include "main.h" // audioThread (I2S beep)
#include "modules/NodeInfoModule.h"
#ifdef HAS_NEOPIXEL
#include "esp32-hal-rgb-led.h" // rgbLedWrite(): stateless RMT driver for the onboard RGB LED
#include <Esp.h>
#include <esp_heap_caps.h>
#endif
#include <algorithm>
#include <cctype>
#include <esp_random.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern uint32_t rebootAtMsec; // main.cpp: set to a future millis() to schedule a reboot
int advui_max_num_nodes();    // AdvNodeCap.cpp; also injected everywhere as MAX_NUM_NODES

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
// MSG_SENT = handed to the mesh as a broadcast (no per-recipient ack possible),
// distinct from MSG_DELIVERED which is an ack-confirmed DM.
enum MsgStatus : uint8_t { MSG_IN = 0, MSG_SENDING = 1, MSG_DELIVERED = 2, MSG_FAILED = 3, MSG_SENT = 4 };

struct Msg {
    uint32_t from;
    uint32_t to; // 0xFFFFFFFF = broadcast/channel, else a DM to that node
    uint32_t rxTime;
    uint32_t id;     // our packet id (outgoing only), to match the delivery ACK
    uint8_t status;  // MsgStatus
    uint8_t err;     // routing error code when status == MSG_FAILED
    uint8_t ch;      // channel index for broadcasts (to == 0xFFFFFFFF)
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
constexpr int kMaxMsgs = 32; // shared across all conversations; DMs are protected by the
                             // channel-first eviction (addMsg), not by size, so this stays small
                             // to keep the heap margin — a connected phone + BLE runs it thin
constexpr int kNumSettings = 16; // the flat item table: Name..Channel, Role..Rebroadcast, UTC, WiFi, MQTT, Screen, Radio, Font

// The Settings menu is two-level: the top lists sections (plus WiFi/MQTT/Radio
// as direct entries); a section lists indices into the flat item table.
const uint8_t kSecNode[] = {0, 1};                    // Name, Short
const uint8_t kSecLora[] = {2, 3, 4, 5, 6, 7, 8, 9};  // Region..Rebroadcast
const uint8_t kSecDevice[] = {10, 13, 15};            // UTC, Screen, Font (read-only)
constexpr int kTopCount = 6;                          // Node, LoRa, WiFi, MQTT, Device, Radio
Msg g_msgs[kMaxMsgs];
int g_msgCount = 0;         // populated slots (grows to kMaxMsgs)
bool g_msgsDirty = false;   // ring changed since the last flash save
uint32_t g_lastSaveMs = 0;  // when we last wrote the ring to flash

// Parallel to g_msgs: the packet id this slot's message replies to (0 = not a reply).
// A separate array so the Msg layout (and the saved-file magic) stays unchanged.
uint32_t g_msgReply[kMaxMsgs];

void addMsg(uint32_t from, uint32_t to, uint8_t ch, uint32_t rxTime, bool unread, const char *text, uint32_t id,
           uint8_t status, uint32_t replyId = 0)
{
    // The ring is a compact chronological array (index 0 = oldest). When it's full
    // we evict the oldest CHANNEL broadcast, not the oldest message — so a busy
    // channel can never push a quiet DM thread out. Only when there are no
    // broadcasts left (an all-DM ring) do we drop the oldest DM.
    int slot;
    if (g_msgCount < kMaxMsgs) {
        slot = g_msgCount++;
    } else {
        int victim = -1;
        for (int i = 0; i < g_msgCount; i++)
            if (g_msgs[i].to == NODENUM_BROADCAST) {
                victim = i;
                break;
            }
        if (victim < 0)
            victim = 0;
        int tail = g_msgCount - 1 - victim;
        memmove(&g_msgs[victim], &g_msgs[victim + 1], tail * sizeof(Msg));
        memmove(&g_msgReply[victim], &g_msgReply[victim + 1], tail * sizeof(uint32_t));
        slot = g_msgCount - 1;
    }
    g_msgReply[slot] = replyId;
    Msg &m = g_msgs[slot];
    m.from = from;
    m.to = to;
    m.ch = ch;
    m.rxTime = rxTime;
    m.id = id;
    m.status = status;
    m.err = 0;
    m.read = !unread;
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = 0;
    g_msgsDirty = true;
}

// Mark the sent message whose id matches an incoming ACK/routing response.
// Skip MSG_IN: incoming messages now carry their sender's packet id (for reactions),
// and a random id collision must not flip their status.
void ackMsg(uint32_t reqId, uint8_t status, uint8_t err)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].id == reqId && g_msgs[i].id != 0 && g_msgs[i].status != MSG_IN) {
            g_msgs[i].status = status;
            g_msgs[i].err = err;
            g_msgsDirty = true;
            return;
        }
}

// --- Reactions (tapback) --------------------------------------------------------
// A reaction is a TEXT_MESSAGE_APP packet with emoji=1 and reply_id = the target
// message's packet id (same wire format the phone apps use). We keep them in their
// own small ring, attached to messages by id at render time.
struct Reaction {
    uint32_t msgId; // packet id of the message reacted to
    uint32_t from;
    char label[9]; // UTF-8 emoji sequence
};
constexpr int kMaxReacts = 32;
Reaction g_reacts[kMaxReacts];
int g_reactCount = 0;
int g_reactNext = 0;

void addReaction(uint32_t msgId, uint32_t from, const char *label)
{
    LOG_INFO("advui: reaction msgId=0x%08x from=0x%08x %s", (unsigned)msgId, (unsigned)from, label);
    for (int i = 0; i < g_reactCount; i++)
        if (g_reacts[i].msgId == msgId && g_reacts[i].from == from) { // replace their previous one
            strncpy(g_reacts[i].label, label, sizeof(g_reacts[i].label) - 1);
            g_reacts[i].label[sizeof(g_reacts[i].label) - 1] = 0;
            g_msgsDirty = true;
            return;
        }
    Reaction &r = g_reacts[g_reactNext];
    r.msgId = msgId;
    r.from = from;
    strncpy(r.label, label, sizeof(r.label) - 1);
    r.label[sizeof(r.label) - 1] = 0;
    g_reactNext = (g_reactNext + 1) % kMaxReacts;
    if (g_reactCount < kMaxReacts)
        g_reactCount++;
    g_msgsDirty = true;
}

// The quick-reaction strip (labels must exist in graphics::emotes[]).
const char *kQuickReacts[6] = {"\U0001F44D", "\U0001F44E", "\U00002764\U0000FE0F",
                               "\U0001F602", "\U00002753", "\U0001F525"};

int unreadCount()
{
    int c = 0;
    for (int i = 0; i < g_msgCount; i++)
        if (!g_msgs[i].read)
            c++;
    return c;
}

// Marks the DM thread with this node read. Only DMs: the node's channel broadcasts
// belong to the channel thread (same filter as hasUnreadFrom — marking them here
// silently killed the channel's unread state and its first-unread jump).
void markReadFrom(uint32_t nodeNum)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].from == nodeNum && g_msgs[i].to != NODENUM_BROADCAST && !g_msgs[i].read) {
            g_msgs[i].read = true;
            g_msgsDirty = true;
        }
}

// True if there's an unread message from this node (drives the envelope marker).
bool hasUnreadFrom(uint32_t nodeNum)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].from == nodeNum && g_msgs[i].to != NODENUM_BROADCAST && !g_msgs[i].read)
            return true;
    return false;
}

// Channel favourites are our own state (Meshtastic channels carry no favourite flag).
uint8_t g_favChannels = 0; // bitmask, bit i set = channel i favourited
bool chanFav(int i) { return g_favChannels & (1u << i); }

// User-set UTC offset (minutes) for message timestamps; the device has no GPS/tz.
int32_t g_utcOffsetMin = 0;
int32_t g_screenOffSec = 300; // screen auto-off timeout; 0 = never

// Transliterated Cyrillic input layer (Fn+L); persisted with the messages so the
// chosen layout survives a reboot.
bool g_ruMode = false;

// --- Radio mode (onboard / BLE companion), its own tiny file -------------------
constexpr uint32_t kRadioMagic = 0x41565231; // "AVR1"
const char *kRadioPath = "/advui_radio.bin";
bool g_radioCompanion = false;
char g_peerAddr[18] = {0}; // paired companion node (empty = not chosen yet)
char g_peerName[24] = {0};
uint8_t g_peerType = 0;    // its BLE address type (public/random) — needed to connect

void saveRadioCfg()
{
    auto f = FSCom.open(kRadioPath, FILE_O_WRITE);
    if (!f)
        return;
    uint32_t magic = kRadioMagic;
    uint8_t mode = g_radioCompanion ? 1 : 0;
    f.write((const uint8_t *)&magic, sizeof(magic));
    f.write(&mode, 1);
    f.write((const uint8_t *)g_peerAddr, sizeof(g_peerAddr));
    f.write((const uint8_t *)g_peerName, sizeof(g_peerName));
    f.write(&g_peerType, 1); // optional tail
    f.close();
}

// Crash guard for the companion link: the flag file exists while a connect attempt
// is in flight. If we boot and it's still there, the last attempt took the device
// down — skip the boot auto-connect so a crash can't loop, and go to the scan.
const char *kBleAttemptPath = "/advui_bleatt";

void bleAttemptMark()
{
    auto f = FSCom.open(kBleAttemptPath, FILE_O_WRITE);
    if (f) {
        uint8_t one = 1;
        f.write(&one, 1);
        f.close();
    }
}

void bleAttemptClear()
{
    FSCom.remove(kBleAttemptPath);
}

bool bleAttemptPending()
{
    auto f = FSCom.open(kBleAttemptPath, FILE_O_READ);
    if (f) {
        f.close();
        return true;
    }
    return false;
}

// --- Mesh-state abstraction: local engine vs companion link ---------------------
// In companion mode the radio lives in another node; the UI reads mesh state from
// the BLE config stream (g_compNodes/g_compChans) instead of the local nodeDB.

uint32_t myNodeNum()
{
    if (g_radioCompanion)
        return g_linkMyNode;
    return nodeDB ? nodeDB->getNodeNum() : 0;
}

// Synthesizes a NodeInfoLite view of a companion node so the existing row/name
// rendering keeps working unchanged. Sequential use only (one shared temp).
meshtastic_NodeInfoLite *compSynth(const CompNode &c)
{
    static meshtastic_NodeInfoLite tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.num = c.num;
    tmp.snr = c.snr;
    tmp.last_heard = c.lastHeard;
    tmp.has_hops_away = c.hops != 255;
    tmp.hops_away = c.hops == 255 ? 0 : c.hops;
    snprintf(tmp.short_name, sizeof(tmp.short_name), "%s", c.shortName);
    snprintf(tmp.long_name, sizeof(tmp.long_name), "%s", c.longName);
    tmp.bitfield = 1 << 5; // NODEINFO_BITFIELD_HAS_USER: names are valid
    return &tmp;
}

meshtastic_NodeInfoLite *nodeByNum(uint32_t num)
{
    if (g_radioCompanion) {
        for (int i = 0; i < g_compNodeCount; i++)
            if (g_compNodes[i].num == num)
                return compSynth(g_compNodes[i]);
        return nullptr;
    }
    return nodeDB ? nodeDB->getMeshNode(num) : nullptr;
}

meshtastic_NodeInfoLite *nodeAt(uint16_t idx) // idx from filtered[]
{
    if (g_radioCompanion)
        return idx < (uint16_t)g_compNodeCount ? compSynth(g_compNodes[idx]) : nullptr;
    return nodeDB ? nodeDB->getMeshNodeByIndex(idx) : nullptr;
}

const char *presetName(meshtastic_Config_LoRaConfig_ModemPreset p); // defined below

const char *chanName(int i)
{
    if (g_radioCompanion) {
        if (g_compChans[i].has_settings && g_compChans[i].settings.name[0])
            return g_compChans[i].settings.name;
        // blank primary channel displays the modem preset, like stock does
        if (g_compChans[i].role == meshtastic_Channel_Role_PRIMARY)
            return presetName((meshtastic_Config_LoRaConfig_ModemPreset)g_compPreset);
        return "?";
    }
    return channels.getName(i);
}

bool chanEnabled(int i)
{
    if (g_radioCompanion)
        return g_compChans[i].role != meshtastic_Channel_Role_DISABLED;
    return channels.getByIndex(i).role != meshtastic_Channel_Role_DISABLED;
}

// Tiny Bluetooth rune for the companion-link indicator (green/yellow/red by state).
void drawBtGlyph(lgfx::LGFXBase *g, int x, int y, uint16_t color)
{
    g->drawFastVLine(x + 3, y, 9, color);
    g->drawLine(x + 3, y, x + 6, y + 2, color);
    g->drawLine(x + 6, y + 2, x, y + 6, color);
    g->drawLine(x + 3, y + 8, x + 6, y + 6, color);
    g->drawLine(x + 6, y + 6, x, y + 2, color);
}

uint16_t linkColor()
{
    switch (g_linkState) {
    case BLE_CONNECTED: return 0x07E0;               // green
    case BLE_CONNECTING:
    case BLE_PAIRING:   return 0xFFE0;               // yellow: in progress
    default:            return 0xF800;               // red: no link
    }
}

void loadRadioCfg()
{
    auto f = FSCom.open(kRadioPath, FILE_O_READ);
    if (!f)
        return;
    uint32_t magic = 0;
    uint8_t mode = 0;
    f.read((uint8_t *)&magic, sizeof(magic));
    f.read(&mode, 1);
    f.read((uint8_t *)g_peerAddr, sizeof(g_peerAddr));
    size_t n = f.read((uint8_t *)g_peerName, sizeof(g_peerName));
    f.read(&g_peerType, 1); // optional tail (0 for older files)
    f.close();
    if (magic != kRadioMagic || n != sizeof(g_peerName)) {
        g_peerAddr[0] = 0;
        g_peerName[0] = 0;
        return;
    }
    g_radioCompanion = mode == 1;
    g_peerAddr[sizeof(g_peerAddr) - 1] = 0;
    g_peerName[sizeof(g_peerName) - 1] = 0;
}

#ifdef HAS_I2S
// Our own single-note alert, played over the I2S codec (the stock beep is disabled).
// One short note; pumped to completion from runOnce, then stopped.
const char kBeepRtttl[] = "adv:d=16,o=6,b=280:g";
bool g_beeping = false;
void startBeep()
{
    if (!audioThread || g_beeping)
        return;
    audioThread->beginRttl(kBeepRtttl, strlen(kBeepRtttl));
    g_beeping = true;
}
#endif

#ifdef HAS_NEOPIXEL
// Notification LED (single onboard RGB on NEOPIXEL_DATA). We drive it with the core's
// stateless rgbLedWrite() (shares the core RMT manager with the stock ambient thread,
// so no begin()/channel conflict). A brief flash on incoming messages, cleared in runOnce.
uint32_t g_ledOffMs = 0;
void flashLed(uint8_t r, uint8_t g, uint8_t b)
{
    rgbLedWrite(NEOPIXEL_DATA, r, g, b);
    g_ledOffMs = millis() + 400;
}
#endif

// Unread on a channel = an unread broadcast we received on that channel index.
bool hasUnreadChannel(int chIdx)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].to == NODENUM_BROADCAST && g_msgs[i].ch == chIdx && !g_msgs[i].read)
            return true;
    return false;
}

void markReadChannel(int chIdx)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].to == NODENUM_BROADCAST && g_msgs[i].ch == chIdx && !g_msgs[i].read) {
            g_msgs[i].read = true;
            g_msgsDirty = true;
        }
}

// Persist the message ring (with delivery status + read flags) to flash, so a
// reboot keeps the conversation. Written debounced from runOnce.
constexpr uint32_t kMsgMagic = 0x41565333;   // "AVS3" — count-based, independent of kMaxMsgs
constexpr uint32_t kMsgMagicV2 = 0x41565332; // "AVS2" — legacy fixed-32 ring, migrated on load
const char *kMsgPath = "/advui_msgs.bin";

// The message ring is a circular buffer; iterate it oldest-first (matches the
// chronological order the loaders write/read).
static int ringIdx(int i, int count, int next, int cap) { return count == cap ? (next + i) % cap : i; }

void saveMsgs()
{
    auto f = FSCom.open(kMsgPath, FILE_O_WRITE);
    if (!f)
        return;
    // Count-based layout: only the populated messages are written, oldest-first,
    // so the file size tracks history depth, not kMaxMsgs — growing the ring in a
    // future build keeps loading old saves (and vice-versa).
    uint32_t magic = kMsgMagic;
    int32_t cnt = g_msgCount;
    uint8_t ru = g_ruMode ? 1 : 0, rc = (uint8_t)g_reactCount;
    f.write((const uint8_t *)&magic, sizeof(magic));
    f.write((const uint8_t *)&cnt, sizeof(cnt));
    f.write((const uint8_t *)&g_favChannels, sizeof(g_favChannels));
    f.write((const uint8_t *)&g_utcOffsetMin, sizeof(g_utcOffsetMin));
    f.write((const uint8_t *)&g_screenOffSec, sizeof(g_screenOffSec));
    f.write(&ru, 1);
    f.write(&rc, 1);
    for (int i = 0; i < g_msgCount; i++) {
        f.write((const uint8_t *)&g_msgs[i], sizeof(Msg));
        f.write((const uint8_t *)&g_msgReply[i], sizeof(uint32_t));
    }
    for (int i = 0; i < g_reactCount; i++) {
        int idx = ringIdx(i, g_reactCount, g_reactNext, kMaxReacts);
        f.write((const uint8_t *)&g_reacts[idx], sizeof(Reaction));
    }
    f.close();
}

// AVS3: the current count-based format. Messages/reactions were written
// oldest-first; keep the newest kMaxMsgs / kMaxReacts if an older, bigger save
// ever holds more than this build's ring.
static bool loadMsgsV3(File &f)
{
    int32_t cnt = 0, off = 0, so = 0;
    uint8_t ru = 0, rc = 0;
    if (f.read((uint8_t *)&cnt, 4) != 4 || cnt < 0)
        return false;
    f.read((uint8_t *)&g_favChannels, sizeof(g_favChannels));
    f.read((uint8_t *)&off, 4);
    f.read((uint8_t *)&so, 4);
    f.read(&ru, 1);
    f.read(&rc, 1);

    int mskip = cnt > kMaxMsgs ? cnt - kMaxMsgs : 0;
    for (int i = 0; i < mskip; i++) { // discard the oldest overflow
        Msg t;
        uint32_t r;
        if (f.read((uint8_t *)&t, sizeof(Msg)) != (int)sizeof(Msg))
            return false;
        f.read((uint8_t *)&r, 4);
    }
    int mkeep = cnt - mskip;
    for (int i = 0; i < mkeep; i++) {
        if (f.read((uint8_t *)&g_msgs[i], sizeof(Msg)) != (int)sizeof(Msg))
            return false;
        if (f.read((uint8_t *)&g_msgReply[i], 4) != 4)
            g_msgReply[i] = 0;
    }
    g_msgCount = mkeep;

    int rkeep = rc > kMaxReacts ? kMaxReacts : rc;
    for (int i = 0; i < rc - rkeep; i++) {
        Reaction t;
        f.read((uint8_t *)&t, sizeof(Reaction));
    }
    int got = 0;
    for (int i = 0; i < rkeep; i++)
        if (f.read((uint8_t *)&g_reacts[i], sizeof(Reaction)) == (int)sizeof(Reaction))
            got++;
    g_reactCount = got;
    g_reactNext = got % kMaxReacts;

    g_utcOffsetMin = off;
    g_ruMode = ru != 0;
    if (so == 0 || (so >= 15 && so <= 3600))
        g_screenOffSec = so;
    return true;
}

// AVS2: the legacy fixed 32-slot layout — read the old arrays and linearise the
// old circular ring into the current one so a firmware update keeps history.
static void loadMsgsV2(File &f)
{
    constexpr int OLD = 32; // old kMaxMsgs and kMaxReacts were both 32
    int32_t cnt = 0, nxt = 0;
    f.read((uint8_t *)&cnt, 4);
    f.read((uint8_t *)&nxt, 4);
    f.read((uint8_t *)&g_favChannels, sizeof(g_favChannels));
    // one-time migration temps: on the heap (plenty free at boot), freed below —
    // no permanent bss for a path that runs at most once per device
    Msg *om = (Msg *)malloc(OLD * sizeof(Msg));
    Reaction *orr = (Reaction *)malloc(OLD * sizeof(Reaction));
    uint32_t *ore = (uint32_t *)malloc(OLD * sizeof(uint32_t));
    if (!om || !orr || !ore) {
        free(om);
        free(orr);
        free(ore);
        return;
    }
    size_t n = f.read((uint8_t *)om, OLD * sizeof(Msg));
    int32_t off = 0;
    f.read((uint8_t *)&off, 4);
    uint8_t ru = 0, rc = 0, rn = 0;
    f.read(&ru, 1);
    f.read(&rc, 1);
    f.read(&rn, 1);
    size_t rb = f.read((uint8_t *)orr, OLD * sizeof(Reaction));
    size_t reb = f.read((uint8_t *)ore, OLD * sizeof(uint32_t));
    int32_t so = 0;
    size_t sob = f.read((uint8_t *)&so, 4);
    if (n != OLD * sizeof(Msg) || cnt < 0 || cnt > OLD || nxt < 0 || nxt >= OLD) {
        free(om);
        free(orr);
        free(ore);
        return;
    }
    for (int i = 0; i < cnt; i++) {
        int idx = ringIdx(i, cnt, nxt, OLD);
        g_msgs[i] = om[idx];
        g_msgReply[i] = (reb == OLD * sizeof(uint32_t)) ? ore[idx] : 0;
    }
    g_msgCount = cnt;
    if (rb == OLD * sizeof(Reaction) && rc <= OLD && rn < OLD) {
        for (int i = 0; i < rc; i++)
            g_reacts[i] = orr[ringIdx(i, rc, rn, OLD)];
        g_reactCount = rc;
        g_reactNext = rc % kMaxReacts;
    }
    g_utcOffsetMin = off;
    g_ruMode = ru != 0;
    if (sob == sizeof(so) && (so == 0 || (so >= 15 && so <= 3600)))
        g_screenOffSec = so;
    free(om);
    free(orr);
    free(ore);
}

void loadMsgs()
{
    auto f = FSCom.open(kMsgPath, FILE_O_READ);
    if (!f)
        return;
    uint32_t magic = 0;
    f.read((uint8_t *)&magic, sizeof(magic));
    if (magic == kMsgMagic)
        loadMsgsV3(f);
    else if (magic == kMsgMagicV2)
        loadMsgsV2(f);
    f.close();

    uint32_t me = myNodeNum();
    for (int i = 0; i < g_msgCount; i++) {
        // an outgoing DM still "sending" before the reboot can't get its ACK now
        if (g_msgs[i].status == MSG_SENDING)
            g_msgs[i].status = MSG_FAILED;
        // upgrade our own channel sends saved before MSG_SENT existed
        else if (g_msgs[i].status == MSG_IN && me && g_msgs[i].from == me && g_msgs[i].to == NODENUM_BROADCAST)
            g_msgs[i].status = MSG_SENT;
    }
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
// UTC offset choices (value = minutes) with a representative city, scrolled in the picker.
const EnumOpt kUtcOpts[] = {
    {"UTC-12", -720},           {"UTC-11 Midway", -660},      {"UTC-10 Honolulu", -600},
    {"UTC-9 Anchorage", -540},  {"UTC-8 Los Angeles", -480},  {"UTC-7 Denver", -420},
    {"UTC-6 Chicago", -360},    {"UTC-5 New York", -300},     {"UTC-4 Halifax", -240},
    {"UTC-3 Sao Paulo", -180},  {"UTC-2", -120},              {"UTC-1 Azores", -60},
    {"UTC+0 London", 0},        {"UTC+1 Berlin", 60},         {"UTC+2 Kaliningrad", 120},
    {"UTC+3 Moscow", 180},      {"UTC+3:30 Tehran", 210},     {"UTC+4 Samara", 240},
    {"UTC+4:30 Kabul", 270},    {"UTC+5 Yekaterinburg", 300}, {"UTC+5:30 Delhi", 330},
    {"UTC+5:45 Kathmandu", 345},{"UTC+6 Omsk", 360},          {"UTC+6:30 Yangon", 390},
    {"UTC+7 Krasnoyarsk", 420}, {"UTC+8 Irkutsk", 480},       {"UTC+9 Yakutsk", 540},
    {"UTC+9:30 Adelaide", 570}, {"UTC+10 Vladivostok", 600},  {"UTC+11 Magadan", 660},
    {"UTC+12 Kamchatka", 720},  {"UTC+13 Samoa", 780},        {"UTC+14 Kiritimati", 840}};
// Radio backend: local Cap LoRa, or another node's radio over BLE (companion).
const EnumOpt kRadioOpts[] = {{"Onboard (Cap LoRa)", 0}, {"Companion via BLE", 1}};
const EnumOpt kScreenOpts[] = {{"15 s", 15}, {"30 s", 30}, {"1 min", 60}, {"5 min", 300}, {"never", 0}};
const EnumOpt kRoleOpts2[] = {{"Client", 0},      {"Client Mute", 1}, {"Client Hidden", 8},
                              {"Router", 2},      {"Router Late", 11}, {"Repeater", 4},
                              {"Tracker", 5},     {"Sensor", 6},       {"TAK", 7}};
const EnumOpt kHopOpts[] = {{"1", 1}, {"2", 2}, {"3", 3}, {"4", 4}, {"5", 5}, {"6", 6}, {"7", 7}};
const EnumOpt kPowerOpts[] = {{"max (region)", 0}, {"2 dBm", 2},   {"5 dBm", 5},   {"8 dBm", 8},  {"11 dBm", 11},
                              {"14 dBm", 14},      {"17 dBm", 17}, {"20 dBm", 20}, {"22 dBm", 22}};
const EnumOpt kRebroadOpts[] = {{"All", 0},        {"All skip decode", 1}, {"Local only", 2},
                                {"Known only", 3}, {"Core ports only", 5}, {"None", 4}};
constexpr int kRegionCount = sizeof(kRegionOpts) / sizeof(kRegionOpts[0]);
constexpr int kPresetCount = sizeof(kPresetOpts) / sizeof(kPresetOpts[0]);
constexpr int kUtcCount = sizeof(kUtcOpts) / sizeof(kUtcOpts[0]);
constexpr int kRadioCount = sizeof(kRadioOpts) / sizeof(kRadioOpts[0]);
constexpr int kScreenCount = sizeof(kScreenOpts) / sizeof(kScreenOpts[0]);
constexpr int kRoleCount = sizeof(kRoleOpts2) / sizeof(kRoleOpts2[0]);
constexpr int kHopCount = sizeof(kHopOpts) / sizeof(kHopOpts[0]);
constexpr int kPowerCount = sizeof(kPowerOpts) / sizeof(kPowerOpts[0]);
constexpr int kRebroadCount = sizeof(kRebroadOpts) / sizeof(kRebroadOpts[0]);

// The settings list-pickers, keyed by pickTarget.
struct PickList {
    const EnumOpt *opts;
    int cnt;
    const char *title;
};
PickList pickListFor(int target); // defined after the option tables it references

const char *optName(const EnumOpt *opts, int cnt, int value)
{
    for (int i = 0; i < cnt; i++)
        if (opts[i].value == value)
            return opts[i].name;
    return "?";
}

PickList pickListFor(int target)
{
    switch (target) {
    case 0: return {kRegionOpts, kRegionCount, "Region"};
    case 1: return {kPresetOpts, kPresetCount, "Preset"};
    case 2: return {kUtcOpts, kUtcCount, "UTC offset"};
    case 4: return {kScreenOpts, kScreenCount, "Screen off"};
    case 5: return {kRoleOpts2, kRoleCount, "Role"};
    case 6: return {kHopOpts, kHopCount, "Hop limit"};
    case 7: return {kPowerOpts, kPowerCount, "TX power"};
    case 8: return {kRebroadOpts, kRebroadCount, "Rebroadcast"};
    default: return {kRadioOpts, kRadioCount, "Radio"};
    }
}

int optIndex(const EnumOpt *opts, int cnt, int value)
{
    for (int i = 0; i < cnt; i++)
        if (opts[i].value == value)
            return i;
    return 0;
}

// Text-editor (MODE_SETNAME) targets: 0 long name, 1 short name, 2 frequency, 3 channel.
// --- WiFi / MQTT sub-settings (MODE_NETPAGE) -----------------------------------
// Two pages of fields backed by the stock config/moduleConfig; no engine changes.
struct NetField {
    const char *label;
    bool isBool;
};
const NetField kWifiFields[] = {{"WiFi on (turns BT off)", true}, {"Network name", false}, {"Password", false}};
const NetField kMqttFields[] = {{"MQTT on", true},   {"Server", false},     {"Username", false},
                                {"Password", false}, {"Root topic", false}, {"Encryption", true},
                                {"TLS", true}};
int netFieldCount(int page) { return page == 0 ? 3 : 7; }
const NetField *netFields(int page) { return page == 0 ? kWifiFields : kMqttFields; }

bool netGetBool(int page, int i)
{
    if (page == 0)
        return config.network.wifi_enabled;
    if (i == 0)
        return moduleConfig.mqtt.enabled;
    if (i == 5)
        return moduleConfig.mqtt.encryption_enabled;
    return moduleConfig.mqtt.tls_enabled;
}
void netSetBool(int page, int i, bool v)
{
    if (page == 0)
        config.network.wifi_enabled = v;
    else if (i == 0)
        moduleConfig.mqtt.enabled = v;
    else if (i == 5)
        moduleConfig.mqtt.encryption_enabled = v;
    else
        moduleConfig.mqtt.tls_enabled = v;
}
const char *netGetText(int page, int i)
{
    if (page == 0)
        return i == 1 ? config.network.wifi_ssid : config.network.wifi_psk;
    switch (i) {
    case 1: return moduleConfig.mqtt.address;
    case 2: return moduleConfig.mqtt.username;
    case 3: return moduleConfig.mqtt.password;
    default: return moduleConfig.mqtt.root;
    }
}
// A net text field is edited via MODE_SETNAME with editTarget = 20 + page*10 + index.
void netTextDecode(int t, int &page, int &i) { page = (t - 20) / 10; i = (t - 20) % 10; }
unsigned netTextMax(int page, int i)
{
    if (page == 0)
        return i == 1 ? 32 : 64; // wifi_ssid[33], wifi_psk[65]
    switch (i) {
    case 3: return 31;  // password[32]
    case 4: return 31;  // root[32]
    default: return 63; // address[64], username[64]
    }
}
void netSetText(int t, const char *s)
{
    int page, i;
    netTextDecode(t, page, i);
    unsigned n = netTextMax(page, i);
    char *dst = page == 0 ? (i == 1 ? config.network.wifi_ssid : config.network.wifi_psk)
                          : (i == 1   ? moduleConfig.mqtt.address
                             : i == 2 ? moduleConfig.mqtt.username
                             : i == 3 ? moduleConfig.mqtt.password
                                      : moduleConfig.mqtt.root);
    strncpy(dst, s, n);
    dst[n] = 0;
}

unsigned editMax(int t)
{
    if (t >= 20) {
        int p, i;
        netTextDecode(t, p, i);
        return netTextMax(p, i);
    }
    return t == 1 ? 4 : t == 2 ? 9 : t == 3 ? 11 : 24;
}
const char *editTitle(int t)
{
    if (t >= 20) {
        int p, i;
        netTextDecode(t, p, i);
        return netFields(p)[i].label;
    }
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

void drawFooter(lgfx::LGFXBase *g, const char *hint, uint16_t color = 0x630c)
{
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(color); // dim gray by default
    g->setCursor(4, 126);
    g->print(hint);
}

// Own battery as short text + colour ("87%", "64%+" charging, "USB" when none).
void batteryText(char *out, size_t cap, uint16_t &col)
{
    if (powerStatus && powerStatus->getHasBattery()) {
        int pct = powerStatus->getBatteryChargePercent();
        if (pct > 100)
            pct = 100;
        snprintf(out, cap, "%d%%%s", pct, powerStatus->getIsCharging() ? "+" : "");
        col = pct > 50 ? 0x07E0 : (pct > 20 ? 0xFFE0 : 0xF800);
    } else {
        snprintf(out, cap, "USB");
        col = 0x9CD3;
    }
}

// Delivery-status glyph for an outgoing message, drawn at the right of its line.
void drawMsgStatus(lgfx::LGFXBase *g, int x, int y, uint8_t status)
{
    if (status == MSG_SENDING) {
        g->fillCircle(x + 6, y + 6, 2, 0xFFE0); // yellow dot: in flight
    } else if (status == MSG_DELIVERED) {
        g->drawLine(x + 2, y + 6, x + 5, y + 9, 0x07E0); // green check: acked
        g->drawLine(x + 5, y + 9, x + 12, y + 2, 0x07E0);
    } else if (status == MSG_SENT) {
        g->drawLine(x + 2, y + 6, x + 5, y + 9, 0x07FF); // cyan check: broadcast sent (no ack)
        g->drawLine(x + 5, y + 9, x + 12, y + 2, 0x07FF);
    } else if (status == MSG_FAILED) {
        g->drawLine(x + 3, y + 2, x + 11, y + 10, 0xF800); // red x: failed
        g->drawLine(x + 11, y + 2, x + 3, y + 10, 0xF800);
    }
}

// Compact "HH:MM " prefix for a message's epoch time (local), or "" when the clock
// wasn't set when the message was stamped (avoids showing bogus 00:xx times).
void msgTimePrefix(uint32_t rxTime, int32_t tzOff, char *out, int cap)
{
    if (rxTime < 1600000000u) { // before 2020-09 => no valid RTC at stamp time
        out[0] = 0;
        return;
    }
    uint32_t local = rxTime + tzOff;
    snprintf(out, cap, "%02u:%02u ", (unsigned)((local / 3600u) % 24u), (unsigned)((local / 60u) % 60u));
}

// --- Transliterated Cyrillic input (Fn+L) --------------------------------------
// Phonetic Latin->Cyrillic with the usual digraphs (sh ш, zh ж, ch ч, sch щ,
// ya/yu/yo/ye) and singles j->й, x->ъ, '->ь. A letter is emitted immediately and morphed
// in place when it turns out to be the head of a digraph.
uint16_t translitSingle(char l)
{
    switch (l) {
    case 'a': return 0x430; case 'b': return 0x431; case 'v': return 0x432; case 'g': return 0x433;
    case 'd': return 0x434; case 'e': return 0x435; case 'z': return 0x437; case 'i': return 0x438;
    case 'j': return 0x439; case 'k': return 0x43A; case 'l': return 0x43B; case 'm': return 0x43C;
    case 'n': return 0x43D; case 'o': return 0x43E; case 'p': return 0x43F; case 'r': return 0x440;
    case 's': return 0x441; case 't': return 0x442; case 'u': return 0x443; case 'f': return 0x444;
    case 'h': return 0x445; case 'c': return 0x446; case 'y': return 0x44B;
    case 'x': return 0x44A; case 'q': return 0x44F;
    default: return 0;
    }
}
uint16_t translitDigraph(char a, char b)
{
    if (b == 'h') {
        if (a == 's') return 0x448; // ш
        if (a == 'z') return 0x436; // ж
        if (a == 'c') return 0x447; // ч
    }
    if (a == 'y') {
        if (b == 'a') return 0x44F; // я
        if (b == 'u') return 0x44E; // ю
        if (b == 'o') return 0x451; // ё
        if (b == 'e') return 0x44D; // э
    }
    return 0;
}
inline char lowerc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
inline bool isUp(char c) { return c >= 'A' && c <= 'Z'; }
uint16_t translitCase(uint16_t cp, bool upper)
{
    if (!upper) return cp;
    if (cp == 0x451) return 0x401; // ё -> Ё
    if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;
    return cp;
}
void appendCp(char *buf, uint8_t &len, size_t cap, uint16_t cp)
{
    if ((size_t)len + 2 >= cap) return;
    buf[len++] = 0xC0 | (cp >> 6);
    buf[len++] = 0x80 | (cp & 0x3F);
    buf[len] = 0;
}
// Feeds one typed key through the translit layer; may morph the previous letter.
void translitFeed(char *buf, uint8_t &len, size_t cap, char raw, char &pending)
{
    if (pending) {
        uint16_t cp = translitDigraph(lowerc(pending), lowerc(raw));
        if (cp) {
            if (len >= 2) { // drop the single already emitted, replace with the digraph
                len -= 2;
                buf[len] = 0;
            }
            uint16_t out = translitCase(cp, isUp(pending));
            // sch -> щ: fold a preceding с/С and this ч (from c+h) into щ/Щ
            if (cp == 0x447 && len >= 2) {
                uint8_t b0 = (uint8_t)buf[len - 2], b1 = (uint8_t)buf[len - 1];
                if (b0 == 0xD1 && b1 == 0x81) { // с
                    len -= 2;
                    buf[len] = 0;
                    out = 0x449; // щ
                } else if (b0 == 0xD0 && b1 == 0xA1) { // С
                    len -= 2;
                    buf[len] = 0;
                    out = 0x429; // Щ
                }
            }
            appendCp(buf, len, cap, out);
            pending = 0;
            return;
        }
        pending = 0;
    }
    if (raw == '\'') {
        appendCp(buf, len, cap, 0x44C); // ь
        return;
    }
    uint16_t cp = translitSingle(lowerc(raw));
    if (cp) {
        appendCp(buf, len, cap, translitCase(cp, isUp(raw)));
        char l = lowerc(raw);
        pending = (l == 's' || l == 'z' || l == 'c' || l == 'y') ? raw : 0;
    } else if ((size_t)len + 1 < cap) {
        buf[len++] = raw; // digits / punctuation pass through literally
        buf[len] = 0;
        pending = 0;
    }
}

constexpr int kEmoteAdv = 18; // on-screen advance for a 16px emoji glyph (+2px gap)

// Curated palette shown in the emoji picker (labels match graphics::emotes[] exactly).
// Incoming messages still render ANY stock emoji; this only limits what we offer to send.
const char *kEmojiPalette[] = {
    "\U0001F44D", "\U0001F44E", "\U00002764\U0000FE0F", "\U0001F602", "\U0001F923", "\U0001F642",
    "\U0001F60A", "\U0001F60D", "\U0001F60E", "\U0001F609", "\U0001F62D", "\U0001F605",
    "\U0001F440", "\U0001F525", "\U0001F64F", "\U0001F4AA", "\U0001F44B", "\U0001F937",
    "\U00002753", "\U0000203C\U0000FE0F", "\U00002705", "\U00002600\U0000FE0F", "\U00002744\U0000FE0F", "\U0001F4A9"};
constexpr int kEmojiCount = sizeof(kEmojiPalette) / sizeof(kEmojiPalette[0]);

// Byte length of the UTF-8 sequence starting at lead byte c.
int utf8Len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c >> 5) == 0x06)
        return 2;
    if ((c >> 4) == 0x0E)
        return 3;
    if ((c >> 3) == 0x1E)
        return 4;
    return 1;
}

// Longest emote label that is a prefix of s; returns its byte length (0 = none) and,
// via *em, the matching table entry. Emote labels all start with 0xE2 or 0xF0, so we
// only scan the table for those lead bytes (ASCII and Cyrillic never match).
int emoteMatch(const char *s, const graphics::Emote **em)
{
    unsigned char c0 = (unsigned char)s[0];
    if (c0 != 0xE2 && c0 != 0xF0)
        return 0;
    int bestLen = 0;
    const graphics::Emote *best = nullptr;
    for (int i = 0; i < graphics::numEmotes; i++) {
        const char *lab = graphics::emotes[i].label;
        int ll = (int)strlen(lab);
        if (ll > bestLen && strncmp(s, lab, ll) == 0) {
            bestLen = ll;
            best = &graphics::emotes[i];
        }
    }
    if (em)
        *em = best;
    return bestLen;
}

// Copies the leading run of `s` that fits `maxW` px in the current font into `out`
// (breaking at a space when the line overflows), and returns how many bytes of `s`
// were consumed. Emoji sequences are atomic tokens of width kEmoteAdv, so they wrap
// as a unit and are never split across lines.
int wrapLine(lgfx::LGFXBase *g, const char *s, int maxW, char *out, int outCap)
{
    int consumed = 0, w = 0, spaceCut = -1;
    while (s[consumed]) {
        const graphics::Emote *em = nullptr;
        int elen = emoteMatch(s + consumed, &em);
        int tlen = elen > 0 ? elen : utf8Len((unsigned char)s[consumed]);
        int tw;
        if (elen > 0) {
            tw = em->width + 2;
        } else {
            char cb[5];
            int k = 0;
            for (; k < tlen && s[consumed + k]; k++)
                cb[k] = s[consumed + k];
            cb[k] = 0;
            tw = g->textWidth(cb);
        }
        if (consumed > 0 && w + tw > maxW)
            break; // doesn't fit -> wrap here
        if (consumed + tlen > outCap - 1)
            break; // out buffer full
        consumed += tlen;
        w += tw;
        if (s[consumed - 1] == ' ')
            spaceCut = consumed;
    }
    int cut = !s[consumed] ? consumed : (spaceCut > 0 ? spaceCut : consumed);
    int dispLen = cut;
    while (dispLen > 0 && s[dispLen - 1] == ' ')
        dispLen--; // don't render the trailing break space
    memcpy(out, s, dispLen);
    out[dispLen] = 0;
    return cut > 0 ? cut : 1;
}

// Pixel width of a string with inline emoji (each emoji counts as its glyph advance).
// Decodes one UTF-8 sequence of known length into a codepoint.
uint32_t utf8Cp(const char *s, int len)
{
    unsigned char c = (unsigned char)s[0];
    if (len == 2)
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    if (len == 3)
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    if (len == 4)
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return c;
}

// What the embedded 9x15 font actually has: ASCII plus Cyrillic. Everything
// else goes to the unicode font when present (and to the tofu box when not).
bool flashFontCovers(uint32_t cp)
{
    return cp < 0x7F || (cp >= 0x400 && cp <= 0x45F) || cp == 0x490 || cp == 0x491;
}

int lineWidthEmotes(lgfx::LGFXBase *g, const char *s)
{
    int w = 0;
    while (*s) {
        const graphics::Emote *em = nullptr;
        int elen = emoteMatch(s, &em);
        if (elen > 0 && em) {
            w += em->width + 2;
            s += elen;
        } else {
            int tlen = utf8Len((unsigned char)s[0]);
            char cb[5];
            int k = 0;
            for (; k < tlen && s[k]; k++)
                cb[k] = s[k];
            cb[k] = 0;
            uint32_t cp = utf8Cp(cb, k);
            int gw = !flashFontCovers(cp) ? sdGlyphWidth(cp) : 0;
            w += gw ? gw + 1 : g->textWidth(cb);
            s += k;
        }
    }
    return w;
}

// Draws a message-line string at (x,y) in `color`, blitting emoji bitmaps inline;
// returns the x after the last glyph (for multi-colour spans on one line).
// emojiDy nudges the bitmaps down to sit on the text's visual band (the 9x15 font
// keeps ~3px of ascender space, so a 16px emoji rides high without it).
int printLineEmotes(lgfx::LGFXBase *g, int x, int y, const char *s, uint16_t color, int emojiDy = 0)
{
    g->setFont(&cyrFont);
    g->setTextSize(1);
    int cx = x;
    while (*s) {
        const graphics::Emote *em = nullptr;
        int elen = emoteMatch(s, &em);
        if (elen > 0 && em) {
            g->drawXBitmap(cx, y + (17 - em->height) / 2 + emojiDy, em->bitmap, em->width, em->height, color);
            cx += em->width + 2;
            s += elen;
        } else { // one non-emote UTF-8 char
            int tlen = utf8Len((unsigned char)s[0]);
            char cb[5];
            int k = 0;
            for (; k < tlen && s[k]; k++)
                cb[k] = s[k];
            cb[k] = 0;
            uint32_t cp = utf8Cp(cb, k);
            uint8_t bits[32];
            int gw = !flashFontCovers(cp) ? sdGlyph(cp, bits) : 0;
            if (gw) { // unicode glyph, aligned onto the emoji band
                g->drawBitmap(cx, y + (17 - 16) / 2 + emojiDy, bits, gw, 16, color);
                cx += gw + 1;
            } else {
                g->setTextColor(color);
                g->setCursor(cx, y);
                g->print(cb);
                cx += g->textWidth(cb);
            }
            s += k;
        }
    }
    return cx;
}

// Copies at most `maxBytes` of s into out without splitting a UTF-8 sequence.
void utf8Copy(char *out, const char *s, int maxBytes)
{
    int n = 0;
    while (s[n]) {
        int l = utf8Len((unsigned char)s[n]);
        if (n + l > maxBytes)
            break;
        n += l;
    }
    memcpy(out, s, n);
    out[n] = 0;
}

// Strips codepoints we can't actually draw (anything beyond ASCII, Cyrillic and the
// stock emoji bitmaps) — the 9x15 font renders those as boxes. In place.
void sanitizeDisplay(char *s)
{
    if (sdFontReady())
        return; // the unicode font draws anything in the BMP — keep names whole
    char out[32];
    int o = 0;
    const char *p = s;
    while (*p && o < (int)sizeof(out) - 5) {
        int el = emoteMatch(p, nullptr);
        if (el > 0) { // a stock emote: rendered as a bitmap
            memcpy(out + o, p, el);
            o += el;
            p += el;
            continue;
        }
        int len = utf8Len((unsigned char)*p);
        bool ok = false;
        if (len == 1) {
            ok = (unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F;
        } else if (len == 2) { // the font also covers Cyrillic
            uint32_t cp = (((unsigned char)p[0] & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
            ok = cp >= 0x400 && cp <= 0x45F;
        }
        if (ok) {
            memcpy(out + o, p, len);
            o += len;
        }
        p += len;
    }
    out[o] = 0;
    strcpy(s, out);
}

// Short name of a node for compact labels (channel senders, reaction lines);
// falls back to the node-number tail when unknown or nothing drawable remains.
void shortNameOf(uint32_t num, char *out, size_t cap)
{
    meshtastic_NodeInfoLite *n = nodeByNum(num);
    if (n && n->short_name[0]) {
        snprintf(out, cap, "%s", n->short_name);
        sanitizeDisplay(out);
        if (out[0])
            return;
    }
    snprintf(out, cap, "%04x", (unsigned)(num & 0xFFFF));
}

} // namespace

AdvUI::AdvUI() : concurrency::OSThread("advui") {}

// Screen auto-off cuts the display power rail (GPIO38 feeds panel + backlight —
// there is no separately dimmable backlight on this board), so waking needs a
// full panel re-init: the ST7789 loses its config with the rail.
void AdvUI::screenSleep()
{
    screenOn = false;
    digitalWrite(38, LOW);
    LOG_INFO("advui: screen sleep");
}

void AdvUI::screenWake()
{
    digitalWrite(38, HIGH);
    delay(20); // let the rail settle before talking to the panel
    display.init();
    display.setRotation(1);
    screenOn = true;
    lastActivityMs = millis();
    LOG_INFO("advui: screen wake");
}

void AdvUI::initHardware()
{
    pinMode(38, OUTPUT); // display power/backlight rail — steady HIGH, not PWM
    digitalWrite(38, HIGH);

    bool ok = display.init();
    display.setRotation(1); // landscape 240x135
    display.fillScreen(0x0000);
    lastActivityMs = millis();

    // 8-bit (rgb332) frame buffer: 32KB instead of 64KB. The full 16-bit buffer
    // starved the internal DMA pool so PKI crypto (esp-aes) couldn't allocate and
    // DMs failed to send. Colours coarsen slightly but stay recognisable.
    //
    // The buffer is static RAM, not createSprite(): a 32KB contiguous malloc can
    // fail on a fragmented heap (WiFi+MQTT setups), and the direct-draw fallback
    // repaints the panel at 5 Hz — the "blinking screen" users reported. A static
    // buffer costs the same RAM but can never fail or fragment.
    static uint8_t fb[240 * 135];
    canvas.setColorDepth(8);
    canvas.setBuffer(fb, display.width(), display.height(), 8);
    haveCanvas = true;
    LOG_INFO("advui: UI up init=%d %dx%d canvas=%d", (int)ok, display.width(), display.height(),
             (int)haveCanvas);

    // Silence the stock ExternalNotificationModule: on this HAS_I2S board its default
    // config beeps the codec on every received message (channel broadcasts included).
    // We drive our own favourites-only single beep instead (see startBeep()).
    moduleConfig.external_notification.enabled = false;

#ifdef HAS_NEOPIXEL
    // Boot self-test: cycle red -> green -> blue so it's unmistakable the LED works.
    rgbLedWrite(NEOPIXEL_DATA, 90, 0, 0);
    delay(250);
    rgbLedWrite(NEOPIXEL_DATA, 0, 90, 0);
    delay(250);
    rgbLedWrite(NEOPIXEL_DATA, 0, 0, 90);
    delay(250);
    rgbLedWrite(NEOPIXEL_DATA, 0, 0, 0);
#endif

    loadRadioCfg();
    if (!g_radioCompanion)
        api.begin(); // companion doesn't subscribe to the local engine's phone stream
    kb.begin();
    LOG_INFO("advui: keyboard ready");

    sdFontInit(); // flash font partition (or /unifont.bin on SD) unlocks full-BMP text
    loadMsgs(); // restore the saved conversation from flash
    if (g_radioCompanion) {
        // The companion is a terminal to another node: free the RAM the stock BT
        // peripheral would take (no phone connects to us) — SMP pairing on the
        // central link needs that heap.
        config.bluetooth.enabled = false;
        bleCompanionInit(); // BLE central for the companion link (scan/pair/connect)
    }
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

    uint32_t me = myNodeNum();

    // A tapback: attach to the target message instead of adding a bubble. Quiet by
    // design (no unread/beep) — just a LED blink so it isn't missed entirely.
    if (p.decoded.emoji && p.decoded.reply_id) {
        if (p.from != me) {
            addReaction(p.decoded.reply_id, p.from, text);
#ifdef HAS_NEOPIXEL
            flashLed(60, 40, 90); // soft violet: a reaction, not a message
#endif
        }
        return;
    }

    bool unread = p.from != me && (p.to == me || p.to == NODENUM_BROADCAST); // DM to us, or a channel broadcast
    // keep the id (reactions/replies reference it) and the reply link (draws the quote)
    addMsg(p.from, p.to, p.channel, p.rx_time, unread, text, p.id, MSG_IN, p.decoded.reply_id);

    if (unread) {
        bool fav = (p.to == NODENUM_BROADCAST) ? chanFav(p.channel) : (!g_radioCompanion && nodeDB && nodeDB->isFavorite(p.from));
#ifdef HAS_NEOPIXEL
        // LED flash on any incoming: green from a favourite, blue otherwise.
        if (fav)
            flashLed(0, 90, 20);
        else
            flashLed(0, 20, 90);
#endif
#ifdef HAS_I2S
        // Beep only from a favourite (channel broadcast or DM); everyone else stays silent.
        if (fav)
            startBeep();
#endif
    }
}

// Builds and transmits a text packet (DM with PKI, or channel broadcast) and returns
// its packet id. replyId + asEmoji turn it into a reaction (tapback) on that message.
static uint32_t sendTextPacket(uint32_t to, const char *text, int chIdx = 0, uint32_t replyId = 0, bool asEmoji = false)
{
    if (g_radioCompanion) { // ship it over BLE: the node's radio does routing/PKI itself
        if (g_linkState != BLE_CONNECTED)
            return 0;
        meshtastic_ToRadio t = meshtastic_ToRadio_init_default;
        t.which_payload_variant = meshtastic_ToRadio_packet_tag;
        meshtastic_MeshPacket &p = t.packet;
        p.id = (uint32_t)esp_random();
        if (!p.id)
            p.id = 1;
        p.to = to;
        p.want_ack = to != NODENUM_BROADCAST;
        if (to == NODENUM_BROADCAST)
            p.channel = chIdx;
        p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        p.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        size_t n = strlen(text);
        if (n > sizeof(p.decoded.payload.bytes))
            n = sizeof(p.decoded.payload.bytes);
        memcpy(p.decoded.payload.bytes, text, n);
        p.decoded.payload.size = n;
        if (replyId) {
            p.decoded.reply_id = replyId;
            if (asEmoji)
                p.decoded.emoji = 1;
        }
        if (to != NODENUM_BROADCAST) { // a DM to a key-capable node must be marked PKI
            for (int i = 0; i < g_compNodeCount; i++)
                if (g_compNodes[i].num == to && g_compNodes[i].hasKey) {
                    p.pki_encrypted = true;
                    p.channel = 0;
                    break;
                }
        }
        uint8_t buf[320];
        size_t len = pb_encode_to_bytes(buf, sizeof(buf), &meshtastic_ToRadio_msg, &t);
        if (!len || !bleQueueToRadio(buf, (uint16_t)len))
            return 0;
        return p.id;
    }

    if (!router || !service)
        return 0;
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = to;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.dest = to;
    size_t n = strlen(text);
    if (n > sizeof(p->decoded.payload.bytes))
        n = sizeof(p->decoded.payload.bytes);
    memcpy(p->decoded.payload.bytes, text, n);
    p->decoded.payload.size = n;
    if (replyId) {
        p->decoded.reply_id = replyId;
        if (asEmoji)
            p->decoded.emoji = 1;
    }

    if (to == NODENUM_BROADCAST) {
        p->channel = chIdx;
        p->want_ack = false; // broadcasts aren't acked
    } else {
        p->want_ack = true;
        // A DM to a key-capable node must be PKI-encrypted (stock forces this); without
        // it the recipient on a modern mesh won't accept/ack the message.
        meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNode(to) : nullptr;
        if (node && node->public_key.size == 32) {
            p->pki_encrypted = true;
            p->channel = 0;
        }
    }

    uint32_t id = p->id;
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    return id;
}

// Remote admin over the companion link: an AdminMessage addressed to the linked
// node itself, exactly how the phone app configures it. Arriving via the node's
// own client API counts as local admin, so no session key or admin channel is
// needed on the node side.
static bool sendAdminToNode(const meshtastic_AdminMessage &adm)
{
    if (!g_radioCompanion || g_linkState != BLE_CONNECTED || !g_linkMyNode)
        return false;
    meshtastic_ToRadio t = meshtastic_ToRadio_init_default;
    t.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket &p = t.packet;
    p.id = (uint32_t)esp_random();
    if (!p.id)
        p.id = 1;
    p.to = g_linkMyNode;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    p.decoded.payload.size =
        pb_encode_to_bytes(p.decoded.payload.bytes, sizeof(p.decoded.payload.bytes), &meshtastic_AdminMessage_msg, &adm);
    if (!p.decoded.payload.size)
        return false;
    uint8_t buf[320];
    size_t len = pb_encode_to_bytes(buf, sizeof(buf), &meshtastic_ToRadio_msg, &t);
    return len && bleQueueToRadio(buf, (uint16_t)len);
}

// Sends a text DM to a node and adds it to our own thread immediately (status
// "sending"); the delivery ACK later flips it to "delivered" via ackMsg().
void AdvUI::sendMessage(uint32_t to, const char *text, uint32_t replyId)
{
    uint32_t id = sendTextPacket(to, text, 0, replyId);
    if (!id)
        return;
    uint32_t me = myNodeNum();
    addMsg(me, to, 0, getTime(false), false, text, id, MSG_SENDING, replyId);
}

// Sends a text broadcast to a channel and adds it to that channel's thread.
void AdvUI::sendChannel(int chIdx, const char *text, uint32_t replyId)
{
    uint32_t id = sendTextPacket(NODENUM_BROADCAST, text, chIdx, replyId);
    if (!id)
        return;
    uint32_t me = myNodeNum();
    addMsg(me, NODENUM_BROADCAST, chIdx, getTime(false), false, text, id, MSG_SENT, replyId); // no ack for broadcast
}

// Sends a tapback on the message at ring index msgIdx and records it locally.
void AdvUI::sendReaction(int msgIdx, const char *label)
{
    if (msgIdx < 0 || msgIdx >= kMaxMsgs)
        return;
    Msg &m = g_msgs[msgIdx];
    if (!m.id) {
        LOG_INFO("advui: can't react - message has no packet id (pre-update history)");
        return;
    }
    uint32_t id = (selectedChannel >= 0) ? sendTextPacket(NODENUM_BROADCAST, label, selectedChannel, m.id, true)
                                         : sendTextPacket(selectedNum, label, 0, m.id, true);
    if (!id)
        return;
    uint32_t me = myNodeNum();
    addReaction(m.id, me, label);
}

// Fills out[] with node-DB indices matching query (all if query is null/empty),
// in the default sorted order. Returns the count (capped at max).
int AdvUI::buildNodeList(uint16_t *out, int max, const char *query)
{
    int count = 0;
    uint32_t me = myNodeNum();

    if (g_radioCompanion) { // companion: nodes come from the BLE config stream
        for (int i = 0; i < g_compNodeCount && count < max; i++) {
            const CompNode &c = g_compNodes[i];
            if (c.num == me)
                continue;
            if (query && query[0]) {
                const char *name = c.longName[0] ? c.longName : c.shortName;
                if (!ciContains(name[0] ? name : "", query))
                    continue;
            }
            out[count++] = (uint16_t)i;
        }
        std::sort(out, out + count, [](uint16_t a, uint16_t b) { // unread first, then freshest
            bool ua = hasUnreadFrom(g_compNodes[a].num), ub = hasUnreadFrom(g_compNodes[b].num);
            if (ua != ub)
                return ua;
            return g_compNodes[a].lastHeard > g_compNodes[b].lastHeard;
        });
        return count;
    }

    if (!nodeDB)
        return 0;
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

// Fills chanList[] with enabled channels (query-matched), favourites first.
int AdvUI::buildChannels(const char *query)
{
    int count = 0;
    for (int i = 0; i < 8 && count < 8; i++) {
        if (!chanEnabled(i))
            continue;
        if (query && query[0]) {
            const char *name = chanName(i);
            if (!ciContains(name && name[0] ? name : "", query))
                continue;
        }
        chanList[count++] = (uint8_t)i;
    }
    std::stable_sort(chanList, chanList + count, [](uint8_t a, uint8_t b) { return chanFav(a) && !chanFav(b); });
    return count;
}

void AdvUI::drawChannelRow(int chIdx, int y)
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    int nameX = 4;
    if (hasUnreadChannel(chIdx)) {
        int ex = 4, ey = y + 5;
        g->drawRect(ex, ey, 11, 8, 0xF800);
        g->drawLine(ex, ey, ex + 5, ey + 4, 0xF800);
        g->drawLine(ex + 10, ey, ex + 5, ey + 4, 0xF800);
        nameX = 18;
    }
    char nm[24];
    snprintf(nm, sizeof(nm), "#%s", chanName(chIdx));
    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(1);
    fitWidth(g, nm, 232 - nameX);
    g->setTextColor(chanFav(chIdx) ? 0xFFE0 : 0x07FF); // favourite = yellow, else cyan
    g->setCursor(nameX, y + 2);
    g->print(nm);
}

// Opens the combined-list entry (channels first, then filtered nodes).
// Ring index of the first (chronological) unread message in the currently open thread,
// or -1 if there are none. Read the read flag BEFORE markRead*, i.e. call on open.
int AdvUI::firstUnreadIdx()
{
    bool isChan = selectedChannel >= 0;
    for (int i = 0; i < g_msgCount; i++) {
        int idx = i;
        Msg &m = g_msgs[idx];
        if (m.read)
            continue;
        bool match = isChan ? (m.to == NODENUM_BROADCAST && m.ch == selectedChannel)
                            : (m.from == selectedNum && m.to != NODENUM_BROADCAST);
        if (match)
            return idx;
    }
    return -1;
}

// Ring index of the back-th newest message in the open thread (0 = newest), or -1.
int AdvUI::matchedFromNewest(int back)
{
    bool isChan = selectedChannel >= 0;
    int seen = 0;
    for (int i = g_msgCount - 1; i >= 0; i--) {
        int idx = i;
        Msg &m = g_msgs[idx];
        bool match = isChan ? (m.to == NODENUM_BROADCAST && m.ch == selectedChannel)
                            : ((m.from == selectedNum || m.to == selectedNum) && m.to != NODENUM_BROADCAST);
        if (match && seen++ == back)
            return idx;
    }
    return -1;
}

void AdvUI::openEntry(int s)
{
    nodeReturn = (mode == MODE_PICKER) ? MODE_PICKER : MODE_NODES;
    chatScroll = 0;          // default: pinned to the newest message
    chatAnchorMsgIdx = -1;
    reactSel = -1;
    reactStrip = false;
    pendingReplyId = 0;
    if (s < chanCount) {
        selectedChannel = chanList[s];
        chatAnchorMsgIdx = firstUnreadIdx(); // jump target; read flags clear as lines get seen
        mode = MODE_NODE;
    } else if (nodeDB) {
        meshtastic_NodeInfoLite *node = nodeAt(filtered[s - chanCount]);
        if (node) {
            selectedChannel = -1;
            selectedNum = node->num;
            chatAnchorMsgIdx = firstUnreadIdx();
            mode = MODE_NODE;
        }
    }
}

// Removes every message of a conversation from the ring (compacting it), plus any
// now-orphaned reactions, and persists. Channel = its broadcasts; DM = both directions.
void AdvUI::deleteConversation(const Conv &c)
{
    int w = 0;
    for (int r = 0; r < g_msgCount; r++) {
        Msg &m = g_msgs[r];
        bool mine = c.isChan ? (m.to == NODENUM_BROADCAST && m.ch == c.ch)
                             : (m.to != NODENUM_BROADCAST && (m.from == c.node || m.to == c.node));
        if (mine)
            continue; // drop it
        if (w != r) {
            g_msgs[w] = m;
            g_msgReply[w] = g_msgReply[r];
        }
        w++;
    }
    g_msgCount = w;
    // drop reactions whose target message no longer exists
    int rw = 0;
    for (int r = 0; r < g_reactCount; r++) {
        bool live = false;
        for (int i = 0; i < g_msgCount; i++)
            if (g_msgs[i].id && g_msgs[i].id == g_reacts[r].msgId) {
                live = true;
                break;
            }
        if (live) {
            if (rw != r)
                g_reacts[rw] = g_reacts[r];
            rw++;
        }
    }
    g_reactCount = rw;
    g_reactNext = rw % kMaxReacts;
    g_msgsDirty = true;
    saveMsgs();
}

void AdvUI::favEntry(int s, bool on)
{
    if (s < chanCount) {
        int ci = chanList[s];
        if (on)
            g_favChannels |= (1u << ci);
        else
            g_favChannels &= ~(1u << ci);
        g_msgsDirty = true; // persisted alongside the messages
    } else if (nodeDB && !g_radioCompanion) { // node favourites live in the local DB only
        meshtastic_NodeInfoLite *node = nodeAt(filtered[s - chanCount]);
        if (node)
            nodeDB->set_favorite(on, node->num);
    }
}

// Collects one entry per conversation (channel or node DM) from the message ring,
// keeping the most recent message of each, newest conversation first.
void AdvUI::buildConversations()
{
    convCount = 0;
    uint32_t me = myNodeNum();
    for (int i = 0; i < g_msgCount; i++) {
        int idx = i;
        Msg &m = g_msgs[idx];
        bool isChan = (m.to == NODENUM_BROADCAST);
        uint32_t node = isChan ? 0 : (m.from == me ? m.to : m.from);
        int found = -1;
        for (int j = 0; j < convCount; j++)
            if (conv[j].isChan == isChan && (isChan ? conv[j].ch == m.ch : conv[j].node == node)) {
                found = j;
                break;
            }
        if (found < 0) {
            if (convCount >= kMaxConv)
                continue;
            found = convCount++;
            conv[found].isChan = isChan;
            conv[found].ch = m.ch;
            conv[found].node = node;
        }
        conv[found].lastIdx = idx;
        conv[found].order = i; // arrival order; higher = more recent
    }
    std::sort(conv, conv + convCount, [](const Conv &a, const Conv &b) { return a.order > b.order; });
    if (sel >= convCount)
        sel = convCount ? convCount - 1 : 0;
    if (sel < 0)
        sel = 0;
}

void AdvUI::openConv(int i)
{
    if (i < 0 || i >= convCount)
        return;
    nodeReturn = MODE_CHATS;
    chatScroll = 0;
    chatAnchorMsgIdx = -1;
    reactSel = -1;
    reactStrip = false;
    pendingReplyId = 0;
    if (conv[i].isChan) {
        selectedChannel = conv[i].ch;
        chatAnchorMsgIdx = firstUnreadIdx();
    } else {
        selectedChannel = -1;
        selectedNum = conv[i].node;
        chatAnchorMsgIdx = firstUnreadIdx();
    }
    mode = MODE_NODE;
}

// Home screen: recent conversations, newest first, with a last-message preview.
void AdvUI::drawChats()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    char bbuf[10];
    uint16_t bcol;
    batteryText(bbuf, sizeof(bbuf), bcol);
    g->setTextColor(0x07FF);
    g->setCursor(4, 3);
    g->print("Chats");
    if (g_radioCompanion)
        drawBtGlyph(g, 40, 2, linkColor()); // companion link state at a glance
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
        g->fillRoundRect(px, 2, tw + 6, 10, 2, 0xF800);
        g->setTextColor(0xFFFF);
        g->setCursor(px + 3, 3);
        g->print(ub);
    }
    g->drawFastHLine(0, 13, 240, 0x39C7);

    buildConversations();
    uint32_t me = myNodeNum();
    int32_t tzOff = g_utcOffsetMin * 60;

    if (convCount == 0) {
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextSize(1);
        g->setTextColor(0x630C);
        g->setCursor(6, 50);
        g->print("No chats yet");
        g->setFont(&lgfx::fonts::Font0);
        g->setTextColor(0x8410);
        g->setCursor(6, 70);
        g->print("type a name to find a node");
        drawFooter(g, "type find   Tab: all nodes");
        if (haveCanvas)
            canvas.pushSprite(0, 0);
        return;
    }

    const int top = 15, rowH = 26, maxRows = (124 - top) / rowH;
    if (sel < scrollTop)
        scrollTop = sel;
    if (sel >= scrollTop + maxRows)
        scrollTop = sel - maxRows + 1;
    if (scrollTop < 0)
        scrollTop = 0;

    int y = top;
    for (int r = 0; r < maxRows; r++) {
        int i = scrollTop + r;
        if (i >= convCount)
            break;
        Conv &c = conv[i];
        Msg &m = g_msgs[c.lastIdx];
        if (i == sel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945);
        bool unread = c.isChan ? hasUnreadChannel(c.ch) : hasUnreadFrom(c.node);
        bool fav = c.isChan ? chanFav(c.ch) : (!g_radioCompanion && nodeDB && nodeDB->isFavorite(c.node));

        int nameX = 6;
        if (unread) { // red envelope before the name
            int ex = 5, ey = y + 3;
            g->drawRect(ex, ey, 11, 8, 0xF800);
            g->drawLine(ex, ey, ex + 5, ey + 4, 0xF800);
            g->drawLine(ex + 10, ey, ex + 5, ey + 4, 0xF800);
            nameX = 20;
        }
        // time (right, dim)
        char tb[8];
        msgTimePrefix(m.rxTime, tzOff, tb, sizeof(tb));
        int tl = (int)strlen(tb);
        if (tl && tb[tl - 1] == ' ')
            tb[tl - 1] = 0;
        g->setFont(&lgfx::fonts::Font0);
        g->setTextSize(1);
        int tw = tb[0] ? g->textWidth(tb) : 0;
        if (tb[0]) {
            g->setTextColor(0x8410);
            g->setCursor(238 - tw, y + 2);
            g->print(tb);
        }
        // name
        char nm[40];
        if (c.isChan) {
            snprintf(nm, sizeof(nm), "#%s", chanName(c.ch));
        } else {
            meshtastic_NodeInfoLite *n = nodeByNum(c.node);
            const char *nn = n ? nodeName(n) : "";
            if (nn[0])
                snprintf(nm, sizeof(nm), "%s", nn);
            else
                snprintf(nm, sizeof(nm), "!%08x", (unsigned)c.node);
        }
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextSize(1);
        fitWidth(g, nm, 236 - nameX - (tw ? tw + 6 : 0));
        g->setTextColor(fav ? 0xFFE0 : (c.isChan ? 0x07FF : 0xFFFF));
        g->setCursor(nameX, y);
        g->print(nm);
        // preview (second line): "> " for our own last message, sender name for channel
        // messages (many senders), inline emoji, truncated
        char pv[80];
        int pfx = 0;
        if (m.from == me) {
            pv[0] = '>';
            pv[1] = ' ';
            pfx = 2;
        } else if (c.isChan) {
            char sn[8];
            shortNameOf(m.from, sn, sizeof(sn));
            pfx = snprintf(pv, sizeof(pv), "%s: ", sn);
        }
        utf8Copy(pv + pfx, m.text, (int)sizeof(pv) - pfx - 1);
        g->setFont(&cyrFont);
        g->setTextSize(1);
        while (pv[0] && lineWidthEmotes(g, pv) > 230) { // trim to fit, whole codepoints
            int k = (int)strlen(pv) - 1;
            while (k > 0 && ((unsigned char)pv[k] & 0xC0) == 0x80)
                k--;
            pv[k] = 0;
        }
        printLineEmotes(g, 6, y + 14, pv, 0x9CD3);
        y += rowH;
    }
    if (confirmDel && sel < convCount)
        drawFooter(g, "Delete this chat? Enter = yes   ESC = no", 0xF800); // red
    else
        drawFooter(g, "ENTER open  Del erase  Tab nodes");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::drawNodeList()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    size_t total = g_radioCompanion ? (size_t)g_compNodeCount : (nodeDB ? nodeDB->getNumMeshNodes() : 0);
    size_t cap = g_radioCompanion ? (size_t)kMaxCompNodes : (size_t)::advui_max_num_nodes();
    uint32_t me = myNodeNum();

    g->setFont(&lgfx::fonts::Font0); // header stays on the compact bitmap font
    g->setTextSize(1);

    char bbuf[10];
    uint16_t bcol;
    batteryText(bbuf, sizeof(bbuf), bcol);

    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    // At the cap the table is full and pinned there (new nodes evict old ones),
    // so show "200+" to make the cap visible rather than a suspicious flat count.
    g->printf("%u%s nodes", (unsigned)total, total >= cap ? "+" : "");

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

    rebuildFiltered(); // channels + filtered nodes, clamps sel
    const int top = 15, rowH = 18, maxRows = (124 - top) / rowH;
    int listTotal = chanCount + filteredCount;
    if (sel < scrollTop)
        scrollTop = sel;
    if (sel >= scrollTop + maxRows)
        scrollTop = sel - maxRows + 1;
    if (scrollTop < 0)
        scrollTop = 0;

    int y = top;
    for (int r = 0; r < maxRows; r++) {
        int idx = scrollTop + r;
        if (idx >= listTotal)
            break;
        if (idx == sel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945); // cursor highlight
        if (idx < chanCount) {
            drawChannelRow(chanList[idx], y);
        } else {
            meshtastic_NodeInfoLite *node = nodeAt(filtered[idx - chanCount]);
            if (node)
                drawNodeRow(g, node, y, node->num == me);
        }
        y += rowH;
    }

    drawFooter(g, "</>fav  ENTER open  type find  ESC/Tab chats");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::rebuildFiltered()
{
    const char *q = queryLen ? query : nullptr;
    chanCount = buildChannels(q);
    filteredCount = buildNodeList(filtered, kMaxFiltered, q);
    int total = chanCount + filteredCount;
    if (sel >= total)
        sel = total ? total - 1 : 0;
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

    uint32_t me = myNodeNum();
    const int rowH = 18, top = 16, maxRows = (124 - top) / rowH;
    int total = chanCount + filteredCount;

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
        if (idx >= total)
            break;
        if (idx == sel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945); // selection highlight
        if (idx < chanCount) {
            drawChannelRow(chanList[idx], y);
        } else {
            meshtastic_NodeInfoLite *node = nodeAt(filtered[idx - chanCount]);
            if (node)
                drawNodeRow(g, node, y, node->num == me);
        }
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

    bool isChan = selectedChannel >= 0;
    meshtastic_NodeInfoLite *node = !isChan ? nodeByNum(selectedNum) : nullptr;
    uint32_t me = myNodeNum();

    // Read flags clear per message as its lines actually reach the screen (see
    // the render loop below) — leaving mid-thread keeps the rest unread.

    // header: the channel, or the node's row (name + signal + hops + role), like the list
    if (isChan) {
        drawChannelRow(selectedChannel, 2);
    } else if (node) {
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
    int bottom = (mode == MODE_COMPOSE) ? (pendingReplyId ? 96 : 112) : 122; // room for compose (+ quote line)
    int maxLines = (bottom - fy0) / lh;
    int matched[kMaxMsgs], mc = 0;
    bool hasFailed = false; // any failed outgoing DM here -> offer ENTER resend
    for (int i = 0; i < g_msgCount; i++) {
        int idx = i;
        bool match = isChan ? (g_msgs[idx].to == NODENUM_BROADCAST && g_msgs[idx].ch == selectedChannel)
                            : ((g_msgs[idx].from == selectedNum || g_msgs[idx].to == selectedNum) &&
                               g_msgs[idx].to != NODENUM_BROADCAST);
        if (match) {
            matched[mc++] = idx;
            if (!isChan && g_msgs[idx].from == me && g_msgs[idx].status == MSG_FAILED)
                hasFailed = true;
        }
    }
    if (mc == 0) {
        g->setTextColor(0x630C);
        g->setCursor(4, fy0);
        g->print("(no messages yet)");
    } else {
        g->setFont(&cyrFont); // Cyrillic-capable, so Russian message text renders
        g->setTextSize(1);
        // Word-wrap each message into display lines; the status indicator sits on the
        // message's LAST line, so long messages keep it visible. We build the wrapped
        // lines into a ring and render only the last `maxLines` of them.
        struct DLine {
            char text[40];
            uint16_t color;
            bool last;        // final line of its message (draws the status marker)
            int16_t msgIdx;   // g_msgs ring index (-1: quote/reaction decoration)
            bool out;
            uint8_t status;
            uint8_t err;
            uint8_t timeLen;  // leading chars that are the dim "HH:MM " prefix (0 on continuation lines)
            uint8_t nameLen;  // chars after the time that are the sender-name span (channels)
        };
        int32_t tzOff = g_utcOffsetMin * 60; // user-set UTC offset (Settings > UTC)
        static DLine dl[160]; // single-threaded UI: static keeps it off the stack; sized so
                              // a full 32-message backlog rarely overflows (the unread anchor
                              // lives or dies with its lines staying in this ring)
        int dlCount = 0;
        int anchorLine = -1;       // dl index of the first unread message's first line (on open)
        bool anchorDropped = false; // its lines got evicted: land at the top of what's left
        int selMsgIdx = reactSel >= 0 ? matchedFromNewest(reactSel) : -1;
        int selFirst = -1, selLast = -1; // dl range of the react-selected message
        auto pushLine = [&]() -> DLine & {
            if (dlCount >= (int)(sizeof(dl) / sizeof(dl[0]))) { // drop the oldest line
                memmove(dl, dl + 1, sizeof(dl) - sizeof(dl[0]));
                dlCount--;
                if (anchorLine == 0)
                    anchorDropped = true; // the anchor's first line is the one being dropped
                if (anchorLine >= 0)
                    anchorLine--; // trackers shift up with the dropped line
                if (selFirst >= 0)
                    selFirst--;
                if (selLast >= 0)
                    selLast--;
            }
            return dl[dlCount++];
        };
        for (int i = 0; i < mc; i++) {
            Msg &m = g_msgs[matched[i]];
            bool out = (m.from == me);
            bool failed = out && m.status == MSG_FAILED;
            bool isSel = (selMsgIdx >= 0 && matched[i] == selMsgIdx);
            int wrapW = !out ? 232 : (failed ? 162 : 214); // reserve room for the marker
            char tpre[8];
            msgTimePrefix(m.rxTime, tzOff, tpre, sizeof(tpre)); // "HH:MM " before the arrow
            uint8_t tlen = (uint8_t)strlen(tpre);
            uint8_t nlen = 0;
            char full[200];
            if (isChan && !out) { // channels have many senders — show who wrote it
                char sn[8];
                shortNameOf(m.from, sn, sizeof(sn));
                nlen = (uint8_t)(strlen(sn) + 2);
                snprintf(full, sizeof(full), "%s%s: %s", tpre, sn, m.text);
            } else {
                snprintf(full, sizeof(full), "%s%s%s", tpre, out ? "> " : "< ", m.text);
            }
            if (g_msgReply[matched[i]]) { // a reply: quote the original above it, if still in the ring
                for (int q = 0; q < g_msgCount; q++)
                    if (g_msgs[q].id == g_msgReply[matched[i]] && g_msgs[q].id != 0) {
                        DLine &d = pushLine();
                        char snip[30];
                        utf8Copy(snip, g_msgs[q].text, (int)sizeof(snip) - 1);
                        snprintf(d.text, sizeof(d.text), "    | %s", snip);
                        d.color = 0x630C; // extra dim: context, not content
                        d.msgIdx = -1;
                        d.out = false;
                        d.status = 0;
                        d.err = 0;
                        d.timeLen = 0;
                        d.nameLen = 0;
                        d.last = false;
                        if (isSel) {
                            if (selFirst < 0)
                                selFirst = dlCount - 1;
                            selLast = dlCount - 1;
                        }
                        break;
                    }
            }
            if (matched[i] == chatAnchorMsgIdx)
                anchorLine = dlCount; // this message's first line
            const char *p = full;
            bool first = true;
            do {
                DLine &d = pushLine();
                p += wrapLine(g, p, wrapW, d.text, sizeof(d.text));
                d.color = out ? 0x07FF : 0xFFFF; // outgoing cyan, incoming white
                d.msgIdx = (int16_t)matched[i];
                d.out = out;
                d.status = m.status;
                d.err = m.err;
                d.timeLen = first ? tlen : 0; // time + name sit only on the first line
                d.nameLen = first ? nlen : 0;
                d.last = (*p == 0);
                if (isSel) {
                    if (selFirst < 0)
                        selFirst = dlCount - 1;
                    selLast = dlCount - 1;
                }
                first = false;
            } while (*p);
            if (m.id && g_reactCount) { // tapbacks attached to this message, one dim line
                char rl[38];
                int rp = 0;
                for (int r = 0; r < g_reactCount && rp < (int)sizeof(rl) - 14; r++) {
                    if (g_reacts[r].msgId != m.id)
                        continue;
                    char sn[8];
                    shortNameOf(g_reacts[r].from, sn, sizeof(sn));
                    rp += snprintf(rl + rp, sizeof(rl) - rp, "%s%s %s", rp ? "  " : "", g_reacts[r].label, sn);
                }
                if (rp > 0) {
                    DLine &d = pushLine();
                    snprintf(d.text, sizeof(d.text), "    %s", rl);
                    d.color = 0x8410; // dim: metadata, not a message
                    d.msgIdx = -1;
                    d.out = false;
                    d.status = 0;
                    d.err = 0;
                    d.timeLen = 0;
                    d.nameLen = 0;
                    d.last = false;
                    if (isSel)
                        selLast = dlCount - 1;
                }
            }
        }
        // chatScroll counts lines scrolled up from the bottom (0 = pinned to newest).
        int maxScroll = dlCount > maxLines ? dlCount - maxLines : 0;
        if (chatAnchorMsgIdx >= 0) { // first render after opening: jump to the first unread
            if (anchorLine >= 0 && anchorLine <= maxScroll)
                chatScroll = maxScroll - anchorLine;
            else if (anchorDropped)
                chatScroll = maxScroll; // unread starts beyond the ring: as far back as we can show
            else
                chatScroll = 0;
            chatAnchorMsgIdx = -1;
        }
        if (chatScroll > maxScroll)
            chatScroll = maxScroll;
        if (chatScroll < 0)
            chatScroll = 0;
        if (reactSel >= 0 && selFirst >= 0) { // keep the react-selected message in view
            int vis0 = maxScroll - chatScroll;
            if (selFirst < vis0)
                chatScroll = maxScroll - selFirst;
            else if (selLast >= vis0 + maxLines)
                chatScroll = maxScroll - (selLast - maxLines + 1);
            if (chatScroll > maxScroll)
                chatScroll = maxScroll;
            if (chatScroll < 0)
                chatScroll = 0;
        }
        int startL = maxScroll - chatScroll;
        int y = fy0;
        for (int i = startL; i < startL + maxLines && i < dlCount; i++) {
            DLine &d = dl[i];
            if (d.msgIdx >= 0 && !g_msgs[d.msgIdx].read) { // it's on screen now: that's "read"
                g_msgs[d.msgIdx].read = true;
                g_msgsDirty = true;
            }
            if (selFirst >= 0 && i >= selFirst && i <= selLast)
                g->fillRect(0, y - 1, 236, lh, 0x2945); // react-mode selection band
            int cx = 4;
            if (d.timeLen) { // dim the "HH:MM " prefix (ASCII, no emoji)
                g->setFont(&cyrFont);
                g->setTextSize(1);
                char save = d.text[d.timeLen];
                d.text[d.timeLen] = 0;
                g->setTextColor(0x8410); // gray
                g->setCursor(cx, y);
                g->print(d.text);
                cx += g->textWidth(d.text);
                d.text[d.timeLen] = save;
            }
            if (d.nameLen) { // sender-name span (channel threads), light blue
                char save = d.text[d.timeLen + d.nameLen];
                d.text[d.timeLen + d.nameLen] = 0;
                cx = printLineEmotes(g, cx, y, d.text + d.timeLen, 0x9CD3, 2); // emote-aware: short names can be emoji
                d.text[d.timeLen + d.nameLen] = save;
            }
            printLineEmotes(g, cx, y, d.text + d.timeLen + d.nameLen, d.color, 2); // inline emoji, nudged onto the text band
            if (d.last && d.out) {
                if (d.status == MSG_FAILED) {
                    g->setFont(&lgfx::fonts::Font0); // red error name at the right
                    g->setTextSize(1);
                    g->setTextColor(0xF800);
                    const char *en = errName(d.err);
                    g->setCursor(238 - g->textWidth(en), y + 4);
                    g->print(en);
                } else {
                    drawMsgStatus(g, 220, y, d.status);
                }
            }
            y += lh;
        }
        // scrollbar on the right edge when the thread overflows the view
        if (maxScroll > 0) {
            int trackY = fy0, trackH = maxLines * lh;
            g->drawFastVLine(238, trackY, trackH, 0x2104); // dark track
            int thumbH = trackH * maxLines / dlCount;
            if (thumbH < 6)
                thumbH = 6;
            int thumbY = trackY + (trackH - thumbH) * startL / maxScroll;
            g->drawFastVLine(238, thumbY, thumbH, 0x07FF); // cyan thumb
            g->drawFastVLine(239, thumbY, thumbH, 0x07FF);
        }
    }

    if (mode == MODE_COMPOSE) {
        if (pendingReplyId) { // replying: show what we're quoting above the input
            g->fillRect(0, 97, 240, 16, 0x0000);
            g->setFont(&cyrFont);
            g->setTextSize(1);
            char qb[44];
            snprintf(qb, sizeof(qb), "| %s", replyPrev);
            printLineEmotes(g, 4, 99, qb, 0x630C);
        }
        g->drawFastHLine(0, 114, 240, 0x39C7);
        g->setFont(&cyrFont);
        g->setTextSize(1);
        char cbuf[210];
        snprintf(cbuf, sizeof(cbuf), "%s_", msgBuf);
        const char *shown = cbuf; // drop leading tokens so the cursor stays visible
        while (lineWidthEmotes(g, shown) > 212) { // leave room for the RU/EN badge
            const graphics::Emote *em = nullptr;
            int el = emoteMatch(shown, &em);
            shown += el > 0 ? el : utf8Len((unsigned char)*shown);
        }
        printLineEmotes(g, 4, 118, shown, 0xFFE0); // yellow, emoji inline
        g->fillRect(216, 116, 24, 13, 0x0000); // input-mode badge (Fn+L toggles)
        g->setFont(&lgfx::fonts::Font0);
        g->setTextSize(1);
        g->setTextColor(g_ruMode ? 0x07FF : 0x630C);
        g->setCursor(220, 119);
        g->print(g_ruMode ? "RU" : "EN");
    } else {
        if (reactStrip) { // quick-reaction strip over the bottom of the feed
            const int sy = 88;
            g->fillRect(8, sy, 224, 26, 0x0000);
            g->drawRect(8, sy, 224, 26, 0x07FF);
            for (int i = 0; i < 6; i++) {
                int x = 20 + i * 35;
                if (i == reactPick)
                    g->fillRect(x - 5, sy + 3, 26, 20, 0x2945);
                const graphics::Emote *em = nullptr;
                if (emoteMatch(kQuickReacts[i], &em) > 0 && em)
                    g->drawXBitmap(x, sy + 5, em->bitmap, em->width, em->height, 0xFFFF);
            }
        }
        const char *hint;
        if (reactStrip) {
            hint = "</> pick  ENTER send  ESC";
        } else if (reactSel >= 0) {
            int smi = matchedFromNewest(reactSel); // old history has no packet id to reference
            hint = (smi >= 0 && g_msgs[smi].id == 0) ? "old msg: no id  up/dn  ESC"
                   : pickReply                       ? "up/dn msg  ENTER reply  ESC"
                                                     : "up/dn msg  ENTER react  ESC";
        } else {
            hint = hasFailed ? "ENTER resend  <reply  >react" : "type  Tab emoji  <reply  >react";
        }
        drawFooter(g, hint);
        char bb[10];
        uint16_t bc;
        batteryText(bb, sizeof(bb), bc); // own battery at the right of the footer
        g->setTextColor(bc);
        g->setCursor(238 - g->textWidth(bb), 126);
        g->print(bb);
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
    if (editTarget >= 20) { // WiFi / MQTT text field -> config/moduleConfig; saved on net-page exit
        netSetText(editTarget, nameBuf);
        netDirty = true;
        return false;
    }
    if (editTarget == 2) { // frequency (MHz) -> override_frequency; radio restart to apply
        if (g_radioCompanion) { // remote admin: the node saves and reboots itself
            if (!g_compLoraValid)
                return false;
            meshtastic_AdminMessage adm = meshtastic_AdminMessage_init_default;
            adm.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
            adm.set_config.which_payload_variant = meshtastic_Config_lora_tag;
            adm.set_config.payload_variant.lora = g_compLora;
            adm.set_config.payload_variant.lora.override_frequency = strtof(nameBuf, nullptr);
            if (!sendAdminToNode(adm))
                return false; // link down: just fall back to Settings
            mode = MODE_BLELINK; // watch the node reboot + the link come back
            return true;
        }
        config.lora.override_frequency = strtof(nameBuf, nullptr);
        if (nodeDB)
            nodeDB->saveToDisk(SEGMENT_CONFIG);
        rebootAtMsec = millis() + 1500;
        mode = MODE_REBOOT;
        return true;
    }
    if (editTarget == 3) { // primary channel name; radio restart to apply
        if (g_radioCompanion) { // remote rename: round-trip the synced channel object,
                                // PSK included, so set_channel can't wipe the key
            meshtastic_AdminMessage adm = meshtastic_AdminMessage_init_default;
            adm.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
            adm.set_channel = g_compChans[0];
            strncpy(adm.set_channel.settings.name, nameBuf, sizeof(adm.set_channel.settings.name));
            adm.set_channel.settings.name[sizeof(adm.set_channel.settings.name) - 1] = 0;
            if (sendAdminToNode(adm)) // applied live on the node (no reboot); mirror it
                g_compChans[0] = adm.set_channel;
            return false;
        }
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
    if (g_radioCompanion) { // rename the linked node via set_owner, like the phone
        meshtastic_AdminMessage adm = meshtastic_AdminMessage_init_default;
        adm.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
        meshtastic_User &u = adm.set_owner;
        for (int i = 0; i < g_compNodeCount; i++) // start from the node's current names
            if (g_compNodes[i].num == g_linkMyNode) {
                snprintf(u.long_name, sizeof(u.long_name), "%s", g_compNodes[i].longName);
                snprintf(u.short_name, sizeof(u.short_name), "%s", g_compNodes[i].shortName);
                if (editTarget == 1)
                    snprintf(u.short_name, sizeof(u.short_name), "%s", nameBuf);
                else
                    snprintf(u.long_name, sizeof(u.long_name), "%s", nameBuf);
                if (sendAdminToNode(adm)) { // mirror locally so Settings shows it at once
                    snprintf(g_compNodes[i].longName, sizeof(g_compNodes[i].longName), "%s", u.long_name);
                    snprintf(g_compNodes[i].shortName, sizeof(g_compNodes[i].shortName), "%s", u.short_name);
                }
                break;
            }
        return false;
    }
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
    g->printf("Settings%s", setSection == 0   ? " / Node"
                            : setSection == 1 ? " / LoRa"
                            : setSection == 2 ? " / Device"
                                              : "");
    g->drawFastHLine(0, 13, 240, 0x39C7);

    const char *labels[kNumSettings] = {"Name", "Short",       "Region", "Preset", "Frequency",
                                        "Channel", "Role",     "Hops",   "Power",  "Rebroadcast",
                                        "UTC",     "WiFi",     "MQTT",   "Screen", "Radio",
                                        "Font"};
    char vals[kNumSettings][24];
    if (g_radioCompanion) { // rows 0-5 show (and remote-admin edit) the linked node
        meshtastic_NodeInfoLite *me = g_linkMyNode ? nodeByNum(g_linkMyNode) : nullptr;
        snprintf(vals[0], sizeof(vals[0]), "%s", me && me->long_name[0] ? me->long_name : "?");
        snprintf(vals[1], sizeof(vals[1]), "%s", me && me->short_name[0] ? me->short_name : "?");
        snprintf(vals[2], sizeof(vals[2]), "%s", g_compLoraValid ? regionName(g_compLora.region) : "?");
        if (g_compLoraValid)
            snprintf(vals[3], sizeof(vals[3]), "%s", g_compLora.use_preset ? presetName(g_compLora.modem_preset) : "custom");
        else
            strcpy(vals[3], "?");
        if (g_compLoraValid && g_compLora.override_frequency > 0)
            snprintf(vals[4], sizeof(vals[4]), "%.3f", (double)g_compLora.override_frequency);
        else
            strcpy(vals[4], g_compLoraValid ? "auto" : "?");
        snprintf(vals[5], sizeof(vals[5]), "%s", chanName(0));
    } else {
        snprintf(vals[0], sizeof(vals[0]), "%s", owner.long_name[0] ? owner.long_name : "(unset)");
        snprintf(vals[1], sizeof(vals[1]), "%s", owner.short_name[0] ? owner.short_name : "(unset)");
        snprintf(vals[2], sizeof(vals[2]), "%s", regionName(config.lora.region));
        snprintf(vals[3], sizeof(vals[3]), "%s", config.lora.use_preset ? presetName(config.lora.modem_preset) : "custom");
        if (config.lora.override_frequency > 0)
            snprintf(vals[4], sizeof(vals[4]), "%.3f", (double)config.lora.override_frequency);
        else
            strcpy(vals[4], "auto");
        snprintf(vals[5], sizeof(vals[5]), "%s", channels.getName(0));
    }
    if (g_radioCompanion) { // LoRa/device rows mirror the linked node too
        snprintf(vals[6], sizeof(vals[6]), "%s",
                 g_compDeviceValid ? optName(kRoleOpts2, kRoleCount, (int)g_compDevice.role) : "?");
        if (g_compLoraValid)
            snprintf(vals[7], sizeof(vals[7]), "%u", (unsigned)g_compLora.hop_limit);
        else
            strcpy(vals[7], "?");
        if (g_compLoraValid)
            snprintf(vals[8], sizeof(vals[8]), "%s", g_compLora.tx_power ? optName(kPowerOpts, kPowerCount, g_compLora.tx_power) : "max");
        else
            strcpy(vals[8], "?");
        snprintf(vals[9], sizeof(vals[9]), "%s",
                 g_compDeviceValid ? optName(kRebroadOpts, kRebroadCount, (int)g_compDevice.rebroadcast_mode) : "?");
    } else {
        snprintf(vals[6], sizeof(vals[6]), "%s", optName(kRoleOpts2, kRoleCount, (int)config.device.role));
        snprintf(vals[7], sizeof(vals[7]), "%u", (unsigned)config.lora.hop_limit);
        snprintf(vals[8], sizeof(vals[8]), "%s", config.lora.tx_power ? optName(kPowerOpts, kPowerCount, config.lora.tx_power) : "max");
        snprintf(vals[9], sizeof(vals[9]), "%s", optName(kRebroadOpts, kRebroadCount, (int)config.device.rebroadcast_mode));
    }
    {
        int om = g_utcOffsetMin, ah = om < 0 ? -om : om;
        if (ah % 60)
            snprintf(vals[10], sizeof(vals[10]), "UTC%c%d:%02d", om < 0 ? '-' : '+', ah / 60, ah % 60);
        else
            snprintf(vals[10], sizeof(vals[10]), "UTC%c%d", om < 0 ? '-' : '+', ah / 60);
    }
    if (config.network.wifi_enabled)
        snprintf(vals[11], sizeof(vals[11]), "%s", config.network.wifi_ssid[0] ? config.network.wifi_ssid : "on");
    else
        strcpy(vals[11], "off");
    strcpy(vals[12], moduleConfig.mqtt.enabled ? "on" : "off");
    strcpy(vals[13], "never");
    for (int i = 0; i < kScreenCount; i++)
        if (kScreenOpts[i].value == (int)g_screenOffSec)
            strcpy(vals[13], kScreenOpts[i].name);
    if (g_radioCompanion)
        snprintf(vals[14], sizeof(vals[14]), "BLE: %s", g_peerName[0] ? g_peerName : "(no node)");
    else
        strcpy(vals[14], "onboard");
    snprintf(vals[15], sizeof(vals[15]), "%s", sdFontState()); // unicode font source (read-only)

    // What the current level shows: top = sections + direct entries (with a
    // representative value as the preview), sub = the section's flat items.
    const char *rowLabel[kNumSettings];
    const char *rowVal[kNumSettings];
    int listCount;
    if (setSection < 0) {
        static const char *topNames[kTopCount] = {"Node", "LoRa", "WiFi", "MQTT", "Device", "Radio"};
        static const uint8_t topPreview[kTopCount] = {0, 3, 11, 12, 10, 14};
        listCount = kTopCount;
        for (int i = 0; i < kTopCount; i++) {
            rowLabel[i] = topNames[i];
            rowVal[i] = vals[topPreview[i]];
        }
    } else {
        const uint8_t *items = setSection == 0 ? kSecNode : setSection == 1 ? kSecLora : kSecDevice;
        listCount = setSection == 0 ? (int)sizeof(kSecNode) : setSection == 1 ? (int)sizeof(kSecLora) : (int)sizeof(kSecDevice);
        for (int i = 0; i < listCount; i++) {
            rowLabel[i] = labels[items[i]];
            rowVal[i] = vals[items[i]];
        }
    }

    const int rowH = 15, top = 15, maxRows = (124 - top) / rowH;
    if (setSel < setScroll)
        setScroll = setSel;
    if (setSel >= setScroll + maxRows)
        setScroll = setSel - maxRows + 1;
    if (setScroll < 0)
        setScroll = 0;
    for (int r = 0; r < maxRows; r++) {
        int i = setScroll + r;
        if (i >= listCount)
            break;
        int y = top + r * rowH;
        if (i == setSel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945); // selection highlight

        g->setFont(&lgfx::fonts::FreeSansBold9pt7b); // same size as the contact list
        g->setTextSize(1);
        g->setTextColor(0xFFFF);
        g->setCursor(6, y + 1);
        g->print(rowLabel[i]);
        int lw = g->textWidth(rowLabel[i]);

        char vbuf[24];
        snprintf(vbuf, sizeof(vbuf), "%s", rowVal[i]);
        fitWidth(g, vbuf, 230 - (6 + lw));
        g->setTextColor(0x9CD3);
        g->setCursor(236 - g->textWidth(vbuf), y + 1);
        g->print(vbuf);
    }

    drawFooter(g, setSection < 0 ? "up/dn   ENTER open   ESC back" : "up/dn   ENTER edit   ESC sections");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// WiFi / MQTT sub-settings page (booleans toggle on Enter, text via the name editor).
void AdvUI::drawNetPage()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF);
    g->setCursor(4, 3);
    g->print(netPage == 0 ? "WiFi" : "MQTT");
    g->drawFastHLine(0, 13, 240, 0x39C7);

    const NetField *f = netFields(netPage);
    int cnt = netFieldCount(netPage);
    const int rowH = 15, top = 15, maxRows = (124 - top) / rowH;
    if (netSel < netScroll)
        netScroll = netSel;
    if (netSel >= netScroll + maxRows)
        netScroll = netSel - maxRows + 1;
    if (netScroll < 0)
        netScroll = 0;

    for (int r = 0; r < maxRows; r++) {
        int i = netScroll + r;
        if (i >= cnt)
            break;
        int y = top + r * rowH;
        if (i == netSel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945);
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextSize(1);
        g->setTextColor(0xFFFF);
        g->setCursor(6, y + 1);
        g->print(f[i].label);
        int lw = g->textWidth(f[i].label);

        char v[26];
        bool on = false;
        if (f[i].isBool) {
            on = netGetBool(netPage, i);
            strcpy(v, on ? "on" : "off");
        } else {
            const char *t = netGetText(netPage, i);
            bool pass = (netPage == 0 && i == 2) || (netPage == 1 && i == 3);
            if (!t[0])
                strcpy(v, (netPage == 1 && i == 1) ? "(default)" : "(unset)");
            else if (pass) {
                int n = (int)strlen(t);
                if (n > 20)
                    n = 20;
                memset(v, '*', n);
                v[n] = 0;
            } else {
                strncpy(v, t, sizeof(v));
                v[sizeof(v) - 1] = 0;
            }
        }
        g->setTextColor(on ? 0x07E0 : 0x9CD3); // green when a toggle is on
        fitWidth(g, v, 230 - (6 + lw));
        g->setCursor(236 - g->textWidth(v), y + 1);
        g->print(v);
    }

    drawFooter(g, netDirty ? "ENTER edit   ESC save+reboot" : "ENTER edit   ESC back");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// Companion: scan for Meshtastic nodes over BLE and pick the radio to pair with.
void AdvUI::drawBleScan()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF);
    g->setCursor(4, 3);
    g->print("Find node");
    g->setTextColor(g_scanning ? 0xFFE0 : 0x8410);
    char st[20];
    if (g_scanning)
        strcpy(st, "scanning...");
    else
        snprintf(st, sizeof(st), "%d found", g_scanCount);
    g->setCursor(238 - g->textWidth(st), 3);
    g->print(st);
    g->drawFastHLine(0, 13, 240, 0x39C7);

    if (bleSel >= g_scanCount)
        bleSel = g_scanCount ? g_scanCount - 1 : 0;
    const int rowH = 18, top = 15;
    if (g_scanCount == 0) {
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextColor(0x630C);
        g->setCursor(6, 50);
        g->print(g_bleUnsupported ? "Companion image needed" : g_scanning ? "Listening..." : "No nodes found");
        g->setFont(&lgfx::fonts::Font0);
        g->setTextColor(0x8410);
        g->setCursor(6, 70);
        g->print(g_bleUnsupported ? "flash the companion firmware from the installer"
                                  : "node powered + BT on, phone app closed");
    }
    for (int i = 0; i < g_scanCount && i < 5; i++) {
        int y = top + i * rowH;
        if (i == bleSel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945);
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextSize(1);
        char nm[24];
        snprintf(nm, sizeof(nm), "%s", g_scanHits[i].name[0] ? g_scanHits[i].name : g_scanHits[i].addr);
        fitWidth(g, nm, 180);
        bool saved = g_peerAddr[0] && !strcmp(g_peerAddr, g_scanHits[i].addr);
        g->setTextColor(saved ? 0xFFE0 : 0xFFFF); // the saved peer shows yellow
        g->setCursor(6, y + 1);
        g->print(nm);
        char rb[10];
        snprintf(rb, sizeof(rb), "%ddB", g_scanHits[i].rssi);
        g->setFont(&lgfx::fonts::Font0);
        g->setTextColor(g_scanHits[i].rssi > -70 ? 0x07E0 : 0x8410);
        g->setCursor(236 - g->textWidth(rb), y + 5);
        g->print(rb);
    }

    if (bleSavedMs && millis() - bleSavedMs < 4000) {
        g->setFont(&lgfx::fonts::Font0);
        g->setTextColor(0x07E0);
        g->setCursor(4, 108);
        g->printf("saved: %s  (link lands in A3)", g_peerName);
    }
    drawFooter(g, "up/dn  ENTER connect  R rescan  ESC");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// Companion: the node is showing a pairing PIN on its screen — type it here.
void AdvUI::drawBlePin()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF);
    g->setCursor(4, 3);
    g->printf("Pairing with %s", g_peerName);
    g->drawFastHLine(0, 13, 240, 0x39C7);

    g->setTextColor(0x8410);
    g->setCursor(24, 34);
    g->print("enter the PIN shown on the node");

    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(2);
    g->setTextColor(0xFFFF);
    char shown[16];
    snprintf(shown, sizeof(shown), "%s_", pinBuf);
    g->setCursor((240 - g->textWidth(shown)) / 2, 58);
    g->print(shown);
    g->setTextSize(1);

    g->setFont(&lgfx::fonts::Font0);
    g->setTextColor(0x630C);
    g->setCursor(30, 96);
    g->print("node has no screen? try 123456");

    drawFooter(g, "digits  ENTER ok  ESC cancel");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// Local mode: a phone is pairing with US — show the passkey it must enter (the
// stock screen that would display it is compiled out).
void AdvUI::drawBtPin()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF);
    g->setCursor(4, 3);
    g->print("Bluetooth pairing");
    g->drawFastHLine(0, 13, 240, 0x39C7);

    g->setTextColor(0x8410);
    g->setCursor(30, 30);
    g->print("enter this code on the phone");

    char pin[16] = "";
    if (bluetoothStatus->getConnectionState() == meshtastic::BluetoothStatus::ConnectionState::PAIRING)
        snprintf(pin, sizeof(pin), "%s", bluetoothStatus->getPasskey().c_str());
    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(2);
    g->setTextColor(0xFFFF);
    g->setCursor((240 - g->textWidth(pin)) / 2, 56);
    g->print(pin);
    g->setTextSize(1);

    drawFooter(g, "ESC hide");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// Companion: link progress/status against the saved node.
void AdvUI::drawBleLink()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF);
    g->setCursor(4, 3);
    g->print("Companion link");
    g->drawFastHLine(0, 13, 240, 0x39C7);

    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(1);
    g->setTextColor(0xFFFF);
    g->setCursor(6, 24);
    char nm[26];
    snprintf(nm, sizeof(nm), "%s", g_peerName[0] ? g_peerName : g_peerAddr);
    fitWidth(g, nm, 228);
    g->print(nm);

    const char *st;
    uint16_t sc;
    switch (g_linkState) {
    case BLE_CONNECTING: st = "connecting..."; sc = 0xFFE0; break;
    case BLE_PAIRING:    st = "pairing: PIN on the node"; sc = 0xFFE0; break;
    case BLE_CONNECTED:  st = "connected"; sc = 0x07E0; break;
    case BLE_FAILED:     st = g_linkErr[0] ? g_linkErr : "failed"; sc = 0xF800; break;
    default:             st = "idle"; sc = 0x8410; break;
    }
    g->setCursor(6, 48);
    g->setTextColor(sc);
    g->print(st);

    g->setFont(&lgfx::fonts::Font0);
    g->setTextColor(0x9CD3);
    g->setCursor(6, 72);
    g->printf("packets rx: %u", (unsigned)g_linkRxPkts);
    if (g_linkMyNode) {
        g->setCursor(6, 84);
        g->printf("node id: !%08x", (unsigned)g_linkMyNode);
    }
    if (g_linkRssi) {
        g->setCursor(130, 72);
        g->printf("link: %ddB", (int)g_linkRssi);
    }
    if (g_linkNodeBatt >= 0) {
        g->setCursor(130, 84);
        g->printf("node batt: %d%%", (int)g_linkNodeBatt);
    }
    if (g_linkState == BLE_CONNECTED && g_linkConfigDone) {
        g->setTextColor(0x07E0);
        g->setCursor(6, 102);
        g->printf("link OK - %d nodes synced", (int)g_compNodeCount);
    }

    drawFooter(g, g_linkState == BLE_FAILED ? "auto-retry  R now  F forget  ESC"
                                            : "R reconnect  F forget  ESC scan");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::drawPickList()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    PickList pl = pickListFor(pickTarget);
    const EnumOpt *opts = pl.opts;
    int cnt = pl.cnt;

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    g->print(pl.title);
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

// Emoji palette: a grid of bitmaps; Enter inserts the picked emoji into the message.
void AdvUI::drawEmoji()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    g->print("Emoji");
    g->drawFastHLine(0, 13, 240, 0x39C7);

    const int cols = 6, cellW = 40, cellH = 26, top = 16;
    for (int i = 0; i < kEmojiCount; i++) {
        int x = (i % cols) * cellW, y = top + (i / cols) * cellH;
        if (i == emojiSel)
            g->fillRect(x + 1, y, cellW - 2, cellH, 0x2945); // selection highlight
        const graphics::Emote *em = nullptr;
        if (emoteMatch(kEmojiPalette[i], &em) > 0 && em)
            g->drawXBitmap(x + (cellW - em->width) / 2, y + (cellH - em->height) / 2, em->bitmap, em->width, em->height,
                           0xFFFF);
    }
    drawFooter(g, "arrows  ENTER insert  ESC back");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

#ifdef ADVUI_SCREENSHOT
// Dumps the current canvas (rgb332) as hex over serial, framed by markers, so a host
// script can rebuild a PNG. Dev-only: gated out of the release build.
void AdvUI::screenshot(const char *name)
{
    if (!haveCanvas)
        return;
    Serial.setTxTimeoutMs(1000); // blocking TX: the USB-CDC guard (0) drops bytes on big writes
    const uint8_t *buf = (const uint8_t *)canvas.getBuffer();
    int w = display.width(), h = display.height();
    int stride = (int)(canvas.bufferLength() / h);
    Serial.printf("\n@@SHOT %s %d %d\n", name, w, h);
    static const char hexd[] = "0123456789abcdef";
    char line[512];
    for (int y = 0; y < h; y++) {
        int p = 0;
        for (int x = 0; x < w; x++) {
            uint8_t v = buf[y * stride + x];
            line[p++] = hexd[v >> 4];
            line[p++] = hexd[v & 0xF];
        }
        line[p] = 0;
        Serial.println(line);
        Serial.flush(); // one row at a time so nothing is dropped
    }
    Serial.println("@@END");
    Serial.flush();
    Serial.setTxTimeoutMs(0); // restore the USB-CDC guard
    delay(30);
}

// Renders each screen with sample data and dumps it, then restores all state in RAM
// (nothing is persisted — saveMsgs is never called, and the message ring is snapshotted
// and put back). No reboot, so the USB CDC stays up for the host to read.
void AdvUI::runDemoDump()
{
    uint32_t me = myNodeNum();

    static Msg backup[kMaxMsgs];
    memcpy(backup, g_msgs, sizeof(g_msgs));
    static Reaction rBackup[kMaxReacts];
    memcpy(rBackup, g_reacts, sizeof(g_reacts));
    int brC = g_reactCount, brN = g_reactNext;
    int bCount = g_msgCount;
    bool bDirty = g_msgsDirty;
    Mode bMode = mode;
    uint32_t bSel = selectedNum;
    int bChan = selectedChannel, bScroll = chatScroll;

    drawSplash();
    screenshot("splash");

    mode = MODE_NODES;
    sel = 0;
    scrollTop = 0;
    rebuildFiltered();
    drawNodeList();
    screenshot("nodes");

    // Controlled demo history: clear the real ring (restored from backup on exit) so
    // screenshots show only our sample data, not real (possibly non-English) chats.
    g_msgCount = 0;
    uint32_t peer = (filteredCount > 0 && nodeDB) ? nodeDB->getMeshNodeByIndex(filtered[0])->num : me;
    uint32_t other = (filteredCount > 1 && nodeDB) ? nodeDB->getMeshNodeByIndex(filtered[1])->num : peer + 1;
    uint32_t t = 1751720400;
    // a channel conversation from a different node, left unread -> envelope in the chats list
    addMsg(other, NODENUM_BROADCAST, 0, t - 200, true, "Anyone up for a mesh test? \U0001F440", 0, MSG_IN);
    // a DM: mostly English with one Cyrillic line to showcase non-Latin text
    addMsg(peer, me, 0, t, false, "Hey, you on the mesh?", 0, MSG_IN);
    addMsg(me, peer, 0, t + 60, false, "Yep, 5/5 \U0001F44D", 1, MSG_DELIVERED);
    addMsg(peer, me, 0, t + 120, false, "Отлично, погнали \U0001F525", 3, MSG_IN); // Cyrillic showcase
    addMsg(me, peer, 0, t + 180, false, "On my way \U0001F642", 2, MSG_SENDING);
    g_reactCount = 0;
    g_reactNext = 0;
    addReaction(1, peer, "\U0001F44D"); // their tapback on our "Yep, 5/5"
    selectedChannel = -1;
    selectedNum = peer;
    chatScroll = 0;
    mode = MODE_NODE;
    drawNode();
    screenshot("chat");

    reactSel = 1; // react mode: strip open on the Cyrillic message
    reactStrip = true;
    reactPick = 0;
    drawNode();
    screenshot("react");
    reactSel = -1;
    reactStrip = false;
    pendingReplyId = 0;

    sel = 0;
    scrollTop = 0;
    mode = MODE_CHATS;
    drawChats();
    screenshot("chats");

    emojiSel = 0;
    mode = MODE_EMOJI;
    drawEmoji();
    screenshot("emoji");

    setSel = 0;
    setSection = -1; // the sectioned top menu (Node / LoRa / WiFi / ...)
    mode = MODE_SETTINGS;
    drawSettings();
    screenshot("settings");

    setSection = 1; // the LoRa section expanded (region, preset, role, hops, power...)
    setSel = 0;
    setScroll = 0;
    drawSettings();
    screenshot("lora");
    setSection = -1;

    pickTarget = 2;
    pickSel = optIndex(kUtcOpts, kUtcCount, 180); // Moscow, for a nice sample
    pickScroll = pickSel > 3 ? pickSel - 3 : 0;
    mode = MODE_PICKLIST;
    drawPickList();
    screenshot("utc");

    // WiFi / MQTT pages with sample values (restored right after — never saved)
    meshtastic_Config_NetworkConfig bnet = config.network;
    meshtastic_ModuleConfig_MQTTConfig bmqtt = moduleConfig.mqtt;
    config.network.wifi_enabled = true;
    strcpy(config.network.wifi_ssid, "HomeWiFi");
    strcpy(config.network.wifi_psk, "s3cret-pass");
    moduleConfig.mqtt.enabled = true;
    moduleConfig.mqtt.address[0] = 0;
    moduleConfig.mqtt.encryption_enabled = true;
    strcpy(moduleConfig.mqtt.root, "msh");
    netSel = 0;
    netScroll = 0;
    netPage = 0;
    drawNetPage();
    screenshot("wifi");
    netPage = 1;
    drawNetPage();
    screenshot("mqtt");
    config.network = bnet;
    moduleConfig.mqtt = bmqtt;

    // Unicode showcase: several scripts in one thread (needs the font partition/SD)
    g_msgCount = 0;
    g_reactCount = 0;
    g_reactNext = 0;
    addMsg(peer, me, 0, t - 200, false, "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x81", 0, MSG_IN);         // 你好！ (Chinese)
    addMsg(peer, me, 0, t - 160, false, "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf", 0, MSG_IN); // こんにちは (Japanese)
    addMsg(me, peer, 0, t - 120, false, "\xce\x93\xce\xb5\xce\xb9\xce\xb1! \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", 0, MSG_DELIVERED); // Γεια! שלום
    addMsg(peer, me, 0, t - 60, false, "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 \xf0\x9f\x91\x8d", 0, MSG_IN); // 한국어 👍 (Korean + emoji)
    selectedChannel = -1;
    selectedNum = peer;
    chatScroll = 0;
    mode = MODE_NODE;
    drawNode();
    screenshot("unicode");

    // Companion screens (sample link state; these globals only matter in companion mode)
    g_scanCount = 3;
    g_scanning = false;
    snprintf(g_scanHits[0].name, sizeof(g_scanHits[0].name), "Heltec-V3 mesh");
    snprintf(g_scanHits[0].addr, sizeof(g_scanHits[0].addr), "a1:b2:c3:d4:e5:f6");
    g_scanHits[0].rssi = -52;
    snprintf(g_scanHits[1].name, sizeof(g_scanHits[1].name), "T-Beam Garden");
    snprintf(g_scanHits[1].addr, sizeof(g_scanHits[1].addr), "11:22:33:44:55:66");
    g_scanHits[1].rssi = -74;
    snprintf(g_scanHits[2].name, sizeof(g_scanHits[2].name), "RAK-Roof");
    snprintf(g_scanHits[2].addr, sizeof(g_scanHits[2].addr), "aa:bb:cc:dd:ee:ff");
    g_scanHits[2].rssi = -81;
    bleSel = 0;
    mode = MODE_BLESCAN;
    drawBleScan();
    screenshot("bscan");

    snprintf(g_peerName, sizeof(g_peerName), "Heltec-V3 mesh");
    snprintf(pinBuf, sizeof(pinBuf), "6579");
    pinLen = 4;
    mode = MODE_BLEPIN;
    drawBlePin();
    screenshot("bpin");

    g_linkState = BLE_CONNECTED;
    g_linkConfigDone = true;
    g_linkRxPkts = 214;
    g_linkMyNode = 0x8fa02080;
    g_linkRssi = -58;
    g_linkNodeBatt = 87;
    g_compNodeCount = 42;
    mode = MODE_BLELINK;
    drawBleLink();
    screenshot("blink");
    g_linkState = BLE_IDLE;
    g_linkConfigDone = false;
    g_scanCount = 0;
    g_peerName[0] = 0;
    pinLen = 0;
    pinBuf[0] = 0;

    // --- Usage-scenario frames (a01..) for the animated hero: pick the chat,
    // type, send, get the delivery check, receive the reply, scroll the history.
    bool bRu = g_ruMode;
    g_ruMode = false;
    g_msgCount = 0;
    g_reactCount = 0;
    g_reactNext = 0;
    addMsg(peer, me, 0, t - 900, false, "Radio check?", 11, MSG_IN);
    addMsg(me, peer, 0, t - 840, false, "Loud and clear \U0001F44D", 12, MSG_DELIVERED);
    addMsg(peer, me, 0, t - 300, false, "Bridge at seven?", 13, MSG_IN);
    addMsg(me, peer, 0, t - 240, false, "Deal", 14, MSG_DELIVERED);
    addMsg(peer, me, 0, t, true, "Ты где? \U0001F440", 15, MSG_IN); // unread -> the envelope on home

    sel = 0;
    scrollTop = 0;
    mode = MODE_CHATS;
    drawChats();
    screenshot("a01"); // home: the DM on top with an unread envelope

    selectedChannel = -1;
    selectedNum = peer;
    chatAnchorMsgIdx = firstUnreadIdx();
    chatScroll = 0;
    mode = MODE_NODE;
    drawNode();
    screenshot("a02"); // opened at the first unread

    mode = MODE_COMPOSE;
    const char *typing[] = {"O", "On m", "On my w", "On my way", "On my way \U0001F642"};
    for (unsigned i = 0; i < sizeof(typing) / sizeof(typing[0]); i++) {
        snprintf(msgBuf, sizeof(msgBuf), "%s", typing[i]);
        msgLen = (uint8_t)strlen(msgBuf);
        drawNode();
        char nm[8];
        snprintf(nm, sizeof(nm), "a%02u", 3 + i);
        screenshot(nm); // a03..a07: the reply being typed
    }
    msgBuf[0] = 0;
    msgLen = 0;
    addMsg(me, peer, 0, t + 60, false, "On my way \U0001F642", 16, MSG_SENDING);
    mode = MODE_NODE;
    chatScroll = 0;
    drawNode();
    screenshot("a08"); // sent: the pending dot
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].id == 16)
            g_msgs[i].status = MSG_DELIVERED; // the routing ACK lands
    drawNode();
    screenshot("a09"); // the green check
    addMsg(peer, me, 0, t + 120, false, "Same, see you there \U0001F44D", 17, MSG_IN);
    drawNode();
    screenshot("a10"); // their reply arrives
    chatScroll = 4;
    drawNode();
    screenshot("a11"); // scrolling back through the history
    chatScroll = 8;
    drawNode();
    screenshot("a12");
    chatScroll = 0;
    drawNode();
    screenshot("a13"); // back at the newest message
    g_ruMode = bRu;

    Serial.println("@@DONE");
    Serial.flush();

    memcpy(g_msgs, backup, sizeof(g_msgs)); // put the real conversation ring back
    memcpy(g_reacts, rBackup, sizeof(g_reacts));
    g_reactCount = brC;
    g_reactNext = brN;
    g_msgCount = bCount;
    g_msgsDirty = bDirty;
    mode = bMode;
    selectedNum = bSel;
    selectedChannel = bChan;
    chatScroll = bScroll;
    rebuildFiltered();
    while (Serial.available())
        Serial.read(); // drop any buffered triggers so we don't dump again in a burst
}
#endif

// Applies a radio-config pick (by pickTarget: 0 region, 1 preset, 5 role,
// 6 hop limit, 7 TX power, 8 rebroadcast mode), persists it, and schedules the
// reboot the radio needs. In companion mode it goes to the linked node as a
// set_config admin message instead — the node saves and reboots itself.
void AdvUI::applyLoRa(int target, int value)
{
    bool devCfg = target == 5 || target == 8; // role / rebroadcast live in DeviceConfig

    if (g_radioCompanion) {
        if (devCfg ? !g_compDeviceValid : !g_compLoraValid)
            return;
        meshtastic_AdminMessage adm = meshtastic_AdminMessage_init_default;
        adm.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        if (devCfg) {
            adm.set_config.which_payload_variant = meshtastic_Config_device_tag;
            meshtastic_Config_DeviceConfig &dc = adm.set_config.payload_variant.device;
            dc = g_compDevice;
            if (target == 5)
                dc.role = (meshtastic_Config_DeviceConfig_Role)value;
            else
                dc.rebroadcast_mode = (meshtastic_Config_DeviceConfig_RebroadcastMode)value;
            if (sendAdminToNode(adm)) {
                g_compDevice = dc; // optimistically ours; the reconnect config stream confirms
                mode = MODE_BLELINK;
            }
            return;
        }
        adm.set_config.which_payload_variant = meshtastic_Config_lora_tag;
        meshtastic_Config_LoRaConfig &lc = adm.set_config.payload_variant.lora;
        lc = g_compLora;
        switch (target) {
        case 0: lc.region = (meshtastic_Config_LoRaConfig_RegionCode)value; break;
        case 6: lc.hop_limit = (uint32_t)value; break;
        case 7: lc.tx_power = value; break;
        default:
            lc.use_preset = true;
            lc.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)value;
        }
        if (sendAdminToNode(adm)) {
            g_compLora = lc;
            g_compPreset = (int)lc.modem_preset;
            mode = MODE_BLELINK; // watch the node reboot + the link come back
        }
        return;
    }

    switch (target) {
    case 0: config.lora.region = (meshtastic_Config_LoRaConfig_RegionCode)value; break;
    case 5: config.device.role = (meshtastic_Config_DeviceConfig_Role)value; break;
    case 6: config.lora.hop_limit = (uint32_t)value; break;
    case 7: config.lora.tx_power = value; break;
    case 8: config.device.rebroadcast_mode = (meshtastic_Config_DeviceConfig_RebroadcastMode)value; break;
    default:
        config.lora.use_preset = true;
        config.lora.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)value;
    }
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CONFIG);
    rebootAtMsec = millis() + 1500;
    mode = MODE_REBOOT;
}

// Opens the editor/picker for one flat settings item (see the labels table).
void AdvUI::openSetting(int item)
{
    if (item <= 1) { // 0 = long name, 1 = short name
        editTarget = item;
        const char *cur;
        if (g_radioCompanion) { // editing the linked node's names (remote admin)
            meshtastic_NodeInfoLite *lm = g_linkMyNode ? nodeByNum(g_linkMyNode) : nullptr;
            cur = lm ? (item == 1 ? lm->short_name : lm->long_name) : "";
        } else {
            cur = (item == 1) ? owner.short_name : owner.long_name;
        }
        strncpy(nameBuf, cur, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = 0;
        nameLen = strlen(nameBuf);
        nameReturn = MODE_SETTINGS;
        mode = MODE_SETNAME;
    } else if (item == 2) { // Region
        if (g_radioCompanion && !g_compLoraValid)
            return; // config not synced yet: nothing to edit against
        pickTarget = 0;
        pickSel = optIndex(kRegionOpts, kRegionCount, (int)(g_radioCompanion ? g_compLora.region : config.lora.region));
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 3) { // Preset
        if (g_radioCompanion && !g_compLoraValid)
            return;
        pickTarget = 1;
        pickSel =
            optIndex(kPresetOpts, kPresetCount, (int)(g_radioCompanion ? g_compLora.modem_preset : config.lora.modem_preset));
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 4) { // Frequency (MHz)
        if (g_radioCompanion && !g_compLoraValid)
            return;
        editTarget = 2;
        float ovr = g_radioCompanion ? g_compLora.override_frequency : config.lora.override_frequency;
        if (ovr > 0)
            snprintf(nameBuf, sizeof(nameBuf), "%.3f", (double)ovr);
        else
            nameBuf[0] = 0;
        nameLen = strlen(nameBuf);
        nameReturn = MODE_SETTINGS;
        mode = MODE_SETNAME;
    } else if (item == 5) { // Channel name
        if (g_radioCompanion && !g_compChans[0].has_settings)
            return; // channel not synced yet: nothing to round-trip
        editTarget = 3;
        strncpy(nameBuf, g_radioCompanion ? g_compChans[0].settings.name : channels.getByIndex(0).settings.name,
                sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = 0;
        nameLen = strlen(nameBuf);
        nameReturn = MODE_SETTINGS;
        mode = MODE_SETNAME;
    } else if (item == 6) { // Role
        if (g_radioCompanion && !g_compDeviceValid)
            return;
        pickTarget = 5;
        pickSel = optIndex(kRoleOpts2, kRoleCount, (int)(g_radioCompanion ? g_compDevice.role : config.device.role));
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 7) { // Hop limit
        if (g_radioCompanion && !g_compLoraValid)
            return;
        pickTarget = 6;
        pickSel = optIndex(kHopOpts, kHopCount, (int)(g_radioCompanion ? g_compLora.hop_limit : config.lora.hop_limit));
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 8) { // TX power
        if (g_radioCompanion && !g_compLoraValid)
            return;
        pickTarget = 7;
        pickSel = optIndex(kPowerOpts, kPowerCount, (int)(g_radioCompanion ? g_compLora.tx_power : config.lora.tx_power));
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 9) { // Rebroadcast mode
        if (g_radioCompanion && !g_compDeviceValid)
            return;
        pickTarget = 8;
        pickSel = optIndex(kRebroadOpts, kRebroadCount,
                           (int)(g_radioCompanion ? g_compDevice.rebroadcast_mode : config.device.rebroadcast_mode));
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 10) { // UTC offset -> city/offset picker
        pickTarget = 2;
        pickSel = optIndex(kUtcOpts, kUtcCount, g_utcOffsetMin);
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 13) { // Screen auto-off -> timeout picker
        pickTarget = 4;
        pickSel = optIndex(kScreenOpts, kScreenCount, (int)g_screenOffSec);
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 14) { // Radio backend -> onboard/companion picker
        pickTarget = 3;
        pickSel = g_radioCompanion ? 1 : 0;
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 11 || item == 12) { // WiFi / MQTT sub-page
        netPage = item == 11 ? 0 : 1;
        netSel = 0;
        netScroll = 0;
        netDirty = false;
        mode = MODE_NETPAGE;
    }
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
    bool tab = c == 0x09;   // TAB   -> emoji picker
    bool printable = c >= 0x20 && c < 0x7f;

    if (c == AdvKeyboard::kLongEsc) { // long-press ESC opens settings from anywhere
        setSel = 0;
        setScroll = 0;
        setSection = -1;
        mode = MODE_SETTINGS;
        return;
    }

    if (c == AdvKeyboard::kLang) { // Fn+L: toggle transliterated Cyrillic input
        g_ruMode = !g_ruMode;
        pendingLat = 0;
        g_msgsDirty = true; // the layout choice is persisted with the message file
        return;
    }

    if (mode == MODE_SETNAME) {
        unsigned maxLen = editMax(editTarget);
        bool numeric = (editTarget == 2);
        if (esc) {
            mode = nameReturn;
        } else if (enter) {
            // A blank channel name is legitimate (it means "the preset default"), so the
            // channel editor applies even when emptied; the other editors need text.
            bool rebooting = (nameLen || editTarget == 3) && applyName();
            if (!rebooting)
                mode = nameReturn;
        } else if (bksp) {
            pendingLat = 0;
            if (nameLen) {
                while (nameLen > 0 && ((unsigned char)nameBuf[nameLen - 1] & 0xC0) == 0x80)
                    nameBuf[--nameLen] = 0; // whole UTF-8 char (Cyrillic is 2 bytes)
                if (nameLen)
                    nameBuf[--nameLen] = 0;
            }
        } else if (g_ruMode && !numeric && editTarget < 20 && printable && nameLen + 2 < sizeof(nameBuf)) {
            translitFeed(nameBuf, nameLen, sizeof(nameBuf), c, pendingLat); // WiFi/MQTT fields stay Latin
        } else if (printable && nameLen < maxLen && (!numeric || (c >= '0' && c <= '9') || c == '.')) {
            nameBuf[nameLen++] = c;
            nameBuf[nameLen] = 0;
        }
        return;
    }

    if (mode == MODE_BTPIN) {
        if (esc)
            mode = btPinReturn; // pairing keeps going in the background
        return;
    }

    if (mode == MODE_SETTINGS) {
        const uint8_t *items = setSection == 0 ? kSecNode : setSection == 1 ? kSecLora : kSecDevice;
        int listCount = setSection < 0    ? kTopCount
                        : setSection == 0 ? (int)sizeof(kSecNode)
                        : setSection == 1 ? (int)sizeof(kSecLora)
                                          : (int)sizeof(kSecDevice);
        if (esc) {
            if (setSection >= 0) { // back to the top level, cursor on the section's row
                setSel = setSection == 0 ? 0 : setSection == 1 ? 1 : 4;
                setSection = -1;
                setScroll = 0;
            } else {
                mode = MODE_NODES;
            }
        } else if (up) {
            if (setSel > 0)
                setSel--;
        } else if (down) {
            if (setSel < listCount - 1)
                setSel++;
        } else if (enter) {
            if (setSection < 0) {
                switch (setSel) {
                case 0: // Node
                case 1: // LoRa
                    setSection = setSel;
                    setSel = 0;
                    setScroll = 0;
                    break;
                case 4: // Device
                    setSection = 2;
                    setSel = 0;
                    setScroll = 0;
                    break;
                case 2: openSetting(11); break; // WiFi
                case 3: openSetting(12); break; // MQTT
                case 5: openSetting(14); break; // Radio
                }
            } else {
                openSetting(items[setSel]);
            }
        }
        return;
    }

    if (mode == MODE_NETPAGE) {
        int cnt = netFieldCount(netPage);
        if (esc) {
            if (netDirty && nodeDB) { // apply on the way out (WiFi needs a reboot)
                config.has_network = true; // ensure the optional submessages serialise
                moduleConfig.has_mqtt = true;
                nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_MODULECONFIG);
                rebootAtMsec = millis() + 1500;
                mode = MODE_REBOOT;
            } else {
                mode = MODE_SETTINGS;
            }
        } else if (up) {
            if (netSel > 0)
                netSel--;
        } else if (down) {
            if (netSel < cnt - 1)
                netSel++;
        } else if (enter) {
            if (netFields(netPage)[netSel].isBool) {
                netSetBool(netPage, netSel, !netGetBool(netPage, netSel));
                netDirty = true;
            } else { // text field -> reuse the name editor
                editTarget = 20 + netPage * 10 + netSel;
                strncpy(nameBuf, netGetText(netPage, netSel), sizeof(nameBuf));
                nameBuf[sizeof(nameBuf) - 1] = 0;
                nameLen = strlen(nameBuf);
                nameReturn = MODE_NETPAGE;
                mode = MODE_SETNAME;
            }
        }
        return;
    }

    if (mode == MODE_BLESCAN) {
        if (esc) {
            bleScanStop();
            setSel = 0;
            mode = MODE_SETTINGS; // back out (e.g. to flip the radio back to onboard)
        } else if (up) {
            if (bleSel > 0)
                bleSel--;
        } else if (down) {
            if (bleSel < g_scanCount - 1)
                bleSel++;
        } else if (enter) {
            if (bleSel < g_scanCount) { // remember the node and connect to it
                snprintf(g_peerAddr, sizeof(g_peerAddr), "%s", g_scanHits[bleSel].addr);
                snprintf(g_peerName, sizeof(g_peerName), "%s",
                         g_scanHits[bleSel].name[0] ? g_scanHits[bleSel].name : g_scanHits[bleSel].addr);
                g_peerType = g_scanHits[bleSel].type;
                saveRadioCfg();
                bleAttemptMark();
                bleAttemptArmed = true;
                bleConnectAsync(g_peerAddr, g_peerType);
                mode = MODE_BLELINK;
            }
        } else if (c == 'r' || c == 'R') {
            bleScanStart();
        }
        return;
    }

    if (mode == MODE_BLEPIN) {
        if (esc) {
            bleCancelPin();
            mode = MODE_BLELINK;
        } else if (enter) {
            if (pinLen) {
                bleSubmitPin((uint32_t)strtoul(pinBuf, nullptr, 10));
                mode = MODE_BLELINK;
            }
        } else if (bksp) {
            if (pinLen)
                pinBuf[--pinLen] = 0;
        } else if (c >= '0' && c <= '9' && pinLen < 6) {
            pinBuf[pinLen++] = c;
            pinBuf[pinLen] = 0;
        }
        return;
    }

    if (mode == MODE_BLELINK) {
        if (esc) { // just a status page now: leave it, the link stays up
            sel = 0;
            scrollTop = 0;
            mode = MODE_CHATS;
        } else if (c == 'r' || c == 'R') {
            if (g_linkState != BLE_CONNECTING && g_linkState != BLE_PAIRING) {
                bleAttemptMark();
                bleAttemptArmed = true;
                bleConnectAsync(g_peerAddr, g_peerType);
            }
        } else if (c == 'f' || c == 'F') { // forget the node -> back to the scan
            bleDisconnect();
            g_peerAddr[0] = 0;
            g_peerName[0] = 0;
            g_peerType = 0;
            saveRadioCfg();
            bleSel = 0;
            mode = MODE_BLESCAN;
            bleScanStart();
        }
        return;
    }

    if (mode == MODE_PICKLIST) {
        int cnt = pickListFor(pickTarget).cnt;
        if (esc) {
            mode = MODE_SETTINGS;
        } else if (up) {
            if (pickSel > 0)
                pickSel--;
        } else if (down) {
            if (pickSel < cnt - 1)
                pickSel++;
        } else if (enter) {
            if (pickTarget == 4) { // screen auto-off: applied live, persisted with msgs
                g_screenOffSec = kScreenOpts[pickSel].value;
                g_msgsDirty = true;
                mode = MODE_SETTINGS;
            } else if (pickTarget == 2) { // UTC offset: applied live, persisted with msgs
                g_utcOffsetMin = kUtcOpts[pickSel].value;
                g_msgsDirty = true;
                mode = MODE_SETTINGS;
            } else if (pickTarget == 3) { // radio backend: persist + reboot into the new role
                bool comp = kRadioOpts[pickSel].value == 1;
                if (comp != g_radioCompanion) {
                    g_radioCompanion = comp;
                    saveRadioCfg();
                    rebootAtMsec = millis() + 1500;
                    mode = MODE_REBOOT;
                } else if (comp) { // already companion: open the link status (F there re-scans)
                    if (g_peerAddr[0]) {
                        mode = MODE_BLELINK;
                    } else {
                        bleSel = 0;
                        mode = MODE_BLESCAN;
                        bleScanStart();
                    }
                } else {
                    mode = MODE_SETTINGS;
                }
            } else {
                // Region/Preset/Role/Hops/Power/Rebroadcast: radio config, needs a
                // reboot (ours, or the linked node's via remote admin).
                applyLoRa(pickTarget, pickListFor(pickTarget).opts[pickSel].value);
            }
        }
        return;
    }

    if (mode == MODE_REBOOT)
        return; // rebooting shortly; ignore input

    if (mode == MODE_NODE && reactStrip) { // quick-reaction strip is open
        if (esc) {
            reactStrip = false;
        } else if (left) {
            if (reactPick > 0)
                reactPick--;
        } else if (right) {
            if (reactPick < 5)
                reactPick++;
        } else if (enter) {
            int idx = matchedFromNewest(reactSel);
            if (idx >= 0)
                sendReaction(idx, kQuickReacts[reactPick]);
            reactStrip = false;
            reactSel = -1;
        }
        return;
    }

    if (mode == MODE_NODE && reactSel >= 0) { // picking which message to react to
        if (esc) {
            reactSel = -1;
        } else if (up) {
            if (matchedFromNewest(reactSel + 1) >= 0)
                reactSel++; // older
        } else if (down) {
            if (reactSel > 0)
                reactSel--;
            else
                reactSel = -1; // stepping past the newest exits react mode
        } else if (enter) {
            int smi = matchedFromNewest(reactSel);
            if (smi >= 0 && g_msgs[smi].id != 0) { // old history has no id to reference
                if (pickReply) { // LEFT flow: compose a quoted reply to this message
                    pendingReplyId = g_msgs[smi].id;
                    utf8Copy(replyPrev, g_msgs[smi].text, (int)sizeof(replyPrev) - 1);
                    reactSel = -1;
                    msgBuf[0] = 0;
                    msgLen = 0;
                    pendingLat = 0;
                    chatScroll = 0;
                    mode = MODE_COMPOSE;
                } else { // RIGHT flow: quick-reaction strip
                    reactStrip = true;
                    reactPick = 0;
                }
            }
        }
        return;
    }

    if (mode == MODE_NODE) {
        if (esc || bksp) {
            if (g_msgsDirty) { // flush read-state now — a reset right after would lose it
                saveMsgs();
                g_msgsDirty = false;
                g_lastSaveMs = millis();
            }
            mode = nodeReturn;
        } else if (right || left) { // pick a message: RIGHT = react, LEFT = reply
            if (matchedFromNewest(0) >= 0) {
                reactSel = 0;
                pickReply = left;
            }
        } else if (up) {
            chatScroll++; // older; drawNode clamps to the top of the thread
        } else if (down) {
            if (chatScroll > 0)
                chatScroll--; // back toward the newest
        } else if (enter && selectedChannel < 0) { // resend the newest failed DM in place
            uint32_t me = myNodeNum();
            for (int i = g_msgCount - 1; i >= 0; i--) {
                int idx = i;
                Msg &m = g_msgs[idx];
                if (m.from == me && m.to == selectedNum && m.status == MSG_FAILED) {
                    uint32_t id = sendTextPacket(selectedNum, m.text);
                    if (id) {
                        m.id = id; // the new ACK will find it by this id
                        m.status = MSG_SENDING;
                        m.err = 0;
                        m.rxTime = getTime(false);
                        g_msgsDirty = true;
                    }
                    break;
                }
            }
        } else if (tab) {
            msgLen = 0; // start a fresh reply built from the picked emoji
            msgBuf[0] = 0;
            chatScroll = 0;
            emojiReturn = MODE_NODE;
            emojiSel = 0;
            mode = MODE_EMOJI;
        } else if (printable) {
            chatScroll = 0; // jump to the bottom to compose in context
            msgBuf[0] = 0;  // start composing a reply to this node
            msgLen = 0;
            pendingLat = 0;
            if (g_ruMode)
                translitFeed(msgBuf, msgLen, sizeof(msgBuf), c, pendingLat);
            else {
                msgBuf[msgLen++] = c;
                msgBuf[msgLen] = 0;
            }
            mode = MODE_COMPOSE;
        }
        return;
    }

    if (mode == MODE_EMOJI) {
        if (esc) {
            mode = emojiReturn;
        } else if (left) {
            if (emojiSel > 0)
                emojiSel--;
        } else if (right) {
            if (emojiSel < kEmojiCount - 1)
                emojiSel++;
        } else if (up) {
            if (emojiSel >= 6)
                emojiSel -= 6;
        } else if (down) {
            if (emojiSel + 6 < kEmojiCount)
                emojiSel += 6;
        } else if (enter) {
            const char *lab = kEmojiPalette[emojiSel];
            size_t ll = strlen(lab);
            if (msgLen + ll < sizeof(msgBuf) - 1) {
                strcpy(msgBuf + msgLen, lab);
                msgLen += ll;
            }
            mode = MODE_COMPOSE; // continue composing with the emoji inserted
        }
        return;
    }

    if (mode == MODE_COMPOSE) {
        if (esc) {
            pendingReplyId = 0; // cancelling compose drops the quote too
            mode = MODE_NODE;
        } else if (tab) {
            emojiReturn = MODE_COMPOSE; // add an emoji into the current message
            emojiSel = 0;
            mode = MODE_EMOJI;
        } else if (enter) {
            if (msgLen) {
                if (selectedChannel >= 0)
                    sendChannel(selectedChannel, msgBuf, pendingReplyId);
                else
                    sendMessage(selectedNum, msgBuf, pendingReplyId);
            }
            pendingReplyId = 0;
            msgLen = 0;
            msgBuf[0] = 0;
            pendingLat = 0;
            chatScroll = 0; // show the just-sent message at the bottom
            mode = MODE_NODE;
        } else if (bksp) {
            pendingLat = 0;
            while (msgLen > 0 && ((unsigned char)msgBuf[msgLen - 1] & 0xC0) == 0x80)
                msgBuf[--msgLen] = 0; // drop UTF-8 continuation bytes, then the lead byte
            if (msgLen)
                msgBuf[--msgLen] = 0;
        } else if (g_ruMode && printable && msgLen + 2 < sizeof(msgBuf)) {
            translitFeed(msgBuf, msgLen, sizeof(msgBuf), c, pendingLat);
        } else if (printable && msgLen < sizeof(msgBuf) - 1) {
            msgBuf[msgLen++] = c;
            msgBuf[msgLen] = 0;
        }
        return;
    }

    // Home: recent conversations. Typing opens node search; Tab switches to all nodes.
    if (mode == MODE_CHATS) {
        buildConversations();
        if (bksp) { // Del arms the delete confirmation
            if (sel < convCount)
                confirmDel = true;
            return;
        }
        if (confirmDel) { // pending delete: Enter confirms, any other key cancels
            if (enter && sel < convCount) {
                deleteConversation(conv[sel]);
                buildConversations();
                if (sel >= convCount)
                    sel = convCount ? convCount - 1 : 0;
            }
            confirmDel = false;
            return;
        }
        if (up) {
            if (sel > 0)
                sel--;
            return;
        }
        if (down) {
            if (sel < convCount - 1)
                sel++;
            return;
        }
        if (enter) {
            openConv(sel);
            return;
        }
        if (left || right) { // favourite the conversation's channel / peer node
            if (sel < convCount) {
                Conv &c = conv[sel];
                if (c.isChan) {
                    if (left)
                        g_favChannels |= (1u << c.ch);
                    else
                        g_favChannels &= ~(1u << c.ch);
                    g_msgsDirty = true;
                } else if (nodeDB && !g_radioCompanion) {
                    nodeDB->set_favorite(left, c.node);
                }
            }
            return;
        }
        if (tab) {
            mode = MODE_NODES;
            sel = 0;
            scrollTop = 0;
            return;
        }
        if (!printable)
            return;
        mode = MODE_PICKER; // a printable char: search all nodes
        queryLen = 0;
        query[0] = 0;
        sel = 0;
        scrollTop = 0;
        // fall through to MODE_PICKER handling
    }

    // Home node list: navigable directly (cursor + scroll); typing opens the filter.
    if (mode == MODE_NODES) {
        rebuildFiltered();
        int total = chanCount + filteredCount;
        if (esc || tab) { // back to the chats home
            mode = MODE_CHATS;
            sel = 0;
            scrollTop = 0;
            return;
        }
        if (up) {
            if (sel > 0)
                sel--;
            return;
        }
        if (down) {
            if (sel < total - 1)
                sel++;
            return;
        }
        if (enter) {
            if (sel < total)
                openEntry(sel); // channel or node
            return;
        }
        if (left || right) { // favourite / unfavourite the selected entry (channel or node)
            if (sel < total)
                favEntry(sel, left);
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
        mode = MODE_CHATS;
        queryLen = 0;
        query[0] = 0;
        sel = 0;
        scrollTop = 0;
        return;
    }

    rebuildFiltered();
    int total = chanCount + filteredCount;

    if (up) {
        if (sel > 0)
            sel--;
        return;
    }
    if (down) {
        if (sel < total - 1)
            sel++;
        return;
    }
    if (enter) {
        if (sel < total)
            openEntry(sel); // channel or node
        return;
    }
    if (left || right) { // favourite / unfavourite the selected entry
        if (sel < total)
            favEntry(sel, left);
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

    { // Heap watchdog: log ONLY on a fresh all-time low (>=1 KB below the last), so a
      // stable device stays silent — no periodic spam, nothing to grow an SD log. A
      // leak shows as a stepping descent; steady operation prints nothing after warmup.
        static uint32_t loggedMin = 0xFFFFFFFF;
        uint32_t mn = ESP.getMinFreeHeap();
        if (mn + 1024 < loggedMin) {
            loggedMin = mn;
            LOG_WARN("advui: heap new low free=%u min=%u largest=%u", (unsigned)ESP.getFreeHeap(), (unsigned)mn,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }

#ifdef ADVUI_SCREENSHOT
    while (Serial.available())
        if (Serial.read() == 'S')
            runDemoDump(); // host sends 'S' -> dump every screen, then reboot
#endif

    while (!g_radioCompanion && api.available() && api.getFromRadio(fromRadioBuf) > 0) {
        handleFromRadio(api.lastFromRadio()); // pick out incoming text messages (local radio mode)
        uiDirty = true;
    }

    if (g_radioCompanion) { // companion: mesh packets arrive from the BLE pump's ring
        uint8_t pbuf[512];
        uint16_t plen;
        static meshtastic_FromRadio fr; // large struct: keep off the stack
        while (bleNextPacket(pbuf, &plen)) {
            fr = meshtastic_FromRadio_init_default;
            if (pb_decode_from_bytes(pbuf, plen, &meshtastic_FromRadio_msg, &fr)) {
                handleFromRadio(fr); // same pipeline: messages, reactions, delivery ACKs
                uiDirty = true;
            }
        }
    }

#ifdef HAS_I2S
    // Advance our alert beep; isPlaying() both plays and reports. Stop when done.
    if (g_beeping) {
        if (!audioThread || !audioThread->isPlaying()) {
            if (audioThread)
                audioThread->stop();
            g_beeping = false;
        }
    }
#endif
#ifdef HAS_NEOPIXEL
    if (g_ledOffMs && millis() > g_ledOffMs) { // clear the notification flash
        rgbLedWrite(NEOPIXEL_DATA, 0, 0, 0);
        g_ledOffMs = 0;
    }
#endif

    kb.setNavKeys(mode != MODE_SETNAME && mode != MODE_COMPOSE); // symbols while typing, arrows otherwise
    kb.trigger();
    bool keyDuringSplash = false;
    bool wakeOnly = !screenOn; // keys pressed on a dark screen only wake it
    bool sawKey = false;
    while (kb.hasEvent()) {
        char ch = kb.dequeueEvent();
        sawKey = true;
        if (wakeOnly) {
            // swallowed: the wake key must not also act
        } else if (splashDone)
            handleKey(ch);
        else
            keyDuringSplash = true; // any key dismisses the splash early
        kb.trigger(); // re-read the FIFO as we drain (matches the stock poll loop)
    }
    if (sawKey) {
        lastActivityMs = millis();
        uiDirty = true;
    }
    if (wakeOnly && sawKey)
        screenWake();
    kb.clearInt(); // re-arm the TCA8418 interrupt, else it stops reporting after the first event

    if (!splashDone && (keyDuringSplash || millis() - bootMs > 2000))
        splashDone = true;

    // Announce ourselves early so neighbours see us right away — stock waits ~30s.
    if (splashDone && !announced && nodeInfoModule) {
        nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, false);
        announced = true;
    }

    // First-boot onboarding: a fresh install has no LoRa region, so the radio can't
    // transmit. Drop new users straight into the region picker instead of a dead node.
    if (splashDone && !regionPrompted) {
        regionPrompted = true;
        if (!g_radioCompanion && config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
            pickTarget = 0;
            pickSel = 0;
            pickScroll = 0;
            mode = MODE_PICKLIST;
        }
    }

    // Companion mode: after the splash, reconnect to the saved node, or scan for one.
    // If the previous connect attempt never finished (crash), don't retry on boot —
    // otherwise a crashing link would reboot-loop the device.
    if (splashDone && g_radioCompanion && !companionEntered) {
        companionEntered = true;
        bleSel = 0;
        if (g_peerAddr[0] && !bleAttemptPending()) {
            bleAttemptMark();
            bleAttemptArmed = true;
            bleConnectAsync(g_peerAddr, g_peerType);
            mode = MODE_BLELINK;
        } else {
            bleAttemptClear();
            mode = MODE_BLESCAN;
            bleScanStart();
        }
    }

    // A finished connect attempt (either way, gracefully) clears the crash guard.
    if (g_linkState == BLE_CONNECTED || g_linkState == BLE_FAILED) {
        if (bleAttemptArmed) {
            bleAttemptClear();
            bleAttemptArmed = false;
        }
    }

    // Once the config download lands, drop the user into the chats — the terminal
    // is ready. The link screen stays reachable for drops (auto-retry shows there).
    if (g_radioCompanion) {
        if (g_linkState != BLE_CONNECTED)
            linkJumped = false;
        else if (!linkJumped && g_linkConfigDone && (mode == MODE_BLELINK || mode == MODE_BLEPIN)) {
            linkJumped = true;
            sel = 0;
            scrollTop = 0;
            mode = MODE_CHATS;
        }
    }

    // Auto-reconnect: a dropped (or otherwise idle) companion link retries every ~7s
    // wherever the user is (except the scan — leaving there means re-picking a node).
    if (g_radioCompanion && (g_linkState == BLE_FAILED || g_linkState == BLE_IDLE) && g_peerAddr[0] &&
        companionEntered && mode != MODE_BLESCAN && mode != MODE_BLEPIN && millis() - bleRetryMs > 7000) {
        bleRetryMs = millis();
        bleAttemptMark();
        bleAttemptArmed = true;
        bleConnectAsync(g_peerAddr, g_peerType);
    }

    // A phone is pairing with us (local mode): pop the passkey screen, waking the
    // display if needed — same as stock behaviour, just on our UI.
    if (!g_radioCompanion && splashDone) {
        bool phonePairing = bluetoothStatus->getConnectionState() == meshtastic::BluetoothStatus::ConnectionState::PAIRING;
        if (phonePairing && mode != MODE_BTPIN) {
            btPinReturn = (mode == MODE_COMPOSE || mode == MODE_NODE) ? MODE_NODE : MODE_CHATS;
            mode = MODE_BTPIN;
            if (!screenOn)
                screenWake();
        } else if (!phonePairing && mode == MODE_BTPIN) {
            mode = btPinReturn; // pairing finished (either way)
        }
    }

    // Pairing wants a passkey: drop whatever we're doing and show the PIN screen.
    if (g_pinRequested && mode != MODE_BLEPIN) {
        pinLen = 0;
        pinBuf[0] = 0;
        mode = MODE_BLEPIN;
    }
    blePump(); // drain the companion link's FromRadio stream (no-op when not connected)

    // Persist the conversation, debounced, so we don't hammer the flash.
    if (g_msgsDirty && millis() - g_lastSaveMs > 3000) {
        saveMsgs();
        g_msgsDirty = false;
        g_lastSaveMs = millis();
    }

    // Screen auto-off: no input for the configured time cuts the display rail.
    // Setup/transient screens keep it awake, and a PIN request relights it (pairing
    // needs the user). While dark, skip all rendering — the panel is unpowered.
    bool sleepable = splashDone && g_screenOffSec > 0 && mode != MODE_BLEPIN && mode != MODE_BLESCAN &&
                     mode != MODE_REBOOT && mode != MODE_BTPIN;
    if (screenOn && sleepable && millis() - lastActivityMs >= (uint32_t)g_screenOffSec * 1000)
        screenSleep();
    if (!screenOn) {
        if (splashDone && !sleepable && g_screenOffSec > 0)
            screenWake(); // e.g. the pairing PIN screen opened while dark
        else
            return 200;
    }

    // Redraw only when something changed: a key, a handled packet, a mode flip
    // from a non-key path (auto-jump, PIN popups), a live screen (splash timing,
    // scan hits, link counters), or the ~10 s refresh for battery %/ages. Idle
    // frames cost a full 32 KB render + SPI push at 5 Hz otherwise.
    bool liveScreen = !splashDone || mode == MODE_BLESCAN || mode == MODE_BLELINK;
    if (!(uiDirty || liveScreen || mode != lastDrawnMode || millis() - lastDrawMs > 10000)) {
#ifdef HAS_I2S
        if (g_beeping)
            return 20; // keep pumping the tone at the fast tick
#endif
        if (kb.navHeld())
            return 40; // a held key is auto-repeating: keep the fast cadence
        return 200;
    }
    uiDirty = false;
    lastDrawnMode = mode;
    lastDrawMs = millis();

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
    else if (mode == MODE_NETPAGE)
        drawNetPage();
    else if (mode == MODE_BLESCAN)
        drawBleScan();
    else if (mode == MODE_BLEPIN)
        drawBlePin();
    else if (mode == MODE_BLELINK)
        drawBleLink();
    else if (mode == MODE_BTPIN)
        drawBtPin();
    else if (mode == MODE_PICKLIST)
        drawPickList();
    else if (mode == MODE_REBOOT)
        drawReboot();
    else if (mode == MODE_EMOJI)
        drawEmoji();
    else if (mode == MODE_NODES)
        drawNodeList();
    else
        drawChats();

#ifdef HAS_I2S
    if (g_beeping)
        return 20; // pump the tone fast until it finishes
#endif
    if (splashDone && kb.navHeld())
        return 40; // a held arrow/backspace is repeating: poll+redraw fast for smooth scroll
    return splashDone ? 200 : 80; // 5 Hz normally; snappier while the splash is up
}

// Created once from an injected call in main.cpp (after setupModules); the
// OSThread base then self-schedules, so no main-loop edits are needed.
void advuiSetup()
{
    static AdvUI advUI;
}

} // namespace advui
