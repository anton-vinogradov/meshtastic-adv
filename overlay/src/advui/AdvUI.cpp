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
#include "gps/RTC.h" // getTime() for message timestamps
#include "graphics/emotes.h" // UTF-8 -> bitmap emoji table (forced in via EmotesData.cpp)
#include "main.h" // audioThread (I2S beep)
#include "modules/NodeInfoModule.h"
#ifdef HAS_NEOPIXEL
#include <Adafruit_NeoPixel.h> // RGB notification LED
#endif
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
constexpr int kMaxMsgs = 32;
constexpr int kNumSettings = 7; // Name, Short, Region, Preset, Frequency, Channel, UTC
Msg g_msgs[kMaxMsgs];
int g_msgCount = 0;         // populated slots (grows to kMaxMsgs)
int g_msgNext = 0;          // next write slot (ring head)
bool g_msgsDirty = false;   // ring changed since the last flash save
uint32_t g_lastSaveMs = 0;  // when we last wrote the ring to flash

void addMsg(uint32_t from, uint32_t to, uint8_t ch, uint32_t rxTime, bool unread, const char *text, uint32_t id,
           uint8_t status)
{
    Msg &m = g_msgs[g_msgNext];
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
        if (g_msgs[i].from == nodeNum && g_msgs[i].to != NODENUM_BROADCAST && !g_msgs[i].read)
            return true;
    return false;
}

// Channel favourites are our own state (Meshtastic channels carry no favourite flag).
uint8_t g_favChannels = 0; // bitmask, bit i set = channel i favourited
bool chanFav(int i) { return g_favChannels & (1u << i); }

// User-set UTC offset (minutes) for message timestamps; the device has no GPS/tz.
int32_t g_utcOffsetMin = 0;

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
// Notification LED (single SK6812 on NEOPIXEL_DATA). The stock AmbientLightingThread
// inits but leaves it idle by default, so our own object can drive it for a brief
// flash on incoming messages, cleared from runOnce.
Adafruit_NeoPixel g_led(NEOPIXEL_COUNT, NEOPIXEL_DATA, NEOPIXEL_TYPE);
uint32_t g_ledOffMs = 0;
void flashLed(uint8_t r, uint8_t g, uint8_t b)
{
    g_led.setPixelColor(0, g_led.Color(r, g, b));
    g_led.show();
    g_ledOffMs = millis() + 350;
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
constexpr uint32_t kMsgMagic = 0x41565332; // "AVS2" — bump when the saved layout changes
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
    f.write((const uint8_t *)&g_favChannels, sizeof(g_favChannels));
    f.write((const uint8_t *)g_msgs, sizeof(g_msgs));
    f.write((const uint8_t *)&g_utcOffsetMin, sizeof(g_utcOffsetMin)); // optional tail
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
    f.read((uint8_t *)&g_favChannels, sizeof(g_favChannels));
    size_t n = f.read((uint8_t *)g_msgs, sizeof(g_msgs));
    int32_t off = 0;
    f.read((uint8_t *)&off, sizeof(off)); // optional tail; stays 0 for pre-offset files
    f.close();
    if (magic != kMsgMagic || n != sizeof(g_msgs) || cnt < 0 || cnt > kMaxMsgs || nxt < 0 || nxt >= kMaxMsgs)
        return;
    g_utcOffsetMin = off;
    g_msgCount = cnt;
    g_msgNext = nxt;
    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;
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
constexpr int kRegionCount = sizeof(kRegionOpts) / sizeof(kRegionOpts[0]);
constexpr int kPresetCount = sizeof(kPresetOpts) / sizeof(kPresetOpts[0]);
constexpr int kUtcCount = sizeof(kUtcOpts) / sizeof(kUtcOpts[0]);

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
// Phonetic Latin->Cyrillic with the usual digraphs (sh ш, zh ж, ch ч, ya/yu/yo/ye)
// and singles w->щ, j->й, x->ъ, '->ь. A letter is emitted immediately and morphed
// in place when it turns out to be the head of a digraph.
uint16_t translitSingle(char l)
{
    switch (l) {
    case 'a': return 0x430; case 'b': return 0x431; case 'v': return 0x432; case 'g': return 0x433;
    case 'd': return 0x434; case 'e': return 0x435; case 'z': return 0x437; case 'i': return 0x438;
    case 'j': return 0x439; case 'k': return 0x43A; case 'l': return 0x43B; case 'm': return 0x43C;
    case 'n': return 0x43D; case 'o': return 0x43E; case 'p': return 0x43F; case 'r': return 0x440;
    case 's': return 0x441; case 't': return 0x442; case 'u': return 0x443; case 'f': return 0x444;
    case 'h': return 0x445; case 'c': return 0x446; case 'y': return 0x44B; case 'w': return 0x449;
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
            appendCp(buf, len, cap, translitCase(cp, isUp(pending)));
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
            w += g->textWidth(cb);
            s += k;
        }
    }
    return w;
}

// Draws a message-line string at (x,y) in `color`, blitting emoji bitmaps inline.
void printLineEmotes(lgfx::LGFXBase *g, int x, int y, const char *s, uint16_t color)
{
    g->setFont(&cyrFont);
    g->setTextSize(1);
    int cx = x;
    while (*s) {
        const graphics::Emote *em = nullptr;
        int elen = emoteMatch(s, &em);
        if (elen > 0 && em) {
            g->drawXBitmap(cx, y + (17 - em->height) / 2, em->bitmap, em->width, em->height, color);
            cx += em->width + 2;
            s += elen;
        } else { // one non-emote UTF-8 char
            int tlen = utf8Len((unsigned char)s[0]);
            char cb[5];
            int k = 0;
            for (; k < tlen && s[k]; k++)
                cb[k] = s[k];
            cb[k] = 0;
            g->setTextColor(color);
            g->setCursor(cx, y);
            g->print(cb);
            cx += g->textWidth(cb);
            s += k;
        }
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

    // Silence the stock ExternalNotificationModule: on this HAS_I2S board its default
    // config beeps the codec on every received message (channel broadcasts included).
    // We drive our own favourites-only single beep instead (see startBeep()).
    moduleConfig.external_notification.enabled = false;

#ifdef HAS_NEOPIXEL
    g_led.begin();
    g_led.setBrightness(70);
    g_led.clear();
    g_led.show();
    flashLed(0, 70, 90); // brief cyan "hello" blink at boot: confirms the LED works
#endif

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
    bool unread = p.from != me && (p.to == me || p.to == NODENUM_BROADCAST); // DM to us, or a channel broadcast
    addMsg(p.from, p.to, p.channel, p.rx_time, unread, text, 0, MSG_IN);

    if (unread) {
        bool fav = (p.to == NODENUM_BROADCAST) ? chanFav(p.channel) : (nodeDB && nodeDB->isFavorite(p.from));
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
    addMsg(me, to, 0, getTime(false), false, text, id, MSG_SENDING);
}

// Sends a text broadcast to a channel and adds it to that channel's thread.
void AdvUI::sendChannel(int chIdx, const char *text)
{
    if (!router || !service)
        return;
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->channel = chIdx;
    p->want_ack = false; // broadcasts aren't acked
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    size_t n = strlen(text);
    if (n > sizeof(p->decoded.payload.bytes))
        n = sizeof(p->decoded.payload.bytes);
    memcpy(p->decoded.payload.bytes, text, n);
    p->decoded.payload.size = n;
    service->sendToMesh(p, RX_SRC_LOCAL, false);

    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;
    addMsg(me, NODENUM_BROADCAST, chIdx, getTime(false), false, text, 0, MSG_SENT); // sent to channel (no ack)
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

// Fills chanList[] with enabled channels (query-matched), favourites first.
int AdvUI::buildChannels(const char *query)
{
    int count = 0;
    for (int i = 0; i < 8 && count < 8; i++) {
        meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED)
            continue;
        if (query && query[0]) {
            const char *name = channels.getName(i);
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
    snprintf(nm, sizeof(nm), "#%s", channels.getName(chIdx));
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
        int idx = (g_msgCount == kMaxMsgs) ? (g_msgNext + i) % kMaxMsgs : i;
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

void AdvUI::openEntry(int s)
{
    nodeReturn = (mode == MODE_PICKER) ? MODE_PICKER : MODE_NODES;
    chatScroll = 0;          // default: pinned to the newest message
    chatAnchorMsgIdx = -1;
    if (s < chanCount) {
        selectedChannel = chanList[s];
        chatAnchorMsgIdx = firstUnreadIdx(); // capture the first unread before clearing it
        markReadChannel(selectedChannel);
        mode = MODE_NODE;
    } else if (nodeDB) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(filtered[s - chanCount]);
        if (node) {
            selectedChannel = -1;
            selectedNum = node->num;
            chatAnchorMsgIdx = firstUnreadIdx(); // markReadFrom() runs later in drawNode
            mode = MODE_NODE;
        }
    }
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
    } else if (nodeDB) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(filtered[s - chanCount]);
        if (node)
            nodeDB->set_favorite(on, node->num);
    }
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
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(filtered[idx - chanCount]);
            if (node)
                drawNodeRow(g, node, y, node->num == me);
        }
        y += rowH;
    }

    drawFooter(g, "up/dn  </>fav  ENTER open  type find");

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

    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;
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
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(filtered[idx - chanCount]);
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
    meshtastic_NodeInfoLite *node = (!isChan && nodeDB) ? nodeDB->getMeshNode(selectedNum) : nullptr;
    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;

    if (isChan)
        markReadChannel(selectedChannel); // opening the thread clears its unread
    else
        markReadFrom(selectedNum);

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
    int bottom = (mode == MODE_COMPOSE) ? 112 : 122; // leave room for the compose bar
    int maxLines = (bottom - fy0) / lh;
    int matched[kMaxMsgs], mc = 0;
    for (int i = 0; i < g_msgCount; i++) {
        int idx = (g_msgCount == kMaxMsgs) ? (g_msgNext + i) % kMaxMsgs : i;
        bool match = isChan ? (g_msgs[idx].to == NODENUM_BROADCAST && g_msgs[idx].ch == selectedChannel)
                            : ((g_msgs[idx].from == selectedNum || g_msgs[idx].to == selectedNum) &&
                               g_msgs[idx].to != NODENUM_BROADCAST);
        if (match)
            matched[mc++] = idx;
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
            bool out;
            uint8_t status;
            uint8_t err;
            uint8_t timeLen;  // leading chars that are the dim "HH:MM " prefix (0 on continuation lines)
        };
        int32_t tzOff = g_utcOffsetMin * 60; // user-set UTC offset (Settings > UTC)
        static DLine dl[80]; // single-threaded UI: static keeps it off the stack
        int dlCount = 0;
        int anchorLine = -1; // dl index of the first unread message's first line (on open)
        for (int i = 0; i < mc; i++) {
            Msg &m = g_msgs[matched[i]];
            bool out = (m.from == me);
            bool failed = out && m.status == MSG_FAILED;
            int wrapW = !out ? 232 : (failed ? 162 : 214); // reserve room for the marker
            char tpre[8];
            msgTimePrefix(m.rxTime, tzOff, tpre, sizeof(tpre)); // "HH:MM " before the arrow
            uint8_t tlen = (uint8_t)strlen(tpre);
            char full[188];
            snprintf(full, sizeof(full), "%s%s%s", tpre, out ? "> " : "< ", m.text);
            if (matched[i] == chatAnchorMsgIdx)
                anchorLine = dlCount; // this message's first line
            const char *p = full;
            bool first = true;
            do {
                if (dlCount >= (int)(sizeof(dl) / sizeof(dl[0]))) { // drop the oldest line
                    memmove(dl, dl + 1, sizeof(dl) - sizeof(dl[0]));
                    dlCount--;
                    if (anchorLine >= 0)
                        anchorLine--; // the anchor shifted up with the dropped line
                }
                DLine &d = dl[dlCount++];
                p += wrapLine(g, p, wrapW, d.text, sizeof(d.text));
                d.color = out ? 0x07FF : 0xFFFF; // outgoing cyan, incoming white
                d.out = out;
                d.status = m.status;
                d.err = m.err;
                d.timeLen = first ? tlen : 0; // time sits only on the first line
                d.last = (*p == 0);
                first = false;
            } while (*p);
        }
        // chatScroll counts lines scrolled up from the bottom (0 = pinned to newest).
        int maxScroll = dlCount > maxLines ? dlCount - maxLines : 0;
        if (chatAnchorMsgIdx >= 0) { // first render after opening: jump to the first unread
            chatScroll = (anchorLine >= 0 && anchorLine <= maxScroll) ? maxScroll - anchorLine : 0;
            chatAnchorMsgIdx = -1;
        }
        if (chatScroll > maxScroll)
            chatScroll = maxScroll;
        if (chatScroll < 0)
            chatScroll = 0;
        int startL = maxScroll - chatScroll;
        int y = fy0;
        for (int i = startL; i < startL + maxLines && i < dlCount; i++) {
            DLine &d = dl[i];
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
            printLineEmotes(g, cx, y, d.text + d.timeLen, d.color); // message text + inline emoji
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
        g->setTextColor(ruMode ? 0x07FF : 0x630C);
        g->setCursor(220, 119);
        g->print(ruMode ? "RU" : "EN");
    } else {
        drawFooter(g, "type reply  Tab emoji  Fn+L RU/EN");
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

    const char *labels[kNumSettings] = {"Name", "Short", "Region", "Preset", "Frequency", "Channel", "UTC"};
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
    {
        int om = g_utcOffsetMin, ah = om < 0 ? -om : om;
        if (ah % 60)
            snprintf(vals[6], sizeof(vals[6]), "UTC%c%d:%02d", om < 0 ? '-' : '+', ah / 60, ah % 60);
        else
            snprintf(vals[6], sizeof(vals[6]), "UTC%c%d", om < 0 ? '-' : '+', ah / 60);
    }

    const int rowH = 15, top = 15;
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

    const EnumOpt *opts = pickTarget == 0 ? kRegionOpts : pickTarget == 1 ? kPresetOpts : kUtcOpts;
    int cnt = pickTarget == 0 ? kRegionCount : pickTarget == 1 ? kPresetCount : kUtcCount;

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    g->print(pickTarget == 0 ? "Region" : pickTarget == 1 ? "Preset" : "UTC offset");
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
    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;

    static Msg backup[kMaxMsgs];
    memcpy(backup, g_msgs, sizeof(g_msgs));
    int bCount = g_msgCount, bNext = g_msgNext;
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

    // sample conversation with the first listed node (Cyrillic + emoji + statuses)
    uint32_t peer = (filteredCount > 0 && nodeDB) ? nodeDB->getMeshNodeByIndex(filtered[0])->num : me;
    uint32_t t = 1751720400;
    addMsg(peer, me, 0, t, false, "Привет! Как слышно?", 0, MSG_IN);
    addMsg(me, peer, 0, t + 60, false, "Чётко, 5/5 \U0001F44D", 1, MSG_DELIVERED);
    addMsg(peer, me, 0, t + 120, false, "Выезжаем через 10, го \U0001F525", 0, MSG_IN);
    addMsg(me, peer, 0, t + 180, false, "Ок, буду \U0001F642", 2, MSG_SENDING);
    selectedChannel = -1;
    selectedNum = peer;
    chatScroll = 0;
    mode = MODE_NODE;
    drawNode();
    screenshot("chat");

    emojiSel = 0;
    mode = MODE_EMOJI;
    drawEmoji();
    screenshot("emoji");

    setSel = 0;
    mode = MODE_SETTINGS;
    drawSettings();
    screenshot("settings");

    pickTarget = 2;
    pickSel = optIndex(kUtcOpts, kUtcCount, 180); // Moscow, for a nice sample
    pickScroll = pickSel > 3 ? pickSel - 3 : 0;
    mode = MODE_PICKLIST;
    drawPickList();
    screenshot("utc");

    Serial.println("@@DONE");
    Serial.flush();

    memcpy(g_msgs, backup, sizeof(g_msgs)); // put the real conversation ring back
    g_msgCount = bCount;
    g_msgNext = bNext;
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
    bool tab = c == 0x09;   // TAB   -> emoji picker
    bool printable = c >= 0x20 && c < 0x7f;

    if (c == AdvKeyboard::kLongEsc) { // long-press ESC opens settings from anywhere
        setSel = 0;
        mode = MODE_SETTINGS;
        return;
    }

    if (c == AdvKeyboard::kLang) { // Fn+L: toggle transliterated Cyrillic input
        ruMode = !ruMode;
        pendingLat = 0;
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
            pendingLat = 0;
            if (nameLen) {
                while (nameLen > 0 && ((unsigned char)nameBuf[nameLen - 1] & 0xC0) == 0x80)
                    nameBuf[--nameLen] = 0; // whole UTF-8 char (Cyrillic is 2 bytes)
                if (nameLen)
                    nameBuf[--nameLen] = 0;
            }
        } else if (ruMode && !numeric && printable && nameLen + 2 < sizeof(nameBuf)) {
            translitFeed(nameBuf, nameLen, sizeof(nameBuf), c, pendingLat);
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
        } else if (enter && setSel == 6) { // UTC offset -> city/offset picker
            pickTarget = 2;
            pickSel = optIndex(kUtcOpts, kUtcCount, g_utcOffsetMin);
            pickScroll = 0;
            mode = MODE_PICKLIST;
        }
        return;
    }

    if (mode == MODE_PICKLIST) {
        int cnt = pickTarget == 0 ? kRegionCount : pickTarget == 1 ? kPresetCount : kUtcCount;
        if (esc) {
            mode = MODE_SETTINGS;
        } else if (up) {
            if (pickSel > 0)
                pickSel--;
        } else if (down) {
            if (pickSel < cnt - 1)
                pickSel++;
        } else if (enter) {
            if (pickTarget == 2) { // UTC offset: applied live, persisted with msgs
                g_utcOffsetMin = kUtcOpts[pickSel].value;
                g_msgsDirty = true;
                mode = MODE_SETTINGS;
            } else {
                const EnumOpt *opts = pickTarget == 0 ? kRegionOpts : kPresetOpts;
                applyLoRa(pickTarget, opts[pickSel].value); // sets config, saves, schedules reboot
            }
        }
        return;
    }

    if (mode == MODE_REBOOT)
        return; // rebooting shortly; ignore input

    if (mode == MODE_NODE) {
        if (esc || bksp) {
            mode = nodeReturn;
        } else if (up) {
            chatScroll++; // older; drawNode clamps to the top of the thread
        } else if (down) {
            if (chatScroll > 0)
                chatScroll--; // back toward the newest
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
            if (ruMode)
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
            mode = MODE_NODE;
        } else if (tab) {
            emojiReturn = MODE_COMPOSE; // add an emoji into the current message
            emojiSel = 0;
            mode = MODE_EMOJI;
        } else if (enter) {
            if (msgLen) {
                if (selectedChannel >= 0)
                    sendChannel(selectedChannel, msgBuf);
                else
                    sendMessage(selectedNum, msgBuf);
            }
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
        } else if (ruMode && printable && msgLen + 2 < sizeof(msgBuf)) {
            translitFeed(msgBuf, msgLen, sizeof(msgBuf), c, pendingLat);
        } else if (printable && msgLen < sizeof(msgBuf) - 1) {
            msgBuf[msgLen++] = c;
            msgBuf[msgLen] = 0;
        }
        return;
    }

    // Home node list: navigable directly (cursor + scroll); typing opens the filter.
    if (mode == MODE_NODES) {
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
        mode = MODE_NODES;
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

#ifdef ADVUI_SCREENSHOT
    while (Serial.available())
        if (Serial.read() == 'S')
            runDemoDump(); // host sends 'S' -> dump every screen, then reboot
#endif

    while (api.available() && api.getFromRadio(fromRadioBuf) > 0) {
        handleFromRadio(api.lastFromRadio()); // pick out incoming text messages
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
        g_led.clear();
        g_led.show();
        g_ledOffMs = 0;
    }
#endif

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

    // First-boot onboarding: a fresh install has no LoRa region, so the radio can't
    // transmit. Drop new users straight into the region picker instead of a dead node.
    if (splashDone && !regionPrompted) {
        regionPrompted = true;
        if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
            pickTarget = 0;
            pickSel = 0;
            pickScroll = 0;
            mode = MODE_PICKLIST;
        }
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
    else if (mode == MODE_EMOJI)
        drawEmoji();
    else
        drawNodeList();

#ifdef HAS_I2S
    if (g_beeping)
        return 20; // pump the tone fast until it finishes
#endif
    return splashDone ? 200 : 80; // 5 Hz normally; snappier while the splash is up
}

// Created once from an injected call in main.cpp (after setupModules); the
// OSThread base then self-schedules, so no main-loop edits are needed.
void advuiSetup()
{
    static AdvUI advUI;
}

} // namespace advui
