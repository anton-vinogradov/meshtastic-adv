#include "AdvUI.h"
#include "PowerStatus.h"
#include "configuration.h"
#include "mesh/NodeDB.h"
#include <cstdio>

namespace advui
{

AdvUI::AdvUI() : concurrency::OSThread("advui") {}

void AdvUI::initHardware()
{
    // GPIO38 is the display power/backlight rail — steady HIGH, not PWM.
    pinMode(38, OUTPUT);
    digitalWrite(38, HIGH);

    bool ok = display.init();
    display.setRotation(1); // landscape 240x135
    display.fillScreen(0xF800); // brief proof-of-life

    canvas.setColorDepth(16);
    haveCanvas = (canvas.createSprite(display.width(), display.height()) != nullptr);
    LOG_INFO("advui: UI up init=%d %dx%d canvas=%d", (int)ok, display.width(), display.height(),
             (int)haveCanvas);

    api.begin();
}

void AdvUI::drawNodeList()
{
    lgfx::LGFXBase *g = haveCanvas ? static_cast<lgfx::LGFXBase *>(&canvas) : static_cast<lgfx::LGFXBase *>(&display);

    g->fillScreen(0x0000);

    size_t total = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    uint32_t me = nodeDB ? nodeDB->getNodeNum() : 0;

    // Header: node count (left), battery (right)
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

    g->drawFastHLine(0, 13, 240, 0x39C7); // separator

    const int rowH = 10;
    const int top = 17;
    const int maxRows = (135 - top) / rowH;
    int y = top;
    int shown = 0;

    for (size_t i = 0; i < total && shown < maxRows; i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node)
            continue;

        bool self = node->num == me;
        const char *name = node->long_name[0] ? node->long_name : node->short_name;

        char nm[18];
        if (name[0])
            snprintf(nm, sizeof(nm), "%-17.17s", name);
        else
            snprintf(nm, sizeof(nm), "!%08x", (unsigned)node->num);

        g->setTextColor(self ? 0xFFE0 : 0xFFFF); // self = yellow
        g->setCursor(4, y);
        g->print(nm);

        g->setCursor(150, y);
        g->setTextColor(0x07E0); // green
        if (node->snr_q4)
            g->printf("%4.1f", node->snr_q4 / 4.0f);
        else
            g->print("   -");

        g->setCursor(202, y);
        g->setTextColor(0x9CD3); // light blue
        if (node->has_hops_away)
            g->printf("%uh", node->hops_away);
        else
            g->print("?");

        y += rowH;
        shown++;
    }

    if (haveCanvas)
        canvas.pushSprite(0, 0);
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

    drawNodeList();
    return 200; // ~5 Hz redraw + FromRadio drain
}

// Created once from an injected call in main.cpp (after setupModules); the
// OSThread base then self-schedules, so no main-loop edits are needed.
void advuiSetup()
{
    static AdvUI advUI;
}

} // namespace advui
