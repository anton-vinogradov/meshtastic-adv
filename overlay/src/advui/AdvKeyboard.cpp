#include "AdvKeyboard.h"
#include "configuration.h"
#include <Arduino.h>
#include <Wire.h>

namespace advui
{

namespace
{

using Key = TCA8418KeyboardBase::TCA8418Key;

constexpr uint8_t kRows = 7;
constexpr uint8_t kCols = 8;
constexpr uint8_t kNumKeys = 56;
constexpr uint32_t kMultiTapThreshold = 1500;
constexpr uint32_t kLongPressMs = 900; // ESC held this long -> settings, not "back"

constexpr uint8_t modifierFnKey = 2;
constexpr uint8_t modifierFn = 0b0010;
constexpr uint8_t modifierShiftKey = 6;
constexpr uint8_t modifierShift = 0b0001;

// Chars per key (base / shift / fn), and the modulus for cycling through them.
// Ported verbatim from the stock CardputerKeyboard.
const uint8_t TapMod[kNumKeys] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

const unsigned char TapMap[kNumKeys][3] = {{'`', '~', Key::ESC},
                                           {Key::TAB, 0x00, 0x00},
                                           {0x00, 0x00, 0x00},
                                           {0x00, 0x00, 0x00},
                                           {'1', '!', 0x00},
                                           {'q', 'Q', Key::REBOOT},
                                           {0x00, 0x00, 0x00},
                                           {0x00, 0x00, 0x00},
                                           {'2', '@', 0x00},
                                           {'w', 'W', 0x00},
                                           {'a', 'A', 0x00},
                                           {0x00, 0x00, 0x00},
                                           {'3', '#', 0x00},
                                           {'e', 'E', 0x00},
                                           {'s', 'S', 0x00},
                                           {'z', 'Z', 0x00},
                                           {'4', '$', 0x00},
                                           {'r', 'R', 0x00},
                                           {'d', 'D', 0x00},
                                           {'x', 'X', 0x00},
                                           {'5', '%', 0x00},
                                           {'t', 'T', 0x00},
                                           {'f', 'F', 0x00},
                                           {'c', 'C', 0x00},
                                           {'6', '^', 0x00},
                                           {'y', 'Y', 0x00},
                                           {'g', 'G', Key::GPS_TOGGLE},
                                           {'v', 'V', 0x00},
                                           {'7', '&', 0x00},
                                           {'u', 'U', 0x00},
                                           {'h', 'H', 0x00},
                                           {'b', 'B', Key::BT_TOGGLE},
                                           {'8', '*', 0x00},
                                           {'i', 'I', 0x00},
                                           {'j', 'J', 0x00},
                                           {'n', 'N', 0x00},
                                           {'9', '(', 0x00},
                                           {'o', 'O', 0x00},
                                           {'k', 'K', 0x00},
                                           {'m', 'M', Key::MUTE_TOGGLE},
                                           {'0', ')', 0x00},
                                           {'p', 'P', Key::SEND_PING},
                                           {'l', 'L', AdvKeyboard::kLang}, // Fn+L: RU/EN input toggle
                                           {',', '<', Key::LEFT},
                                           {'_', '-', 0x00},
                                           {'[', '{', 0x00},
                                           {';', ':', Key::UP},
                                           {'.', '>', Key::DOWN},
                                           {'=', '+', 0x00},
                                           {']', '}', 0x00},
                                           {'\'', '"', 0x00},
                                           {'/', '?', Key::RIGHT},
                                           {Key::BSP, 0x00, 0x00},
                                           {'\\', '|', 0x00},
                                           {Key::SELECT, 0x00, 0x00},
                                           {' ', ' ', ' '}};

// Register-level I2C via the shared Wire bus (already on GPIO 8/9). Using the
// callback form of TCA8418KeyboardBase::begin() avoids re-running Wire.begin()
// with the wrong default pins.
uint8_t i2cRead(uint8_t dev, uint8_t reg, uint8_t *data, uint8_t len)
{
    Wire.beginTransmission(dev);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return 1;
    Wire.requestFrom((int)dev, (int)len);
    uint8_t i = 0;
    while (Wire.available() && i < len)
        data[i++] = Wire.read();
    return 0;
}

uint8_t i2cWrite(uint8_t dev, uint8_t reg, uint8_t *data, uint8_t len)
{
    Wire.beginTransmission(dev);
    Wire.write(reg);
    Wire.write(data, len);
    return Wire.endTransmission();
}

} // namespace

AdvKeyboard::AdvKeyboard() : TCA8418KeyboardBase(kRows, kCols) {}

void AdvKeyboard::begin()
{
    Wire.begin(I2C_SDA, I2C_SCL); // keyboard bus (idempotent if the engine already did it)
    TCA8418KeyboardBase::begin(i2cRead, i2cWrite, 0x34);
}

void AdvKeyboard::trigger()
{
    // Emit ESC's long-press the moment the hold crosses the threshold (while still
    // held), independent of the FIFO — so settings open on hold, not on release.
    if (escDown && !escLongFired && millis() - escPressMs >= kLongPressMs) {
        queueEvent(kLongEsc);
        escLongFired = true;
    }

    // Pop exactly one event from the FIFO (reading KEY_EVENT_A advances it). The
    // caller re-triggers in a loop to drain, so keyCount always makes progress.
    if (keyCount() == 0)
        return;
    uint8_t k = readRegister(TCA8418_REG_KEY_EVENT_A);
    uint8_t key = k & 0x7F;
    if (k & 0x80) {
        pressed(key);
    } else {
        released();
        state = Idle;
    }
}

void AdvKeyboard::pressed(uint8_t key)
{
    if (state == Init || state == Busy)
        return;

    if (modifierFlag && (millis() - last_modifier_time > kMultiTapThreshold))
        modifierFlag = 0;

    int row = (key - 1) / 10;
    int col = (key - 1) % 10;
    if (row >= kRows || col >= kCols)
        return;

    next_key = row * kCols + col;
    state = Held;

    uint32_t now = millis();
    tap_interval = now - last_tap;

    if (next_key == 0) { // ESC key down — start the long-press timer
        escPressMs = now;
        escDown = true;
        escLongFired = false;
    }

    updateModifierFlag(next_key);
    if (isModifierKey(next_key))
        last_modifier_time = now;

    if ((int32_t)tap_interval < 0) {
        last_tap = 0;
        state = Busy;
        return;
    }

    if (next_key != last_key || tap_interval > kMultiTapThreshold)
        char_idx = 0;
    else
        char_idx += 1;

    last_key = next_key;
    last_tap = now;
}

void AdvKeyboard::released()
{
    if (state != Held)
        return;

    if (last_key < 0 || last_key >= kNumKeys) {
        last_key = -1;
        state = Idle;
        return;
    }

    last_tap = millis();

    // ESC: the long-press already fires from trigger() while held. On release just
    // clean up, and emit the normal "back" code only if it was a short tap.
    if (last_key == 0) {
        bool longFired = escLongFired;
        escDown = false;
        escLongFired = false;
        if (longFired)
            return;
        modifierFlag = modifierFn;
        queueEvent(TapMap[0][modifierFlag % TapMod[0]]);
        return;
    }

    // The four arrow keys (, ; . /) emit their arrow code while navigating, but their
    // literal symbol while typing (navKeys == false) — otherwise '.' etc. are untypable.
    if (modifierFlag == 0 && navKeys &&
        (last_key == 43 || last_key == 46 || last_key == 47 || last_key == 51)) {
        modifierFlag = modifierFn;
    }

    queueEvent(TapMap[last_key][modifierFlag % TapMod[last_key]]);

    if (!isModifierKey(last_key))
        modifierFlag = 0;
}

void AdvKeyboard::updateModifierFlag(uint8_t key)
{
    if (key == modifierShiftKey)
        modifierFlag ^= modifierShift;
    else if (key == modifierFnKey)
        modifierFlag ^= modifierFn;
}

bool AdvKeyboard::isModifierKey(uint8_t key)
{
    return (key == modifierShiftKey || key == modifierFnKey);
}

} // namespace advui
