#pragma once

// Our own ST7789 driver for the Cardputer ADV (240x135), independent of the
// stock graphics::Screen (compiled out via MESHTASTIC_EXCLUDE_SCREEN).
// Pins from variants/esp32s3/m5stack_cardputer_adv/variant.h.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace advui
{

class AdvDisplay : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
    // Backlight/power-rail (GPIO38) is driven as a steady digital HIGH in
    // AdvUI::setup() — on this board pin 38 is a power-enable, not a dimmable
    // backlight, so PWM leaves the rail under-powered.

  public:
    AdvDisplay()
    {
        {
            auto cfg = _bus.config();
            // Dedicated bus on SPI3/HSPI. The LoRa radio owns SPI2/FSPI (Arduino `SPI`),
            // so keeping the display on its own peripheral avoids corrupting radio SPI.
            cfg.spi_host = SPI3_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.pin_sclk = 36; // ST7789_SCK
            cfg.pin_mosi = 35; // ST7789_SDA
            cfg.pin_miso = -1;
            cfg.pin_dc = 34; // ST7789_RS
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 37;  // ST7789_NSS
            cfg.pin_rst = 33; // ST7789_RESET
            cfg.pin_busy = -1;
            // 1.14" 240x135 ST7789: 135x240 native window with these offsets.
            cfg.panel_width = 135;
            cfg.panel_height = 240;
            cfg.offset_x = 52;
            cfg.offset_y = 40;
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false; // dedicated SPI3 bus, not shared with the radio
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};

} // namespace advui
