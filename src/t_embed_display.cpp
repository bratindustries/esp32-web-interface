#include "t_embed_display.h"

#ifdef T_EMBED_DISPLAY

#include <Arduino.h>
#include <TFT_eSPI.h>

namespace {

constexpr int PIN_POWER_ON = 46;
constexpr int PIN_LCD_BL = 15;

TFT_eSPI panel;

// LILYGO's panel-specific ST7789V initialization values.  TFT_eSPI performs
// the standard controller setup first; these values match the manufacturer
// example and correct the porch, power and gamma settings for this panel.
struct LcdCommand {
  uint8_t command;
  uint8_t data[14];
  uint8_t length;
  bool delayAfter;
};

constexpr LcdCommand INIT_COMMANDS[] = {
  {0x11, {0}, 0, true},
  {0x3A, {0x05}, 1, false},
  {0xB2, {0x0B, 0x0B, 0x00, 0x33, 0x33}, 5, false},
  {0xB7, {0x75}, 1, false},
  {0xBB, {0x28}, 1, false},
  {0xC0, {0x2C}, 1, false},
  {0xC2, {0x01}, 1, false},
  {0xC3, {0x1F}, 1, false},
  {0xC6, {0x13}, 1, false},
  {0xD0, {0xA7}, 1, false},
  {0xD0, {0xA4, 0xA1}, 2, false},
  {0xD6, {0xA1}, 1, false},
  {0xE0, {0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B, 0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32}, 14, false},
  {0xE1, {0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B, 0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37}, 14, false},
};

} // namespace

namespace TEmbedDisplay {

void begin()
{
  // GPIO46 holds the T-Embed peripheral power rail on.  Bring the backlight
  // up only after initialization so boot does not show a white flash.
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);

  panel.begin();
  for (const LcdCommand& item : INIT_COMMANDS) {
    panel.writecommand(item.command);
    for (uint8_t i = 0; i < item.length; ++i) panel.writedata(item.data[i]);
    if (item.delayAfter) delay(120);
  }

  panel.setRotation(3);
  panel.fillScreen(TFT_BLACK);
  panel.setSwapBytes(true);
  setBacklight(true);
}

void setBacklight(bool enabled)
{
  digitalWrite(PIN_LCD_BL, enabled ? HIGH : LOW);
}

TFT_eSPI& device()
{
  return panel;
}

} // namespace TEmbedDisplay

#endif
