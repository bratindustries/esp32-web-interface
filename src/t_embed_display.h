#pragma once

// Hardware support for the original LILYGO T-Embed (ESP32-S3, 320x170
// ST7789V).  The implementation is only compiled for the dedicated
// esp32_tembed targets, so the existing ESP32 builds do not gain a display
// dependency.
#ifdef T_EMBED_DISPLAY

class TFT_eSPI;

namespace TEmbedDisplay {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 170;

// Powers and initializes the panel, clears it, and enables the backlight.
void begin();

// Change the backlight without disturbing panel contents.
void setBacklight(bool enabled);

// Access to the configured TFT_eSPI instance for the future on-device GUI.
TFT_eSPI& device();

} // namespace TEmbedDisplay

#endif
