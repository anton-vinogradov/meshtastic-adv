#pragma once

// Optional external ILI9341 (240x320 panel, driven landscape as 320x240) that
// mirrors the built-in screen, wired to the EXT header.
//
// Pin choice is not ours: it is the wiring the M5PORKCHOP_DualScreen fork has
// already proven on this exact board, so anyone who built that harness can use
// it here unchanged — SCK 40, MOSI 14, DC 6, CS 5, RST 3, MISO left unwired.
//
// ⚠️ Those pins are the LoRa cap's. CS 5, RST 3 and DC 6 are the SX1262's NSS,
// RST and BUSY, so this panel and an onboard radio cannot coexist: the driver is
// only ever constructed in companion mode, where the cap is absent. See
// AdvUI::extInit(), which refuses to start it otherwise.
//
// ⚠️ The bus is shared with the SD card (its own CS on GPIO 12), hence
// bus_shared = true and every push wrapped in spiLock, the same discipline
// AdvFont already follows for the card.
//
// ⚠️ The EXT header carries no 3.3 V — only +5VIN, +5VOUT and GND. A bare 3.3 V
// panel wired straight to those rails dies; use a board with its own regulator,
// or feed it 3.3 V from elsewhere.

#ifndef LGFX_USE_V1
#define LGFX_USE_V1
#endif
#include <LovyanGFX.hpp>

namespace advui
{

class AdvExtDisplay : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI _bus;

  public:
    AdvExtDisplay()
    {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST; // FSPI: the SD card's bus, and the radio's when a cap is fitted
            cfg.spi_mode = 0;
            cfg.freq_write = 27000000; // the fork's proven rate; higher risks SD stability
            cfg.freq_read = 16000000;
            cfg.pin_sclk = 40;
            cfg.pin_mosi = 14;
            cfg.pin_miso = -1; // write-only: a panel driving MISO can corrupt SD reads
            cfg.pin_dc = 6;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 5;
            cfg.pin_rst = 3;
            cfg.pin_busy = -1;
            cfg.panel_width = 240;
            cfg.panel_height = 320;
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true; // shared with the SD card
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};

} // namespace advui
