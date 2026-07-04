#pragma once

#include "AdvDisplay.h"
#include "InternalAPI.h"
#include "concurrency/OSThread.h"
#include <cstddef>
#include <cstdint>

namespace advui
{

/**
 * Our own UI layer, replacing the stock graphics::Screen — additive overlay,
 * no edits to upstream files.
 *
 * Wired in via the sanctioned setupNicheGraphics() hook; runs as an OSThread so
 * the engine's scheduler drives it (no main-loop edits). Renders into an
 * off-screen sprite (double buffer) pushed to the ST7789 in one transfer.
 */
class AdvUI : public concurrency::OSThread
{
  public:
    AdvUI();

  protected:
    int32_t runOnce() override;

  private:
    void initHardware();
    void drawNodeList();

    InternalAPI api;
    AdvDisplay display;
    lgfx::LGFX_Sprite canvas{&display}; // off-screen frame buffer

    bool inited = false;
    bool haveCanvas = false;
    uint32_t fromRadioCount = 0;
    uint8_t fromRadioBuf[MAX_TO_FROM_RADIO_SIZE]; // sized by PhoneAPI.h
};

// Create the UI once (from an injected call in main.cpp after setupModules);
// the OSThread base then schedules itself.
void advuiSetup();

} // namespace advui
