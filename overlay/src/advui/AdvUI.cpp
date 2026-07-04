#include "AdvUI.h"
#include "PowerStatus.h"
#include "configuration.h"
#include "mesh/NodeDB.h"
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

void drawRow(lgfx::LGFXBase *g, const meshtastic_NodeInfoLite *n, int y, bool self)
{
    bool fav = isFav(n);
    char nm[20];
    const char *name = nodeName(n);
    if (name[0])
        snprintf(nm, sizeof(nm), "%s%-.17s", fav ? "*" : "", name);
    else
        snprintf(nm, sizeof(nm), "!%08x", (unsigned)n->num);

    g->setTextColor(self ? 0xFFE0 : (fav ? 0xFD20 : 0xFFFF)); // self=yellow, fav=orange
    g->setCursor(4, y);
    g->print(nm);

    g->setCursor(150, y);
    g->setTextColor(0x07E0); // green
    if (n->snr_q4)
        g->printf("%4.1f", n->snr_q4 / 4.0f);
    else
        g->print("   -");

    g->setCursor(202, y);
    g->setTextColor(0x9CD3); // light blue
    if (n->has_hops_away)
        g->printf("%uh", n->hops_away);
    else
        g->print("?");
}

void drawFooter(lgfx::LGFXBase *g, const char *hint)
{
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
}

void AdvUI::drawNodeList()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    size_t total = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;

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

    const int rowH = 10, top = 17, maxRows = (124 - top) / rowH; // leave a row for the footer hint
    int y = top, shown = 0;
    for (size_t i = 0; i < total && shown < maxRows; i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node)
            continue;
        drawRow(g, node, y, node->num == me);
        y += rowH;
        shown++;
    }

    drawFooter(g, "type / ESC : find contact");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::drawPicker()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    // Query bar
    g->setTextSize(1);
    g->setTextColor(0xFFE0); // yellow
    g->setCursor(4, 3);
    g->printf("> %s_", query);
    g->drawFastHLine(0, 13, 240, 0x39C7);

    size_t total = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;

    const int rowH = 10, top = 17, maxRows = (124 - top) / rowH; // leave a row for the footer hint
    int y = top, shown = 0;

    // Pass 0: favourites, pass 1: the rest. (Recent-message senders bucket is
    // added once incoming-message tracking lands — see M1b-3.)
    for (int pass = 0; pass < 2 && shown < maxRows; pass++) {
        for (size_t i = 0; i < total && shown < maxRows; i++) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (!node)
                continue;
            if ((pass == 0) != isFav(node))
                continue;
            const char *name = nodeName(node);
            if (queryLen && !ciContains(name[0] ? name : "", query))
                continue;
            drawRow(g, node, y, node->num == me);
            y += rowH;
            shown++;
        }
    }

    drawFooter(g, "type=filter  bksp=del  ESC=back");

    if (haveCanvas)
        canvas.pushSprite(0, 0);
}

void AdvUI::handleKey(char ch)
{
    unsigned char c = (unsigned char)ch;
    bool esc = c == 0x1b;       // TCA8418Key::ESC
    bool backspace = c == 0x08; // TCA8418Key::BSP
    bool printable = c >= 0x20 && c < 0x7f;

    if (esc) {
        mode = (mode == MODE_PICKER) ? MODE_NODES : MODE_PICKER;
        queryLen = 0;
        query[0] = 0;
        return;
    }

    // Typing in the node list opens the picker and starts filtering.
    if (mode == MODE_NODES) {
        if (!printable)
            return;
        mode = MODE_PICKER;
        queryLen = 0;
        query[0] = 0;
    }

    if (backspace) {
        if (queryLen)
            query[--queryLen] = 0;
    } else if (printable && queryLen < sizeof(query) - 1) {
        query[queryLen++] = c;
        query[queryLen] = 0;
    }
    // Arrow/select codes (0xb4-0xb7, 0x0d) fall through unhandled for now.
}

int32_t AdvUI::runOnce()
{
    if (!inited) {
        initHardware();
        inited = true;
    }

    size_t n;
    while (api.available() && (n = api.getFromRadio(fromRadioBuf)) > 0) {
        fromRadioCount++;
        (void)n;
    }

    kb.trigger();
    while (kb.hasEvent()) {
        handleKey(kb.dequeueEvent());
        kb.trigger(); // re-read the FIFO as we drain (matches the stock poll loop)
    }
    kb.clearInt(); // re-arm the TCA8418 interrupt, else it stops reporting after the first event

    if (mode == MODE_PICKER)
        drawPicker();
    else
        drawNodeList();

    return 200; // 5 Hz redraw — responsive typing, light load on the shared SPI bus
}

// Created once from an injected call in main.cpp (after setupModules); the
// OSThread base then self-schedules, so no main-loop edits are needed.
void advuiSetup()
{
    static AdvUI advUI;
}

} // namespace advui
