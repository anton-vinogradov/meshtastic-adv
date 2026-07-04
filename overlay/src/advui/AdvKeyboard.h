#pragma once

// Our own Cardputer keyboard reader. Extends the stock TCA8418KeyboardBase (which
// owns all the low-level I2C: init/reset/matrix/FIFO), and ports the key map +
// decode from the stock CardputerKeyboard — but without the InputBroker/menuMode
// coupling. We poll it ourselves; no InputBroker, no stock keyboard thread.

#include "input/TCA8418KeyboardBase.h"

namespace advui
{

class AdvKeyboard : public TCA8418KeyboardBase
{
  public:
    AdvKeyboard();
    void begin();           // init the TCA8418 over the keyboard I2C bus (GPIO 8/9)
    void trigger() override; // poll the FIFO; queues chars, read via hasEvent()/dequeueEvent()

  protected:
    void pressed(uint8_t key) override;
    void released() override;

  private:
    void updateModifierFlag(uint8_t key);
    bool isModifierKey(uint8_t key);

    uint8_t modifierFlag = 0;
    uint32_t last_modifier_time = 0;
    int last_key = -1;
    int next_key = -1;
    uint32_t last_tap = 0;
    uint8_t char_idx = 0;
    uint32_t tap_interval = 0;
};

} // namespace advui
