#pragma once

#include "AdvDisplay.h"
#include "AdvKeyboard.h"
#include "InternalAPI.h"
#include "concurrency/OSThread.h"
#include <cstddef>
#include <cstdint>

namespace advui
{

/**
 * Our own UI layer, replacing the stock graphics::Screen — additive overlay.
 *
 * Runs as an OSThread (scheduler-driven, no main-loop edits). Two screens:
 *  - node list (default)
 *  - contact picker (ESC / start typing): a query bar + filtered node list.
 * Keyboard input comes from our own AdvKeyboard (TCA8418), polled here — the
 * stock InputBroker/keyboard is disabled.
 */
class AdvUI : public concurrency::OSThread
{
  public:
    AdvUI();

  protected:
    int32_t runOnce() override;

  private:
    enum Mode : uint8_t { MODE_NODES, MODE_PICKER };

    void initHardware();
    void drawNodeList();
    void drawPicker();
    void handleKey(char c);

    InternalAPI api;
    AdvDisplay display;
    AdvKeyboard kb;
    lgfx::LGFX_Sprite canvas{&display}; // off-screen frame buffer

    Mode mode = MODE_NODES;
    char query[24] = {0};
    uint8_t queryLen = 0;

    bool inited = false;
    bool haveCanvas = false;
    uint32_t fromRadioCount = 0;
    uint8_t fromRadioBuf[MAX_TO_FROM_RADIO_SIZE]; // sized by PhoneAPI.h
};

void advuiSetup();

} // namespace advui
