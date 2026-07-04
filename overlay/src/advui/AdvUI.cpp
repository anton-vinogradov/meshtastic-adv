#include "AdvUI.h"
#include "PowerStatus.h"
#include "configuration.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

extern uint32_t rebootAtMsec; // main.cpp: set to a future millis() to schedule a reboot

namespace advui
{

namespace
{

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
struct Msg {
    uint32_t from;
    uint32_t to; // 0xFFFFFFFF = broadcast/channel, else a DM to that node
    uint32_t rxTime;
    bool read;
    char text[160];
};
constexpr int kMaxMsgs = 32;
constexpr int kNumSettings = 6; // Name, Short, Region, Preset, Frequency, Channel
Msg g_msgs[kMaxMsgs];
int g_msgCount = 0; // populated slots (grows to kMaxMsgs)
int g_msgNext = 0;  // next write slot (ring head)

void addMsg(uint32_t from, uint32_t to, uint32_t rxTime, bool unread, const char *text)
{
    Msg &m = g_msgs[g_msgNext];
    m.from = from;
    m.to = to;
    m.rxTime = rxTime;
    m.read = !unread;
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = 0;
    g_msgNext = (g_msgNext + 1) % kMaxMsgs;
    if (g_msgCount < kMaxMsgs)
        g_msgCount++;
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
        if (g_msgs[i].from == nodeNum)
            g_msgs[i].read = true;
}

// True for nodes we've heard from or messaged — they sort ahead of plain neighbours.
bool hasConversation(uint32_t nodeNum)
{
    for (int i = 0; i < g_msgCount; i++)
        if (g_msgs[i].from == nodeNum || g_msgs[i].to == nodeNum)
            return true;
    return false;
}

// Default node ordering: favourites first, then nodes we have a conversation with,
// then everyone else by hop distance (nearest first; unknown hops last). Ties are
// broken by most-recently-heard.
bool nodeLess(uint16_t a, uint16_t b)
{
    const meshtastic_NodeInfoLite *na = nodeDB->getMeshNodeByIndex(a);
    const meshtastic_NodeInfoLite *nb = nodeDB->getMeshNodeByIndex(b);
    auto bucket = [](const meshtastic_NodeInfoLite *n) { return isFav(n) ? 0 : hasConversation(n->num) ? 1 : 2; };
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
    if (!n->snr_q4)
        return 0;
    float snr = n->snr_q4 / 4.0f;
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
    uint16_t sigCol = level >= 3 ? 0x07E0 : level == 2 ? 0xFFE0 : level == 1 ? 0xFD20 : 0x2124;
    for (int i = 0; i < nbars; i++) {
        uint16_t c = (level > 0 && i < level) ? sigCol : 0x2124; // dim for empty/relayed
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

    bool fav = isFav(n);
    char nm[28];
    const char *name = nodeName(n);
    if (name[0])
        snprintf(nm, sizeof(nm), "%s%s", fav ? "*" : "", name);
    else
        snprintf(nm, sizeof(nm), "!%08x", (unsigned)n->num);

    g->setFont(&lgfx::fonts::FreeSansBold9pt7b);
    g->setTextSize(1);
    fitWidth(g, nm, xBars - 10);
    g->setTextColor(self ? 0xFFE0 : (fav ? 0xFD20 : 0xFFFF));
    g->setCursor(4, y + 2);
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

} // namespace

AdvUI::AdvUI() : concurrency::OSThread("advui") {}

void AdvUI::initHardware()
{
    pinMode(38, OUTPUT); // display power/backlight rail — steady HIGH, not PWM
    digitalWrite(38, HIGH);

    bool ok = display.init();
    display.setRotation(1); // landscape 240x135
    display.fillScreen(0x0000);

    canvas.setColorDepth(16);
    haveCanvas = (canvas.createSprite(display.width(), display.height()) != nullptr);
    LOG_INFO("advui: UI up init=%d %dx%d canvas=%d", (int)ok, display.width(), display.height(),
             (int)haveCanvas);

    api.begin();
    kb.begin();
    LOG_INFO("advui: keyboard ready");

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
    addMsg(p.from, p.to, p.rx_time, unread, text);
}

// Fills out[] with node-DB indices matching query (all if query is null/empty),
// in the default sorted order. Returns the count (capped at max).
int AdvUI::buildNodeList(uint16_t *out, int max, const char *query)
{
    int count = 0;
    if (!nodeDB)
        return 0;
    size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total && count < max; i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node)
            continue;
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

    uint16_t order[kMaxFiltered];
    int count = buildNodeList(order, kMaxFiltered, nullptr);
    const int top = 15, rowH = 18, maxRows = (124 - top) / rowH;
    int y = top;
    for (int r = 0; r < maxRows && r < count; r++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(order[r]);
        if (!node)
            continue;
        drawNodeRow(g, node, y, node->num == me);
        y += rowH;
    }

    drawFooter(g, "type a name to find a contact");

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

    drawFooter(g, "up/dn move  ENTER open  ESC back");

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

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    if (node && nodeName(node)[0])
        g->printf("%-.30s", nodeName(node));
    else
        g->printf("!%08x", (unsigned)selectedNum);
    g->drawFastHLine(0, 13, 240, 0x39C7);

    // compact status line
    g->setTextColor(0x8410); // gray
    g->setCursor(4, 17);
    if (node) {
        g->printf("!%08x", (unsigned)node->num);
        if (node->has_hops_away)
            g->printf("  %uh", node->hops_away);
        if (node->snr_q4)
            g->printf("  %.0fdB", node->snr_q4 / 4.0f);
    } else {
        g->print("(node no longer in DB)");
    }

    // message thread (chronological, most recent at the bottom)
    const int fy0 = 30, lh = 11, maxLines = (120 - fy0) / lh;
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
        int start = mc > maxLines ? mc - maxLines : 0;
        int y = fy0;
        for (int i = start; i < mc; i++) {
            Msg &m = g_msgs[matched[i]];
            bool out = (m.from == me);
            char line[180];
            snprintf(line, sizeof(line), "%s%s", out ? "> " : "< ", m.text);
            fitWidth(g, line, 232);
            g->setTextColor(out ? 0x07FF : 0xFFFF); // outgoing cyan, incoming white
            g->setCursor(4, y);
            g->print(line);
            y += lh;
        }
    }

    drawFooter(g, selectedNum == me ? "ENTER: rename   ESC: back" : "ESC: back");

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
    g->print(editShort ? "Set short name" : "Set long name");
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
    g->printf("%u / %u", (unsigned)nameLen, editShort ? 4u : 24u);

    drawFooter(g, "type   ENTER save   ESC cancel");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

// Writes the edited name into the engine's owner and broadcasts it — the same
// path AdminModule::handleSetOwner takes, but driven from our UI.
void AdvUI::applyName()
{
    if (editShort) {
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
    bool printable = c >= 0x20 && c < 0x7f;

    if (c == AdvKeyboard::kLongEsc) { // long-press ESC opens settings from anywhere
        setSel = 0;
        mode = MODE_SETTINGS;
        return;
    }

    if (mode == MODE_SETNAME) {
        unsigned maxLen = editShort ? 4 : (unsigned)(sizeof(nameBuf) - 1);
        if (esc) {
            mode = nameReturn;
        } else if (enter) {
            if (nameLen)
                applyName();
            mode = nameReturn;
        } else if (bksp) {
            if (nameLen)
                nameBuf[--nameLen] = 0;
        } else if (printable && nameLen < maxLen) {
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
            editShort = (setSel == 1);
            const char *cur = editShort ? owner.short_name : owner.long_name;
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
        }
        // setSel 4 (Frequency) and 5 (Channel) get their editors next.
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
            mode = MODE_PICKER;
        } else if (enter && nodeDB && selectedNum == nodeDB->getNodeNum()) {
            // our own node: open the long-name editor, pre-filled with the current name
            editShort = false;
            strncpy(nameBuf, owner.long_name, sizeof(nameBuf));
            nameBuf[sizeof(nameBuf) - 1] = 0;
            nameLen = strlen(nameBuf);
            nameReturn = MODE_NODE;
            mode = MODE_SETNAME;
        }
        return;
    }

    // Node list: any interaction opens the picker.
    if (mode == MODE_NODES) {
        if (!(esc || printable || up || down || enter))
            return;
        mode = MODE_PICKER;
        queryLen = 0;
        query[0] = 0;
        sel = 0;
        scrollTop = 0;
        if (esc)
            return; // opened an empty picker
        // fall through so a printable/arrow acts immediately
    }

    // MODE_PICKER
    if (esc) {
        mode = MODE_NODES;
        queryLen = 0;
        query[0] = 0;
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
                mode = MODE_NODE;
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

    while (api.available() && api.getFromRadio(fromRadioBuf) > 0) {
        handleFromRadio(api.lastFromRadio()); // pick out incoming text messages
    }

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

    if (!splashDone)
        drawSplash();
    else if (mode == MODE_PICKER)
        drawPicker();
    else if (mode == MODE_NODE)
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
