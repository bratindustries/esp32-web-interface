#pragma once

#include <Arduino.h>

// The LCD is intentionally a view of the existing web interface's CAN data.
// No second TWAI driver or SDO client is started here.
class TEmbedDisplay {
 public:
  static constexpr int MaxFavorites = 24;
  static constexpr int MaxValues = 32;
  static constexpr int HistoryLength = 32;
  void begin();
  void loadConfig();
  void tick(bool canReady, int nodeId);
  int count() const { return valueCount; }
  const String& name(int index) const { return values[index].name; }
  void setValue(int index, const String& text, const String& unit, float raw, bool numeric);
  void invalidate(int index);

 private:
  struct Value {
    String name, text, unit;
    uint32_t updated = 0;
    float history[HistoryLength] = {};
    uint8_t historyCount = 0, historyNext = 0;
    float gaugeMin = 0, gaugeMax = 0;
    bool numeric = false, hasGaugeRange = false;
  };
  Value values[MaxValues];
  int valueCount = 0, favoriteCount = 0, metricCount = 0;
  int favoriteIndexes[MaxFavorites] = {};
  int metricIndexes[5] = {};
  int statusIndex = -1, modeIndex = -1, errorIndex = -1, socIndex = -1;
  int page = 0;
  int graphSelection = 0;
  int lastEncoder = 0;
  bool buttonDown = false, dirty = true;
  uint32_t buttonChanged = 0, lastDraw = 0;
  int add(const String& name);
  int pages() const;
  int graphIndex() const;
  String display(int index) const;
  bool live(int index) const;
  bool range(int index, float& low, float& high) const;
  bool gaugeRange(int index, float& low, float& high, bool& fixed) const;
  bool gaugeArc(int index, int cx, int cy, int radius, int thickness,
                uint16_t color, uint16_t background,
                float& low, float& high, bool& fixed);
  void sparkline(int index, int x, int y, int w, int h);
  void gauge(int index, int cx, int cy, int radius);
  void metricCard(int index, int x, int y, uint16_t color);
  void graph(int index);
  void draw(bool canReady, int nodeId);
};
