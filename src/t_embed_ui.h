#pragma once

#ifdef T_EMBED_DISPLAY

#include <Arduino.h>

namespace TEmbedUi {

// The UI deliberately knows nothing about UART or the inverter protocol.  Its
// backend is supplied by the CAN transport in the main sketch.
struct Backend {
  bool (*readValue)(const char* name, float* value);
  bool (*writeValue)(const char* name, float value);
  bool (*sendCan)(uint32_t id, const uint8_t* data, uint8_t length);
  String (*formatValue)(const char* name, float value);
  String (*unitFor)(const char* name);
  bool (*isOnline)();
  int (*nodeId)();
};

// Loads /gauges.json and /uiprefs.json from SPIFFS, initializes the rotary
// encoder and draws the first dashboard frame.
void begin(const Backend& backend);

// Poll from Arduino loop().  Handles the encoder, condition-based page
// selection, CAN-backed values and display refreshes.
void loop();

// Force a configuration reload (the regular loop also detects file changes).
void reload();

} // namespace TEmbedUi

#endif
