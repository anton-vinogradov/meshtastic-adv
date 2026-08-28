#include "AdvUI.h"
#include "AdvBle.h"
#include "AdvVersion.h"
#include "AdvFont.h"
#include "AdvMeshCompat.h"
#include "AdvStorage.h"
#include "AdvUtf8.h"
#include "SPILock.h" // the external panel shares SPI2 with the SD card
#include "SafeFile.h"
#include "Throttle.h"
#include <new> // std::nothrow: the second-screen driver must fail soft, not abort
#include "BluetoothStatus.h"
#include "CyrillicFont.h"
#include "FSCommon.h"
#include "PowerStatus.h"
#include "configuration.h"
#include "mesh/Channels.h"
#include "mesh/MeshModule.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/RadioLibInterface.h" // operating frequency for the Settings row
#include "mqtt/MQTT.h"              // broker link state for the MQTT page header
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/admin.pb.h"
#include "mesh/mesh-pb-constants.h"
#include "gps/RTC.h" // getTime() for message timestamps
#include "graphics/emotes.h" // UTF-8 -> bitmap emoji table (forced in via EmotesData.cpp)
#include "main.h" // audioThread (I2S beep)
#include "modules/NodeInfoModule.h"
#if HAS_WIFI
#include "mesh/wifi/WiFiAPClient.h" // getWifiDisconnectReason()
#include <WiFi.h>                    // live WiFi.status()/localIP()/RSSI() for the settings page
#endif
#ifdef HAS_NEOPIXEL
#include "esp32-hal-rgb-led.h" // stateless RMT driver for the onboard RGB LED
#include "esp_arduino_version.h"
#include <Esp.h>
#include <esp_heap_caps.h>
#endif
#include <algorithm>
#include <cctype>
#include <esp_random.h>
#ifdef ADVUI_HIL
#include <esp_system.h>
#endif
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

extern uint8_t g_nameShort;

#ifdef ADVUI_HIL
ErrorCode g_hilNextSendError = ERRNO_OK;
uint8_t g_hilLastSendError = ERRNO_OK;
uint32_t g_hilLastTxReply = 0;
#endif

#ifdef ADVUI_SCREENSHOT
constexpr uint32_t kDemoMe = 0x00A11CE1;
constexpr uint32_t kDemoPeer = 0x00BEEF01;
bool g_demoActive = false;
bool g_demoNetActive = false;
meshtastic_NodeInfoLite g_demoMeNode;
meshtastic_NodeInfoLite g_demoPeerNode;
#endif

bool demoNetActive()
{
#ifdef ADVUI_SCREENSHOT
    return g_demoNetActive;
#else
    return false;
#endif
}

const char *sendFailureText(SendFailure failure)
{
    switch (failure) {
    case SendFailure::LINK_DOWN: return "link disconnected";
    case SendFailure::QUEUE_FULL: return "TX queue full";
    case SendFailure::ENCODE_FAILED: return "packet encode failed";
    case SendFailure::RADIO_REJECTED: return "radio rejected send";
    default: return "";
    }
}

const char *nodeName(const meshtastic_NodeInfoLite *n)
{
    if (g_nameShort)
        return nodeShortName(n)[0] ? nodeShortName(n) : nodeLongName(n);
    return nodeLongName(n)[0] ? nodeLongName(n) : nodeShortName(n);
}

// Favourites the Cardputer owns, as opposed to the ones living in a node database.
// In companion mode the database belongs to the node being driven, not to us, so the
// flag there is not ours to set — which is why the arrows silently did nothing in that
// mode while channel favourites, which were always local state, worked fine.
constexpr int kMaxFavNodes = 24;
uint32_t g_favNodes[kMaxFavNodes];
uint8_t g_favNodeCount = 0;

bool favNodeLocal(uint32_t num)
{
    for (uint8_t i = 0; i < g_favNodeCount; i++)
        if (g_favNodes[i] == num)
            return true;
    return false;
}

// Returns true when the set actually changed, so callers know whether to persist.
bool setFavNodeLocal(uint32_t num, bool on)
{
    for (uint8_t i = 0; i < g_favNodeCount; i++) {
        if (g_favNodes[i] != num)
            continue;
        if (on)
            return false;
        g_favNodes[i] = g_favNodes[--g_favNodeCount]; // order carries no meaning here
        return true;
    }
    if (!on || g_favNodeCount >= kMaxFavNodes)
        return false;
    g_favNodes[g_favNodeCount++] = num;
    return true;
}

bool isFav(const meshtastic_NodeInfoLite *n)
{
    if (nodeIsFavorite(n))
        return true;
    return g_favNodeCount && favNodeLocal(n->num);
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
// MSG_SENT is retained for legacy/untracked broadcasts. New broadcasts request
// an implicit routing result and remain MSG_SENDING until it arrives, just like
// an end-to-end-acknowledged DM.
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
    char text[236]; // fits the 233-byte wire maximum (DATA_PAYLOAD_LEN) + NUL;
                    // 160 used to cut long Cyrillic messages mid-word
};
static_assert(sizeof(Msg) == 256, "AVS4/AVH2 Msg layout changed; bump both persistence formats");

// Short names for meshtastic_Routing_Error, shown next to a failed message.
//
// A broadcast is a special case worth its own word. Nobody owes it a reply, so the
// only confirmation available is an implicit one: the radio listens for a neighbour
// repeating the packet onward. With no neighbour in range nothing repeats it, the
// stack reports MAX_RETRANSMIT, and calling that "no ack" reads as "your message
// never went out" — two users in a row reported it as a send failure when the
// message had in fact been transmitted and simply had nobody to carry it further.
const char *errName(uint8_t e, bool broadcast)
{
    if (broadcast && e == 5)
        return "no relay";
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
constexpr int kNumSettings = 21; // the flat item table: Name..Channel, Role..Rebroadcast, UTC, WiFi, MQTT, Screen, Radio, Font, Clock, GPS, Sort, Names, 2nd screen

// The Settings menu is two-level: the top lists sections (plus WiFi/MQTT/Radio
// as direct entries); a section lists indices into the flat item table.
const uint8_t kSecNode[] = {0, 1};                    // Name, Short
const uint8_t kSecLora[] = {2, 3, 4, 5, 6, 7, 8, 9};  // Region..Rebroadcast
const uint8_t kSecDevice[] = {10, 16, 18, 19, 13, 20, 17, 15}; // UTC, Clock, Sort, Names, Screen, 2nd screen, GPS, Font (read-only)

constexpr int kTopCount = 7;                          // Node, LoRa, WiFi, MQTT, Device, Radio, About
Msg g_msgs[kMaxMsgs];
int g_msgCount = 0;         // populated slots (grows to kMaxMsgs)
bool g_msgsDirty = false;   // ring changed since the last flash save
uint32_t g_lastMsgChangeMs = 0; // restart the quiet-time debounce on every mutation

void markMsgsDirty()
{
    const uint32_t now = millis();
    g_msgsDirty = true;
    g_lastMsgChangeMs = now;
}

// Parallel to g_msgs: the packet id this slot's message replies to (0 = not a reply).
// A separate array so the Msg layout (and the saved-file magic) stays unchanged.
uint32_t g_msgReply[kMaxMsgs];

// Real epochs start past this; anything below is "the clock wasn't set". The
// range doubles as storage for clockless arrivals: they are stamped with the
// UPTIME second instead, and backfilled to real time once the clock syncs
// (see the transition check in runOnce). Uptime stamps from a previous boot
// can't be recovered (the powered-off gap is unknowable) and load as 0.
constexpr uint32_t kValidEpoch = 1600000000u;

void histAppend(const Msg &m, uint32_t replyId); // eviction victims go to the flash archive

bool addMsg(uint32_t from, uint32_t to, uint8_t ch, uint32_t rxTime, bool unread, const char *text, uint32_t id,
            uint8_t status, uint32_t replyId = 0)
{
    // MeshPacket.id is guaranteed unique only per sender. A global-id dedupe
    // silently dropped one of two unrelated nodes when their random IDs happened
    // to collide; loopback suppression needs the complete (sender, id) identity.
    if (id) // a loopback copy of a message we already hold (e.g. our own send)
        for (int i = 0; i < g_msgCount; i++)
            if (g_msgs[i].from == from && g_msgs[i].id == id)
                return false;
    if (rxTime < kValidEpoch || !validStoredEpoch(rxTime)) {
        // The engine quality-gates rx_time, so it hands us 0 even when the system
        // clock is actually fine (it survives soft resets). Prefer the live clock;
        // only truly clockless arrivals fall back to the uptime moment.
        uint32_t now = getTime(false);
        rxTime = validStoredEpoch(now) ? now : millis() / 1000 + 1;
    }
    // The ring is a compact chronological array (index 0 = oldest). When it's full
    // we evict the oldest CHANNEL broadcast, not the oldest message — so a busy
    // channel can never push a quiet DM thread out. Only when there are no
    // broadcasts left (an all-DM ring) do we drop the oldest DM.
    int slot;
    if (g_msgCount < kMaxMsgs) {
        slot = g_msgCount++;
    } else {
        // Victim tiers: trim conversations before erasing them. A conversation's
        // NEWEST message is what keeps it on the Chats list (and previews it), so
        // it goes last: (1) an old broadcast with a newer same-channel message,
        // (2) an old DM with a newer same-peer message, (3) any broadcast,
        // (4) anything. Evicted content stays readable via the flash archive.
        auto samePair = [](const Msg &a, const Msg &b) {
            if ((a.to == NODENUM_BROADCAST) != (b.to == NODENUM_BROADCAST))
                return false;
            if (a.to == NODENUM_BROADCAST)
                return a.ch == b.ch;
            return (a.from == b.from && a.to == b.to) || (a.from == b.to && a.to == b.from);
        };
        auto hasNewer = [&](int i) {
            for (int j = i + 1; j < g_msgCount; j++)
                if (samePair(g_msgs[i], g_msgs[j]))
                    return true;
            return false;
        };
        int victim = -1;
        // First run every tier without touching an in-flight packet: once a
        // message leaves RAM, a later routing ACK can no longer update it. Only
        // an all-pending ring is allowed to fall through to the same tiers with
        // that protection relaxed.
        for (int pass = 0; pass < 8 && victim < 0; pass++)
            for (int i = 0; i < g_msgCount; i++) {
                if (pass < 4 && g_msgs[i].status == MSG_SENDING)
                    continue;
                const int tier = pass % 4;
                bool bcast = g_msgs[i].to == NODENUM_BROADCAST;
                bool ok = tier == 0   ? (bcast && hasNewer(i))
                          : tier == 1 ? (!bcast && hasNewer(i))
                          : tier == 2 ? bcast
                                      : true;
                if (ok) {
                    victim = i;
                    break;
                }
            }
        histAppend(g_msgs[victim], g_msgReply[victim]); // evicted, not gone: pageable in the thread view
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
    utf8CopyValid(m.text, sizeof(m.text), text);
    markMsgsDirty();
    return true;
}

// Mark the sent message whose id matches an incoming ACK/routing response.
// Skip MSG_IN: incoming messages now carry their sender's packet id (for reactions),
// and a random id collision must not flip their status.
bool ackMsg(uint32_t reqId, uint8_t status, uint8_t err)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].id == reqId && g_msgs[i].id != 0 && g_msgs[i].status != MSG_IN) {
            if (g_msgs[i].status == status && g_msgs[i].err == err)
                return false;
            g_msgs[i].status = status;
            g_msgs[i].err = err;
            markMsgsDirty();
            return true;
        }
    return false;
}

// --- Reactions (tapback) --------------------------------------------------------
// A reaction is a TEXT_MESSAGE_APP packet with emoji=1 and reply_id = the target
// message's packet id (same wire format the phone apps use). We keep them in their
// own small ring, attached to messages by id at render time.
// AVS4 stored reactions before the target sender became part of their identity.
// Keep the byte-exact record so existing saves can be widened on load.
struct ReactionV4 {
    uint32_t msgId; // packet id of the message reacted to
    uint32_t from;
    char label[9]; // UTF-8 emoji sequence
};
static_assert(sizeof(ReactionV4) == 20, "legacy AVS4 Reaction layout must remain byte-exact");

struct Reaction {
    uint32_t msgId;      // packet id of the message reacted to
    uint32_t from;       // node that sent the reaction
    uint32_t targetFrom; // sender of the target message; 0 = ambiguous/legacy wildcard
    char label[9];       // UTF-8 emoji sequence
};
static_assert(sizeof(Reaction) == 24, "AVS5 Reaction layout changed; bump the persistence format");
constexpr int kMaxReacts = 32;
Reaction g_reacts[kMaxReacts];
int g_reactCount = 0;
int g_reactNext = 0;

void markReactionsForMessage(uint32_t msgId, uint32_t targetFrom, bool *drop)
{
    if (!msgId || !drop)
        return;
    for (int i = 0; i < g_reactCount; i++)
        if (g_reacts[i].msgId == msgId && (!g_reacts[i].targetFrom || g_reacts[i].targetFrom == targetFrom))
            drop[i] = true;
}

void preserveReactionsForMessage(uint32_t msgId, uint32_t targetFrom, bool *drop)
{
    if (!msgId || !drop)
        return;
    for (int i = 0; i < g_reactCount; i++)
        if (g_reacts[i].msgId == msgId && (!g_reacts[i].targetFrom || g_reacts[i].targetFrom == targetFrom))
            drop[i] = false;
}

void dropMarkedReactions(bool *drop)
{
    if (!drop)
        return;
    // A full reaction store is circular. Rotate it to chronological order before
    // compacting, otherwise deleting anything also changes which surviving entry
    // is considered oldest and gets overwritten next.
    if (g_reactCount == kMaxReacts && g_reactNext) {
        const int first = g_reactNext;
        std::rotate(g_reacts, g_reacts + first, g_reacts + kMaxReacts);
        bool orderedDrop[kMaxReacts];
        for (int i = 0; i < kMaxReacts; i++)
            orderedDrop[i] = drop[(first + i) % kMaxReacts];
        memcpy(drop, orderedDrop, sizeof(orderedDrop));
    }
    int kept = 0;
    for (int i = 0; i < g_reactCount; i++)
        if (!drop[i]) {
            if (kept != i)
                g_reacts[kept] = g_reacts[i];
            kept++;
        }
    g_reactCount = kept;
    g_reactNext = kept % kMaxReacts;
}

bool addReaction(uint32_t msgId, uint32_t from, const char *label, uint32_t targetFrom = 0)
{
    LOG_INFO("advui: reaction msgId=0x%08x targetFrom=0x%08x from=0x%08x %s", (unsigned)msgId,
             (unsigned)targetFrom, (unsigned)from, label);
    for (int i = 0; i < g_reactCount; i++)
        if (g_reacts[i].msgId == msgId && g_reacts[i].targetFrom == targetFrom && g_reacts[i].from == from) {
            // Replace this sender's previous reaction to this exact message.
            if (!strcmp(g_reacts[i].label, label))
                return false;
            utf8CopyValid(g_reacts[i].label, sizeof(g_reacts[i].label), label);
            markMsgsDirty();
            return true;
        }
    Reaction &r = g_reacts[g_reactNext];
    r.msgId = msgId;
    r.from = from;
    r.targetFrom = targetFrom;
    utf8CopyValid(r.label, sizeof(r.label), label);
    g_reactNext = (g_reactNext + 1) % kMaxReacts;
    if (g_reactCount < kMaxReacts)
        g_reactCount++;
    markMsgsDirty();
    return true;
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
            markMsgsDirty();
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

// Node-list sort mode (kSortOpts value), persisted in its own one-byte file so
// the message-file header stays format-frozen. Load/save live further down,
// after the option table that bounds the value.
uint8_t g_nodeSort = 0;
// Node-list name style: 0 = long name (falls back to short), 1 = short name
// (falls back to long). Long reads better on this 240px list, but users coming
// from the phone app expect the short call-sign — so it's a choice, not a rule.
uint8_t g_nameShort = 0;
// Mirror to an external ILI9341 on the EXT header. Off by default and, when off,
// costs exactly nothing: the driver object is never constructed and pushFrame()
// returns after the built-in push. Only meaningful in companion mode — see extInit().
// 0 = no second panel; otherwise the LovyanGFX rotation it is mounted at. Which one
// is the builder's business, not ours: the M5PORKCHOP_DualScreen harness wants 7, and
// the first panel built against this firmware came out mirrored with exactly that.
uint8_t g_extRot = 0;
AdvExtDisplay *g_ext = nullptr;
#ifdef ADVUI_HIL
const char *kUiCfgPath = "/advui_hil_ui.bin";
#else
const char *kUiCfgPath = "/advui_ui.bin";
#endif
int32_t g_screenOffSec = 300; // screen auto-off timeout; 0 = never
bool g_uiCfgDirty = false;
uint32_t g_lastUiCfgSaveMs = 0;

// Transliterated Cyrillic input layer (Fn+L); persisted with the messages so the
// chosen layout survives a reboot.
bool g_ruMode = false;

// --- Radio mode (onboard / BLE companion), its own tiny file -------------------
constexpr uint32_t kRadioMagic = 0x41565231; // "AVR1"
#ifdef ADVUI_HIL
const char *kRadioPath = "/advui_hil_radio.bin";
#else
const char *kRadioPath = "/advui_radio.bin";
#endif
bool g_radioCompanion = false;
char g_peerAddr[18] = {0}; // paired companion node (empty = not chosen yet)
char g_peerName[24] = {0};
uint8_t g_peerType = 0;    // its BLE address type (public/random) — needed to connect
bool g_radioCfgDirty = false;
uint32_t g_lastRadioCfgSaveMs = 0;

bool saveRadioCfg()
{
    SafeFile f(kRadioPath, true);
    uint32_t magic = kRadioMagic;
    uint8_t mode = g_radioCompanion ? 1 : 0;
    bool ok = f.write((const uint8_t *)&magic, sizeof(magic)) == sizeof(magic) && f.write(&mode, 1) == 1 &&
              f.write((const uint8_t *)g_peerAddr, sizeof(g_peerAddr)) == sizeof(g_peerAddr) &&
              f.write((const uint8_t *)g_peerName, sizeof(g_peerName)) == sizeof(g_peerName) &&
              f.write(&g_peerType, 1) == 1;
    if (!ok || !f.close()) {
        LOG_ERROR("advui: radio config save failed; previous file kept");
        return false;
    }
    return true;
}

bool persistRadioCfg()
{
    const bool saved = saveRadioCfg();
    g_radioCfgDirty = !saved;
    g_lastRadioCfgSaveMs = millis();
    return saved;
}

// Crash guard for the companion link: the flag file exists while a connect attempt
// is in flight. If we boot and it's still there, the last attempt took the device
// down — skip the boot auto-connect so a crash can't loop, and go to the scan.
#ifdef ADVUI_HIL
const char *kBleAttemptPath = "/advui_hil_bleatt";
#else
const char *kBleAttemptPath = "/advui_bleatt";
#endif

bool bleAttemptMark()
{
    auto f = FSCom.open(kBleAttemptPath, FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("advui: cannot arm BLE connect crash guard");
        return false;
    }
    uint8_t one = 1;
    f.write(&one, 1); // file existence is the guard; its contents are immaterial
    f.close();
    return true;
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
// the BLE config-stream snapshots instead of the local nodeDB.

uint32_t myNodeNum()
{
#ifdef ADVUI_SCREENSHOT
    if (g_demoActive)
        return kDemoMe;
#endif
    if (g_radioCompanion)
        return g_linkMyNode;
    return nodeDB ? nodeDB->getNodeNum() : 0;
}

#ifdef ADVUI_SCREENSHOT
void beginDemoIdentity()
{
    memset(&g_demoMeNode, 0, sizeof(g_demoMeNode));
    g_demoMeNode.num = kDemoMe;
    nodeSetNames(&g_demoMeNode, "Field Kit", "ME");

    memset(&g_demoPeerNode, 0, sizeof(g_demoPeerNode));
    g_demoPeerNode.num = kDemoPeer;
    g_demoPeerNode.snr = 8.5f;
    g_demoPeerNode.has_hops_away = true;
    g_demoPeerNode.hops_away = 0;
    g_demoPeerNode.last_heard = getTime(false);
    nodeSetNames(&g_demoPeerNode, "TrailOps", "TOP");
    g_demoActive = true;
}
#endif

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
    nodeSetNames(&tmp, c.longName, c.shortName);
    return &tmp;
}

meshtastic_NodeInfoLite *nodeByNum(uint32_t num)
{
#ifdef ADVUI_SCREENSHOT
    if (g_demoActive) {
        if (num == kDemoMe)
            return &g_demoMeNode;
        if (num == kDemoPeer)
            return &g_demoPeerNode;
        return nullptr;
    }
#endif
    if (g_radioCompanion) {
        CompNode node;
        return bleCopyCompNode(num, &node) ? compSynth(node) : nullptr;
    }
    return nodeDB ? nodeDB->getMeshNode(num) : nullptr;
}

meshtastic_NodeInfoLite *nodeAt(uint16_t idx) // idx from filtered[]
{
    if (g_radioCompanion) {
        CompNode node;
        return bleCopyCompNodeAt(idx, &node) ? compSynth(node) : nullptr;
    }
    return nodeDB ? nodeDB->getMeshNodeByIndex(idx) : nullptr;
}

const char *presetName(meshtastic_Config_LoRaConfig_ModemPreset p); // defined below

const char *chanName(int i)
{
    if (g_radioCompanion) {
        static char name[32];
        meshtastic_Channel channel = meshtastic_Channel_init_default;
        if (!bleCopyCompChannel(i, &channel))
            return "?";
        if (channel.has_settings && channel.settings.name[0]) {
            snprintf(name, sizeof(name), "%s", channel.settings.name);
            return name;
        }
        // blank primary channel displays the modem preset, like stock does
        if (channel.role == meshtastic_Channel_Role_PRIMARY)
            return presetName((meshtastic_Config_LoRaConfig_ModemPreset)g_compPreset.load());
        return "?";
    }
    return channels.getName(i);
}

bool chanEnabled(int i)
{
    if (g_radioCompanion) {
        meshtastic_Channel channel = meshtastic_Channel_init_default;
        return bleCopyCompChannel(i, &channel) && channel.role != meshtastic_Channel_Role_DISABLED;
    }
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

// Charging bolt, drawn scanline by scanline so the zigzag is exact at this size:
// the upper stroke falls to the left, the waist juts back out, the lower stroke
// falls again to the point. 5x7, sitting level with the header text.
void drawBoltGlyph(lgfx::LGFXBase *g, int x, int y, uint16_t color)
{
    static const uint8_t kRows[][2] = {// {first pixel, width} per row
                                       {3, 2}, {2, 2}, {1, 2}, {0, 5}, {2, 2}, {1, 2}, {0, 2}};
    for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++)
        g->drawFastHLine(x + kRows[i][0], y + (int)i, kRows[i][1], color);
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
    char addr[sizeof(g_peerAddr)] = {0}, name[sizeof(g_peerName)] = {0};
    uint8_t peerType = 0;
    const size_t size = f.size();
    bool ok = (size == 4 + 1 + sizeof(addr) + sizeof(name) || size == 4 + 1 + sizeof(addr) + sizeof(name) + 1) &&
              f.read((uint8_t *)&magic, sizeof(magic)) == sizeof(magic) && f.read(&mode, 1) == 1 &&
              f.read((uint8_t *)addr, sizeof(addr)) == sizeof(addr) &&
              f.read((uint8_t *)name, sizeof(name)) == sizeof(name);
    if (ok && size == 4 + 1 + sizeof(addr) + sizeof(name) + 1)
        ok = f.read(&peerType, 1) == 1;
    f.close();
    const bool peerOk = !addr[0] || (validBleAddress(addr) && peerType <= 3);
    if (!ok || magic != kRadioMagic || mode > 1 || !peerOk) {
        g_radioCompanion = false;
        g_peerAddr[0] = 0;
        g_peerName[0] = 0;
        g_peerType = 0;
        return;
    }
    memcpy(g_peerAddr, addr, sizeof(g_peerAddr));
    memcpy(g_peerName, name, sizeof(g_peerName));
    g_peerType = peerType;
    g_radioCompanion = mode == 1;
    g_peerAddr[sizeof(g_peerAddr) - 1] = 0;
    g_peerName[sizeof(g_peerName) - 1] = 0;
    utf8Sanitize(g_peerName, sizeof(g_peerName));
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
// stateless RMT helper (shares the core RMT manager with the stock ambient thread,
// so no begin()/channel conflict). A brief flash on incoming messages, cleared in runOnce.
uint32_t g_ledOffMs = 0;
void setLedRgb(uint8_t r, uint8_t g, uint8_t b)
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    rgbLedWrite(NEOPIXEL_DATA, r, g, b);
#else
    neopixelWrite(NEOPIXEL_DATA, r, g, b);
#endif
}

void flashLed(uint8_t r, uint8_t g, uint8_t b)
{
    setLedRgb(r, g, b);
    g_ledOffMs = armDeadline(millis(), 400);
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
            markMsgsDirty();
        }
}

// Persist the message ring (with delivery status + read flags) to flash, so a
// reboot keeps the conversation. Written debounced from runOnce.
constexpr uint32_t kMsgMagic = 0x41565335;   // "AVS5" — reactions include their target sender
constexpr uint32_t kMsgMagicV4 = 0x41565334; // "AVS4" — target-less reactions, widened on load
constexpr uint32_t kMsgMagicV3 = 0x41565333; // "AVS3" — the 160-byte-text Msg layout, migrated on load

// The pre-v0.4.8 Msg layout (text[160]); V3 files and V1 archives hold these.
struct MsgV3 {
    uint32_t from;
    uint32_t to;
    uint32_t rxTime;
    uint32_t id;
    uint8_t status;
    uint8_t err;
    uint8_t ch;
    bool read;
    char text[160];
};
static_assert(sizeof(MsgV3) == 180, "legacy AVS3/AVH1 layout must remain byte-exact");

void msgFromV3(Msg &out, const MsgV3 &in)
{
    out = {};
    out.from = in.from;
    out.to = in.to;
    out.rxTime = in.rxTime;
    out.id = in.id;
    out.status = in.status;
    out.err = in.err;
    out.ch = in.ch;
    out.read = in.read;
    memcpy(out.text, in.text, sizeof(in.text));
    out.text[sizeof(in.text)] = 0;
}

bool normalizeStoredMsg(Msg &m)
{
    if (m.status > MSG_SENT || m.ch >= 8)
        return false;
    if (m.rxTime >= kValidEpoch && !validStoredEpoch(m.rxTime))
        m.rxTime = 0;
    m.text[sizeof(m.text) - 1] = 0;
    utf8Sanitize(m.text, sizeof(m.text));
    return true;
}
#ifdef ADVUI_HIL
const char *kMsgPath = "/advui_hil_msgs.bin";
uint32_t g_hilSaveCount = 0;
uint32_t g_hilSaveHeapBefore = 0;
uint32_t g_hilSaveHeapAfter = 0;
uint32_t g_hilSaveMinBefore = 0;
uint32_t g_hilSaveMinAfter = 0;
#else
const char *kMsgPath = "/advui_msgs.bin";
#endif

// The message ring is a circular buffer; iterate it oldest-first (matches the
// chronological order the loaders write/read).
static int ringIdx(int i, int count, int next, int cap) { return count == cap ? (next + i) % cap : i; }

bool saveMsgs()
{
#ifdef ADVUI_HIL
    g_hilSaveCount++;
    g_hilSaveHeapBefore = ESP.getFreeHeap();
    g_hilSaveMinBefore = ESP.getMinFreeHeap();
#endif
    SafeFile f(kMsgPath, true);
    // Count-based layout: only the populated messages are written, oldest-first,
    // so the file size tracks history depth, not kMaxMsgs — growing the ring in a
    // future build keeps loading old saves (and vice-versa).
    uint32_t magic = kMsgMagic;
    int32_t cnt = g_msgCount;
    uint8_t ru = g_ruMode ? 1 : 0, rc = (uint8_t)g_reactCount;
    bool ok = f.write((const uint8_t *)&magic, sizeof(magic)) == sizeof(magic) &&
              f.write((const uint8_t *)&cnt, sizeof(cnt)) == sizeof(cnt) &&
              f.write((const uint8_t *)&g_favChannels, sizeof(g_favChannels)) == sizeof(g_favChannels) &&
              f.write((const uint8_t *)&g_utcOffsetMin, sizeof(g_utcOffsetMin)) == sizeof(g_utcOffsetMin) &&
              f.write((const uint8_t *)&g_screenOffSec, sizeof(g_screenOffSec)) == sizeof(g_screenOffSec) &&
              f.write(&ru, 1) == 1 && f.write(&rc, 1) == 1;
    for (int i = 0; ok && i < g_msgCount; i++) {
        ok = f.write((const uint8_t *)&g_msgs[i], sizeof(Msg)) == sizeof(Msg) &&
             f.write((const uint8_t *)&g_msgReply[i], sizeof(uint32_t)) == sizeof(uint32_t);
    }
    for (int i = 0; ok && i < g_reactCount; i++) {
        int idx = ringIdx(i, g_reactCount, g_reactNext, kMaxReacts);
        ok = f.write((const uint8_t *)&g_reacts[idx], sizeof(Reaction)) == sizeof(Reaction);
    }
    const bool committed = ok && f.close();
#ifdef ADVUI_HIL
    g_hilSaveHeapAfter = ESP.getFreeHeap();
    g_hilSaveMinAfter = ESP.getMinFreeHeap();
#endif
    if (!committed) {
        LOG_ERROR("advui: message save failed; previous file kept");
        return false;
    }
    return true;
}

// AVS5/AVS4/AVS3: count-based formats. Messages/reactions were written
// oldest-first; keep the newest kMaxMsgs / kMaxReacts if an older, bigger save
// ever holds more than this build's ring.
static bool loadMsgsCounted(File &f, bool v3Messages, bool v4Reactions)
{
    int32_t cnt = 0, off = 0, so = 0;
    uint8_t ru = 0, rc = 0;
    if (f.read((uint8_t *)&cnt, 4) != 4 || cnt < 0)
        return false;
    uint8_t favChannels = 0;
    if (f.read((uint8_t *)&favChannels, sizeof(favChannels)) != sizeof(favChannels) ||
        f.read((uint8_t *)&off, 4) != 4 || f.read((uint8_t *)&so, 4) != 4 || f.read(&ru, 1) != 1 ||
        f.read(&rc, 1) != 1 || !validUtcOffsetMinutes(off))
        return false;

    // Same count-based envelope either way. V3 holds smaller pre-v0.4.8
    // messages; V4 reactions do not yet identify the sender of their target.
    size_t msz = v3Messages ? sizeof(MsgV3) : sizeof(Msg);
    size_t rsz = v4Reactions ? sizeof(ReactionV4) : sizeof(Reaction);
    const size_t pos = f.position(), size = f.size();
    if (size < pos ||
        !checkedRecordRegion((size_t)cnt, msz + sizeof(uint32_t), rc, rsz, size - pos, nullptr))
        return false;
    int mskip = cnt > kMaxMsgs ? cnt - kMaxMsgs : 0;
    // Skip old overflow by offset, not by reading a variable-size record into
    // MsgV3. The old code used sizeof(Msg) with a MsgV3 stack buffer here.
    if (mskip && !f.seek(pos + (size_t)mskip * (msz + sizeof(uint32_t))))
        return false;
    int mkeep = cnt - mskip;
    for (int i = 0; i < mkeep; i++) {
        bool ok;
        if (v3Messages) {
            MsgV3 t;
            ok = f.read((uint8_t *)&t, sizeof(t)) == (int)sizeof(t);
            if (ok)
                msgFromV3(g_msgs[i], t);
        } else {
            ok = f.read((uint8_t *)&g_msgs[i], sizeof(Msg)) == (int)sizeof(Msg);
        }
        if (!ok)
            return false;
        if (f.read((uint8_t *)&g_msgReply[i], 4) != 4)
            return false;
        if (!normalizeStoredMsg(g_msgs[i]))
            return false;
    }

    int rkeep = rc > kMaxReacts ? kMaxReacts : rc;
    for (int i = 0; i < rc - rkeep; i++) {
        if (v4Reactions) {
            ReactionV4 t;
            if (f.read((uint8_t *)&t, sizeof(t)) != (int)sizeof(t))
                return false;
        } else {
            Reaction t;
            if (f.read((uint8_t *)&t, sizeof(t)) != (int)sizeof(t))
                return false;
        }
    }
    for (int i = 0; i < rkeep; i++) {
        if (v4Reactions) {
            ReactionV4 old;
            if (f.read((uint8_t *)&old, sizeof(old)) != (int)sizeof(old))
                return false;
            g_reacts[i] = {};
            g_reacts[i].msgId = old.msgId;
            g_reacts[i].from = old.from;
            memcpy(g_reacts[i].label, old.label, sizeof(old.label));
        } else if (f.read((uint8_t *)&g_reacts[i], sizeof(Reaction)) != (int)sizeof(Reaction)) {
            return false;
        }
        g_reacts[i].label[sizeof(g_reacts[i].label) - 1] = 0;
        utf8Sanitize(g_reacts[i].label, sizeof(g_reacts[i].label));
    }

    g_msgCount = mkeep;
    g_reactCount = rkeep;
    g_reactNext = rkeep % kMaxReacts;
    g_favChannels = favChannels;
    g_utcOffsetMin = off;
    g_ruMode = ru != 0;
    if (so == 0 || (so >= 15 && so <= 3600))
        g_screenOffSec = so;
    return true;
}

void loadMsgs()
{
    auto f = FSCom.open(kMsgPath, FILE_O_READ);
    if (!f)
        return;
    uint32_t magic = 0;
    bool ok = f.read((uint8_t *)&magic, sizeof(magic)) == sizeof(magic);
    if (magic == kMsgMagic)
        ok = ok && loadMsgsCounted(f, false, false);
    else if (magic == kMsgMagicV4)
        ok = ok && loadMsgsCounted(f, false, true);
    else if (magic == kMsgMagicV3)
        ok = ok && loadMsgsCounted(f, true, true); // pre-v0.4.8 records widen on the way in
    else
        ok = false;
    f.close();
    if (!ok) {
        g_msgCount = 0;
        g_reactCount = 0;
        g_reactNext = 0;
        LOG_ERROR("advui: rejected corrupt message save");
        return;
    }

    uint32_t me = myNodeNum();
    for (int i = 0; i < g_msgCount; i++) {
        // an outgoing packet still "sending" before the reboot can't get its ACK now
        if (g_msgs[i].status == MSG_SENDING)
            g_msgs[i].status = MSG_FAILED;
        // upgrade our own channel sends saved before MSG_SENT existed
        else if (g_msgs[i].status == MSG_IN && me && g_msgs[i].from == me && g_msgs[i].to == NODENUM_BROADCAST)
            g_msgs[i].status = MSG_SENT;
        // an uptime stamp from a previous boot: the powered-off gap is unknowable,
        // so the arrival time is permanently lost — 0 keeps it blank, not backfilled
        if (g_msgs[i].rxTime < kValidEpoch)
            g_msgs[i].rxTime = 0;
    }
}

// Evicted-message archive on the internal LittleFS. The RAM ring stays the
// small live window; eviction victims are appended here and the thread view
// pages back into them (8 at a time, a static staging buffer). Fixed
// fixed-size records — Msg + reply id — oldest first behind an "AVH2" magic;
// the newest kHistCap records are kept, compacting once the tail overgrows.
// Internal flash shares no bus with the LoRa cap, so paging can't stall RX.
#ifdef ADVUI_HIL
const char *kHistPath = "/advui_hil_hist.bin";
#else
const char *kHistPath = "/advui_hist.bin";
#endif
constexpr uint32_t kHistMagic = 0x32485641;   // "AVH2"
constexpr uint32_t kHistMagicV1 = 0x31485641; // "AVH1" — pre-v0.4.8 records, migrated in place
struct HistRec {
    Msg m;
    uint32_t rid;
};
static_assert(sizeof(HistRec) == 260, "AVH2 archive layout changed; bump the persistence format");
constexpr int kHistRec = (int)sizeof(HistRec); // Msg (256, no tail padding) + reply id
constexpr int kHistCap = 256;
constexpr int kHistSlack = 64;
constexpr int kHistWin = 8;   // staged slice size — RAM is precious on this board,
                              // so the window is small and slides often instead
constexpr int kHistSlide = 4; // half the slice: the anchor message survives every restage
// Lazily allocated on the first scroll into history: cardless-of-history users
// (and the tight boot window) never pay the ~1.5 KB. Kept once allocated.
Msg *g_arch = nullptr;
uint32_t *g_archReply = nullptr;
int g_histTotal = -1; // records on disk; -1 = not read yet

bool histStagingReady()
{
    if (g_arch)
        return true;
    g_arch = (Msg *)calloc(kHistWin, sizeof(Msg));
    g_archReply = (uint32_t *)calloc(kHistWin, sizeof(uint32_t));
    if (!g_arch || !g_archReply) { // heap too tight right now: history waits, UI lives
        free(g_arch);
        free(g_archReply);
        g_arch = nullptr;
        g_archReply = nullptr;
        return false;
    }
    return true;
}

bool histRewriteCurrent(int skip, int keep)
{
    if (skip < 0 || keep < 0)
        return false;
    auto in = FSCom.open(kHistPath, FILE_O_READ);
    if (!in)
        return false;
    uint32_t magic = 0;
    const size_t first = 4 + (size_t)skip * kHistRec;
    const size_t needed = first + (size_t)keep * kHistRec;
    bool ok = in.read((uint8_t *)&magic, 4) == 4 && magic == kHistMagic && in.size() >= needed && in.seek(first);
    SafeFile out(kHistPath, true);
    ok = ok && out.write((const uint8_t *)&kHistMagic, 4) == 4;
    HistRec rec;
    for (int i = 0; ok && i < keep; i++) {
        ok = in.read((uint8_t *)&rec, kHistRec) == kHistRec && normalizeStoredMsg(rec.m) &&
             out.write((const uint8_t *)&rec, kHistRec) == kHistRec;
    }
    in.close();
    if (!ok || !out.close()) {
        LOG_ERROR("advui: history rewrite failed; previous archive kept");
        return false;
    }
    g_histTotal = keep;
    return true;
}

bool histMigrateV1(); // widen pre-v0.4.8 archive records once

int histTotal()
{
    if (g_histTotal >= 0)
        return g_histTotal;
    g_histTotal = 0;
    auto f = FSCom.open(kHistPath, FILE_O_READ);
    if (f) {
        uint32_t magic = 0;
        bool got = f.read((uint8_t *)&magic, 4) == 4;
        const size_t fileSize = f.size();
        size_t records = 0;
        bool ok = got && magic == kHistMagic && checkedFixedRecordFile(fileSize, 4, kHistRec, &records);
        bool torn = got && magic == kHistMagic && !ok && fileSize >= 4;
        bool v1 = got && magic == kHistMagicV1;
        if (ok || torn)
            g_histTotal = (int)((fileSize - 4) / kHistRec);
        f.close();
        if (v1)
            histMigrateV1(); // sets g_histTotal itself
        else if (torn) {
            LOG_WARN("advui: repairing torn history tail after %d records", g_histTotal);
            histRewriteCurrent(0, g_histTotal);
        }
        else if (!ok)
            FSCom.remove(kHistPath); // unknown leftovers: start clean
    }
    return g_histTotal;
}

// One-shot: rewrite an AVH1 archive (MsgV3-sized records) into the current layout.
bool histMigrateV1()
{
    auto in = FSCom.open(kHistPath, FILE_O_READ);
    if (!in)
        return false;
    uint32_t magic = 0;
    size_t total = 0;
    const size_t oldRec = sizeof(MsgV3) + sizeof(uint32_t);
    bool ok = in.read((uint8_t *)&magic, 4) == 4 && magic == kHistMagicV1 &&
              checkedFixedRecordFile(in.size(), 4, oldRec, &total);
    const size_t keep = std::min(total, (size_t)kHistCap);
    const size_t skip = total - keep;
    ok = ok && in.seek(4 + skip * oldRec);
    if (!ok) {
        in.close();
        LOG_ERROR("advui: rejected corrupt legacy history archive");
        return false;
    }
    SafeFile out(kHistPath, true);
    ok = out.write((const uint8_t *)&kHistMagic, 4) == 4;
    MsgV3 om;
    uint32_t rid;
    int kept = 0;
    while (ok && kept < (int)keep) {
        ok = in.read((uint8_t *)&om, sizeof(om)) == (int)sizeof(om) && in.read((uint8_t *)&rid, 4) == 4;
        if (!ok)
            break;
        HistRec rec = {};
        msgFromV3(rec.m, om);
        rec.rid = rid;
        ok = ok && normalizeStoredMsg(rec.m) && out.write((const uint8_t *)&rec, sizeof(rec)) == sizeof(rec);
        kept++;
    }
    in.close();
    if (!ok || !out.close()) {
        LOG_ERROR("advui: history migration failed; legacy archive kept");
        return false;
    }
    g_histTotal = kept;
    return true;
}

bool histMatch(const Msg &m, bool isChan, uint8_t ch, uint32_t node)
{
    return isChan ? (m.to == NODENUM_BROADCAST && m.ch == ch)
                  : (m.to != NODENUM_BROADCAST && (m.from == node || m.to == node));
}

void histCompact()
{
    if (g_histTotal <= kHistCap)
        return;
    histRewriteCurrent(g_histTotal - kHistCap, kHistCap);
}

void histAppend(const Msg &m, uint32_t replyId)
{
    bool fresh = histTotal() == 0;
    if (!fresh) {
        auto check = FSCom.open(kHistPath, FILE_O_READ);
        const size_t expected = 4 + (size_t)g_histTotal * kHistRec;
        if (!check || check.size() != expected) {
            if (check)
                check.close();
            LOG_ERROR("advui: refusing append to malformed history archive");
            return;
        }
        check.close();
    } else {
        // A failed AVH1 migration deliberately leaves the legacy file intact.
        // Do not mistake that preserved archive for a fresh AVH2 file and erase it.
        auto check = FSCom.open(kHistPath, FILE_O_READ);
        if (check) {
            uint32_t magic = 0;
            const bool current = check.read((uint8_t *)&magic, 4) == 4 && magic == kHistMagic && check.size() == 4;
            check.close();
            if (!current) {
                LOG_ERROR("advui: refusing append while legacy history needs recovery");
                return;
            }
        }
    }
    auto f = FSCom.open(kHistPath, fresh ? FILE_O_WRITE : "a");
    if (!f)
        return;
    bool ok = !fresh || f.write((const uint8_t *)&kHistMagic, 4) == 4;
    Msg rec = m;
    rec.read = true; // history is read by definition
    if (rec.status == MSG_SENDING) { // forced out of an all-pending RAM ring: no future ACK can reach this record
        rec.status = MSG_FAILED;
        rec.err = 3; // routing timeout
    }
    if (rec.rxTime < kValidEpoch)
        rec.rxTime = 0; // an uptime stamp is meaningless once archived: honest blank
    ok = ok && normalizeStoredMsg(rec) && f.write((const uint8_t *)&rec, sizeof(rec)) == sizeof(rec) &&
         f.write((const uint8_t *)&replyId, 4) == 4;
    f.close();
    if (!ok) {
        LOG_ERROR("advui: history append failed");
        g_histTotal = -1;
        histTotal(); // trim a torn tail now so later evictions can append safely
        return;
    }
    if (++g_histTotal >= kHistCap + kHistSlack)
        histCompact();
}

int histCountFor(bool isChan, uint8_t ch, uint32_t node)
{
    int total = histTotal(), n = 0;
    if (!total)
        return 0;
    auto f = FSCom.open(kHistPath, FILE_O_READ);
    if (!f)
        return 0;
    if (!f.seek(4)) {
        f.close();
        return 0;
    }
    HistRec rec;
    for (int i = 0; i < total; i++) {
        if (f.read((uint8_t *)&rec, sizeof(rec)) != (int)sizeof(rec))
            break;
        if (normalizeStoredMsg(rec.m) && histMatch(rec.m, isChan, ch, node))
            n++;
    }
    f.close();
    return n;
}

// Stages into g_arch the `max` conversation messages that come right before the
// newest `skipNewest` matches. Returns the count.
int histLoadSlice(bool isChan, uint8_t ch, uint32_t node, int skipNewest, int max)
{
    if (!histStagingReady())
        return 0;
    int matches = histCountFor(isChan, ch, node);
    int end = matches - skipNewest; // exclusive match-index of the slice end
    int start = end - max;
    if (start < 0)
        start = 0;
    if (end <= 0)
        return 0;
    auto f = FSCom.open(kHistPath, FILE_O_READ);
    if (!f)
        return 0;
    if (!f.seek(4)) {
        f.close();
        return 0;
    }
    Msg m;
    uint32_t rid;
    int seen = 0, got = 0, total = histTotal();
    for (int i = 0; i < total && got < end - start; i++) {
        if (f.read((uint8_t *)&m, sizeof(m)) != (int)sizeof(m) || f.read((uint8_t *)&rid, 4) != 4)
            break;
        if (!normalizeStoredMsg(m))
            continue;
        if (!histMatch(m, isChan, ch, node))
            continue;
        if (seen >= start) {
            g_arch[got] = m;
            g_archReply[got] = rid;
            got++;
        }
        seen++;
    }
    f.close();
    return got;
}

bool histPurge(bool isChan, uint8_t ch, uint32_t node, bool *dropReacts)
{
    int total = histTotal();
    if (!total)
        return true;
    auto in = FSCom.open(kHistPath, FILE_O_READ);
    if (!in)
        return false;
    SafeFile out(kHistPath, true);
    bool ok = out.write((const uint8_t *)&kHistMagic, 4) == 4 && in.seek(4);
    HistRec rec;
    int kept = 0;
    for (int i = 0; ok && i < total; i++) {
        ok = in.read((uint8_t *)&rec, kHistRec) == kHistRec && normalizeStoredMsg(rec.m);
        if (!ok)
            break;
        if (histMatch(rec.m, isChan, ch, node)) {
            markReactionsForMessage(rec.m.id, rec.m.from, dropReacts);
            continue; // deleting the conversation drops its archive too
        }
        ok = out.write((const uint8_t *)&rec, kHistRec) == kHistRec;
        kept++;
    }
    in.close();
    if (!ok || !out.close()) {
        LOG_ERROR("advui: history purge failed; previous archive kept");
        return false;
    }
    g_histTotal = kept;
    return true;
}

// A reaction packet carries only reply_id, but packet IDs are unique per sender,
// not globally. Recover the target sender from the reaction's conversation so a
// phone/user may react independently to colliding IDs in different threads. A
// genuinely ambiguous collision within one thread remains targetFrom=0 and gets
// the conservative legacy/wildcard behaviour instead of guessing destructively.
uint32_t reactionTargetFrom(const meshtastic_MeshPacket &p, uint32_t me)
{
    const bool isChan = p.to == NODENUM_BROADCAST;
    const uint8_t ch = p.channel;
    const uint32_t node = p.from == me ? p.to : p.from;
    if (!p.decoded.reply_id || (!isChan && (!node || node == NODENUM_BROADCAST)))
        return 0;

    uint32_t targetFrom = 0;
    bool found = false, ambiguous = false;
    auto consider = [&](const Msg &m) {
        if (m.id != p.decoded.reply_id || !histMatch(m, isChan, ch, node))
            return;
        if (!found) {
            targetFrom = m.from;
            found = true;
        } else if (targetFrom != m.from) {
            ambiguous = true;
        }
    };
    for (int i = 0; i < g_msgCount; i++)
        consider(g_msgs[i]);

    const int total = histTotal();
    auto history = total > 0 ? FSCom.open(kHistPath, FILE_O_READ) : File();
    if (history && history.seek(4)) {
        HistRec rec;
        for (int i = 0; i < total; i++) {
            if (history.read((uint8_t *)&rec, sizeof(rec)) != (int)sizeof(rec) || !normalizeStoredMsg(rec.m)) {
                ambiguous = true; // do not claim exact identity from a partial/corrupt scan
                break;
            }
            consider(rec.m);
        }
        history.close();
    } else if (total > 0) {
        ambiguous = true;
    }
    return found && !ambiguous ? targetFrom : 0;
}

// Distinct-node ledger on flash: one {num, lastHeard} record per recently heard
// identity, reconciled against the hot store when the node list opens. The
// header counts entries heard in the last 24 h — the size of the LIVING mesh —
// with zero persistent RAM per node. At its much larger flash cap the oldest
// identities absent from the current hot DB are recycled instead of freezing
// the count forever.
#ifdef ADVUI_HIL
const char *kSeenPath = "/advui_hil_seen.bin";
#else
const char *kSeenPath = "/advui_seen.bin";
#endif
constexpr uint32_t kSeenMagic = 0x314E5641; // "AVN1"
struct SeenRec {
    uint32_t num;
    uint32_t last;
};
constexpr int kSeenCap = 2048; // at most ~16 KB of LittleFS
constexpr int kSeenReplaceCap = 32;
int g_seen24 = -1;             // nodes heard in the last 24 h (-1 = not computed yet)

// Hearings observed by US: the engine quality-gates last_heard to 0 after every
// reboot until a time source arrives, which froze the 24 h counter — but the raw
// system clock survives resets, so we stamp packet senders ourselves and fold
// them into the ledger on the next reconcile.
struct SeenPend {
    uint32_t num, t;
};
constexpr int kSeenPendCap = 16;
SeenPend g_seenPend[kSeenPendCap];
int g_seenPendN = 0;
uint8_t g_seenPendNext = 0;

void seenNote(uint32_t num)
{
    uint32_t now = getTime(false);
    if (!num || num == NODENUM_BROADCAST || !validStoredEpoch(now))
        return;
    for (int i = 0; i < g_seenPendN; i++)
        if (g_seenPend[i].num == num) {
            g_seenPend[i].t = now;
            return;
        }
    if (g_seenPendN < kSeenPendCap)
        g_seenPend[g_seenPendN++] = {num, now};
    else {
        // Keep the most recent 16 distinct senders. Replacing slot zero forever
        // retained 15 stale entries and lost every intermediate sender in a busy
        // post-boot minute, which made the 24-hour live-node count under-report.
        g_seenPend[g_seenPendNext] = {num, now};
        g_seenPendNext = (uint8_t)((g_seenPendNext + 1) % kSeenPendCap);
    }
}

void seenReconcile()
{
    uint32_t now = getTime(false);
    if (!nodeDB || !validStoredEpoch(now))
        return; // clockless: a 24 h window is meaningless right now
    int hotN = (int)nodeDB->getNumMeshNodes();
    if (hotN > 250)
        hotN = 250;
    uint32_t hotId[266]; // 250 hot slots + up to 16 pending self-observations
    uint32_t hotLast[266];
    bool inFile[266];
    bool observed[266] = {}; // packet senders are admitted before passive DB entries at saturation
    for (int i = 0; i < hotN; i++) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        hotId[i] = n ? n->num : 0;
        hotLast[i] = n ? n->last_heard : 0;
        if (hotLast[i] && !validStoredEpoch(hotLast[i]))
            hotLast[i] = 0;
        else if (hotLast[i] > now)
            hotLast[i] = now; // the local clock was corrected backwards
        inFile[i] = false;
    }
    for (int i = 0; i < g_seenPendN; i++) { // our own stamps beat quality-gated zeros
        int h = 0;
        while (h < hotN && hotId[h] != g_seenPend[i].num)
            h++;
        if (h < hotN) {
            if (g_seenPend[i].t > hotLast[h])
                hotLast[h] = g_seenPend[i].t;
            observed[h] = true;
        } else {
            hotId[hotN] = g_seenPend[i].num;
            hotLast[hotN] = g_seenPend[i].t;
            inFile[hotN] = false;
            observed[hotN] = true;
            hotN++;
        }
    }
    bool fresh = false;
    auto f = FSCom.open(kSeenPath, "r+");
    if (!f) {
        f = FSCom.open(kSeenPath, FILE_O_WRITE);
        if (!f)
            return;
        if (f.write((const uint8_t *)&kSeenMagic, 4) != 4) {
            f.close();
            FSCom.remove(kSeenPath);
            return;
        }
        fresh = true;
    } else {
        uint32_t magic = 0;
        size_t records = 0;
        if (f.read((uint8_t *)&magic, 4) != 4 || magic != kSeenMagic ||
            !checkedFixedRecordFile(f.size(), 4, sizeof(SeenRec), &records) || records > kSeenCap) {
            f.close();
            FSCom.remove(kSeenPath); // derived cache: rebuild it from the hot DB below
            f = FSCom.open(kSeenPath, FILE_O_WRITE);
            if (!f || f.write((const uint8_t *)&kSeenMagic, 4) != 4) {
                if (f)
                    f.close();
                FSCom.remove(kSeenPath);
                return;
            }
            fresh = true;
        }
    }
    int active = 0, total = 0;
    bool ioOk = true;
    uint32_t dayAgo = now - 86400;
    OldestSlot replace[kSeenReplaceCap];
    size_t replaceN = 0;
    if (!fresh) {
        SeenRec chunk[32];
        uint32_t pos = 4;
        for (;;) {
            if (!f.seek(pos)) {
                ioOk = false;
                break;
            }
            const int bytes = (int)f.read((uint8_t *)chunk, sizeof(chunk));
            if (bytes % (int)sizeof(SeenRec)) {
                ioOk = false;
                break;
            }
            int got = bytes / (int)sizeof(SeenRec);
            if (got <= 0)
                break;
            bool dirty = false;
            for (int i = 0; i < got; i++) {
                if (!chunk[i].num || chunk[i].num == NODENUM_BROADCAST) {
                    ioOk = false;
                    break;
                }
                const uint32_t saneLast = !chunk[i].last || !validStoredEpoch(chunk[i].last)
                                                  ? 0
                                                  : std::min(chunk[i].last, now);
                if (saneLast != chunk[i].last) {
                    chunk[i].last = saneLast;
                    dirty = true;
                }
                bool inHot = false;
                for (int h = 0; h < hotN; h++)
                    if (hotId[h] && hotId[h] == chunk[i].num) {
                        inHot = true;
                        inFile[h] = true;
                        if (hotLast[h] > chunk[i].last) {
                            chunk[i].last = hotLast[h];
                            dirty = true;
                        }
                        break;
                    }
                if (!inHot)
                    considerOldestSlot(replace, kSeenReplaceCap, &replaceN,
                                       pos + (uint32_t)i * sizeof(SeenRec), chunk[i].last);
                if (chunk[i].last >= dayAgo)
                    active++;
            }
            if (!ioOk)
                break;
            if (dirty) {
                ioOk = f.seek(pos) && f.write((const uint8_t *)chunk, got * sizeof(SeenRec)) ==
                                                (size_t)got * sizeof(SeenRec);
                if (!ioOk)
                    break;
            }
            total += got;
            pos += got * sizeof(SeenRec);
        }
    }
    if (!ioOk) {
        f.close();
        FSCom.remove(kSeenPath); // derived cache will be rebuilt on the next reconciliation
        g_seen24 = -1;
        return;
    }
    // nodes the ledger has never met: append them (the file only ever grows by
    // genuinely new identities, so entries stay distinct by construction).
    // Direct packet observations go first; a large passive node DB must not
    // delay the sender that triggered this reconciliation.
    ioOk = f.seek(f.size());
    for (int pass = 0; pass < 2 && ioOk && total < kSeenCap; pass++)
        for (int h = 0; h < hotN && total < kSeenCap; h++) {
            if ((pass == 0) != observed[h] || !hotId[h] || !hotLast[h] || inFile[h])
                continue;
            SeenRec r = {hotId[h], hotLast[h]};
            if (f.write((const uint8_t *)&r, sizeof(r)) != sizeof(r)) {
                ioOk = false;
                break;
            }
            inFile[h] = true;
            total++;
            if (r.last >= dayAgo)
                active++;
        }
    // Once the cap is full, recycle the oldest identities that are no longer in
    // the current hot DB. Work is deliberately bounded: later reconciliations
    // admit further newcomers without a large stack allocation or a full-file
    // rewrite. Zero-timestamp DB placeholders never evict an actually heard node.
    for (int pass = 0; pass < 2 && ioOk && replaceN; pass++)
        for (int h = 0; ioOk && h < hotN && replaceN; h++) {
            if ((pass == 0) != observed[h] || !hotId[h] || !hotLast[h] || inFile[h])
                continue;
            size_t oldest = 0;
            for (size_t i = 1; i < replaceN; i++)
                if (replace[i].stamp < replace[oldest].stamp ||
                    (replace[i].stamp == replace[oldest].stamp && replace[i].position < replace[oldest].position))
                    oldest = i;
            const bool wasActive = replace[oldest].stamp >= dayAgo;
            SeenRec r = {hotId[h], hotLast[h]};
            ioOk = f.seek(replace[oldest].position) && f.write((const uint8_t *)&r, sizeof(r)) == sizeof(r);
            if (!ioOk)
                break;
            if (wasActive != (r.last >= dayAgo))
                active += r.last >= dayAgo ? 1 : -1;
            inFile[h] = true;
            replace[oldest] = replace[--replaceN];
        }
    f.close();
    if (!ioOk) {
        FSCom.remove(kSeenPath);
        g_seen24 = -1;
        return;
    }
    g_seen24 = active;
    // Consume observations only after the ledger is safely reconciled. A
    // transient open/write failure must leave them available for the next
    // attempt: these timestamps specifically cover nodes whose engine-owned
    // last_heard was quality-gated to zero.
    g_seenPendN = 0;
    g_seenPendNext = 0;
}

// Default node ordering: favourites first, then nodes we have a conversation with,
// then everyone else by hop distance (nearest first; unknown hops last). Ties are
// broken by most-recently-heard.
bool nodeLess(uint16_t a, uint16_t b)
{
    const meshtastic_NodeInfoLite *na = nodeDB->getMeshNodeByIndex(a);
    const meshtastic_NodeInfoLite *nb = nodeDB->getMeshNodeByIndex(b);
    bool ua = hasUnreadFrom(na->num), ub = hasUnreadFrom(nb->num);
    if (ua != ub) // unread pinned on top in every mode — it's what the list is for
        return ua;
    int ha = na->has_hops_away ? na->hops_away : 255;
    int hb = nb->has_hops_away ? nb->hops_away : 255;
    switch (g_nodeSort) {
    case 1: // freshest first: who's alive on the mesh right now
        if (na->last_heard != nb->last_heard)
            return na->last_heard > nb->last_heard;
        break;
    case 2: { // alphabetical, by the same name the row shows
        int c = strcasecmp(nodeName(na), nodeName(nb));
        if (c != 0)
            return c < 0;
        break;
    }
    case 3: // nearest first, unknown distance last
        if (ha != hb)
            return ha < hb;
        if (na->last_heard != nb->last_heard)
            return na->last_heard > nb->last_heard;
        break;
    default: { // smart: favourites, then nearest, then freshest
        bool fa = isFav(na), fb = isFav(nb);
        if (fa != fb)
            return fa;
        if (ha != hb)
            return ha < hb;
        if (na->last_heard != nb->last_heard)
            return na->last_heard > nb->last_heard;
        break;
    }
    }
    return na->num < nb->num; // stable total order either way
}

// Trim s in place until it fits within budget px in the current font.
void fitWidth(lgfx::LGFXBase *g, char *s, int budget)
{
    while (s[0] && g->textWidth(s) > budget)
        utf8TrimLast(s);
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

// Two-letter role code: one per distinct role (the old 3-letter tags collapsed
// every client and every router variant together, hiding mute/hidden/base and
// router/late). Two chars leaves a column free for the unmessageable marker.
const char *roleTag(const meshtastic_NodeInfoLite *n)
{
    switch (nodeRole(n)) {
    case meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE:
        return "CM";
    case meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN:
        return "CH";
    case meshtastic_Config_DeviceConfig_Role_CLIENT_BASE:
        return "CB";
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
        return "RE";
    case meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT:
        return "RC";
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
        return "RL";
    case meshtastic_Config_DeviceConfig_Role_REPEATER:
        return "RO";
    case meshtastic_Config_DeviceConfig_Role_TRACKER:
        return "TR";
    case meshtastic_Config_DeviceConfig_Role_TAK_TRACKER:
        return "TT";
    case meshtastic_Config_DeviceConfig_Role_SENSOR:
        return "SE";
    case meshtastic_Config_DeviceConfig_Role_TAK:
        return "TK";
    case meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND:
        return "LF";
    default:
        return "CL";
    }
}

const char *regionName(meshtastic_Config_LoRaConfig_RegionCode r)
{
    // Keep this aligned with RadioInterface::regions in the pinned engine, not
    // merely the wider protobuf enum: 27..33 are schema-reserved but have no
    // regional radio definition in Meshtastic 2.7.26.
    static const char *const names[] = {
        "UNSET",  "US",      "EU_433", "EU_868", "CN",      "JP",      "ANZ",
        "KR",     "TW",      "RU",     "IN",     "NZ_865",  "TH",      "LORA_24",
        "UA_433", "UA_868",  "MY_433", "MY_919", "SG_923",  "PH_433",  "PH_868",
        "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865",  "BR_902",
    };
    const int value = (int)r;
    if (value >= 0 && value < (int)(sizeof(names) / sizeof(names[0])))
        return names[value];
    static char unknown[8];
    snprintf(unknown, sizeof(unknown), "R%d", value);
    return unknown;
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
const EnumOpt kRegionOpts[] = {
    {"UNSET", 0},   {"US", 1},       {"EU_433", 2},  {"EU_868", 3}, {"CN", 4},      {"JP", 5},
    {"ANZ", 6},     {"KR", 7},       {"TW", 8},      {"RU", 9},     {"IN", 10},     {"NZ_865", 11},
    {"TH", 12},     {"LORA_24", 13}, {"UA_433", 14}, {"UA_868", 15}, {"MY_433", 16}, {"MY_919", 17},
    {"SG_923", 18}, {"PH_433", 19},  {"PH_868", 20}, {"PH_915", 21}, {"ANZ_433", 22}, {"KZ_433", 23},
    {"KZ_863", 24}, {"NP_865", 25},  {"BR_902", 26},
};
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
// Node-list order (community ask): smart mixes favourites/hops/freshness, the
// rest are single-criterion. Unread stays pinned on top in every mode.
const EnumOpt kSortOpts[] = {{"Smart", 0}, {"Last heard", 1}, {"Name", 2}, {"Hops", 3}};
const EnumOpt kNameOpts[] = {{"Long", 0}, {"Short", 1}};
// One row, not two: nobody wants to switch a panel on and then go hunting for which
// way up it is. The value is the LovyanGFX rotation, with 0 standing for "no panel" —
// rotation 0 is portrait, which is never offered, so it is free to mean off. Only
// landscape orientations are listed: 1 and 3 are the two ways round, 5 and 7 the same
// mirrored. Portrait would squeeze a 240x135 frame into a stripe down the middle.
const EnumOpt kExtOpts[] = {{"Off", 0}, {"Normal", 3}, {"Flip", 1}, {"Mirror", 7}, {"Mirror flip", 5}};
constexpr int kExtCount = 5;
constexpr int kNameCount = 2;
// Only offered when there's no font partition: loading from the card costs
// ~32 KB of heap (see AdvFont.h), enough to cost this board its networking.
const EnumOpt kFontOpts[] = {{"Off (saves RAM)", 0}, {"SD card (-32KB RAM)", 1}};
constexpr int kFontCount = 2;
constexpr int kSortCount = (int)(sizeof(kSortOpts) / sizeof(kSortOpts[0]));

// Last percentage read while NOT charging. Shown in place of the live reading
// whenever USB is in — this board has no fuel gauge, so under charge the
// terminal voltage sits near full and the OCV table reads ~100% no matter the
// real charge (the "100% on USB, 58% on battery" surprise). Persisted so a
// boot on USB shows the last honest figure instead of a fresh lie. -1 = unknown.
int g_battRest = -1;

bool saveUiCfg()
{
    SafeFile f(kUiCfgPath, true);
    bool ok = f.write(&g_nodeSort, 1) == 1;
    int8_t b = (int8_t)g_battRest; // -1 when never sampled off-charge
    ok = ok && f.write((uint8_t *)&b, 1) == 1;
    ok = ok && f.write(&g_nameShort, 1) == 1;
    uint8_t on = g_extRot ? 1 : 0; // v0.5.0 wrote the pair; keep the shape, derive both
    ok = ok && f.write(&on, 1) == 1;
    ok = ok && f.write(&g_extRot, 1) == 1;
    ok = ok && f.write(&g_favNodeCount, 1) == 1;
    for (uint8_t i = 0; ok && i < g_favNodeCount; i++)
        ok = f.write((uint8_t *)&g_favNodes[i], 4) == 4;
    if (!ok || !f.close()) {
        LOG_ERROR("advui: UI config save failed; previous file kept");
        return false;
    }
    return true;
}

void persistUiCfg()
{
    g_uiCfgDirty = !saveUiCfg();
    g_lastUiCfgSaveMs = millis();
}

void loadUiCfg()
{
    auto f = FSCom.open(kUiCfgPath, FILE_O_READ);
    if (!f)
        return;
    uint8_t v = 0;
    if (f.read(&v, 1) == 1 && v < kSortCount)
        g_nodeSort = v;
    int8_t b = -1;
    if (f.read((uint8_t *)&b, 1) == 1 && b >= 0 && b <= 100)
        g_battRest = b;
    uint8_t ns = 0;
    if (f.read(&ns, 1) == 1 && ns <= 1)
        g_nameShort = ns;
    uint8_t es = 0, er = 0;
    const bool haveOn = f.read(&es, 1) == 1;
    const bool haveRot = f.read(&er, 1) == 1;
    if (haveOn && es) // a v0.5.0 file says on; take its orientation when the menu still offers it
        g_extRot = (haveRot && (er == 1 || er == 3 || er == 5 || er == 7)) ? er : 3;
    uint8_t fc = 0;
    g_favNodeCount = 0; // replace, never append: a second call would run off the array
    if (f.read(&fc, 1) == 1) {
        for (uint8_t i = 0; i < fc && g_favNodeCount < kMaxFavNodes; i++) {
            uint32_t num = 0;
            if (f.read((uint8_t *)&num, 4) != 4)
                break;
            if (num)
                g_favNodes[g_favNodeCount++] = num;
        }
    }
    f.close();
}
const EnumOpt kRoleOpts2[] = {
    {"Client", 0},        {"Client Mute", 1},  {"Router", 2},       {"Router Client", 3},
    {"Repeater", 4},      {"Tracker", 5},      {"Sensor", 6},       {"TAK", 7},
    {"Client Hidden", 8}, {"Lost & Found", 9}, {"TAK Tracker", 10}, {"Router Late", 11},
    {"Client Base", 12},
};
const EnumOpt kHopOpts[] = {{"1", 1}, {"2", 2}, {"3", 3}, {"4", 4}, {"5", 5}, {"6", 6}, {"7", 7}};
// Include each active region limit plus the common hardware/user values. In
// particular, 26 dBm is a deliberate US-profile value used by existing nodes;
// opening this picker must never turn it into the old fallback value.
const EnumOpt kPowerOpts[] = {
    {"max (region)", 0}, {"2 dBm", 2},   {"5 dBm", 5},   {"8 dBm", 8},   {"10 dBm", 10},
    {"11 dBm", 11},     {"13 dBm", 13}, {"14 dBm", 14}, {"17 dBm", 17}, {"19 dBm", 19},
    {"20 dBm", 20},     {"22 dBm", 22}, {"23 dBm", 23}, {"24 dBm", 24}, {"26 dBm", 26},
    {"27 dBm", 27},     {"30 dBm", 30}, {"36 dBm", 36},
};
const EnumOpt kRebroadOpts[] = {{"All", 0},        {"All skip decode", 1}, {"Local only", 2},
                                {"Known only", 3}, {"Core ports only", 5}, {"None", 4}};
const EnumOpt kGpsOpts[] = {{"On", 1}, {"Off", 0}}; // config.position.gps_mode ENABLED / DISABLED
constexpr int kRegionCount = sizeof(kRegionOpts) / sizeof(kRegionOpts[0]);
constexpr int kPresetCount = sizeof(kPresetOpts) / sizeof(kPresetOpts[0]);
constexpr int kUtcCount = sizeof(kUtcOpts) / sizeof(kUtcOpts[0]);
constexpr int kRadioCount = sizeof(kRadioOpts) / sizeof(kRadioOpts[0]);
constexpr int kScreenCount = sizeof(kScreenOpts) / sizeof(kScreenOpts[0]);
constexpr int kRoleCount = sizeof(kRoleOpts2) / sizeof(kRoleOpts2[0]);
constexpr int kHopCount = sizeof(kHopOpts) / sizeof(kHopOpts[0]);
constexpr int kPowerCount = sizeof(kPowerOpts) / sizeof(kPowerOpts[0]);
constexpr int kRebroadCount = sizeof(kRebroadOpts) / sizeof(kRebroadOpts[0]);
constexpr int kGpsCount = sizeof(kGpsOpts) / sizeof(kGpsOpts[0]);

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
    case 10: return {kSortOpts, kSortCount, "Node sort"};
    case 11: return {kNameOpts, kNameCount, "Node names"};
    case 13: return {kExtOpts, kExtCount, "2nd screen"};  // off, or how it is mounted
    case 12: return {kFontOpts, kFontCount, "Unicode font"};
    case 4: return {kScreenOpts, kScreenCount, "Screen off"};
    case 5: return {kRoleOpts2, kRoleCount, "Role"};
    case 6: return {kHopOpts, kHopCount, "Hop limit"};
    case 7: return {kPowerOpts, kPowerCount, "TX power"};
    case 8: return {kRebroadOpts, kRebroadCount, "Rebroadcast"};
    case 9: return {kGpsOpts, kGpsCount, "GPS"};
    default: return {kRadioOpts, kRadioCount, "Radio"};
    }
}

int optIndex(const EnumOpt *opts, int cnt, int value)
{
    for (int i = 0; i < cnt; i++)
        if (opts[i].value == value)
            return i;
    return -1; // never let an unknown current value silently become the first option
}

// Text-editor (MODE_SETNAME) targets: 0 long name, 1 short name, 2 frequency, 3 channel, 4 clock.
// --- WiFi / MQTT sub-settings (MODE_NETPAGE) -----------------------------------
// Two pages of fields backed by the stock config/moduleConfig; no engine changes.
struct NetField {
    const char *label;
    bool isBool;
};
const NetField kWifiFields[] = {{"WiFi on (turns BT off)", true}, {"Network name", false}, {"Password", false}};
// Uplink/downlink live on the CHANNEL, not the module — but MQTT is dead without
// them (the engine's thread shuts itself down for good unless some channel opts
// in), and the stock menu that carried these toggles is compiled out here. So
// they belong on this page: without them MQTT simply cannot be set up on-device.
const NetField kMqttFields[] = {{"MQTT on", true},        {"Server", false},      {"Username", false},
                                {"Password", false},      {"Root topic", false},  {"Encryption", true},
                                {"TLS", true},            {"Uplink (send)", true}, {"Downlink (receive)", true}};
int netFieldCount(int page) { return page == 0 ? 3 : 9; }
const NetField *netFields(int page) { return page == 0 ? kWifiFields : kMqttFields; }

bool netGetBool(int page, int i)
{
#ifdef ADVUI_SCREENSHOT
    if (demoNetActive()) {
        if (page == 0)
            return true;
        return i == 0 || i == 5; // MQTT + encryption on; TLS/uplink/downlink off
    }
#endif
    if (page == 0)
        return config.network.wifi_enabled;
    if (i == 0)
        return moduleConfig.mqtt.enabled;
    if (i == 5)
        return moduleConfig.mqtt.encryption_enabled;
    if (i == 6)
        return moduleConfig.mqtt.tls_enabled;
    // 7/8: primary channel's MQTT opt-in
    meshtastic_Channel ch = channels.getByIndex(0);
    return i == 7 ? ch.settings.uplink_enabled : ch.settings.downlink_enabled;
}
void netSetBool(int page, int i, bool v)
{
    if (page == 0)
        config.network.wifi_enabled = v;
    else if (i == 0)
        moduleConfig.mqtt.enabled = v;
    else if (i == 5)
        moduleConfig.mqtt.encryption_enabled = v;
    else if (i == 6)
        moduleConfig.mqtt.tls_enabled = v;
    else { // channel flags: written through the channel API, not moduleConfig
        meshtastic_Channel ch = channels.getByIndex(0);
        if (i == 7)
            ch.settings.uplink_enabled = v;
        else
            ch.settings.downlink_enabled = v;
        channels.setChannel(ch);
        channels.onConfigChanged();
        if (nodeDB)
            nodeDB->saveToDisk(SEGMENT_CHANNELS);
    }
}
const char *netGetText(int page, int i)
{
#ifdef ADVUI_SCREENSHOT
    if (demoNetActive()) {
        if (page == 0)
            return i == 1 ? "HomeWiFi" : "s3cret-pass";
        switch (i) {
        case 1: return ""; // default broker
        case 2: return "";
        case 3: return "";
        default: return "msh";
        }
    }
#endif
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
    return t == 1 ? 4 : t == 2 ? 9 : t == 3 ? 11 : t == 4 ? 5 : 24;
}
const char *editTitle(int t)
{
    if (t >= 20) {
        int p, i;
        netTextDecode(t, p, i);
        return netFields(p)[i].label;
    }
    return t == 1   ? "Set short name"
           : t == 2 ? "Set frequency (MHz)"
           : t == 3 ? "Set channel name"
           : t == 4 ? "Set clock (HH:MM local)"
                    : "Set long name";
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
#ifdef ADVUI_SCREENSHOT
    if (g_demoActive) {
        strcpy(abuf, "now");
        acol = 0x07E0;
    } else
#endif
    if (!n->last_heard) {
        strcpy(abuf, "?");
        acol = 0x630C;
    } else {
        const uint32_t now = getTime(false);
        if (!validStoredEpoch(now) || now < n->last_heard) {
            // No valid clock (or it's behind the stored stamp): sinceLastSeen() clamps
            // the negative delta to 0 and every flash-loaded node would lie "now".
            // Field report: GPS on with no fix showed the whole list as "now" while
            // Clock honestly said "not set". A dim "?" is the honest answer.
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
    }
    g->setTextColor(acol);
    g->setCursor(xAgeR - (int)strlen(abuf) * cw, metaY);
    g->print(abuf);

    // Role code, with a red "x" in the column the two-letter tag freed when the
    // node can't be messaged (unmessagable) — you can't DM it, so the list says so.
    int roleX = xRole;
    if (nodeIsUnmessagable(n)) {
        g->setTextColor(0xF800); // red
        g->setCursor(roleX, metaY);
        g->print("x");
        roleX += cw;
    }
    g->setTextColor(0x8410); // gray
    g->setCursor(roleX, metaY);
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

// Own battery as short text + colour ("87%", "USB" when there is no battery).
// Charging is the bolt drawn beside it, not a "+" glued to the number: at this
// size a lone plus reads as part of the percentage.
bool batteryCharging();

void batteryText(char *out, size_t cap, uint16_t &col)
{
#ifdef ADVUI_SCREENSHOT
    if (g_demoActive) {
        snprintf(out, cap, "87%%");
        col = 0x07E0;
        return;
    }
#endif
    if (powerStatus && powerStatus->getHasBattery()) {
        int pct = powerStatus->getBatteryChargePercent();
        if (pct > 100)
            pct = 100;
        // Under charge the voltage-derived % is meaningless (see g_battRest); show
        // the last honest off-charge figure instead so it doesn't jump to a fake full.
        if (batteryCharging() && g_battRest >= 0)
            pct = g_battRest;
        snprintf(out, cap, "%d%%", pct);
        col = pct > 50 ? 0x07E0 : (pct > 20 ? 0xFFE0 : 0xF800);
    } else {
        snprintf(out, cap, "USB");
        col = 0x9CD3;
    }
}

// This board has no charge-detect hardware — only a voltage divider on
// BATTERY_PIN. Three imperfect signals, OR'ed:
//  - the stock flag is `getBattVoltage() > chargingVolt` where chargingVolt =
//    (OCV[0]+10) = exactly 4200; a dumb charger that parks the rail AT 4.200 V
//    (field-observed) misses the strict '>' forever;
//  - HWCDC::isPlugged() sees only a USB HOST — true for a computer, false for
//    wall chargers and data-less cables;
//  - so also accept a reading a hair under that threshold (>= 4195).
//
// That last one is a hint, not a verdict. 4195 sits 5 mV above OCV[0] = 4190,
// the table's own 100 % point, so a pack that simply finished charging trips it
// with no cable in sight — reported from the field, reproduced here. Raising the
// bar does not help: a charger floats anywhere in 4233-4281 and a half-full pack
// under charge read 4253, so the two bands overlap and no threshold separates
// them. Hence nothing that matters may hang on this answer — see drawBattery().
bool batteryCharging()
{
#ifdef ADVUI_SCREENSHOT
    if (g_demoActive)
        return false;
#endif
    if (!powerStatus || !powerStatus->getHasBattery())
        return false;
    return powerStatus->getIsCharging() || HWCDC::isPlugged() || powerStatus->getBatteryVoltageMv() >= 4195;
}

// Battery block right-aligned on `right`; returns the width it took so the
// caller can park the unread badge clear of it.
//
// The number is always drawn; the bolt joins it when batteryCharging() fires.
// Under charge the terminal voltage tracks the charger rail rather than the cell
// — a half-full pack read 4253 while a genuinely full one floats in 4233-4281
// (both field-measured) — so the live percentage is worthless there and
// batteryText() substitutes the last off-charge figure instead. That substitution
// is what makes a number safe to show at all times, and showing it at all times
// is what keeps a misfired bolt cheap: the guess decides whether a small glyph
// appears, never whether the user can see their charge level. The earlier
// bolt-only header cost them the number every time a full pack tripped the
// threshold on battery, which is most of the time right after a charge.
int drawBattery(lgfx::LGFXBase *g, int right, int y)
{
    int bolt = 0;
    if (batteryCharging()) {
        drawBoltGlyph(g, right - 5, y - 1, 0xFFE0);
        bolt = 6; // 5 px glyph + 1 px gap
    }
    char buf[10];
    uint16_t col;
    batteryText(buf, sizeof(buf), col);
    int tw = g->textWidth(buf);
    g->setTextColor(col);
    g->setCursor(right - bolt - tw, y);
    g->print(buf);
    return tw + bolt;
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

// The manual clock's date problem: HH:MM entry needs a date, and the build date
// (below) can be days behind the wall calendar. Setting RTC into the past then
// stamps every node/message with old dates, and the moment a real time source
// syncs, the whole node list ages by the build's age at once (field report:
// "все ноды разом стали 4 дня назад" — exactly the firmware's age). So remember
// the last honestly-known DAY in flash (written at most once a day, spares the
// flash) and base manual entry on it: right after the first real sync in the
// device's life, the manual date is correct to within a day.
#ifdef ADVUI_HIL
const char *kEpochPath = "/advui_hil_epoch.bin";
#else
const char *kEpochPath = "/advui_epoch.bin";
#endif

void epochNote()
{
    uint32_t now = getTime(false);
    if (!validStoredEpoch(now))
        return;
    static uint32_t savedDay = 0;
    if (!savedDay) { // first call this boot: don't rewrite a same-day record
        auto f = FSCom.open(kEpochPath, FILE_O_READ);
        if (f) {
            uint32_t v = 0;
            const bool ok = f.size() == sizeof(v) && f.read((uint8_t *)&v, sizeof(v)) == sizeof(v) &&
                            validStoredEpoch(v);
            f.close();
            if (ok)
                savedDay = v / 86400;
        }
    }
    if (now / 86400 == savedDay)
        return;
    // This value is only a best-effort date hint. A torn/missing record is
    // already rejected by the loader and falls back to the build date, so a
    // full SafeFile temp/readback/rename transaction buys no recoverability.
    // Avoiding the temporary file and rename keeps this tiny advisory write out
    // of LittleFS's heavier atomic path while messages spill into their archive.
    auto f = FSCom.open(kEpochPath, FILE_O_WRITE);
    const bool wrote = f && f.write((uint8_t *)&now, sizeof(now)) == sizeof(now);
    if (f)
        f.close();
    auto check = wrote ? FSCom.open(kEpochPath, FILE_O_READ) : File();
    uint32_t saved = 0;
    const bool verified = check && check.size() == sizeof(saved) &&
                          check.read((uint8_t *)&saved, sizeof(saved)) == sizeof(saved) && saved == now;
    if (check)
        check.close();
    if (verified) {
        savedDay = now / 86400;
    } else
        LOG_ERROR("advui: epoch save failed; previous file kept");
}

uint32_t savedDateEpoch()
{
    auto f = FSCom.open(kEpochPath, FILE_O_READ);
    if (!f)
        return 0;
    uint32_t v = 0;
    const bool ok = f.size() == sizeof(v) && f.read((uint8_t *)&v, sizeof(v)) == sizeof(v);
    f.close();
    return ok && validStoredEpoch(v) ? (v / 86400) * 86400 : 0; // midnight of the last known day
}

// Compact "HH:MM " prefix for a message's epoch time (local), or "" when the clock
// wasn't set when the message was stamped (avoids showing bogus 00:xx times).
// Midnight UTC of the firmware build date. The manual clock (Settings > Device >
// Clock) asks only for HH:MM, so the date part comes from here: the displayed
// time is exact, the date is at worst a little stale, and any real time source
// still outranks it and corrects the full date.
uint32_t buildDateEpoch()
{
    static const char mn[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
    const char *hit = strstr(mn, m);
    int mo = hit ? (int)(hit - mn) / 3 : 0;            // 0-based month
    int d = atoi(__DATE__ + 4), y = atoi(__DATE__ + 7);
    y -= mo <= 1; // days-from-civil (Hinnant): shift Jan/Feb to the previous March-year
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (mo <= 1 ? mo + 10 : mo - 2) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(((int64_t)era * 146097 + doe - 719468) * 86400);
}

void msgTimePrefix(uint32_t rxTime, int32_t tzOff, char *out, int cap)
{
    if (!validStoredEpoch(rxTime)) { // no valid RTC at stamp time (see kValidEpoch)
        out[0] = 0;
        return;
    }
    const int64_t local = (int64_t)rxTime + tzOff;
    snprintf(out, cap, "%02u:%02u ", (unsigned)((local / 3600) % 24), (unsigned)((local / 60) % 60));
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

bool cpInvisible(uint32_t cp);

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
        Utf8Rune rune = utf8Decode(s + consumed);
        int tlen = elen > 0 ? elen : rune.bytes;
        int tw;
        if (elen > 0) {
            tw = em->width + 2;
        } else {
            char cb[5];
            if (rune.valid) {
                memcpy(cb, s + consumed, rune.bytes);
                cb[rune.bytes] = 0;
            } else {
                cb[0] = '?';
                cb[1] = 0;
            }
            tw = cpInvisible(rune.codepoint) ? 0 : g->textWidth(cb);
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

// What the embedded 9x15 font actually has: ASCII plus Cyrillic. Everything
// else goes to the unicode font when present (and to the tofu box when not).
bool flashFontCovers(uint32_t cp)
{
    return cp < 0x7F || (cp >= 0x400 && cp <= 0x45F) || cp == 0x490 || cp == 0x491;
}

// Codepoints that shape neighbours but have no visual of their own — phone
// emoji lean on them heavily ("❤️" = U+2764 + FE0F). Unifont has literal
// glyphs for some, so without this they'd draw as stray boxes mid-text.
bool cpInvisible(uint32_t cp)
{
    return cp == 0xFE0E || cp == 0xFE0F                 // variation selectors (text/emoji style)
           || cp == 0x200D                              // zero-width joiner (family/profession combos)
           || (cp >= 0x1F3FB && cp <= 0x1F3FF);         // skin-tone modifiers
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
            Utf8Rune rune = utf8Decode(s);
            char cb[5];
            if (rune.valid) {
                memcpy(cb, s, rune.bytes);
                cb[rune.bytes] = 0;
            } else {
                cb[0] = '?';
                cb[1] = 0;
            }
            if (!cpInvisible(rune.codepoint)) {
                int gw = !flashFontCovers(rune.codepoint) ? sdGlyphWidth(rune.codepoint) : 0;
                w += gw ? gw + 1 : g->textWidth(cb);
            }
            s += rune.bytes;
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
            Utf8Rune rune = utf8Decode(s);
            char cb[5];
            if (rune.valid) {
                memcpy(cb, s, rune.bytes);
                cb[rune.bytes] = 0;
            } else {
                cb[0] = '?';
                cb[1] = 0;
            }
            if (cpInvisible(rune.codepoint)) {
                s += rune.bytes;
                continue;
            }
            uint8_t bits[32];
            int gw = !flashFontCovers(rune.codepoint) ? sdGlyph(rune.codepoint, bits) : 0;
            if (gw) { // unicode glyph, aligned onto the emoji band
                g->drawBitmap(cx, y + (17 - 16) / 2 + emojiDy, bits, gw, 16, color);
                cx += gw + 1;
            } else {
                g->setTextColor(color);
                g->setCursor(cx, y);
                g->print(cb);
                cx += g->textWidth(cb);
            }
            s += rune.bytes;
        }
    }
    return cx;
}

// Copies at most `maxBytes` of s into out without splitting a UTF-8 sequence.
void utf8Copy(char *out, const char *s, int maxBytes)
{
    utf8CopyValid(out, (size_t)maxBytes + 1, s);
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
        Utf8Rune rune = utf8Decode(p);
        int len = rune.bytes;
        bool ok = false;
        if (rune.valid && len == 1) {
            ok = (unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F;
        } else if (rune.valid && len == 2) { // the font also covers Cyrillic
            ok = rune.codepoint >= 0x400 && rune.codepoint <= 0x45F;
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
    if (n && nodeShortName(n)[0]) {
        snprintf(out, cap, "%s", nodeShortName(n));
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
    // With the panel rail down the CPU at 240 MHz is the biggest remaining
    // draw. Idle in the pocket runs fine at 80: on the S3 the APB bus stays at
    // 80 MHz regardless, so LoRa SPI, I2C and BLE timing are untouched — only
    // the core slows. On USB we're charging anyway, and full speed keeps the
    // dev/serial tooling snappy.
    if (!HWCDC::isPlugged()) {
        setCpuFrequencyMhz(80);
        bleAdvSlow(true); // a pocket device doesn't need 100 ms discoverability
        LOG_INFO("advui: screen sleep, cpu 80 MHz");
    } else
        LOG_INFO("advui: screen sleep (on USB, cpu stays 240)");
}

void AdvUI::screenWake()
{
    setCpuFrequencyMhz(240); // full speed first — the panel re-init is timing-heavy
    bleAdvSlow(false);
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
    setLedRgb(90, 0, 0);
    delay(250);
    setLedRgb(0, 90, 0);
    delay(250);
    setLedRgb(0, 0, 90);
    delay(250);
    setLedRgb(0, 0, 0);
#endif

    loadRadioCfg();
    loadUiCfg();
    kb.begin();
    kbMissing = !kb.present(); // classic Cardputer / dead controller -> notice screen
    if (!kbMissing)
        LOG_INFO("advui: keyboard ready");

    sdFontInit(); // flash font partition (or /unifont.bin on SD) unlocks full-BMP text
    loadMsgs(); // restore the saved conversation from flash
    if (g_radioCompanion) {
        // The companion is a terminal to another node: free the RAM the stock BT
        // peripheral would take (no phone connects to us) — SMP pairing on the
        // central link needs that heap.
        config.bluetooth.enabled = false;
        bleCompanionInit(); // BLE central for the companion link (scan/pair/connect)
        extInit();          // external panel, if the user turned it on (companion only)
    }
    bootMs = millis();
    drawSplash(); // branded splash instead of black while the mesh comes up
}

// No TCA8418 answered on the keyboard bus: almost certainly this firmware was
// flashed onto a classic Cardputer (GPIO-matrix keyboard) instead of the ADV.
// Say so plainly — the engine still runs, so the phone app keeps working.
void AdvUI::drawKbMissing()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);
    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0xF800);
    const char *t1 = "KEYBOARD NOT FOUND";
    g->setCursor((240 - g->textWidth(t1)) / 2, 24);
    g->print(t1);
    g->setTextColor(0xFFFF);
    const char *t2 = "This build is for the Cardputer ADV";
    g->setCursor((240 - g->textWidth(t2)) / 2, 52);
    g->print(t2);
    g->setTextColor(0x8410);
    const char *t3 = "The classic Cardputer isn't supported";
    g->setCursor((240 - g->textWidth(t3)) / 2, 68);
    g->print(t3);
    const char *t4 = "Mesh engine is up: the phone app works";
    g->setCursor((240 - g->textWidth(t4)) / 2, 96);
    g->print(t4);
    if (haveCanvas)
        pushFrame();
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

    // Which build is this? Saves everyone the "which version are you on"
    // round-trip — ours from the release tag, plus the engine underneath.
    g->setTextColor(0x630C);
    char vline[48];
    snprintf(vline, sizeof(vline), "%s  ·  mesh %s", ADVUI_VERSION, optstr(APP_VERSION_SHORT));
    g->setCursor((240 - g->textWidth(vline)) / 2, 126);
    g->print(vline);

    if (haveCanvas)
        pushFrame();
}

// Picks incoming text messages out of the FromRadio stream and files them in the
// ring. Everything else (config, node DB, telemetry, ...) is ignored here.
void AdvUI::onMeshPacket(const meshtastic_FromRadio &fr)
{
    uiDirty = handleFromRadio(fr) || uiDirty;
}

bool AdvUI::handleFromRadio(const meshtastic_FromRadio &fr)
{
    if (fr.which_payload_variant == meshtastic_FromRadio_queueStatus_tag) {
        const meshtastic_QueueStatus &status = fr.queueStatus;
        if (status.mesh_packet_id && status.res != ERRNO_OK)
            return ackMsg(status.mesh_packet_id, MSG_FAILED, (uint8_t)status.res);
        return false;
    }
    if (fr.which_payload_variant != meshtastic_FromRadio_packet_tag)
        return false;
    const meshtastic_MeshPacket &p = fr.packet;
    seenNote(p.from); // ledger sees every sender, whatever the engine stamps
    if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag)
        return false;

    // A routing response carrying our request id is the ACK/NAK for a sent message.
    // Decode it: error_reason NONE = delivered, anything else (no route, no ack,
    // max retransmit, ...) = failed. Route request/reply variants aren't acks.
    if (p.decoded.portnum == meshtastic_PortNum_ROUTING_APP && p.decoded.request_id) {
        meshtastic_Routing routing = meshtastic_Routing_init_zero;
        bool ok = pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_Routing_msg, &routing);
        if (ok && routing.which_variant == meshtastic_Routing_error_reason_tag) {
            bool delivered = routing.error_reason == meshtastic_Routing_Error_NONE;
            return ackMsg(p.decoded.request_id, delivered ? MSG_DELIVERED : MSG_FAILED,
                          (uint8_t)routing.error_reason);
        }
        return false;
    }

    if (p.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return false;

    char text[236]; // sized like Msg.text: the full wire payload fits
    size_t n = p.decoded.payload.size;
    if (n > sizeof(text) - 1)
        n = sizeof(text) - 1;
    memcpy(text, p.decoded.payload.bytes, n);
    text[n] = 0;
    utf8Sanitize(text, sizeof(text));

    uint32_t me = myNodeNum();

    // A tapback: attach to the target message instead of adding a bubble. Quiet by
    // design (no unread/beep) — just a LED blink so it isn't missed entirely.
    if (p.decoded.emoji && p.decoded.reply_id) {
        // Keep self echoes too: a reaction sent from the phone has our node id,
        // and must appear on the Cardputer. sendReaction() may also add the same
        // tuple locally, but addReaction is idempotent per (message, sender).
        const bool changed = addReaction(p.decoded.reply_id, p.from, text, reactionTargetFrom(p, me));
#ifdef HAS_NEOPIXEL
        if (changed && p.from != me)
            flashLed(60, 40, 90); // soft violet: a reaction, not a message
#endif
        return changed;
    }

    bool unread = p.from != me && (p.to == me || p.to == NODENUM_BROADCAST); // DM to us, or a channel broadcast
    // Our own broadcasts are delivered back to us locally (loopbackOk), and that echo
    // runs SYNCHRONOUSLY inside sendTextPacket() — before sendChannel() tags the message
    // MSG_SENT. Storing the echo as MSG_IN would then win the id dedupe in addMsg() and
    // the "sent" check would never draw. So stamp our own echoed sends with their real
    // outgoing status: MSG_SENDING until the engine's routing result (implicit ack for
    // broadcasts, end-to-end ACK for DMs) flips it via ackMsg().
    uint8_t status = MSG_IN;
    if (p.from == me)
        status = MSG_SENDING;
    // keep the id (reactions/replies reference it) and the reply link (draws the quote)
    const bool changed = addMsg(p.from, p.to, p.channel, p.rx_time, unread, text, p.id, status, p.decoded.reply_id);

    if (changed && unread) {
        bool fav = (p.to == NODENUM_BROADCAST)
                       ? chanFav(p.channel)
                       : ((!g_radioCompanion && nodeDB && nodeDB->isFavorite(p.from)) || favNodeLocal(p.from));
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
    return changed;
}

struct SendResult {
    uint32_t id;
    SendFailure failure;
    uint8_t error;
};

// Builds and transmits a text packet (DM with PKI, or channel broadcast).
// replyId + asEmoji turn it into a reaction (tapback) on that message.
static SendResult sendTextPacket(uint32_t to, const char *text, int chIdx = 0, uint32_t replyId = 0,
                                 bool asEmoji = false)
{
    if (g_radioCompanion) { // ship it over BLE: the node's radio does routing/PKI itself
#ifdef ADVUI_HIL
        // A persisted companion preference must not turn USB HIL into an
        // indirect live-mesh transmitter through a bonded radio node.
        return {0, SendFailure::RADIO_REJECTED, ERRNO_DISABLED};
#else
        if (g_linkState != BLE_CONNECTED)
            return {0, SendFailure::LINK_DOWN, 0};
        meshtastic_ToRadio t = meshtastic_ToRadio_init_default;
        t.which_payload_variant = meshtastic_ToRadio_packet_tag;
        meshtastic_MeshPacket &p = t.packet;
        p.id = (uint32_t)esp_random();
        if (!p.id)
            p.id = 1;
        p.to = to;
        // Ask the companion node to track broadcasts too.  Meshtastic reports an
        // implicit result after hearing a neighbour rebroadcast (or exhausting
        // retries), giving channel sends the same terminal state as local-radio
        // sends instead of leaving them on the yellow "sending" dot forever.
        p.want_ack = true;
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
            CompNode node;
            if (bleCopyCompNode(to, &node) && node.hasKey) {
                p.pki_encrypted = true;
                p.channel = 0;
            }
        }
        uint8_t buf[320];
        size_t len = pb_encode_to_bytes(buf, sizeof(buf), &meshtastic_ToRadio_msg, &t);
        if (!len)
            return {0, SendFailure::ENCODE_FAILED, 0};
        if (!bleQueueToRadio(buf, (uint16_t)len))
            return {0, SendFailure::QUEUE_FULL, ERRNO_UNKNOWN};
        return {p.id, SendFailure::NONE, ERRNO_OK};
#endif
    }

    if (!router || !service)
        return {0, SendFailure::RADIO_REJECTED, ERRNO_NO_INTERFACES};
    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p)
        return {0, SendFailure::QUEUE_FULL, ERRNO_UNKNOWN};
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
        // want_ack on a broadcast is what the phone app does: the engine then
        // tracks it with implicit acks (a heard rebroadcast confirms, silence
        // retransmits up to 3x, then NAKs "max retransmission") and reports the
        // result back with our request id — ackMsg() turns that into the green
        // check / red x. Without it the engine never says anything and the
        // message sits at the terminal cyan "sent" forever (field report: the
        // app showed delivery state, the device could not).
        p->want_ack = true;
    } else {
        p->want_ack = true;
        // A DM to a key-capable node must be PKI-encrypted (stock forces this); without
        // it the recipient on a modern mesh won't accept/ack the message.
        meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNode(to) : nullptr;
        if (nodeHasPublicKey(node)) {
            p->pki_encrypted = true;
            p->channel = 0;
        }
    }

    uint32_t id = p->id;
#ifdef ADVUI_HIL
    // An injected admission failure must happen before MeshService sees the
    // packet. Overriding its result afterwards leaves a reliable-retry copy in
    // the engine even though the UI correctly kept the draft, contaminating the
    // next send and the rest of the deterministic HIL session.
    const ErrorCode injected = g_hilNextSendError;
    g_hilNextSendError = ERRNO_OK;
    if (injected != ERRNO_OK) {
        packetPool.release(p);
        g_hilLastSendError = (uint8_t)injected;
        return {id, injected == ERRNO_UNKNOWN ? SendFailure::QUEUE_FULL : SendFailure::RADIO_REJECTED,
                (uint8_t)injected};
    }
    // The synthetic peer intentionally does not exist in the production
    // NodeDB. Meshtastic 2.7 therefore rejects the DM before RadioLib with
    // PKI_SEND_FAIL_PUBLIC_KEY. ADV HIL tests the application above that
    // engine boundary: emulate successful queue admission, then feed the real
    // UI routing/QueueStatus handlers below. The immutable RadioLibInterface
    // guard remains defence in depth for every engine-owned packet.
    g_hilLastTxReply = p->decoded.reply_id; // capture the exact packet structure admitted by ADV HIL
    packetPool.release(p);
    g_hilLastSendError = ERRNO_DISABLED;
    return {id, SendFailure::NONE, ERRNO_OK};
#else
    ErrorCode error = service->sendToMesh(p, RX_SRC_LOCAL, true); // a connected phone mirrors our sends
    if (error != ERRNO_OK) {
        ackMsg(id, MSG_FAILED, (uint8_t)error); // catches the synchronous self-echo of a broadcast
        return {id, error == ERRNO_UNKNOWN ? SendFailure::QUEUE_FULL : SendFailure::RADIO_REJECTED,
                (uint8_t)error};
    }
    return {id, SendFailure::NONE, ERRNO_OK};
#endif
}

// Remote admin over the companion link: an AdminMessage addressed to the linked
// node itself, exactly how the phone app configures it. Arriving via the node's
// own client API counts as local admin, so no session key or admin channel is
// needed on the node side.
static bool sendAdminToNode(const meshtastic_AdminMessage &adm)
{
#ifdef ADVUI_HIL
    // Remote admin is a real persistent write on the companion node.
    (void)adm;
    return false;
#else
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
#endif
}

// Sends a text DM to a node and adds it to our own thread immediately (status
// "sending"); the delivery ACK later flips it to "delivered" via ackMsg().
SendFailure AdvUI::sendMessage(uint32_t to, const char *text, uint32_t replyId)
{
    SendResult result = sendTextPacket(to, text, 0, replyId);
    if (result.failure != SendFailure::NONE)
        return result.failure;
    uint32_t me = myNodeNum();
    addMsg(me, to, 0, getTime(false), false, text, result.id, MSG_SENDING, replyId);
    return SendFailure::NONE;
}

// Sends a text broadcast to a channel and adds it to that channel's thread.
SendFailure AdvUI::sendChannel(int chIdx, const char *text, uint32_t replyId)
{
    SendResult result = sendTextPacket(NODENUM_BROADCAST, text, chIdx, replyId);
    if (result.failure != SendFailure::NONE)
        return result.failure;
    uint32_t me = myNodeNum();
    addMsg(me, NODENUM_BROADCAST, chIdx, getTime(false), false, text, result.id, MSG_SENDING, replyId);
    return SendFailure::NONE;
}

// Sends a tapback on the message at ring index msgIdx and records it locally.
SendFailure AdvUI::sendReaction(int msgIdx, const char *label)
{
    if (msgIdx < 0 || msgIdx >= g_msgCount)
        return SendFailure::RADIO_REJECTED;
    Msg &m = g_msgs[msgIdx];
    if (!m.id) {
        LOG_INFO("advui: can't react - message has no packet id (pre-update history)");
        return SendFailure::RADIO_REJECTED;
    }
    SendResult result = (selectedChannel >= 0)
                            ? sendTextPacket(NODENUM_BROADCAST, label, selectedChannel, m.id, true)
                            : sendTextPacket(selectedNum, label, 0, m.id, true);
    if (result.failure != SendFailure::NONE)
        return result.failure;
    uint32_t me = myNodeNum();
    addReaction(m.id, me, label, m.from);
    return SendFailure::NONE;
}

// Fills out[] with node-DB indices matching query (all if query is null/empty),
// in the default sorted order. Returns the count (capped at max).
int AdvUI::buildNodeList(uint16_t *out, int max, const char *query)
{
    int count = 0;
    uint32_t me = myNodeNum();

    if (g_radioCompanion) { // companion: nodes come from the BLE config stream
        struct SortKey {
            uint16_t index;
            bool unread;
            uint32_t lastHeard;
        } keys[kMaxCompNodes];
        const int total = g_compNodeCount.load();
        for (int i = 0; i < total && count < max; i++) {
            CompNode c;
            if (!bleCopyCompNodeAt(i, &c))
                continue;
            if (c.num == me)
                continue;
            if (query && query[0]) {
                const char *name = c.longName[0] ? c.longName : c.shortName;
                if (!ciContains(name[0] ? name : "", query))
                    continue;
            }
            keys[count++] = {(uint16_t)i, hasUnreadFrom(c.num), c.lastHeard};
        }
        std::sort(keys, keys + count, [](const SortKey &a, const SortKey &b) { // unread first, then freshest
            if (a.unread != b.unread)
                return a.unread;
            return a.lastHeard > b.lastHeard;
        });
        for (int i = 0; i < count; i++)
            out[i] = keys[i].index;
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

// Stage the archive slice for `depth` archived messages above the seam. The
// staged window is the oldest-most 8 of those; the gap up to the seam (if
// any) stays unrendered, and the live ring renders only when the slice
// touches the seam — one continuous timeline either way. The view anchor
// keeps the screen position stable across restagings, so sliding the window
// feels like plain scrolling, never like a page swap.
void AdvUI::histStage(int depth)
{
    if (depth <= 0) {
        histExit();
        return;
    }
    if (histAvail >= 0 && depth > histAvail)
        depth = histAvail;
    bool isChan = selectedChannel >= 0;
    int sliceLen = depth < kHistWin ? depth : kHistWin;
    int skipNewest = depth - sliceLen; // matches between the slice and the seam
    int got = histLoadSlice(isChan, (uint8_t)(isChan ? selectedChannel : 0), selectedNum, skipNewest, sliceLen);
    if (got <= 0)
        return; // racing purge/compaction: keep the current window
    histDepth = depth;
    histCount = got;
    histSeam = (skipNewest == 0); // touching the seam: render the live ring after the slice
}

void AdvUI::histExit()
{
    histDepth = 0;
    histCount = 0;
    histSeam = true;
    chatScroll = 0; // pinned back to the live newest
    viewAnchorId = 0;
    lastDrawScroll = 0;
}

void AdvUI::openEntry(int s)
{
    nodeReturn = (mode == MODE_PICKER) ? MODE_PICKER : MODE_NODES;
    chatScroll = 0;          // default: pinned to the newest message
    chatAnchorMsgIdx = -1;
    reactSel = -1;
    reactStrip = false;
    pendingReplyId = 0;
    sendFailure = SendFailure::NONE;
    histDepth = 0;           // live view; archive re-counted lazily per thread
    histCount = 0;
    histSeam = true;
    histAvail = -1;
    chatAtTop = false;
    viewAnchorId = 0;
    lastDrawScroll = 0;
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
    bool dropReacts[kMaxReacts] = {};
    for (int r = 0; r < g_msgCount; r++) {
        Msg &m = g_msgs[r];
        bool mine = c.isChan ? (m.to == NODENUM_BROADCAST && m.ch == c.ch)
                             : (m.to != NODENUM_BROADCAST && (m.from == c.node || m.to == c.node));
        if (mine) {
            markReactionsForMessage(m.id, m.from, dropReacts);
            continue; // drop it
        }
        if (w != r) {
            g_msgs[w] = m;
            g_msgReply[w] = g_msgReply[r];
        }
        w++;
    }
    g_msgCount = w;
    markMsgsDirty();
    bool historyPurged = histPurge(c.isChan, c.ch, c.node, dropReacts); // erase means erase: archive too

    // Packet IDs are only per-sender unique, while a reaction carries just the
    // target ID. If another conversation still has the same ID, the reaction is
    // ambiguous but not orphaned: preserving it avoids deleting valid metadata.
    // Re-check both surviving stores after the purge so archive/live collisions
    // cannot erase each other's reactions.
    for (int i = 0; i < g_msgCount; i++)
        preserveReactionsForMessage(g_msgs[i].id, g_msgs[i].from, dropReacts);
    bool archiveChecked = historyPurged && g_histTotal == 0;
    auto hist = historyPurged && g_histTotal > 0 ? FSCom.open(kHistPath, FILE_O_READ) : File();
    if (hist && historyPurged) {
        uint32_t magic = 0;
        HistRec rec;
        bool valid = hist.read((uint8_t *)&magic, 4) == 4 && magic == kHistMagic;
        int records = 0;
        while (valid && records < g_histTotal && hist.read((uint8_t *)&rec, sizeof(rec)) == (int)sizeof(rec)) {
            if (normalizeStoredMsg(rec.m)) {
                preserveReactionsForMessage(rec.m.id, rec.m.from, dropReacts);
                records++;
            } else {
                valid = false;
            }
        }
        archiveChecked = valid && records == g_histTotal;
        hist.close();
    }
    if (!archiveChecked)
        memset(dropReacts, 0, sizeof(dropReacts)); // fail safe: uncertain metadata is never destroyed
    dropMarkedReactions(dropReacts);
    if (saveMsgs())
        g_msgsDirty = false;
}

// One node, favourited or not, wherever the user pressed the arrow. Both the chats
// list and the node list route through here so they cannot drift apart — the chats
// screen had its own copy, which kept the companion-mode bug alive in half the UI
// after the node list was fixed.
void AdvUI::favNode(uint32_t num, bool on)
{
    // Onboard, the node database is ours and its flag is the one the phone app and the
    // rest of Meshtastic read. In companion mode that database belongs to the node we
    // are driving, so the favourite is kept here instead.
    if (nodeDB && !g_radioCompanion)
        nodeDB->set_favorite(on, num);
    else if (setFavNodeLocal(num, on))
        persistUiCfg();
}

// Node number at a position in the filtered list, or 0 when there is nothing there.
uint32_t AdvUI::nodeNumAt(int i)
{
    if (i < 0 || i >= filteredCount)
        return 0;
    meshtastic_NodeInfoLite *n = nodeAt(filtered[i]);
    return n ? n->num : 0;
}

void AdvUI::favEntry(int s, bool on)
{
    if (s < chanCount) {
        int ci = chanList[s];
        if (on)
            g_favChannels |= (1u << ci);
        else
            g_favChannels &= ~(1u << ci);
        markMsgsDirty(); // persisted alongside the messages
    } else {
        meshtastic_NodeInfoLite *node = nodeAt(filtered[s - chanCount]);
        if (node)
            favNode(node->num, on);
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
    sendFailure = SendFailure::NONE;
    histDepth = 0; // live view; archive re-counted lazily per thread
    histCount = 0;
    histSeam = true;
    histAvail = -1;
    chatAtTop = false;
    viewAnchorId = 0;
    lastDrawScroll = 0;
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
    g->setTextColor(0x07FF);
    g->setCursor(4, 3);
    g->print("Chats");
    if (g_radioCompanion)
        drawBtGlyph(g, 40, 2, linkColor()); // companion link state at a glance
    int bw = drawBattery(g, 238, 3);
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
            pushFrame();
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
        bool fav;
        if (c.isChan) {
            fav = chanFav(c.ch);
        } else {
#ifdef ADVUI_SCREENSHOT
            // Promotional frames must not inherit even visual metadata from a
            // production NodeDB entry that happens to share the fixture id.
            if (g_demoActive) {
                meshtastic_NodeInfoLite *n = nodeByNum(c.node);
                fav = n && isFav(n);
            } else
#endif
                fav = (!g_radioCompanion && nodeDB && nodeDB->isFavorite(c.node)) || favNodeLocal(c.node);
        }

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
        pushFrame();
}

void AdvUI::drawNodeList()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    // The honest node count. Local: the flash ledger's 24 h window — the size of
    // the LIVING mesh, unbounded by the hot/warm identity caps (reconciled at
    // most once a minute; hot+warm is the fallback until the clock lets the
    // window mean something). Companion: the config stream sends each of the
    // node's DB entries exactly once per sync, so counting the stream beats our
    // 64-slot mirror. The list below still shows only full-data nodes either way.
    size_t total, cap = SIZE_MAX;
    bool day = false;
    if (g_radioCompanion) {
        const int seen = g_compNodesSeen.load(), mirrored = g_compNodeCount.load();
        total = (size_t)(seen > mirrored ? seen : mirrored);
    } else {
        static uint32_t lastRecon = 0;
        if (g_seen24 < 0 || !Throttle::isWithinTimespanMs(lastRecon, 60000)) {
            seenReconcile();
            lastRecon = millis();
        }
        if (g_seen24 > 0) {
            total = (size_t)g_seen24;
            day = true;
        } else {
            total = nodeDB ? nodeDB->getNumMeshNodes() : 0;
            cap = (size_t)::advui_max_num_nodes();
#if WARM_NODE_COUNT > 0
            if (nodeDB) {
                total += nodeDB->warmStore.count();
                cap += WARM_NODE_COUNT;
            }
#endif
        }
    }
    uint32_t me = myNodeNum();

    g->setFont(&lgfx::fonts::Font0); // header stays on the compact bitmap font
    g->setTextSize(1);

    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    // "+" only in the fallback mode, when even the warm tier is pinned at its
    // capacity — the ledger's 24 h number is exact and needs no marker.
    if (day)
        g->printf("%u nodes/24h", (unsigned)total);
    else
        g->printf("%u%s nodes", (unsigned)total, total >= cap ? "+" : "");

    int bw = drawBattery(g, 238, 3);

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
        pushFrame();
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
        pushFrame();
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
    int bottom = (mode == MODE_COMPOSE) ? ((pendingReplyId || sendFailure != SendFailure::NONE) ? 96 : 112) : 122;
    int maxLines = (bottom - fy0) / lh;
    // One continuous timeline: the staged archive slice first, then the live
    // ring whenever the window touches the seam — scrolling slides the window,
    // it never swaps a "page" in front of the reader.
    struct SrcRef {
        const Msg *m;
        uint32_t rid;
        int16_t ringIdx; // g_msgs index for live messages, -1 for archived ones
    };
    SrcRef srcs[kMaxMsgs + kHistWin];
    int mc = 0;
    int liveMc = 0; // live matches in the ring (whether rendered or not): scrollbar math
    for (int i = 0; i < g_msgCount; i++)
        if (isChan ? (g_msgs[i].to == NODENUM_BROADCAST && g_msgs[i].ch == selectedChannel)
                   : ((g_msgs[i].from == selectedNum || g_msgs[i].to == selectedNum) &&
                      g_msgs[i].to != NODENUM_BROADCAST))
            liveMc++;
    bool hasFailed = false; // any failed outgoing DM here -> offer ENTER resend
    for (int i = 0; i < histCount; i++)
        srcs[mc++] = {&g_arch[i], g_archReply[i], (int16_t)-1};
    if (histSeam) {
        for (int i = 0; i < g_msgCount; i++) {
            bool match = isChan ? (g_msgs[i].to == NODENUM_BROADCAST && g_msgs[i].ch == selectedChannel)
                                : ((g_msgs[i].from == selectedNum || g_msgs[i].to == selectedNum) &&
                                   g_msgs[i].to != NODENUM_BROADCAST);
            if (match) {
                srcs[mc++] = {&g_msgs[i], g_msgReply[i], (int16_t)i};
                if (!isChan && g_msgs[i].from == me && g_msgs[i].status == MSG_FAILED)
                    hasFailed = true;
            }
        }
    }
    if (mc == 0) {
        g->setTextColor(0x630C);
        g->setCursor(4, fy0);
        g->print("(no messages yet)");
        chatAtTop = true; // even an empty live view can page into the archive
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
            bool quote;       // the replied-to snippet: drawn inside a frame, not as plain text
            uint32_t msgId;   // packet id of the owning message (0: decoration/no id) — view anchor
        };
        int32_t tzOff = g_utcOffsetMin * 60; // user-set UTC offset (Settings > UTC)
        static DLine dl[96];  // single-threaded UI: static keeps it off the stack; sized so
                              // a typical backlog window rarely overflows (the unread anchor
                              // lives or dies with its lines staying in this ring)
        int dlCount = 0;
        int dlHead = 0;
        int anchorLine = -1;       // dl index of the first unread message's first line (on open)
        bool anchorDropped = false; // its lines got evicted: land at the top of what's left
        int anchorNewLine = -1;     // dl index where the view-anchor message starts this frame
        int selMsgIdx = reactSel >= 0 ? matchedFromNewest(reactSel) : -1;
        int selFirst = -1, selLast = -1; // dl range of the react-selected message
        const int dlCapacity = (int)(sizeof(dl) / sizeof(dl[0]));
        auto lineAt = [&](int index) -> DLine & { return dl[(dlHead + index) % dlCapacity]; };
        auto pushLine = [&]() -> DLine & {
            if (dlCount >= dlCapacity) { // drop the oldest line without copying the whole 5.3 KiB ring
                dlHead = (dlHead + 1) % dlCapacity;
                dlCount--;
                if (anchorLine == 0)
                    anchorDropped = true; // the anchor's first line is the one being dropped
                if (anchorLine >= 0)
                    anchorLine--; // trackers shift up with the dropped line
                if (anchorNewLine >= 0)
                    anchorNewLine--; // may reach -1: the anchor message fell off entirely
                if (selFirst >= 0)
                    selFirst--;
                if (selLast >= 0)
                    selLast--;
            }
            DLine &line = lineAt(dlCount);
            dlCount++;
            return line;
        };
        for (int i = 0; i < mc; i++) {
            const Msg &m = *srcs[i].m;
            uint32_t replyId = srcs[i].rid;
            bool out = (m.from == me);
            bool failed = out && m.status == MSG_FAILED;
            bool isSel = (selMsgIdx >= 0 && srcs[i].ringIdx >= 0 && srcs[i].ringIdx == selMsgIdx);
            int wrapW = !out ? 232 : (failed ? 162 : 214); // reserve room for the marker
            char tpre[8];
            msgTimePrefix(m.rxTime, tzOff, tpre, sizeof(tpre)); // "HH:MM " before the arrow
            uint8_t tlen = (uint8_t)strlen(tpre);
            uint8_t nlen = 0;
            char full[260]; // time prefix + sender + a full-size message text
            if (isChan && !out) { // channels have many senders — show who wrote it
                char sn[8];
                shortNameOf(m.from, sn, sizeof(sn));
                nlen = (uint8_t)(strlen(sn) + 2);
                snprintf(full, sizeof(full), "%s%s: %s", tpre, sn, m.text);
            } else {
                snprintf(full, sizeof(full), "%s%s%s", tpre, out ? "> " : "< ", m.text);
            }
            if (replyId) { // a reply: quote the original above it, if we still hold it
                // Packet IDs are unique per sender, not globally. Search only
                // backward in this already conversation-scoped timeline; a
                // global ring/archive scan could quote another sender's packet
                // with the same ID, or even a later packet after ID reuse.
                const Msg *om = nullptr;
                for (int q = i - 1; q >= 0; q--)
                    if (srcs[q].m->id == replyId && srcs[q].m->id != 0) {
                        om = srcs[q].m;
                        break;
                    }
                if (om) {
                    DLine &d = pushLine();
                    char qt[8]; // the original's own arrival time — the frame is often
                    msgTimePrefix(om->rxTime, tzOff, qt, sizeof(qt)); // its only surviving trace
                    char qs[10];
                    if (om->from != me && isChan) {
                        char sn[8];
                        shortNameOf(om->from, sn, sizeof(sn));
                        snprintf(qs, sizeof(qs), "%s: ", sn);
                    } else
                        snprintf(qs, sizeof(qs), om->from == me ? "> " : "< ");
                    char snip[24];
                    utf8Copy(snip, om->text, (int)sizeof(snip) - 1);
                    snprintf(d.text, sizeof(d.text), "%s%s%s", qt, qs, snip);
                    while (lineWidthEmotes(g, d.text) > 204) // keep the text inside the frame
                        utf8TrimLast(d.text);
                    d.color = 0x8410; // readable, but subordinate to its frame
                    d.msgIdx = -1;
                    d.out = false;
                    d.status = 0;
                    d.err = 0;
                    d.timeLen = 0;
                    d.nameLen = 0;
                    d.last = false;
                    d.quote = true;
                    d.msgId = 0;
                    if (isSel) {
                        if (selFirst < 0)
                            selFirst = dlCount - 1;
                        selLast = dlCount - 1;
                    }
                }
            }
            if (chatAnchorMsgIdx >= 0 && srcs[i].ringIdx == chatAnchorMsgIdx)
                anchorLine = dlCount; // this message's first line
            if (viewAnchorId && m.id == viewAnchorId && anchorNewLine < 0)
                anchorNewLine = dlCount; // where the view-anchor message begins this frame
            const char *p = full;
            bool first = true;
            do {
                DLine &d = pushLine();
                p += wrapLine(g, p, wrapW, d.text, sizeof(d.text));
                d.color = out ? 0x07FF : 0xFFFF; // outgoing cyan, incoming white
                d.msgIdx = srcs[i].ringIdx; // -1 for archived lines: no read-marking or picking
                d.out = out;
                d.status = m.status;
                d.err = m.err;
                d.timeLen = first ? tlen : 0; // time + name sit only on the first line
                d.nameLen = first ? nlen : 0;
                d.last = (*p == 0);
                d.quote = false;
                d.msgId = m.id;
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
                    if (g_reacts[r].msgId != m.id ||
                        (g_reacts[r].targetFrom && g_reacts[r].targetFrom != m.from))
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
                    d.quote = false;
                    d.msgId = 0;
                    if (isSel)
                        selLast = dlCount - 1;
                }
            }
        }
        // chatScroll counts lines scrolled up from the bottom (0 = pinned to newest).
        int maxScroll = dlCount > maxLines ? dlCount - maxLines : 0;
        bool jumpedNow = false;
        if (chatAnchorMsgIdx >= 0) { // first render after opening
            // A short thread with only a handful of unread is effectively all on
            // screen on open (even a wrapped 2-3-message channel is a few lines over
            // the fold). Jumping to the first unread at the top would then strand the
            // newest line just below the visible window, so it never gets "seen" and
            // the channel stays unread until you scroll. Instead: pin to the newest
            // and mark the whole thread read now. A genuine backlog (more unread than
            // a screenful) still jumps to the first unread and clears as you read down.
            int unread = 0;
            for (int j = 0; j < g_msgCount; j++) {
                const Msg &m = g_msgs[j];
                bool match = isChan ? (m.to == NODENUM_BROADCAST && m.ch == selectedChannel)
                                    : (m.from == selectedNum && m.to != NODENUM_BROADCAST);
                if (match && !m.read)
                    unread++;
            }
            if (unread > 0 && unread <= maxLines) { // not a backlog: mark the whole thread read
                // on open — including any wrapped tail below the fold — so the badge clears
                // without a keypress. The scroll is left to the normal jump-to-first-unread
                // below, so opening doesn't yank the view down to the last message.
                if (isChan)
                    markReadChannel(selectedChannel);
                else
                    markReadFrom(selectedNum);
                uiDirty = true; // the header/badge already drew as "unread" this frame — force
                                // one more frame so it clears now, not on the next stray packet
            }
            if (anchorLine >= 0 && anchorLine <= maxScroll)
                chatScroll = maxScroll - anchorLine;
            else if (anchorDropped)
                chatScroll = maxScroll; // unread starts beyond the ring: as far back as we can show
            else
                chatScroll = 0;
            chatAnchorMsgIdx = -1;
            jumpedNow = true;
        }
        if (!jumpedNow && viewAnchorId && anchorNewLine >= 0) {
            // Re-anchor the view to the message it was showing: arrivals grow the
            // bottom, evictions eat the top and window slides restage the archive
            // part — the reader shouldn't move for any of them. userDelta is the
            // scrolling done since the last draw; the math is an identity when idle.
            int userDelta = chatScroll - lastDrawScroll;
            chatScroll = maxScroll - (anchorNewLine + viewAnchorOff - userDelta);
        }
        if (chatScroll > maxScroll)
            chatScroll = maxScroll;
        if (chatScroll < 0)
            chatScroll = 0;
        chatAtTop = (chatScroll >= maxScroll); // "up" past this point pages into the archive

        // Scrolled to the newest end: the whole thread has been seen, so mark it
        // read wholesale. Per-line marking below only clears what actually landed
        // on screen, and a message whose wrapped tail sits just past the last
        // drawn line stays unread forever — the reported "read all 32, 2 remain".
        if (chatScroll == 0) {
            if (isChan)
                markReadChannel(selectedChannel);
            else
                markReadFrom(selectedNum);
        }
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
        lastDrawScroll = chatScroll;
        viewAnchorId = 0;
        // Remember what the reader sees whenever the view isn't pinned to the
        // newest message (deep windows are "unpinned" even at scroll 0: their
        // bottom is an archive message, not the newest).
        if (chatScroll > 0 || (histDepth > 0 && !histSeam)) {
            for (int i = startL; i < dlCount; i++)
                if (lineAt(i).msgId) {
                    int a = i;
                    while (a > 0 && lineAt(a - 1).msgId == lineAt(i).msgId)
                        a--; // back up to the message's first line
                    viewAnchorId = lineAt(i).msgId;
                    viewAnchorOff = startL - a;
                    break;
                }
        }
        int y = fy0;
        for (int i = startL; i < startL + maxLines && i < dlCount; i++) {
            DLine &d = lineAt(i);
            if (d.msgIdx >= 0 && !g_msgs[d.msgIdx].read) { // it's on screen now: that's "read"
                g_msgs[d.msgIdx].read = true;
                markMsgsDirty();
            }
            if (selFirst >= 0 && i >= selFirst && i <= selLast)
                g->fillRect(0, y - 1, 236, lh, 0x2945); // react-mode selection band
            if (d.quote) { // the replied-to message: frame it so the reply reads as a reply
                g->setFont(&cyrFont);
                g->setTextSize(1);
                int qx = 10;
                int bw = lineWidthEmotes(g, d.text) + 10;
                if (bw > 224)
                    bw = 224;
                g->drawRect(qx, y - 1, bw, lh - 2, 0x4a49);   // frame
                g->drawFastVLine(qx, y - 1, lh - 2, 0x07FF);  // cyan reply accent on the left edge
                printLineEmotes(g, qx + 6, y, d.text, d.color, 2);
                y += lh;
                continue;
            }
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
                    // Amber, not red, when a broadcast merely went unrepeated: on a mesh
                    // with nobody in range that is the state of the network, not a fault.
                    const bool unrelayed = isChan && d.err == 5;
                    g->setFont(&lgfx::fonts::Font0); // error name at the right
                    g->setTextSize(1);
                    g->setTextColor(unrelayed ? 0xFD20 : 0xF800);
                    const char *en = errName(d.err, isChan);
                    g->setCursor(238 - g->textWidth(en), y + 4);
                    g->print(en);
                } else {
                    drawMsgStatus(g, 220, y, d.status);
                }
            }
            y += lh;
        }
        // scrollbar on the right edge when the thread overflows the view. With the
        // window in the archive it spans the WHOLE timeline (archive + live) at
        // message granularity — window slides no longer pulse the thumb size.
        if (maxScroll > 0 || histDepth > 0) {
            int trackY = fy0, trackH = maxLines * lh;
            g->drawFastVLine(238, trackY, trackH, 0x2104); // dark track
            int thumbY, thumbH;
            int totalMsgs = (histAvail > 0 ? histAvail : 0) + liveMc;
            if (histDepth > 0 && totalMsgs > 0 && mc > 0) {
                int before = histAvail - histDepth; // messages above the rendered window
                float visFrac = dlCount > maxLines ? (float)maxLines / dlCount : 1.0f;
                float posFrac = maxScroll > 0 ? (float)startL / maxScroll : 0.0f;
                float winTop = before + posFrac * mc * (1.0f - visFrac);
                thumbH = (int)(trackH * (mc * visFrac) / totalMsgs);
                if (thumbH < 6)
                    thumbH = 6;
                thumbY = trackY + (int)(trackH * winTop / totalMsgs);
                if (thumbY + thumbH > trackY + trackH)
                    thumbY = trackY + trackH - thumbH;
            } else {
                thumbH = trackH * maxLines / dlCount;
                if (thumbH < 6)
                    thumbH = 6;
                thumbY = trackY + (trackH - thumbH) * startL / (maxScroll > 0 ? maxScroll : 1);
            }
            g->drawFastVLine(238, thumbY, thumbH, 0x07FF); // cyan thumb
            g->drawFastVLine(239, thumbY, thumbH, 0x07FF);
        }
    }

    if (mode == MODE_COMPOSE) {
        if (sendFailure != SendFailure::NONE) {
            g->fillRect(0, 97, 240, 16, 0x0000);
            char error[40];
            snprintf(error, sizeof(error), "! %s", sendFailureText(sendFailure));
            printLineEmotes(g, 4, 99, error, 0xF800);
        } else if (pendingReplyId) { // replying: show what we're quoting above the input
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
            shown += el > 0 ? el : utf8Decode(shown).bytes;
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
        char histHint[36];
        if (histDepth > 0) { // the window reaches into the flash archive
            snprintf(histHint, sizeof(histHint), "history %d/%d  dn newer  ESC", histDepth, histAvail);
            hint = histHint;
        } else if (reactStrip) {
            hint = "</> pick  ENTER send  ESC";
        } else if (reactSel >= 0) {
            int smi = matchedFromNewest(reactSel); // old history has no packet id to reference
            hint = (smi >= 0 && g_msgs[smi].id == 0) ? "old msg: no id  up/dn  ESC"
                   : pickReply                       ? "up/dn msg  ENTER reply  ESC"
                                                     : "up/dn msg  ENTER react  ESC";
        } else if (sendFailure != SendFailure::NONE) {
            hint = sendFailureText(sendFailure);
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
        pushFrame();
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
    char field[sizeof(nameBuf) + 2];
    snprintf(field, sizeof(field), "%s_", nameBuf);
    const char *shown = field; // keep the cursor visible for 64-byte WiFi/MQTT fields
    while (shown[0] && g->textWidth(shown) > 228)
        shown += utf8Decode(shown).bytes;
    g->setCursor(6, 44);
    g->print(shown);

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x8410); // gray
    g->setCursor(6, 74);
    g->printf("%u / %u", (unsigned)nameLen, editMax(editTarget));

    drawFooter(g, editTarget == 2 ? "blank = auto   ENTER save" : "type   ENTER save   ESC cancel");

    if (haveCanvas)
        pushFrame();
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
        float frequency = 0.0f;
        if (!parseFrequencyOverride(nameBuf, &frequency))
            return false;
        if (g_radioCompanion) { // remote admin: the node saves and reboots itself
            meshtastic_Config_LoRaConfig lora = meshtastic_Config_LoRaConfig_init_default;
            if (!bleCopyCompLora(&lora))
                return false;
            meshtastic_AdminMessage adm = meshtastic_AdminMessage_init_default;
            adm.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
            adm.set_config.which_payload_variant = meshtastic_Config_lora_tag;
            adm.set_config.payload_variant.lora = lora;
            adm.set_config.payload_variant.lora.override_frequency = frequency;
            if (!sendAdminToNode(adm))
                return false; // link down: just fall back to Settings
            mode = MODE_BLELINK; // watch the node reboot + the link come back
            return true;
        }
        config.lora.override_frequency = frequency;
        if (nodeDB)
            nodeDB->saveToDisk(SEGMENT_CONFIG);
        rebootAtMsec = armDeadline(millis(), 1500);
        mode = MODE_REBOOT;
        return true;
    }
    if (editTarget == 3) { // primary channel name; radio restart to apply
        if (g_radioCompanion) { // remote rename: round-trip the synced channel object,
                                // PSK included, so set_channel can't wipe the key
            meshtastic_AdminMessage adm = meshtastic_AdminMessage_init_default;
            adm.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
            if (!bleCopyCompChannel(0, &adm.set_channel))
                return false;
            utf8CopyValid(adm.set_channel.settings.name, sizeof(adm.set_channel.settings.name), nameBuf);
            if (sendAdminToNode(adm)) // applied live on the node (no reboot); mirror it
                bleUpdateCompChannel(0, adm.set_channel);
            return false;
        }
        meshtastic_Channel ch = channels.getByIndex(0);
        utf8CopyValid(ch.settings.name, sizeof(ch.settings.name), nameBuf);
        channels.setChannel(ch);
        channels.onConfigChanged();
        if (nodeDB)
            nodeDB->saveToDisk(SEGMENT_CHANNELS);
        rebootAtMsec = armDeadline(millis(), 1500);
        mode = MODE_REBOOT;
        return true;
    }

    if (editTarget == 4) { // manual clock: HH:MM local -> RTC, for meshes with no time source
        int hh = -1, mm = -1;
        if (!parseClockHm(nameBuf, &hh, &mm))
            return false; // unparsable: back to Settings, row still shows "not set"
        // Date base: the last honestly-known day if we ever had one (survives
        // reboots via flash), else the build date — see epochNote().
        uint32_t base = savedDateEpoch();
        if (base < buildDateEpoch())
            base = buildDateEpoch();
        int64_t utc = (int64_t)base + hh * 3600 + mm * 60 - (int64_t)g_utcOffsetMin * 60;
#ifdef BUILD_EPOCH
        // perhapsSetRTC() silently refuses any time before the firmware build moment
        // (BUILD_EPOCH) — even with forceUpdate, the check runs first. buildDateEpoch()
        // is midnight of the build DATE, so a local HH:MM in the small hours maps to a
        // UTC instant before that moment and is rejected: the reported "00:10 won't
        // stick, 12:10 does". Roll the date forward whole days until the instant is at
        // or after the build epoch — same HH:MM, nearest date the RTC will accept.
        while (utc < (int64_t)BUILD_EPOCH)
            utc += 86400;
#endif
        struct timeval tv = {(time_t)utc, 0};
        perhapsSetRTC(RTCQualityDevice, &tv, true); // Device quality: phone/mesh still outrank it
        return false;
    }

    // name (long / short): applied live, no reboot
    if (g_radioCompanion) { // rename the linked node via set_owner, like the phone
        meshtastic_AdminMessage adm = meshtastic_AdminMessage_init_default;
        adm.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
        meshtastic_User &u = adm.set_owner;
        CompNode node;
        if (!bleCopyCompNode(g_linkMyNode.load(), &node))
            return false;
        snprintf(u.long_name, sizeof(u.long_name), "%s", node.longName);
        snprintf(u.short_name, sizeof(u.short_name), "%s", node.shortName);
        if (editTarget == 1)
            utf8CopyValid(u.short_name, sizeof(u.short_name), nameBuf);
        else
            utf8CopyValid(u.long_name, sizeof(u.long_name), nameBuf);
        if (sendAdminToNode(adm)) // mirror locally so Settings shows it at once
            bleUpdateCompNodeNames(node.num, u.long_name, u.short_name);
        return false;
    }
    if (editTarget == 1) {
        utf8CopyValid(owner.short_name, sizeof(owner.short_name), nameBuf);
    } else {
        utf8CopyValid(owner.long_name, sizeof(owner.long_name), nameBuf);
    }
    snprintf(owner.id, sizeof(owner.id), "!%08x", (unsigned)(nodeDB ? nodeDB->getNodeNum() : 0));
    if (service)
        service->reloadOwner(true); // update local node DB + broadcast NodeInfo
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE); // persist across reboots
    return false;
}


// Brings up the external panel. Guarded twice on purpose: the pins it drives are
// CS 5, RST 3 and DC 6, which are the SX1262's NSS, RST and BUSY. Touching them with
// a cap fitted would fight the radio for its chip select, so this refuses to run
// outside companion mode even if the stored flag says otherwise.
void AdvUI::extInit()
{
    if (g_ext || !g_extRot || !g_radioCompanion)
        return;
    g_ext = new (std::nothrow) AdvExtDisplay();
    if (!g_ext) {
        LOG_WARN("advui: no memory for the second screen");
        g_extRot = 0;
        return;
    }
    {
        concurrency::LockGuard guard(spiLock); // the SD card is on this bus too
        g_ext->init();
        g_ext->setRotation(g_extRot); // however the builder mounted it
        g_ext->fillScreen(0);
    }
    canvas.setPivot(120, 67); // centre of our frame, the anchor pushFrame() scales about
    LOG_INFO("advui: second screen up (ILI9341 320x240 on the EXT header)");
}

void AdvUI::extStop()
{
    if (!g_ext)
        return;
    {
        concurrency::LockGuard guard(spiLock);
        g_ext->fillScreen(0);
    }
    delete g_ext;
    g_ext = nullptr;
    LOG_INFO("advui: second screen off");
}

// The single place a finished frame reaches the panels. The external one gets the
// same canvas scaled to its width — no second framebuffer, which is the whole reason
// this fits: a second 240x135 buffer would be another 32,400 bytes, and 64,800 total
// is the figure that already starved esp-aes and broke PKI on this chip.
void AdvUI::pushFrame()
{
    if (!haveCanvas)
        return;
    canvas.pushSprite(0, 0);
    if (!g_ext)
        return;
    // Fit rather than a fixed factor: rotation changes which way round the panel is,
    // and a 90-degree mount turns 320x240 into 240x320. Scale to whichever axis runs
    // out first and centre it; the remainder stays black.
    const float zx = (float)g_ext->width() / 240.0f;
    const float zy = (float)g_ext->height() / 135.0f;
    const float z = zx < zy ? zx : zy;
    concurrency::LockGuard guard(spiLock);
    canvas.pushRotateZoom(g_ext, g_ext->width() / 2.0f, g_ext->height() / 2.0f, 0.0f, z, z);
}

// Who made this, what it is built on, and where to go with a question. The font
// credit is not decoration: GNU Unifont ships under the OFL / GPL font exception,
// and this firmware carries 2.2 MB of it.
void AdvUI::drawAbout(lgfx::LGFXBase *g)
{
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);

    int y = 20;
    g->setTextColor(0xFFE0); // yellow
    g->setCursor(4, y);
    g->print("Meshtastic ADV");
    g->setTextColor(0x9CD3);
    g->setCursor(236 - (int)strlen(ADVUI_VERSION) * 6, y);
    g->print(ADVUI_VERSION);

    y += 12;
    g->setTextColor(0x8410); // gray
    g->setCursor(4, y);
    g->print("Keyboard-first client, Cardputer ADV");

    struct Row {
        const char *k, *v;
    };
    char engine[24];
    snprintf(engine, sizeof(engine), "Meshtastic %s", optstr(APP_VERSION_SHORT));
    const Row rows[] = {
        {"Engine",  engine            },
        {"Author",  "anton-vinogradov"},
        {"Licence", "GPL-3.0"         },
    };
    y += 15;
    for (const Row &r : rows) {
        g->setTextColor(0x8410);
        g->setCursor(4, y);
        g->print(r.k);
        g->setTextColor(0xFFFF);
        g->setCursor(58, y);
        g->print(r.v);
        y += 11;
    }

    y += 4;
    g->setTextColor(0x07FF); // cyan: the address is the point of the screen
    g->setCursor(4, y);
    g->print("github.com/anton-vinogradov/");
    g->setCursor(4, y + 10);
    g->print("meshtastic-adv");

    g->setTextColor(0x4208); // dim: a credit, not a message for the user
    g->setCursor(4, 122);
    g->print("Font: GNU Unifont (OFL / GPL+FE)");
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
                            : setSection == 3 ? " / About"
                                              : "");
    if (setSection < 0) { // app version, right-aligned on the header of the top menu
        const char *ver = ADVUI_VERSION;
        g->setTextColor(0x8410); // gray
        g->setCursor(236 - (int)strlen(ver) * 6, 3);
        g->print(ver);
    }
    g->drawFastHLine(0, 13, 240, 0x39C7);

    if (setSection == 3) { // About is a page, not a list of editable rows
        drawAbout(g);
        drawFooter(g, "ESC back");
        if (haveCanvas)
            pushFrame();
        return;
    }

    const char *labels[kNumSettings] = {"Name", "Short",       "Region", "Preset", "Frequency",
                                        "Channel", "Role",     "Hops",   "Power",  "Rebroadcast",
                                        "UTC",     "WiFi",     "MQTT",   "Screen", "Radio",
                                        "Font",    "Clock",    "GPS",    "Sort",  "Names",
                                        "2nd screen"};
    char vals[kNumSettings][24];
    meshtastic_Config_LoRaConfig compLora = meshtastic_Config_LoRaConfig_init_default;
    meshtastic_Config_DeviceConfig compDevice = meshtastic_Config_DeviceConfig_init_default;
    const bool compLoraValid = g_radioCompanion && bleCopyCompLora(&compLora);
    const bool compDeviceValid = g_radioCompanion && bleCopyCompDevice(&compDevice);
    if (g_radioCompanion) { // rows 0-5 show (and remote-admin edit) the linked node
        meshtastic_NodeInfoLite *me = g_linkMyNode ? nodeByNum(g_linkMyNode) : nullptr;
        snprintf(vals[0], sizeof(vals[0]), "%s", me && nodeLongName(me)[0] ? nodeLongName(me) : "?");
        snprintf(vals[1], sizeof(vals[1]), "%s", me && nodeShortName(me)[0] ? nodeShortName(me) : "?");
        snprintf(vals[2], sizeof(vals[2]), "%s", compLoraValid ? regionName(compLora.region) : "?");
        if (compLoraValid)
            snprintf(vals[3], sizeof(vals[3]), "%s", compLora.use_preset ? presetName(compLora.modem_preset) : "custom");
        else
            strcpy(vals[3], "?");
        if (compLoraValid && compLora.override_frequency > 0)
            snprintf(vals[4], sizeof(vals[4]), "%.3f", (double)compLora.override_frequency);
        else
            strcpy(vals[4], compLoraValid ? "auto" : "?");
        snprintf(vals[5], sizeof(vals[5]), "%s", chanName(0));
    } else {
        snprintf(vals[0], sizeof(vals[0]), "%s", owner.long_name[0] ? owner.long_name : "(unset)");
        snprintf(vals[1], sizeof(vals[1]), "%s", owner.short_name[0] ? owner.short_name : "(unset)");
        snprintf(vals[2], sizeof(vals[2]), "%s", regionName(config.lora.region));
        snprintf(vals[3], sizeof(vals[3]), "%s", config.lora.use_preset ? presetName(config.lora.modem_preset) : "custom");
        if (config.lora.override_frequency > 0)
            snprintf(vals[4], sizeof(vals[4]), "%.3f", (double)config.lora.override_frequency);
        else if (RadioLibInterface::instance && RadioLibInterface::instance->getFreq() > 0)
            // No override: show the OPERATING frequency (region + preset + slot).
            // "auto" here made a slot-20 user doubt their setting was applied and
            // reach for a manual override they didn't need.
            snprintf(vals[4], sizeof(vals[4]), "%.3f", (double)RadioLibInterface::instance->getFreq());
        else
            strcpy(vals[4], "auto");
        snprintf(vals[5], sizeof(vals[5]), "%s", channels.getName(0));
    }
    if (g_radioCompanion) { // LoRa/device rows mirror the linked node too
        snprintf(vals[6], sizeof(vals[6]), "%s",
                 compDeviceValid ? optName(kRoleOpts2, kRoleCount, (int)compDevice.role) : "?");
        if (compLoraValid)
            snprintf(vals[7], sizeof(vals[7]), "%u", (unsigned)compLora.hop_limit);
        else
            strcpy(vals[7], "?");
        if (compLoraValid)
            snprintf(vals[8], sizeof(vals[8]), "%s", compLora.tx_power ? optName(kPowerOpts, kPowerCount, compLora.tx_power) : "max");
        else
            strcpy(vals[8], "?");
        snprintf(vals[9], sizeof(vals[9]), "%s",
                 compDeviceValid ? optName(kRebroadOpts, kRebroadCount, (int)compDevice.rebroadcast_mode) : "?");
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
    // Font row: "flash" is the healthy install; "not installed" tells the
    // M5Launcher crowd why their Cyrillic/emoji are boxes, and that row is
    // clickable to load it off the card anyway.
    if (sdFontOffered())
        strcpy(vals[15], "not installed");
    else
        snprintf(vals[15], sizeof(vals[15]), "%s", sdFontState());
    {
        uint32_t now = getTime(false); // device clock: the row doubles as the RTC status
        if (validStoredEpoch(now)) {
            const int64_t loc = (int64_t)now + (int64_t)g_utcOffsetMin * 60;
            snprintf(vals[16], sizeof(vals[16]), "%02u:%02u", (unsigned)((loc / 3600) % 24),
                     (unsigned)((loc / 60) % 60));
        } else
            strcpy(vals[16], "not set");
    }
    strcpy(vals[17], config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED ? "on" : "off");
    strcpy(vals[18], kSortOpts[g_nodeSort < kSortCount ? g_nodeSort : 0].name);
    strcpy(vals[19], kNameOpts[g_nameShort ? 1 : 0].name);
    // Says why it is unavailable rather than silently showing "Off": these pins belong
    // to the radio, so the answer depends on which radio mode the node is running.
    strcpy(vals[20], !g_radioCompanion ? "companion only" : optName(kExtOpts, kExtCount, (int)g_extRot));

    // What the current level shows: top = sections + direct entries (with a
    // representative value as the preview), sub = the section's flat items.
    const char *rowLabel[kNumSettings];
    const char *rowVal[kNumSettings];
    int listCount;
    if (setSection < 0) {
        static const char *topNames[kTopCount] = {"Node", "LoRa", "WiFi", "MQTT", "Device", "Radio", "About"};
        // Sections show a ">" marker, not a value: previewing ONE of many
        // sub-items ("LoRa: MediumFast") read as if that WAS the setting and
        // sent people hunting in the wrong place. Direct entries (WiFi, MQTT,
        // Radio) are single-valued, so their previews stay.
        static const uint8_t topPreview[kTopCount] = {0, 0, 11, 12, 0, 14, 0};
        static const bool topIsSection[kTopCount] = {true, true, false, false, true, false, true};
        listCount = kTopCount;
        for (int i = 0; i < kTopCount; i++) {
            rowLabel[i] = topNames[i];
            rowVal[i] = topIsSection[i] ? ">" : vals[topPreview[i]];
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
        pushFrame();
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
    if (netPage == 1) {
        // Live connection state in the header: a user staring at correct-looking
        // settings can't otherwise tell "connected" from "still trying" (field
        // question). isConnectedDirectly() is the broker link itself.
        const char *st;
        uint16_t sc;
        if (!netGetBool(1, 0)) {
            st = "off";
            sc = 0x8410;
        }
#ifdef ADVUI_SCREENSHOT
        else if (!demoNetActive() && mqtt && mqtt->isConnectedDirectly()) {
#else
        else if (mqtt && mqtt->isConnectedDirectly()) {
#endif
            st = "connected";
            sc = 0x07E0;
        }
#ifdef ADVUI_SCREENSHOT
        else if (demoNetActive() || !channels.anyMqttEnabled()) {
#else
        else if (!channels.anyMqttEnabled()) {
#endif
            // The engine parks the MQTT thread for good unless a channel opts in
            // (uplink or downlink), so "connecting..." here was a lie: nothing was
            // ever dialled. Say what's actually missing — the toggles are below.
            st = "no channel";
            sc = 0xF800;
        } else if (moduleConfig.mqtt.proxy_to_client_enabled) {
            st = "via phone"; // proxied: a direct socket is never opened by design
            sc = 0x07FF;
        } else {
            st = "connecting...";
            sc = 0xFFE0;
        }
        g->setTextColor(sc);
        g->setCursor(238 - (int)strlen(st) * 6, 3);
        g->print(st);
    }
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

#if HAS_WIFI
    // Live connection status (WiFi page only): the rows above show what's configured,
    // this shows what the radio is actually doing right now — so a wrong network or a
    // 5 GHz SSID reads as "AP not found" on the device instead of just silently not
    // connecting (which is exactly how it used to fail with no on-device feedback).
    if (netPage == 0 && netGetBool(0, 0)) {
        int sy = top + 3 * rowH + 8; // just below the 3 WiFi fields
        g->drawFastHLine(0, sy - 6, 240, 0x39C7);
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextSize(1);
        g->setTextColor(0xFFFF);
        g->setCursor(6, sy);
        g->print("Status");

        char st[24];
        uint16_t col;
        bool connected = false;
        if (demoNetActive()) {
            col = 0x07E0;
            strcpy(st, "connected");
            connected = true;
        } else if (netDirty) {
            // Edits on this page aren't live until ESC saves + reboots — the radio
            // still runs the old config, so a WiFi.status() readout here would be
            // misleading (it reads "connecting" while nothing is actually happening).
            col = 0xFFE0; // yellow
            strcpy(st, "ESC to apply");
        } else {
            int ws = WiFi.status();
            uint8_t rsn = getWifiDisconnectReason();
            if (ws == WL_CONNECTED) {
                col = 0x07E0; // green
                strcpy(st, "connected");
                connected = true;
            } else if (ws == WL_NO_SSID_AVAIL || rsn == 201) { // 201 = NO_AP_FOUND
                col = 0xF800;                                  // red
                strcpy(st, "AP not found");
            } else if (ws == WL_CONNECT_FAILED || rsn == 15 || rsn == 2) { // bad password / auth
                col = 0xF800;
                strcpy(st, "auth failed");
            } else {
                col = 0xFFE0; // yellow
                strcpy(st, "connecting...");
            }
        }
        fitWidth(g, st, 150);
        g->setTextColor(col);
        g->setCursor(236 - g->textWidth(st), sy);
        g->print(st);

        if (connected) { // second line: IP + signal once the link is up
            char ln[40];
            if (demoNetActive())
                strcpy(ln, "192.0.2.10  -55dBm");
            else
                snprintf(ln, sizeof(ln), "%s  %ddBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            g->setFont(&lgfx::fonts::Font0);
            g->setTextColor(0x9CD3);
            g->setCursor(6, sy + 16);
            g->print(ln);
        }
    }
#endif

    drawFooter(g, netDirty ? "ENTER edit   ESC save+reboot" : "ENTER edit   ESC back");

    if (haveCanvas)
        pushFrame();
}

// Companion: scan for Meshtastic nodes over BLE and pick the radio to pair with.
void AdvUI::drawBleScan()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);
    BleScanHit hits[kMaxScanHits];
    bool scanning = false;
    const int scanCount = bleScanSnapshot(hits, kMaxScanHits, &scanning);

    g->fillScreen(0x0000);
    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF);
    g->setCursor(4, 3);
    g->print("Find node");
    g->setTextColor(scanning ? 0xFFE0 : 0x8410);
    char st[20];
    if (scanning)
        strcpy(st, "scanning...");
    else
        snprintf(st, sizeof(st), "%d found", scanCount);
    g->setCursor(238 - g->textWidth(st), 3);
    g->print(st);
    g->drawFastHLine(0, 13, 240, 0x39C7);

    if (bleSel >= scanCount)
        bleSel = scanCount ? scanCount - 1 : 0;
    const int rowH = 18, top = 15;
    if (scanCount == 0) {
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextColor(0x630C);
        g->setCursor(6, 50);
        g->print(g_bleUnsupported ? "Companion image needed" : scanning ? "Listening..." : "No nodes found");
        g->setFont(&lgfx::fonts::Font0);
        g->setTextColor(0x8410);
        g->setCursor(6, 70);
        g->print(g_bleUnsupported ? "flash the companion firmware from the installer"
                                  : "node powered + BT on, phone app closed");
    }
    for (int i = 0; i < scanCount && i < 5; i++) {
        int y = top + i * rowH;
        if (i == bleSel)
            g->fillRect(0, y - 1, 240, rowH, 0x2945);
        g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
        g->setTextSize(1);
        char nm[24];
        snprintf(nm, sizeof(nm), "%s", hits[i].name[0] ? hits[i].name : hits[i].addr);
        fitWidth(g, nm, 180);
        bool saved = g_peerAddr[0] && !strcmp(g_peerAddr, hits[i].addr);
        g->setTextColor(saved ? 0xFFE0 : 0xFFFF); // the saved peer shows yellow
        g->setCursor(6, y + 1);
        g->print(nm);
        char rb[10];
        snprintf(rb, sizeof(rb), "%ddB", hits[i].rssi);
        g->setFont(&lgfx::fonts::Font0);
        g->setTextColor(hits[i].rssi > -70 ? 0x07E0 : 0x8410);
        g->setCursor(236 - g->textWidth(rb), y + 5);
        g->print(rb);
    }

    if (bleSavedMs && Throttle::isWithinTimespanMs(bleSavedMs, 4000)) {
        g->setFont(&lgfx::fonts::Font0);
        g->setTextColor(0x07E0);
        g->setCursor(4, 108);
        g->printf("saved: %s  (link lands in A3)", g_peerName);
    }
    drawFooter(g, "up/dn  ENTER connect  R rescan  ESC");

    if (haveCanvas)
        pushFrame();
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
        pushFrame();
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
        // The engine stores the passkey via std::to_string, which drops leading
        // zeros (its own screen re-pads with %06u; the status string doesn't).
        // A "1234" shown for passkey 001234 makes the phone reject the pairing.
        snprintf(pin, sizeof(pin), "%06u", (unsigned)strtoul(bluetoothStatus->getPasskey().c_str(), nullptr, 10));
    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(2);
    g->setTextColor(0xFFFF);
    g->setCursor((240 - g->textWidth(pin)) / 2, 56);
    g->print(pin);
    g->setTextSize(1);

    drawFooter(g, "ESC hide");

    if (haveCanvas)
        pushFrame();
}

// Companion: link progress/status against the saved node.
void AdvUI::drawBleLink()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);
    const BleLinkState linkState = g_linkState.load();
    const uint32_t linkRxPkts = g_linkRxPkts.load();
    const uint32_t linkMyNode = g_linkMyNode.load();
    const int linkRssi = g_linkRssi.load();
    const int linkNodeBatt = g_linkNodeBatt.load();
    const bool configDone = g_linkConfigDone.load();
    const int nodeCount = g_compNodeCount.load();
    char linkError[kBleLinkErrorSize];
    bleCopyLinkError(linkError, sizeof(linkError));

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
    switch (linkState) {
    case BLE_CONNECTING: st = "connecting..."; sc = 0xFFE0; break;
    case BLE_PAIRING:    st = "pairing: PIN on the node"; sc = 0xFFE0; break;
    case BLE_CONNECTED:  st = "connected"; sc = 0x07E0; break;
    case BLE_FAILED:     st = linkError[0] ? linkError : "failed"; sc = 0xF800; break;
    default:             st = "idle"; sc = 0x8410; break;
    }
    g->setCursor(6, 48);
    g->setTextColor(sc);
    g->print(st);

    g->setFont(&lgfx::fonts::Font0);
    g->setTextColor(0x9CD3);
    g->setCursor(6, 72);
    g->printf("packets rx: %u", (unsigned)linkRxPkts);
    if (linkMyNode) {
        g->setCursor(6, 84);
        g->printf("node id: !%08x", (unsigned)linkMyNode);
    }
    if (linkRssi) {
        g->setCursor(130, 72);
        g->printf("link: %ddB", linkRssi);
    }
    if (linkNodeBatt >= 0) {
        g->setCursor(130, 84);
        g->printf("node batt: %d%%", linkNodeBatt);
    }
    if (linkState == BLE_CONNECTED && configDone) {
        g->setTextColor(0x07E0);
        g->setCursor(6, 102);
        g->printf("link OK - %d nodes synced", nodeCount);
    }

    drawFooter(g, linkState == BLE_FAILED ? "auto-retry  R now  F forget  ESC"
                                          : "R reconnect  F forget  ESC scan");

    if (haveCanvas)
        pushFrame();
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

    if (pickTarget == 12) {
        // The honest recommendation, shown exactly where the user is about to
        // make the trade: the card works, but the proper install is better in
        // every way (no RAM cost, no card needed, full Unicode incl. CJK).
        g->setFont(&lgfx::fonts::Font0);
        g->setTextSize(1);
        g->setTextColor(0xFFE0); // amber
        g->setCursor(6, y + 4);
        g->print("No font partition: this install");
        g->setCursor(6, y + 14);
        g->print("came from M5Launcher. Reflash via");
        g->setCursor(6, y + 24);
        g->setTextColor(0x07E0); // green: the recommended path
        g->print("M5Burner / web installer for full");
        g->setCursor(6, y + 34);
        g->print("Unicode without the RAM cost.");
    }

    drawFooter(g, "up/dn   ENTER select   ESC back");

    if (haveCanvas)
        pushFrame();
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
        pushFrame();
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
        pushFrame();
}

#ifdef ADVUI_SCREENSHOT
static bool shotLive = false; // 'L' over serial: dump the frame right after the next render
#ifdef ADVUI_HIL
static bool hilDigestPending = false;
static bool hilHistorySeeded = false;
static uint32_t hilBootNonce = 0;

uint32_t hilBootId()
{
    if (!hilBootNonce) {
        hilBootNonce = esp_random();
        if (!hilBootNonce)
            hilBootNonce = micros() | 1U;
    }
    return hilBootNonce;
}

const char *AdvUI::hilModeName() const
{
    static const char *names[] = {"chats",   "nodes",   "picker", "node",    "compose",
                                  "setname", "settings", "picklist", "reboot", "emoji",
                                  "netpage", "blescan", "blepin", "blelink", "btpin"};
    const unsigned index = static_cast<unsigned>(mode);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}

void AdvUI::hilState()
{
    // Native USB CDC defaults to a zero TX timeout. A response longer than its
    // current endpoint capacity is then silently truncated, so keep each record
    // short as well as enabling bounded blocking for the dev-only protocol.
    Serial.setTxTimeoutMs(1000);
    Serial.printf("@@STATE v=2 boot=%08x reset=%d mode=%s splash=%u screen=%u keyboard=%u canvas=%u node=%08x selected=%08x channel=%d "
                  "compose=%u send_error=%u send_errno=%u query=%u set_section=%d set_sel=%d net_page=%d backend=%s\n",
                  (unsigned)hilBootId(), (int)esp_reset_reason(), hilModeName(), splashDone ? 1U : 0U,
                  screenOn ? 1U : 0U, kbMissing ? 0U : 1U,
                  haveCanvas ? 1U : 0U, (unsigned)myNodeNum(), (unsigned)selectedNum, selectedChannel, (unsigned)msgLen,
                  (unsigned)sendFailure, (unsigned)g_hilLastSendError, (unsigned)queryLen, setSection, setSel, netPage,
                  g_radioCompanion ? "companion" : "onboard");
    Serial.printf("@@STATS v=2 messages=%d hist=%d unread=%d reactions=%d conversations=%d filtered=%d heap=%u "
                  "min_heap=%u link=%u rx_drop=%u tx_drop=%u radio_tx=%u rf_region=%d rf_power=%d sort=%u names=%u\n",
                  g_msgCount, histTotal(), unreadCount(), g_reactCount, convCount, filteredCount, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)g_linkState.load(), (unsigned)bleRxDrops(), (unsigned)bleTxDrops(),
                  0U, // effective HIL TX ability: immutable Router/RadioLib guards always deny it
                  (int)config.lora.region, (int)config.lora.tx_power,
                  (unsigned)g_nodeSort, (unsigned)g_nameShort);
    unsigned statuses[5] = {};
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].status < 5)
            statuses[g_msgs[i].status]++;
    const int last = g_msgCount ? g_msgCount - 1 : -1;
    const Msg empty = {};
    const Msg &m = last >= 0 ? g_msgs[last] : empty;
    uint32_t textFnv = 2166136261U;
    for (const unsigned char *p = (const unsigned char *)m.text; *p; p++)
        textFnv = (textFnv ^ *p) * 16777619U;
    Serial.printf("@@LAST v=2 incoming=%u sending=%u delivered=%u failed=%u sent=%u id=%08x from=%08x to=%08x "
                  "ch=%u status=%u err=%u reply=%08x tx_reply=%08x read=%u text_len=%u text_fnv=%08x\n",
                  statuses[MSG_IN], statuses[MSG_SENDING], statuses[MSG_DELIVERED], statuses[MSG_FAILED],
                  statuses[MSG_SENT], (unsigned)m.id, (unsigned)m.from, (unsigned)m.to, (unsigned)m.ch,
                  (unsigned)m.status, (unsigned)m.err, (unsigned)(last >= 0 ? g_msgReply[last] : 0),
                  (unsigned)g_hilLastTxReply, m.read ? 1U : 0U, (unsigned)strlen(m.text), (unsigned)textFnv);

    CompNode comp = {};
    const int compNodes = g_compNodeCount.load();
    const bool haveComp = compNodes > 0 && bleCopyCompNodeAt(compNodes - 1, &comp);
    uint32_t compNameFnv = 2166136261U;
    for (const unsigned char *p = (const unsigned char *)comp.longName; haveComp && *p; p++)
        compNameFnv = (compNameFnv ^ *p) * 16777619U;
    meshtastic_Channel compCh = meshtastic_Channel_init_default;
    const bool haveCh = bleCopyCompChannel(0, &compCh) &&
                        (compCh.has_settings || compCh.role != meshtastic_Channel_Role_DISABLED);
    uint32_t chNameFnv = 2166136261U;
    for (const unsigned char *p = (const unsigned char *)compCh.settings.name; haveCh && compCh.has_settings && *p; p++)
        chNameFnv = (chNameFnv ^ *p) * 16777619U;
    meshtastic_Config_LoRaConfig compLora = meshtastic_Config_LoRaConfig_init_default;
    meshtastic_Config_DeviceConfig compDevice = meshtastic_Config_DeviceConfig_init_default;
    const bool haveLora = bleCopyCompLora(&compLora), haveDevice = bleCopyCompDevice(&compDevice);
    Serial.printf("@@COMP my=%08x nodes=%d seen=%d done=%u batt=%d last=%08x hops=%u key=%u name_fnv=%08x "
                  "channel_present=%u channel_fnv=%08x lora=%u preset=%d region=%d tx=%d device=%u role=%d "
                  "\n",
                  (unsigned)g_linkMyNode.load(), compNodes, (int)g_compNodesSeen.load(),
                  g_linkConfigDone.load() ? 1U : 0U, (int)g_linkNodeBatt.load(),
                  haveComp ? (unsigned)comp.num : 0U, haveComp ? (unsigned)comp.hops : 0U,
                  haveComp && comp.hasKey ? 1U : 0U, (unsigned)compNameFnv, haveCh ? 1U : 0U,
                  (unsigned)chNameFnv, haveLora ? 1U : 0U, haveLora ? (int)compLora.modem_preset : 0,
                  haveLora ? (int)compLora.region : 0, haveLora ? (int)compLora.tx_power : 0,
                  haveDevice ? 1U : 0U, haveDevice ? (int)compDevice.role : 0);
    // Keep diagnostics on their own bounded CDC record. Extending @@COMP past
    // the native-USB TX buffer's comfortable line size made query-heavy HIL
    // runs retain transport buffers and eventually starve the companion arena.
    Serial.printf("@@CHEAP buffers=%u ingress_first=%u ingress_last=%u ingress_count=%u ingress_pre=%u "
                  "ingress_direct=%d ingress_max=%u\n",
                  bleHilBuffersReady() ? 1U : 0U,
                  (unsigned)bleHilIngressFirstHeap(), (unsigned)bleHilIngressLastHeap(),
                  (unsigned)bleHilIngressCount(), (unsigned)bleHilIngressLastBeforeHeap(),
                  (int)bleHilIngressDirectDelta(), (unsigned)bleHilIngressMaxDrop());
    Serial.flush();
}

void AdvUI::hilReactionState()
{
    unsigned exact = 0;
    for (int i = 0; i < g_reactCount; i++)
        if (g_reacts[i].targetFrom)
            exact++;
    const int last = g_reactCount ? ringIdx(g_reactCount - 1, g_reactCount, g_reactNext, kMaxReacts) : -1;
    // This deliberately has its own on-demand command. Adding another record to
    // every query retained a native-USB TX buffer until the first archive write,
    // producing an artificial 2 KiB minimum-heap trough in an otherwise steady run.
    Serial.printf("@@REACT v=1 react_exact=%u react_ambiguous=%u react_last_target=%08x\n", exact,
                  (unsigned)(g_reactCount - (int)exact),
                  last >= 0 ? (unsigned)g_reacts[last].targetFrom : 0U);
    Serial.flush();
}

void AdvUI::hilMemoryState()
{
    const uint32_t heap = ESP.getFreeHeap(), minimum = ESP.getMinFreeHeap();
    unsigned incoming = 0, sending = 0;
    for (int i = 0; i < g_msgCount; i++) {
        incoming += g_msgs[i].status == MSG_IN;
        sending += g_msgs[i].status == MSG_SENDING;
    }
    const int history = g_histTotal >= 0 ? g_histTotal : histTotal();
    Serial.printf("@@MEM v=1 saves=%u save_before=%u save_after=%u min_before=%u min_after=%u heap=%u min_heap=%u "
                  "messages=%d hist=%d incoming=%u sending=%u\n",
                  (unsigned)g_hilSaveCount, (unsigned)g_hilSaveHeapBefore, (unsigned)g_hilSaveHeapAfter,
                  (unsigned)g_hilSaveMinBefore, (unsigned)g_hilSaveMinAfter, (unsigned)heap, (unsigned)minimum,
                  g_msgCount, history, incoming, sending);
    Serial.flush();
}

void AdvUI::hilFrameDigest()
{
    if (!haveCanvas) {
        Serial.println("@@FRAME v=1 error=no_canvas");
        Serial.flush();
        return;
    }
    const uint8_t *buf = static_cast<const uint8_t *>(canvas.getBuffer());
    const int w = display.width(), h = display.height();
    const int stride = static_cast<int>(canvas.bufferLength() / h);
    uint32_t fnv = 2166136261U;
    bool seen[256] = {};
    unsigned colors = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t value = buf[y * stride + x];
            fnv = (fnv ^ value) * 16777619U;
            if (!seen[value]) {
                seen[value] = true;
                colors++;
            }
        }
    Serial.printf("@@FRAME v=1 mode=%s w=%d h=%d bytes=%u fnv=%08x colors=%u\n", hilModeName(), w, h,
                  (unsigned)(w * h), (unsigned)fnv, colors);
    Serial.flush();
}

void AdvUI::hilHome()
{
    histExit();
    mode = MODE_CHATS;
    query[0] = 0;
    queryLen = 0;
    sel = 0;
    scrollTop = 0;
    msgBuf[0] = 0;
    msgLen = 0;
    sendFailure = SendFailure::NONE;
    g_hilLastSendError = ERRNO_OK;
    g_hilLastTxReply = 0;
    pendingLat = 0;
    pendingReplyId = 0;
    reactSel = -1;
    reactStrip = false;
    confirmDel = false;
    setSection = -1;
    setSel = 0;
    setScroll = 0;
    uiDirty = true;
    Serial.println("@@ACK home");
    Serial.flush();
}

void AdvUI::hilInject(const uint8_t *bytes, uint16_t len)
{
    static meshtastic_FromRadio fr;
    fr = meshtastic_FromRadio_init_default;
    const bool ok = bytes && len && pb_decode_from_bytes(bytes, len, &meshtastic_FromRadio_msg, &fr);
    const bool changed = ok && handleFromRadio(fr); // identical ingress to radio/BLE traffic
    if (changed) {
        uiDirty = true;
    }
    Serial.printf("@@INJECT v=1 ok=%u changed=%u bytes=%u variant=%u\n", ok ? 1U : 0U, changed ? 1U : 0U,
                  (unsigned)len, ok ? (unsigned)fr.which_payload_variant : 0U);
    Serial.flush();
}

void AdvUI::hilInjectCompanion(const uint8_t *bytes, uint16_t len)
{
    const bool ok = bleHilInjectFromRadio(bytes, len);
    bool uiChanged = false;
    if (ok) {
        // Companion mesh packets normally cross the fixed pump->UI queue. Drain
        // it here as runOnce would after a real BLE read, retaining that boundary.
        BleFrame frame;
        while (bleNextPacket(&frame)) {
            meshtastic_FromRadio fr = meshtastic_FromRadio_init_default;
            if (pb_decode_from_bytes(frame.data, frame.len, &meshtastic_FromRadio_msg, &fr)) {
                uiChanged = handleFromRadio(fr) || uiChanged;
            }
        }
        // Production routes node/channel/config frames on the BLE pump without
        // forcing a full display push for every entry. HIL must not invent 66
        // back-to-back 32 KB redraws while exercising that same config stream.
        if (uiChanged)
            uiDirty = true;
    }
    Serial.printf("@@CINJECT v=1 ok=%u changed=%u bytes=%u\n", ok ? 1U : 0U, uiChanged ? 1U : 0U,
                  (unsigned)len);
    Serial.flush();
}

void AdvUI::hilClearData()
{
    histExit();
    g_msgCount = 0;
    memset(g_msgs, 0, sizeof(g_msgs));
    memset(g_msgReply, 0, sizeof(g_msgReply));
    g_reactCount = 0;
    g_reactNext = 0;
    memset(g_reacts, 0, sizeof(g_reacts));
    g_msgsDirty = false;
    g_hilSaveCount = 0;
    g_hilSaveHeapBefore = g_hilSaveHeapAfter = 0;
    g_hilSaveMinBefore = g_hilSaveMinAfter = 0;
    g_histTotal = -1;
    g_seen24 = -1;
    g_seenPendN = 0;
    g_seenPendNext = 0;
    hilHistorySeeded = false;
    FSCom.remove(kMsgPath);
    FSCom.remove(kHistPath);
    FSCom.remove(kSeenPath);
    FSCom.remove(kEpochPath);
    FSCom.remove(kUiCfgPath);
    FSCom.remove(kRadioPath);
    FSCom.remove(kBleAttemptPath);
    g_uiCfgDirty = false;
    g_radioCfgDirty = false;
    bleAttemptArmed = false;
    bleHilClearState();
    uiDirty = true;
    Serial.println("@@ACK clear");
    Serial.flush();
}

void AdvUI::hilPersist()
{
    const bool saved = saveMsgs();
    if (saved)
        g_msgsDirty = false;
    Serial.printf("@@ACK persist ok=%u messages=%d\n", saved ? 1U : 0U, g_msgCount);
    Serial.flush();
}

void AdvUI::hilLoadLegacyStore()
{
    // Isolated, synthetic AVS4 record: prove the current loader widens its
    // target-less ReactionV4 without touching any production-namespaced file.
    SafeFile f(kMsgPath, true);
    const uint32_t magic = kMsgMagicV4;
    const int32_t count = 1;
    const uint8_t ru = g_ruMode ? 1 : 0, reactCount = 1;
    Msg message = {};
    message.from = 0x000BEEF1;
    message.to = 0x00A11CE1;
    message.rxTime = 1750000000U;
    message.id = 0x41565334U;
    message.status = MSG_IN;
    message.read = true;
    utf8CopyValid(message.text, sizeof(message.text), "legacy AVS4 fixture");
    const uint32_t replyId = 0;
    ReactionV4 reaction = {};
    reaction.msgId = message.id;
    reaction.from = 0x000BEEF2;
    utf8CopyValid(reaction.label, sizeof(reaction.label), "\U0001F44D");
    bool ok = f.write((const uint8_t *)&magic, sizeof(magic)) == sizeof(magic) &&
              f.write((const uint8_t *)&count, sizeof(count)) == sizeof(count) &&
              f.write((const uint8_t *)&g_favChannels, sizeof(g_favChannels)) == sizeof(g_favChannels) &&
              f.write((const uint8_t *)&g_utcOffsetMin, sizeof(g_utcOffsetMin)) == sizeof(g_utcOffsetMin) &&
              f.write((const uint8_t *)&g_screenOffSec, sizeof(g_screenOffSec)) == sizeof(g_screenOffSec) &&
              f.write(&ru, 1) == 1 && f.write(&reactCount, 1) == 1 &&
              f.write((const uint8_t *)&message, sizeof(message)) == sizeof(message) &&
              f.write((const uint8_t *)&replyId, sizeof(replyId)) == sizeof(replyId) &&
              f.write((const uint8_t *)&reaction, sizeof(reaction)) == sizeof(reaction);
    const bool closed = f.close();
    ok = ok && closed;
    if (ok) {
        g_msgCount = 0;
        g_reactCount = 0;
        g_reactNext = 0;
        loadMsgs();
        ok = g_msgCount == 1 && g_reactCount == 1 && g_reacts[0].targetFrom == 0;
    }
    Serial.printf("@@LEGACY v=1 ok=%u messages=%d reactions=%d\n", ok ? 1U : 0U, g_msgCount, g_reactCount);
    Serial.flush();
}

void AdvUI::hilSeenSaturation()
{
    constexpr uint32_t freshId = 0x7FFEF001;
    const uint32_t now = getTime(false);
    bool ok = validStoredEpoch(now);
    FSCom.remove(kSeenPath);
    String tempPath = String(kSeenPath) + ".tmp";
    FSCom.remove(tempPath.c_str());

    SafeFile out(kSeenPath, true);
    ok = ok && out.write((const uint8_t *)&kSeenMagic, sizeof(kSeenMagic)) == sizeof(kSeenMagic);
    for (int i = 0; ok && i < kSeenCap; i++) {
        SeenRec rec = {0x60000000U + (uint32_t)i, now - 2U * 86400U - (uint32_t)i};
        ok = out.write((const uint8_t *)&rec, sizeof(rec)) == sizeof(rec);
    }
    const bool committed = out.close();
    ok = ok && committed;

    if (ok) {
        g_seenPendN = 0;
        g_seenPendNext = 0;
        seenNote(freshId);
        seenReconcile();
    }

    bool found = false;
    size_t records = 0;
    auto check = FSCom.open(kSeenPath, FILE_O_READ);
    uint32_t magic = 0;
    ok = ok && check && check.read((uint8_t *)&magic, sizeof(magic)) == sizeof(magic) && magic == kSeenMagic &&
         checkedFixedRecordFile(check.size(), sizeof(magic), sizeof(SeenRec), &records) && records == kSeenCap;
    SeenRec rec;
    for (size_t i = 0; ok && i < records; i++) {
        if (check.read((uint8_t *)&rec, sizeof(rec)) != (int)sizeof(rec)) {
            ok = false;
            break;
        }
        found = found || rec.num == freshId;
    }
    if (check)
        check.close();
    ok = ok && found;
    const int active = g_seen24;

    // Leave no synthetic state behind; HIL paths are separate from production,
    // but each case must still be independently repeatable.
    FSCom.remove(kSeenPath);
    FSCom.remove(tempPath.c_str());
    g_seen24 = -1;
    g_seenPendN = 0;
    g_seenPendNext = 0;
    Serial.printf("@@SEEN v=1 ok=%u records=%u found=%u active=%d\n", ok ? 1U : 0U, (unsigned)records,
                  found ? 1U : 0U, active);
    Serial.flush();
}
#endif

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
        if ((y & 7) == 7)
            delay(1); // let the idle task feed the watchdog during a full demo dump
    }
    Serial.println("@@END");
    Serial.flush();
    Serial.setTxTimeoutMs(0); // restore the USB-CDC guard
    delay(30);
}

// Renders each screen with controlled RAM-only data and then soft-reboots. Nothing is
// persisted: runOnce cannot reach saveMsgs while this function owns the UI thread, so
// rebooting transactionally restores the real messages/config without a fragile manual
// snapshot of every UI field.
void AdvUI::runDemoDump()
{
    uint32_t me = myNodeNum();

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
    meshtastic_NodeInfoLite *demoNode = filteredCount > 0 ? nodeAt(filtered[0]) : nullptr;
    uint32_t peer = demoNode ? demoNode->num : me;
    demoNode = filteredCount > 1 ? nodeAt(filtered[1]) : nullptr;
    uint32_t other = demoNode ? demoNode->num : peer + 1;
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
    addReaction(1, peer, "\U0001F44D", me); // their tapback on our "Yep, 5/5"
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

    // WiFi / MQTT pages use view-only fixtures. Never mutate the engine's live
    // config: its network threads run concurrently and a save from another
    // subsystem must not be able to persist promotional credentials.
    g_demoNetActive = true;
    netSel = 0;
    netScroll = 0;
    netPage = 0;
    drawNetPage();
    g_demoNetActive = false;
    screenshot("wifi");
    g_demoNetActive = true;
    netPage = 1;
    drawNetPage();
    g_demoNetActive = false;
    screenshot("mqtt");

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

    // Worst-case wrapping: 32 long messages produce far more than the 96-line
    // display ring. This exercises its eviction/anchor path on every release
    // HIL run and catches both blank frames and watchdog-level render regressions.
    g_msgCount = 0;
    g_reactCount = 0;
    g_reactNext = 0;
    for (int i = 0; i < kMaxMsgs; i++) {
        char dense[160];
        snprintf(dense, sizeof(dense),
                 "%02d Long wrapped message checks the fixed display ring without moving five kilobytes per line. "
                 "Cyrillic: проверка. Emoji: \U0001F44D \U0001F525. Tail: abcdefghijklmnopqrstuvwxyz.",
                 i + 1);
        addMsg((i & 1) ? me : peer, (i & 1) ? peer : me, 0, t + i, false, dense, 100 + i,
               (i & 1) ? MSG_DELIVERED : MSG_IN);
    }
    selectedChannel = -1;
    selectedNum = peer;
    chatScroll = 0;
    mode = MODE_NODE;
    drawNode();
    screenshot("stress");

    // Companion screens (sample link state; these globals only matter in companion mode)
    BleScanHit demoHits[3] = {};
    snprintf(demoHits[0].name, sizeof(demoHits[0].name), "Heltec-V3 mesh");
    snprintf(demoHits[0].addr, sizeof(demoHits[0].addr), "a1:b2:c3:d4:e5:f6");
    demoHits[0].rssi = -52;
    snprintf(demoHits[1].name, sizeof(demoHits[1].name), "T-Beam Garden");
    snprintf(demoHits[1].addr, sizeof(demoHits[1].addr), "11:22:33:44:55:66");
    demoHits[1].rssi = -74;
    snprintf(demoHits[2].name, sizeof(demoHits[2].name), "RAK-Roof");
    snprintf(demoHits[2].addr, sizeof(demoHits[2].addr), "aa:bb:cc:dd:ee:ff");
    demoHits[2].rssi = -81;
    bleSetScanSnapshot(demoHits, 3, false);
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
    bleSetScanSnapshot(nullptr, 0, false);
    g_peerName[0] = 0;
    pinLen = 0;
    pinBuf[0] = 0;

    // --- Usage-scenario frames (a01..) for the animated hero.
    beginDemoIdentity();
    me = myNodeNum();
    peer = kDemoPeer;
    g_radioCompanion = false;
    g_ruMode = false;
    g_nameShort = 0;
    g_utcOffsetMin = 0;
    g_favChannels = 0;
    g_favNodeCount = 0;
    histExit();
    sendFailure = SendFailure::NONE;
    confirmDel = false;
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
    snprintf(msgBuf, sizeof(msgBuf), "O");
    msgLen = (uint8_t)strlen(msgBuf);
    drawNode();
    screenshot("a03");
    snprintf(msgBuf, sizeof(msgBuf), "On m");
    msgLen = (uint8_t)strlen(msgBuf);
    drawNode();
    screenshot("a04");
    snprintf(msgBuf, sizeof(msgBuf), "On my w");
    msgLen = (uint8_t)strlen(msgBuf);
    drawNode();
    screenshot("a05");
    snprintf(msgBuf, sizeof(msgBuf), "On my way");
    msgLen = (uint8_t)strlen(msgBuf);
    drawNode();
    screenshot("a06");

    emojiSel = 5; // slightly smiling face
    emojiReturn = MODE_COMPOSE;
    mode = MODE_EMOJI;
    drawEmoji();
    screenshot("a07");

    snprintf(msgBuf, sizeof(msgBuf), "On my way \U0001F642");
    msgLen = (uint8_t)strlen(msgBuf);
    mode = MODE_COMPOSE;
    drawNode();
    screenshot("a08");

    msgBuf[0] = 0;
    msgLen = 0;
    addMsg(me, peer, 0, t + 60, false, "On my way \U0001F642", 16, MSG_SENDING);
    mode = MODE_NODE;
    chatScroll = 0;
    drawNode();
    screenshot("a09"); // sent: the pending dot
    ackMsg(16, MSG_DELIVERED, 0);
    drawNode();
    screenshot("a10"); // the routing ACK lands: green check
    addMsg(peer, me, 0, t + 120, false, "Copy. See you there \U0001F44D", 17, MSG_IN);
    drawNode();
    screenshot("a11"); // their reply arrives

    reactSel = 0;
    reactStrip = true;
    reactPick = 5; // fire
    drawNode();
    screenshot("a12");

    addReaction(17, me, "\U0001F525", peer);
    reactSel = -1;
    reactStrip = false;
    drawNode();
    screenshot("a13");

    pendingReplyId = 17;
    utf8Copy(replyPrev, "Copy. See you there \U0001F44D", (int)sizeof(replyPrev) - 1);
    g_ruMode = true;
    mode = MODE_COMPOSE;
    snprintf(msgBuf, sizeof(msgBuf), "\xD0\x94");
    msgLen = (uint8_t)strlen(msgBuf);
    drawNode();
    screenshot("a14");
    snprintf(msgBuf, sizeof(msgBuf), "\xD0\x94\xD0\xB0, \xD0\xB2\xD0\xB8\xD0\xB6\xD1\x83");
    msgLen = (uint8_t)strlen(msgBuf);
    drawNode();
    screenshot("a15");
    snprintf(msgBuf, sizeof(msgBuf),
             "\xD0\x94\xD0\xB0, \xD0\xB2\xD0\xB8\xD0\xB6\xD1\x83 \xD1\x81\xD0\xB2\xD0\xB5\xD1\x82 \U0001F525");
    msgLen = (uint8_t)strlen(msgBuf);
    drawNode();
    screenshot("a16"); // quoted Cyrillic reply

    msgBuf[0] = 0;
    msgLen = 0;
    pendingReplyId = 0;
    g_ruMode = false;
    addMsg(me, peer, 0, t + 180, false,
           "\xD0\x94\xD0\xB0, \xD0\xB2\xD0\xB8\xD0\xB6\xD1\x83 \xD1\x81\xD0\xB2\xD0\xB5\xD1\x82 \U0001F525",
           18, MSG_SENDING, 17);
    mode = MODE_NODE;
    chatScroll = 0;
    drawNode();
    screenshot("a17");

    ackMsg(18, MSG_DELIVERED, 0);
    drawNode();
    screenshot("a18");

    sel = 0;
    scrollTop = 0;
    mode = MODE_CHATS;
    drawChats();
    screenshot("a19"); // updated preview, no unread badge
    Serial.println("@@DONE");
    Serial.flush();
    delay(200); // give the host time to consume @@DONE before USB resets
    ESP.restart();
    for (;;)
        delay(1000); // ESP.restart() is not expected to return
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
        meshtastic_AdminMessage adm = meshtastic_AdminMessage_init_default;
        adm.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        if (devCfg) {
            adm.set_config.which_payload_variant = meshtastic_Config_device_tag;
            meshtastic_Config_DeviceConfig &dc = adm.set_config.payload_variant.device;
            if (!bleCopyCompDevice(&dc))
                return;
            if (target == 5)
                dc.role = (meshtastic_Config_DeviceConfig_Role)value;
            else
                dc.rebroadcast_mode = (meshtastic_Config_DeviceConfig_RebroadcastMode)value;
            if (sendAdminToNode(adm)) {
                bleUpdateCompDevice(dc); // optimistically ours; the reconnect config stream confirms
                mode = MODE_BLELINK;
            }
            return;
        }
        adm.set_config.which_payload_variant = meshtastic_Config_lora_tag;
        meshtastic_Config_LoRaConfig &lc = adm.set_config.payload_variant.lora;
        if (!bleCopyCompLora(&lc))
            return;
        switch (target) {
        case 0: lc.region = (meshtastic_Config_LoRaConfig_RegionCode)value; break;
        case 6: lc.hop_limit = (uint32_t)value; break;
        case 7: lc.tx_power = value; break;
        default:
            lc.use_preset = true;
            lc.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)value;
        }
        if (sendAdminToNode(adm)) {
            bleUpdateCompLora(lc);
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
    rebootAtMsec = armDeadline(millis(), 1500);
    mode = MODE_REBOOT;
}

// Opens the editor/picker for one flat settings item (see the labels table).
void AdvUI::openSetting(int item)
{
    meshtastic_Config_LoRaConfig compLora = meshtastic_Config_LoRaConfig_init_default;
    meshtastic_Config_DeviceConfig compDevice = meshtastic_Config_DeviceConfig_init_default;
    meshtastic_Channel compPrimary = meshtastic_Channel_init_default;
    const bool compLoraValid = g_radioCompanion && bleCopyCompLora(&compLora);
    const bool compDeviceValid = g_radioCompanion && bleCopyCompDevice(&compDevice);
    const bool compPrimaryValid = g_radioCompanion && bleCopyCompChannel(0, &compPrimary);
    auto openPick = [this](int target, int current) {
        const PickList list = pickListFor(target);
        const int index = optIndex(list.opts, list.cnt, current);
        if (index < 0) {
            LOG_WARN("advui: refusing %s picker for unsupported current value %d", list.title, current);
            return false;
        }
        pickTarget = target;
        pickSel = index;
        pickScroll = 0;
        mode = MODE_PICKLIST;
        return true;
    };
    if (item <= 1) { // 0 = long name, 1 = short name
        editTarget = item;
        const char *cur;
        if (g_radioCompanion) { // editing the linked node's names (remote admin)
            meshtastic_NodeInfoLite *lm = g_linkMyNode ? nodeByNum(g_linkMyNode) : nullptr;
            cur = lm ? (item == 1 ? nodeShortName(lm) : nodeLongName(lm)) : "";
        } else {
            cur = (item == 1) ? owner.short_name : owner.long_name;
        }
        strncpy(nameBuf, cur, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = 0;
        nameLen = strlen(nameBuf);
        nameReturn = MODE_SETTINGS;
        mode = MODE_SETNAME;
    } else if (item == 2) { // Region
        if (g_radioCompanion && !compLoraValid)
            return; // config not synced yet: nothing to edit against
        openPick(0, (int)(g_radioCompanion ? compLora.region : config.lora.region));
    } else if (item == 3) { // Preset
        if (g_radioCompanion && !compLoraValid)
            return;
        openPick(1, (int)(g_radioCompanion ? compLora.modem_preset : config.lora.modem_preset));
    } else if (item == 4) { // Frequency (MHz)
        if (g_radioCompanion && !compLoraValid)
            return;
        editTarget = 2;
        float ovr = g_radioCompanion ? compLora.override_frequency : config.lora.override_frequency;
        if (ovr > 0)
            snprintf(nameBuf, sizeof(nameBuf), "%.3f", (double)ovr);
        else
            nameBuf[0] = 0;
        nameLen = strlen(nameBuf);
        nameReturn = MODE_SETTINGS;
        mode = MODE_SETNAME;
    } else if (item == 5) { // Channel name
        if (g_radioCompanion && (!compPrimaryValid || !compPrimary.has_settings))
            return; // channel not synced yet: nothing to round-trip
        editTarget = 3;
        strncpy(nameBuf, g_radioCompanion ? compPrimary.settings.name : channels.getByIndex(0).settings.name,
                sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = 0;
        nameLen = strlen(nameBuf);
        nameReturn = MODE_SETTINGS;
        mode = MODE_SETNAME;
    } else if (item == 6) { // Role
        if (g_radioCompanion && !compDeviceValid)
            return;
        openPick(5, (int)(g_radioCompanion ? compDevice.role : config.device.role));
    } else if (item == 7) { // Hop limit
        if (g_radioCompanion && !compLoraValid)
            return;
        openPick(6, (int)(g_radioCompanion ? compLora.hop_limit : config.lora.hop_limit));
    } else if (item == 8) { // TX power
        if (g_radioCompanion && !compLoraValid)
            return;
        openPick(7, (int)(g_radioCompanion ? compLora.tx_power : config.lora.tx_power));
    } else if (item == 9) { // Rebroadcast mode
        if (g_radioCompanion && !compDeviceValid)
            return;
        openPick(8, (int)(g_radioCompanion ? compDevice.rebroadcast_mode : config.device.rebroadcast_mode));
    } else if (item == 10) { // UTC offset -> city/offset picker
        openPick(2, g_utcOffsetMin);
    } else if (item == 15 && (sdFontOffered() || strcmp(sdFontState(), "sd") == 0)) {
        pickTarget = 12; // unicode font source (only meaningful without the partition)
        pickSel = strcmp(sdFontState(), "sd") == 0 ? 1 : 0;
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 20) { // second screen -> picker, companion only
        // Refuse outright rather than let the picker set a flag that extInit() will
        // ignore: with a cap fitted, CS/RST/DC are the radio's NSS/RST/BUSY.
        if (!g_radioCompanion)
            return;
        openPick(13, (int)g_extRot);
    } else if (item == 19) { // node-list name style -> picker
        pickTarget = 11;
        pickSel = g_nameShort ? 1 : 0;
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 18) { // node-list sort -> mode picker
        openPick(10, (int)g_nodeSort);
    } else if (item == 16) { // manual clock -> HH:MM entry (applied in applyName)
        editTarget = 4;
        nameBuf[0] = 0;
        nameLen = 0;
        nameReturn = MODE_SETTINGS;
        mode = MODE_SETNAME;
    } else if (item == 13) { // Screen auto-off -> timeout picker
        openPick(4, (int)g_screenOffSec);
    } else if (item == 14) { // Radio backend -> onboard/companion picker
        pickTarget = 3;
        pickSel = g_radioCompanion ? 1 : 0;
        pickScroll = 0;
        mode = MODE_PICKLIST;
    } else if (item == 17) { // GPS on/off (local device position config)
        openPick(9, config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED ? 1 : 0);
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
        markMsgsDirty(); // the layout choice is persisted with the message file
        return;
    }

    if (mode == MODE_SETNAME) {
        unsigned maxLen = editMax(editTarget);
        if (maxLen > sizeof(nameBuf) - 1) // never write past the shared editor buffer
            maxLen = sizeof(nameBuf) - 1;
        bool numeric = (editTarget == 2 || editTarget == 4); // frequency: digits + '.'; clock: digits + ':'
        if (esc) {
            mode = nameReturn;
        } else if (enter) {
            // Blank channel/frequency values are legitimate: the preset supplies
            // the channel name and a zero override restores automatic frequency.
            bool rebooting = (nameLen || editTarget == 2 || editTarget == 3) && applyName();
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
        } else if (g_ruMode && !numeric && editTarget < 20 && printable && nameLen < maxLen) {
            translitFeed(nameBuf, nameLen, maxLen + 1, c, pendingLat); // limits are UTF-8 bytes on the wire
        } else if (printable && nameLen < maxLen &&
                   (!numeric || (c >= '0' && c <= '9') || c == (editTarget == 4 ? ':' : '.'))) {
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
                        : setSection == 3 ? 0 // About has no rows to move between
                                          : (int)sizeof(kSecDevice);
        if (esc) {
            if (setSection >= 0) { // back to the top level, cursor on the section's row
                setSel = setSection == 0 ? 0 : setSection == 1 ? 1 : setSection == 3 ? 6 : 4;
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
                case 6: // About
                    setSection = 3;
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
                rebootAtMsec = armDeadline(millis(), 1500);
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
        BleScanHit hit;
        const bool haveHit = bleScanHit(bleSel, &hit);
        if (esc) {
            bleScanStop();
            setSel = 0;
            mode = MODE_SETTINGS; // back out (e.g. to flip the radio back to onboard)
        } else if (up) {
            if (bleSel > 0)
                bleSel--;
        } else if (down) {
            BleScanHit next;
            if (bleScanHit(bleSel + 1, &next))
                bleSel++;
        } else if (enter) {
            if (haveHit) { // remember the node and connect to it
                snprintf(g_peerAddr, sizeof(g_peerAddr), "%s", hit.addr);
                snprintf(g_peerName, sizeof(g_peerName), "%s", hit.name[0] ? hit.name : hit.addr);
                g_peerType = hit.type;
                persistRadioCfg();
                bleAttemptArmed = bleAttemptMark();
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
                bleAttemptArmed = bleAttemptMark();
                bleConnectAsync(g_peerAddr, g_peerType);
            }
        } else if (c == 'f' || c == 'F') { // forget the node -> back to the scan
            if (g_linkState == BLE_CONNECTING || g_linkState == BLE_PAIRING)
                return; // let the single transport worker finish before replacing it with a scan
            if (!bleDisconnect())
                return;
            g_peerAddr[0] = 0;
            g_peerName[0] = 0;
            g_peerType = 0;
            persistRadioCfg();
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
            if (pickTarget == 10) { // node-list sort: applied live, own tiny file
                g_nodeSort = (uint8_t)kSortOpts[pickSel].value;
                persistUiCfg();
                mode = MODE_SETTINGS;
            } else if (pickTarget == 11) { // long vs short names: live, same tiny file
                g_nameShort = (uint8_t)kNameOpts[pickSel].value;
                persistUiCfg();
                mode = MODE_SETTINGS;
            } else if (pickTarget == 13) { // second screen: off, or mounted this way round
                g_extRot = (uint8_t)kExtOpts[pickSel].value;
                persistUiCfg();
                if (!g_extRot) {
                    extStop();
                } else if (g_ext) { // already up: just turn the picture round
                    concurrency::LockGuard guard(spiLock);
                    g_ext->setRotation(g_extRot);
                    g_ext->fillScreen(0); // the letterbox bands move with it
                } else {
                    extInit();
                }
                mode = MODE_SETTINGS;
            } else if (pickTarget == 12) { // unicode font from the card: live, both ways
                if (kFontOpts[pickSel].value)
                    sdFontLoadFromSd();
                else
                    sdFontUnload();
                mode = MODE_SETTINGS;
            } else if (pickTarget == 4) { // screen auto-off: applied live, persisted with msgs
                g_screenOffSec = kScreenOpts[pickSel].value;
                markMsgsDirty();
                mode = MODE_SETTINGS;
            } else if (pickTarget == 2) { // UTC offset: applied live, persisted with msgs
                g_utcOffsetMin = kUtcOpts[pickSel].value;
                markMsgsDirty();
                mode = MODE_SETTINGS;
            } else if (pickTarget == 3) { // radio backend: persist + reboot into the new role
                bool comp = kRadioOpts[pickSel].value == 1;
                if (comp != g_radioCompanion) {
                    const bool oldCompanion = g_radioCompanion;
                    g_radioCompanion = comp;
                    if (persistRadioCfg()) {
                        rebootAtMsec = armDeadline(millis(), 1500);
                        mode = MODE_REBOOT;
                    } else {
                        // The old atomic file is still authoritative. Do not
                        // reboot into a role the user did not actually persist.
                        g_radioCompanion = oldCompanion;
                        g_radioCfgDirty = false;
                        mode = MODE_SETTINGS;
                    }
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
            } else if (pickTarget == 9) { // GPS on/off: persist + reboot so the GPS thread re-reads it
                config.position.gps_mode = kGpsOpts[pickSel].value
                                               ? meshtastic_Config_PositionConfig_GpsMode_ENABLED
                                               : meshtastic_Config_PositionConfig_GpsMode_DISABLED;
                config.has_position = true;
                if (nodeDB)
                    nodeDB->saveToDisk(SEGMENT_CONFIG);
                rebootAtMsec = armDeadline(millis(), 1500);
                mode = MODE_REBOOT;
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
                sendFailure = sendReaction(idx, kQuickReacts[reactPick]);
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
                    sendFailure = SendFailure::NONE;
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
            histExit(); // one press leaves the chat from anywhere, history included
            if (g_msgsDirty) { // flush read-state now — a reset right after would lose it
                if (saveMsgs()) {
                    g_msgsDirty = false;
                    g_lastMsgChangeMs = millis();
                }
            }
            mode = nodeReturn;
        } else if (up) {
            // A held arrow drains several key events between two frames, but the
            // edge conditions describe the LAST DRAWN frame — consuming chatAtTop
            // limits window slides to one per rendered frame; the view anchor then
            // keeps the content still, so a slide reads as one more line of scroll.
            if (chatAtTop) { // window top: slide the window deeper into the archive
                chatAtTop = false;
                if (histAvail < 0) // lazy: counted only when someone actually scrolls up here
                    histAvail = histCountFor(selectedChannel >= 0, (uint8_t)(selectedChannel >= 0 ? selectedChannel : 0),
                                             selectedNum);
                if (histDepth < histAvail) {
                    int want = histDepth + kHistSlide;
                    histStage(want > histAvail ? histAvail : want);
                    chatScroll = lastDrawScroll + 1; // the keypress itself still moves a line
                } // else: the very top of everything we have
            } else
                chatScroll++; // older; drawNode clamps to the top of the thread
        } else if (down) {
            if (histDepth > 0 && !histSeam && chatScroll == 0) { // window bottom, gap below
                histStage(histDepth - kHistSlide); // slide back toward the seam
                chatScroll = lastDrawScroll - 1;   // and keep moving one line newer
            } else if (chatScroll > 0)
                chatScroll--; // back toward the newest
        } else if (histDepth > 0 && !(histSeam && chatScroll == 0)) {
            histExit(); // deep in history: an action key returns to the live view first
        } else if (right || left) { // pick a message: RIGHT = react, LEFT = reply
            if (matchedFromNewest(0) >= 0) {
                reactSel = 0;
                pickReply = left;
            }
        } else if (enter && selectedChannel < 0) { // resend the newest failed DM in place
            uint32_t me = myNodeNum();
            for (int i = g_msgCount - 1; i >= 0; i--) {
                int idx = i;
                Msg &m = g_msgs[idx];
                if (m.from == me && m.to == selectedNum && m.status == MSG_FAILED) {
                    SendResult result = sendTextPacket(selectedNum, m.text, 0, g_msgReply[idx]);
                    sendFailure = result.failure;
                    if (result.failure == SendFailure::NONE) {
                        m.id = result.id; // the new ACK will find it by this id
                        m.status = MSG_SENDING;
                        m.err = 0;
                        const uint32_t now = getTime(false);
                        m.rxTime = validStoredEpoch(now) ? now : millis() / 1000 + 1;
                        markMsgsDirty();
                    }
                    break;
                }
            }
        } else if (tab) {
            sendFailure = SendFailure::NONE;
            msgLen = 0; // start a fresh reply built from the picked emoji
            msgBuf[0] = 0;
            chatScroll = 0;
            emojiReturn = MODE_NODE;
            emojiSel = 0;
            mode = MODE_EMOJI;
        } else if (printable) {
            sendFailure = SendFailure::NONE;
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
            sendFailure = SendFailure::NONE;
            pendingReplyId = 0; // cancelling compose drops the quote too
            mode = MODE_NODE;
        } else if (tab) {
            sendFailure = SendFailure::NONE;
            emojiReturn = MODE_COMPOSE; // add an emoji into the current message
            emojiSel = 0;
            mode = MODE_EMOJI;
        } else if (enter) {
            if (!msgLen)
                return;
            sendFailure = selectedChannel >= 0 ? sendChannel(selectedChannel, msgBuf, pendingReplyId)
                                               : sendMessage(selectedNum, msgBuf, pendingReplyId);
            if (sendFailure != SendFailure::NONE) {
                pendingLat = 0;
                return; // preserve both the draft and its reply target for retry
            }
            pendingReplyId = 0;
            msgLen = 0;
            msgBuf[0] = 0;
            pendingLat = 0;
            chatScroll = 0; // show the just-sent message at the bottom
            mode = MODE_NODE;
        } else if (bksp) {
            sendFailure = SendFailure::NONE;
            pendingLat = 0;
            while (msgLen > 0 && ((unsigned char)msgBuf[msgLen - 1] & 0xC0) == 0x80)
                msgBuf[--msgLen] = 0; // drop UTF-8 continuation bytes, then the lead byte
            if (msgLen)
                msgBuf[--msgLen] = 0;
        } else if (g_ruMode && printable && msgLen + 2 < sizeof(msgBuf)) {
            sendFailure = SendFailure::NONE;
            translitFeed(msgBuf, msgLen, sizeof(msgBuf), c, pendingLat);
        } else if (printable && msgLen < sizeof(msgBuf) - 1) {
            sendFailure = SendFailure::NONE;
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
                    markMsgsDirty();
                } else {
                    favNode(c.node, left);
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
        if (sel < total) {
            // Favouriting re-sorts the list under the cursor: the node jumps to the top
            // and a different one takes its place at this index. Held down, the key
            // repeat then favourited whatever slid underneath, painting a run of nodes
            // yellow. Follow the node instead of the index, so the cursor stays on the
            // one the user was pointing at, wherever the sort has moved it.
            const bool isChan = sel < chanCount;
            const uint32_t was = isChan ? 0 : nodeNumAt(sel - chanCount);
            favEntry(sel, left);
            if (!isChan && was) {
                rebuildFiltered();
                for (int i = 0; i < filteredCount; i++)
                    if (nodeNumAt(i) == was) {
                        sel = chanCount + i;
                        break;
                    }
            }
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
#ifdef ADVUI_HIL
    static bool hilAnnounced = false;
    if (!hilAnnounced) {
        hilAnnounced = true;
        Serial.printf("@@READY hil=1 protocol=2 version=%s engine=%s\n", ADVUI_VERSION, optstr(APP_VERSION_SHORT));
        hilState();
    }
#endif

#ifndef ADVUI_HIL
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
#endif

    { // Sample the honest (off-charge) battery % every 30 s and persist it when it
      // moves, so the header can show a real figure while USB hides the true charge.
        static uint32_t lastBattMs = 0;
        if (powerStatus && powerStatus->getHasBattery() &&
            (!lastBattMs || !Throttle::isWithinTimespanMs(lastBattMs, 30000))) {
            lastBattMs = millis();
            int p = powerStatus->getBatteryChargePercent();
            // With the bolt up the live figure cannot be trusted upwards — under charge
            // it reads the charger rail. Downwards it can: nothing on a charger makes the
            // resting figure fall, so a drop means we are running off the pack. That lets
            // a bolt misfired by a freshly-charged pack thaw the number on its own.
            bool trustworthy = !batteryCharging() || (g_battRest >= 0 && p < g_battRest);
            if (trustworthy && p >= 0 && p <= 100 && p != g_battRest) {
                g_battRest = p;
                persistUiCfg();
            }
        }
    }

    { // Remember the last honestly-known day (manual-clock date base), once a minute.
      // It is advisory, so never compete with a live receive/archive burst. The
      // dirty ring is flushed after its quiet debounce; the next tick records
      // the day. A permanently busy stream can safely defer this daily hint.
        static uint32_t lastEpochNoteMs = 0;
        if (!g_msgsDirty && !Throttle::isWithinTimespanMs(lastEpochNoteMs, 60 * 1000UL)) {
            lastEpochNoteMs = millis();
            epochNote();
        }
    }

    { // The clock just synced (phone, NTP, mesh or the Clock setting): messages that
      // arrived clockless this boot carry uptime stamps — rewrite them to real time,
      // real = now - (uptime_now - uptime_then). One-shot per boot.
        static bool clockSeen = false;
        uint32_t now = getTime(false);
        if (!clockSeen && validStoredEpoch(now)) {
            clockSeen = true;
            uint32_t up = millis() / 1000;
            int fixed = 0;
            for (int i = 0; i < g_msgCount; i++) {
                uint32_t rt = g_msgs[i].rxTime;
                if (rt && rt < kValidEpoch) {
                    g_msgs[i].rxTime = now - (up > rt ? up - rt : 0);
                    markMsgsDirty();
                    uiDirty = true;
                    fixed++;
                }
            }
            LOG_INFO("advui: clock synced at uptime %us, backfilled %d stamps", (unsigned)up, fixed);
        }
    }

#ifdef ADVUI_SCREENSHOT
    // Remote debug driving over serial (dev builds only): 'S' = demo dump,
    // 'L' = live screenshot, 'K'+byte = inject a key ('^'=ESC '~'=ENTER
    // 'U'/'D'/'<'/'>'=arrows '!'=long-ESC, anything else = the char itself).
    static bool shotPendingKey = false;
#ifdef ADVUI_HIL
    // `I` + uint16 little-endian length + raw FromRadio protobuf. The persistent
    // state lets USB deliver a frame over multiple UI ticks without interpreting
    // payload bytes as commands. kBleFrameMax also bounds companion ingress.
    static uint8_t hilInjectPhase = 0;
    static bool hilInjectToCompanion = false;
    static bool hilSendFaultPending = false;
    static uint8_t hilInjectLenLo = 0;
    static uint16_t hilInjectExpected = 0, hilInjectUsed = 0;
    static uint8_t hilInjectBuf[kBleFrameMax];
#endif
    while (Serial.available()) {
        int sc = Serial.read();
        if (!screenOn)
            screenWake(); // remote session must survive the screen auto-off: the
                          // render (and so the 'L' dump) is gated on a lit screen
#ifdef ADVUI_HIL
        if (hilSendFaultPending) {
            hilSendFaultPending = false;
            g_hilNextSendError = (uint8_t)sc;
            Serial.printf("@@FAULT v=1 ok=1 error=%u\n", (unsigned)g_hilNextSendError);
            Serial.flush();
        } else if (hilInjectPhase) {
            if (hilInjectPhase == 1) {
                hilInjectLenLo = (uint8_t)sc;
                hilInjectPhase = 2;
            } else if (hilInjectPhase == 2) {
                hilInjectExpected = (uint16_t)(hilInjectLenLo | ((uint16_t)(uint8_t)sc << 8));
                hilInjectUsed = 0;
                if (!hilInjectExpected || hilInjectExpected > sizeof(hilInjectBuf)) {
                    Serial.printf("@@INJECT v=1 ok=0 error=size bytes=%u\n", (unsigned)hilInjectExpected);
                    Serial.flush();
                    hilInjectPhase = 0;
                } else {
                    hilInjectPhase = 3;
                }
            } else {
                hilInjectBuf[hilInjectUsed++] = (uint8_t)sc;
                if (hilInjectUsed == hilInjectExpected) {
                    if (hilInjectToCompanion)
                        this->hilInjectCompanion(hilInjectBuf, hilInjectExpected);
                    else
                        hilInject(hilInjectBuf, hilInjectExpected);
                    hilInjectPhase = 0;
                }
            }
        } else
#endif
        if (shotPendingKey) {
            shotPendingKey = false;
            unsigned char k = (unsigned char)sc;
            if (sc == '^')
                k = 0x1b;
            else if (sc == '~')
                k = 0x0d;
            else if (sc == 'U')
                k = 0xb5;
            else if (sc == 'D')
                k = 0xb6;
            else if (sc == '<')
                k = 0xb4;
            else if (sc == '>')
                k = 0xb7;
            else if (sc == '!')
                k = AdvKeyboard::kLongEsc;
            handleKey((char)k);
            lastActivityMs = millis(); // remote keys hold the screen awake like real ones
            uiDirty = true;
        } else if (sc == 'S')
            runDemoDump(); // dump every screen with sample data, then reboot
        else if (sc == 'L')
            shotLive = true;
        else if (sc == 'K')
            shotPendingKey = true;
        else if (sc == 'G') { // ghost DM: SMP emoji + a 191-byte Cyrillic text (render/width tests)
            addMsg(0x000BEEF1, myNodeNum(), 0, 0, true, "SMP: \U0001F480\U0001F923\U0001F970 sel: ❤️ ok", 777,
                   MSG_IN);
            addReaction(777, 0x000BEEF1, "\U0001F480", 0x000BEEF1);
            addMsg(0x000BEEF1, myNodeNum(), 0, 0, true,
                   "Погода огонь, как и пробки на каде. Кому с юга на север гоу по ЗСД. От зуздальского уже час до "
                   "мурино стоим.",
                   778, MSG_IN);
            uiDirty = true;
        } else if (sc == 'H') { // 48 numbered ghost DMs: overflows the ring into the archive
#ifdef ADVUI_HIL
            if (!hilHistorySeeded) {
#endif
            char t[40];
            for (int i = 1; i <= 48; i++) {
                snprintf(t, sizeof(t), "hist test %d/48", i);
                addMsg(0x000BEEF1, myNodeNum(), 0, 0, i > 40, t, 7000 + i, MSG_IN);
            }
            uiDirty = true;
#ifdef ADVUI_HIL
                hilHistorySeeded = true;
            }
            Serial.println("@@ACK history");
            Serial.flush();
#endif
#ifdef ADVUI_HIL
        } else if (sc == 'I') {
            hilInjectToCompanion = false;
            hilInjectPhase = 1;
            hilInjectExpected = hilInjectUsed = 0;
        } else if (sc == 'J') {
            hilInjectToCompanion = true;
            hilInjectPhase = 1;
            hilInjectExpected = hilInjectUsed = 0;
        } else if (sc == 'F') {
            hilSendFaultPending = true;
        } else if (sc == 'P') {
            hilPersist();
        } else if (sc == 'M') {
            hilLoadLegacyStore();
        } else if (sc == 'V') {
            hilSeenSaturation();
        } else if (sc == 'E') {
            hilClearData();
        } else if (sc == 'B') {
            Serial.println("@@ACK reboot");
            Serial.flush();
            delay(200);
            ESP.restart();
        } else if (sc == 'Q') {
            hilState();
        } else if (sc == 'R') {
            hilReactionState();
        } else if (sc == 'T') {
            hilMemoryState();
        } else if (sc == 'Y') {
            const uint32_t before = ESP.getFreeHeap();
            bleHilReleaseState();
            const uint32_t after = ESP.getFreeHeap();
            Serial.printf("@@BRELEASE v=1 ok=%u before=%u after=%u freed=%u\n", after > before ? 1U : 0U,
                          (unsigned)before, (unsigned)after, after > before ? (unsigned)(after - before) : 0U);
            Serial.flush();
        } else if (sc == 'C') {
            hilDigestPending = true;
            uiDirty = true; // digest the frame for the current state after a fresh render
        } else if (sc == 'X') {
            hilHome();
#endif
        }
#ifdef ADVUI_HIL
        // One complete request per UI tick. A host that immediately follows each
        // response with another query must not starve keyboard polling, splash
        // expiry, mesh pumping or rendering inside this Serial.available() loop.
        if (!shotPendingKey && !hilInjectPhase && !hilSendFaultPending)
            break;
#endif
    }
#endif

    if (g_radioCompanion) { // companion: mesh packets arrive from the BLE pump's ring
        BleFrame frame;
        static meshtastic_FromRadio fr; // large struct: keep off the stack
        while (bleNextPacket(&frame)) {
            fr = meshtastic_FromRadio_init_default;
            if (pb_decode_from_bytes(frame.data, frame.len, &meshtastic_FromRadio_msg, &fr)) {
                uiDirty = handleFromRadio(fr) || uiDirty; // messages, reactions, delivery ACKs only
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
    if (deadlineReached(millis(), g_ledOffMs)) { // clear the notification flash, including across millis() rollover
        setLedRgb(0, 0, 0);
        g_ledOffMs = 0;
    }
#endif

    if (!kbMissing) {
        kb.setNavKeys(mode != MODE_SETNAME && mode != MODE_COMPOSE); // symbols while typing, arrows otherwise
        kb.trigger();
    }
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
    if (!kbMissing)
        kb.clearInt(); // re-arm the TCA8418 interrupt, else it stops reporting after the first event

    if (!splashDone && (keyDuringSplash || !Throttle::isWithinTimespanMs(bootMs, 2000)))
        splashDone = true;

    // Announce ourselves early so neighbours see us right away — stock waits ~30s.
    if (splashDone && !announced && nodeInfoModule) {
#ifdef ADVUI_HIL
        // The HIL build is radio-silent by contract. Skip this eager ADV
        // announcement before it is queued; Router and RadioLib have immutable
        // compile-time guards for every other engine-owned path.
        announced = true;
#else
        nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, false);
        announced = true;
#endif
    }

    // Nameless-node healing. On a busy mesh the periodic NodeInfo broadcast
    // drowns in collisions, so entries sit as bare "!hex" indefinitely (field
    // report; also took five broadcast attempts to name one of our own nodes).
    // A directed exchange is unicast and reliable: send our info with
    // want_response and the reply carries their name. One node per pass, a
    // pass every 10 min, each node asked at most once per 6 h — negligible
    // airtime even on a congested channel.
#ifndef ADVUI_HIL
    // This is engine-owned radio traffic, not an ADV receive/UI path. The HIL
    // build blocks TX at the engine too; skip allocating a doomed NodeInfo
    // packet so its one-minute timer cannot contaminate ADV heap measurements.
    if (splashDone && !g_radioCompanion && nodeInfoModule && nodeDB) {
        static uint32_t nextAskMs = 60 * 1000UL; // let the boot-time chatter settle first
        static uint32_t asked[16], askedAtMs[16];
        static uint8_t askSlot = 0;
        if ((int32_t)(millis() - nextAskMs) >= 0) {
            nextAskMs = millis() + 10 * 60 * 1000UL;
            uint32_t me = myNodeNum();
            for (int i = 0; i < (int)nodeDB->getNumMeshNodes(); i++) {
                meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
                if (!n || n->num == me || nodeHasName(n))
                    continue;
                if (sinceLastSeen(n) > 2 * 60 * 60)
                    continue; // not around: don't spend airtime on it
                bool fresh = false;
                for (int k = 0; k < 16; k++)
                    if (asked[k] == n->num && Throttle::isWithinTimespanMs(askedAtMs[k], 6 * 60 * 60 * 1000UL))
                        fresh = true;
                if (fresh)
                    continue;
                asked[askSlot & 15] = n->num;
                askedAtMs[askSlot & 15] = millis();
                askSlot++;
                // shorterTimeout=true: allocReply() otherwise throttles our
                // NodeInfo to ~10 min, so a recent periodic broadcast would
                // swallow this directed ask — the exact failure it's healing.
                // The 60 s floor still can't be tripped: we pace asks 10 min apart.
                nodeInfoModule->sendOurNodeInfo(n->num, true, 0, true);
                LOG_INFO("advui: asked !%08x for its name", (unsigned)n->num);
                break;
            }
        }
    }
#endif

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
        if (g_peerAddr[0] && !bleAttemptPending() && (bleAttemptArmed = bleAttemptMark())) {
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
        companionEntered && mode != MODE_BLESCAN && mode != MODE_BLEPIN &&
        !Throttle::isWithinTimespanMs(bleRetryMs, 7000)) {
        bleRetryMs = millis();
        bleAttemptArmed = bleAttemptMark();
        if (bleAttemptArmed)
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

    // Atomic config writes can fail transiently (full/torn LittleFS, interrupted
    // readback). Keep the in-RAM choice dirty and retry at a bounded cadence;
    // otherwise a successful-looking setting silently rolls back after reboot.
    if (g_uiCfgDirty && !Throttle::isWithinTimespanMs(g_lastUiCfgSaveMs, 3000))
        persistUiCfg();
    if (g_radioCfgDirty && !Throttle::isWithinTimespanMs(g_lastRadioCfgSaveMs, 3000))
        persistRadioCfg();

    // Persist after three quiet seconds. The previous fixed-cadence throttle
    // could launch a full atomic ring rewrite in the middle of a receive burst,
    // overlapping LittleFS archive appends and briefly exhausting this
    // no-PSRAM board's heap. A new mutation restarts the debounce. Under a
    // continuous stream, evictions are already durable in the append-only
    // archive and only the bounded 32-record live tail waits for a quiet gap.
    // Explicit thread exit and HIL persist flush synchronously when required.
    const uint32_t saveNow = millis();
    if (g_msgsDirty && dirtyWriteDue(saveNow, g_lastMsgChangeMs, 3000)) {
        if (saveMsgs())
            g_msgsDirty = false;
        g_lastMsgChangeMs = saveNow; // bound retries after a transient failure
    }

    // Screen auto-off: no input for the configured time cuts the display rail.
    // Setup/transient screens keep it awake, and a PIN request relights it (pairing
    // needs the user). While dark, skip all rendering — the panel is unpowered.
    bool sleepable = splashDone && g_screenOffSec > 0 && mode != MODE_BLEPIN && mode != MODE_BLESCAN &&
                     mode != MODE_REBOOT && mode != MODE_BTPIN;
    if (screenOn && sleepable && !Throttle::isWithinTimespanMs(lastActivityMs, (uint32_t)g_screenOffSec * 1000))
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
    if (!(uiDirty || liveScreen || mode != lastDrawnMode || !Throttle::isWithinTimespanMs(lastDrawMs, 10000))) {
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
    else if (kbMissing)
        drawKbMissing(); // wrong device: a clear notice beats a dead keyboard
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

#ifdef ADVUI_SCREENSHOT
    if (shotLive) {
        shotLive = false;
        screenshot("live"); // whatever just rendered — real state, not demo data
    }
#ifdef ADVUI_HIL
    if (hilDigestPending) {
        hilDigestPending = false;
        hilFrameDigest();
    }
#endif
#endif

#ifdef HAS_I2S
    if (g_beeping)
        return 20; // pump the tone fast until it finishes
#endif
    if (splashDone && kb.navHeld())
        return 40; // a held arrow/backspace is repeating: poll+redraw fast for smooth scroll
    return splashDone ? 200 : 80; // 5 Hz normally; snappier while the splash is up
}

// Local-mode receive path. The UI used to subscribe as a PhoneAPI client, but
// the engine keeps ONE destructive to-phone queue shared by every client — our
// 200 ms polling starved a real phone of live packets and admin responses
// (initial config is per-client and worked, which made it look like a hang).
// A module gets an independent copy of every packet and consumes nothing:
// ProcessMessage::CONTINUE leaves the phone path untouched. loopbackOk also
// hands us phone-SENT messages, so they finally show on the device screen
// (the id dedupe in addMsg keeps our own sends from double-adding).
class AdvRxModule : public MeshModule
{
  public:
    explicit AdvRxModule(AdvUI *owner) : MeshModule("advrx"), ui(owner) { loopbackOk = true; }

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override
    {
#ifdef ADVUI_HIL
        // Deterministic release tests consume only host-injected FromRadio
        // frames. Live RF may still be received by the embedded engine, but it
        // must not race the isolated ADV message/history assertions.
        (void)p;
        return false;
#else
        if (g_radioCompanion) // companion feeds the UI over BLE; the local engine is idle
            return false;
        if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag)
            return false;
        return p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP ||
               (p->decoded.portnum == meshtastic_PortNum_ROUTING_APP && p->decoded.request_id);
#endif
    }

    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override
    {
        // Same thread as runOnce (the main loop), so calling straight into the
        // UI pipeline is safe — this is exactly what the queue drain used to do.
        meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
        fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
        fr.packet = mp;
        ui->onMeshPacket(fr);
        return ProcessMessage::CONTINUE; // the phone still gets its own copy
    }

    AdvUI *ui;
};

// Created once from an injected call in main.cpp (after setupModules); the
// OSThread base then self-schedules, so no main-loop edits are needed.
void advuiSetup()
{
    // Park the SD card's chip-select before the radio touches the bus. The slot
    // shares SPI with the SX1262 (SD CS 12, LoRa CS 5, same host), and a card
    // whose CS floats can hold MISO down — the radio then reads garbage and the
    // node wedges or reboots. Field reports of "keyboard dead, device restarts
    // by itself" cleared up by removing the card; this makes removing it
    // unnecessary. Runs from the setupModules() injection: after SPI.begin(),
    // still before initLoRa().
    pinMode(12, OUTPUT);
    digitalWrite(12, HIGH);

    static AdvUI advUI;
    static AdvRxModule advRx(&advUI); // registers with the module list built in setupModules()
}

} // namespace advui
