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
 * Runs as an OSThread (scheduler-driven, no main-loop edits). Screens:
 *  - splash (branded boot screen while the mesh comes up)
 *  - node list (overview): sorted favourites -> conversations -> by hops
 *  - contact picker (ESC / type): query bar + filtered node list, a moving
 *    selection cursor (up/down), Enter opens the node
 *  - node view (per-node detail; the future conversation lives here)
 * Keyboard input comes from our own AdvKeyboard (TCA8418), polled here.
 */
class AdvUI : public concurrency::OSThread
{
  public:
    AdvUI();

  protected:
    int32_t runOnce() override;

  private:
    enum Mode : uint8_t { MODE_NODES, MODE_PICKER, MODE_NODE, MODE_SETNAME, MODE_SETTINGS, MODE_PICKLIST, MODE_REBOOT };

    void initHardware();
    void drawSplash();
    void drawNodeList();
    void drawPicker();
    void drawNode();
    void drawSettings();
    void drawSetName();
    void drawPickList();
    void drawReboot();
    bool applyName(); // returns true if it scheduled a reboot (frequency/channel)
    void applyLoRa(int target, int value);
    void rebuildFiltered();
    int buildNodeList(uint16_t *out, int max, const char *query);
    void handleFromRadio(const meshtastic_FromRadio &fr);
    void handleKey(char c);

    InternalAPI api;
    AdvDisplay display;
    AdvKeyboard kb;
    lgfx::LGFX_Sprite canvas{&display}; // off-screen frame buffer

    Mode mode = MODE_NODES;
    char query[24] = {0};
    uint8_t queryLen = 0;

    static constexpr int kMaxFiltered = 128;
    uint16_t filtered[kMaxFiltered]; // node DB indices, sorted + query-matched (picker)
    int filteredCount = 0;
    int sel = 0;          // selection cursor into filtered[]
    int scrollTop = 0;    // first visible row
    uint32_t selectedNum = 0; // node chosen with Enter (MODE_NODE)

    char nameBuf[25] = {0};   // node-name editor buffer (MODE_SETNAME)
    uint8_t nameLen = 0;
    int editTarget = 0;       // MODE_SETNAME target: 0 long name, 1 short, 2 frequency, 3 channel
    Mode nameReturn = MODE_SETTINGS; // where the name editor returns on save/cancel
    int setSel = 0;           // settings-menu cursor (MODE_SETTINGS)
    int pickTarget = 0;       // MODE_PICKLIST: 0 = region, 1 = preset
    int pickSel = 0;          // list-picker cursor
    int pickScroll = 0;       // first visible list-picker row

    bool inited = false;
    bool haveCanvas = false;
    bool splashDone = false;
    uint32_t bootMs = 0;
    uint8_t fromRadioBuf[MAX_TO_FROM_RADIO_SIZE]; // sized by PhoneAPI.h
};

void advuiSetup();

} // namespace advui
