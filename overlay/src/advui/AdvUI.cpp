#include "AdvUI.h"
#include "PowerStatus.h"
#include "configuration.h"
#include "mesh/NodeDB.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

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

// Placeholder until direct messages land (M1b-3): true for nodes we've exchanged
// messages with, so they sort ahead of plain neighbours.
bool hasConversation(const meshtastic_NodeInfoLite *)
{
    return false;
}

// Default node ordering: favourites first, then nodes we have a conversation with,
// then everyone else by hop distance (nearest first; unknown hops last). Ties are
// broken by most-recently-heard.
bool nodeLess(uint16_t a, uint16_t b)
{
    const meshtastic_NodeInfoLite *na = nodeDB->getMeshNodeByIndex(a);
    const meshtastic_NodeInfoLite *nb = nodeDB->getMeshNodeByIndex(b);
    auto bucket = [](const meshtastic_NodeInfoLite *n) { return isFav(n) ? 0 : hasConversation(n) ? 1 : 2; };
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

    g->setFont(&lgfx::fonts::Font0);
    g->setTextSize(1);
    g->setTextColor(0x07FF); // cyan
    g->setCursor(4, 3);
    if (node && nodeName(node)[0])
        g->printf("%-.30s", nodeName(node));
    else
        g->printf("!%08x", (unsigned)selectedNum);
    g->drawFastHLine(0, 13, 240, 0x39C7);

    g->setTextColor(0xFFFF);
    if (node) {
        g->setCursor(4, 20);
        g->printf("!%08x", (unsigned)node->num);
        g->setCursor(4, 34);
        if (node->snr_q4)
            g->printf("SNR %.1f dB", node->snr_q4 / 4.0f);
        else
            g->print("SNR  -");
        g->setCursor(4, 48);
        if (node->has_hops_away)
            g->printf("hops %u", node->hops_away);
        else
            g->print("hops ?");
        g->setCursor(4, 62);
        g->printf("seen %us ago", (unsigned)sinceLastSeen(node));
    } else {
        g->setCursor(4, 20);
        g->print("(node no longer in DB)");
    }

    g->setTextColor(0x9CD3);
    g->setCursor(4, 84);
    g->print("(no messages yet)");

    drawFooter(g, "ESC: back");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
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

    if (mode == MODE_NODE) {
        if (esc || bksp)
            mode = MODE_PICKER;
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
        // drain the FromRadio stream in-process: config, node DB, then live packets
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
