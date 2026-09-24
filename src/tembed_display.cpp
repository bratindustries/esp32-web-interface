#include "tembed_display.h"

#ifdef T_EMBED_DISPLAY

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <TFT_eSPI.h>
#include <math.h>

namespace {
TFT_eSPI tft;
TFT_eSprite lcd(&tft);
// Instrument colours follow ZombieVerterDisplay's high-contrast black face:
// cyan primary gauge, grey tracks and distinct auxiliary readouts.
constexpr uint16_t bg = 0x0000, panel = 0x0843, accent = 0x5E7F;
constexpr uint16_t white = 0xFFFF, muted = 0x8410, amber = 0xFD60;
constexpr uint16_t red = 0xF800, green = 0x07E0, yellow = 0xFFE0, purple = 0xD81F;
constexpr int encoderA = 2, encoderB = 1, encoderButton = 0;
// A full pass over 32 SDO values takes about four seconds at 120 ms per read.
constexpr uint32_t staleMs = 6000;

// The original T-Embed panel needs these ST7789V parameters after the generic
// TFT_eSPI reset sequence.  This is the table used by LilyGO's factory and TFT
// examples; without it some panel revisions power their backlight but remain
// black.
struct LcdCommand {
  uint8_t command;
  uint8_t data[14];
  uint8_t length;
};

constexpr LcdCommand tEmbedLcdInit[] = {
    {0x11, {0}, 0x80},
    {0x3A, {0x05}, 1},
    {0xB2, {0x0B, 0x0B, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x75}, 1},
    {0xBB, {0x28}, 1},
    {0xC0, {0x2C}, 1},
    {0xC2, {0x01}, 1},
    {0xC3, {0x1F}, 1},
    {0xC6, {0x13}, 1},
    {0xD0, {0xA7}, 1},
    {0xD0, {0xA4, 0xA1}, 2},
    {0xD6, {0xA1}, 1},
    {0xE0, {0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B,
            0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32}, 14},
    {0xE1, {0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B,
            0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37}, 14},
};

String readJson(const char* path) {
  File f = SPIFFS.open(path, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

String shortNumber(float value) { return String(value, fabsf(value) >= 100 ? 0 : 1); }
}

int TEmbedDisplay::add(const String& n) {
  if (!n.length()) return -1;
  for (int i = 0; i < valueCount; ++i) if (values[i].name == n) return i;
  if (valueCount >= MaxValues) return -1;
  values[valueCount] = Value{};
  values[valueCount].name = n;
  return valueCount++;
}

void TEmbedDisplay::begin() {
  pinMode(46, OUTPUT);
  digitalWrite(46, HIGH); // LilyGO board power hold
  pinMode(encoderA, INPUT_PULLUP);
  pinMode(encoderB, INPUT_PULLUP);
  pinMode(encoderButton, INPUT_PULLUP);
  tft.init();

  for (const LcdCommand& item : tEmbedLcdInit) {
    tft.writecommand(item.command);
    for (uint8_t i = 0; i < (item.length & 0x7F); ++i) tft.writedata(item.data[i]);
    if (item.length & 0x80) delay(120);
  }

  tft.setRotation(3);
  tft.fillScreen(bg);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  lcd.setColorDepth(16);
  lcd.createSprite(320, 170);
  lcd.fillSprite(bg);
  lastEncoder = (digitalRead(encoderA) << 1) | digitalRead(encoderB);
  loadConfig();
}

void TEmbedDisplay::loadConfig() {
  valueCount = favoriteCount = metricCount = 0;
  statusIndex = add("status");
  modeIndex = add("opmode");
  errorIndex = add("lasterr");

  String prefs = readJson("/uiprefs.json");
  DynamicJsonDocument prefDoc(2048);
  if (deserializeJson(prefDoc, prefs) == DeserializationError::Ok &&
      prefDoc["dashMetrics"].is<JsonArray>()) {
    for (JsonVariant v : prefDoc["dashMetrics"].as<JsonArray>()) {
      if (metricCount >= 5) break;
      int idx = add(v.as<String>());
      if (idx >= 0) metricIndexes[metricCount++] = idx;
    }
  }
  if (!metricCount) {
    metricIndexes[metricCount++] = add("udc");
    metricIndexes[metricCount++] = add("tmphs");
  }
  // SOC is always the dominant driving instrument. If it is also one of the
  // five web-dashboard presets, add() reuses that value instead of duplicating it.
  socIndex = add("SOC");

  String favs = readJson("/favorites.json");
  DynamicJsonDocument favDoc(4096);
  if (deserializeJson(favDoc, favs) == DeserializationError::Ok &&
      favDoc["s"].is<JsonArray>()) {
    for (JsonVariant v : favDoc["s"].as<JsonArray>()) {
      if (favoriteCount >= MaxFavorites) break;
      int idx = add(v.as<String>());
      if (idx < 0) continue;
      bool duplicate = false;
      for (int i = 0; i < favoriteCount; ++i) duplicate |= favoriteIndexes[i] == idx;
      if (!duplicate) favoriteIndexes[favoriteCount++] = idx;
    }
  }
  // Saved web gauge limits take precedence over a range inferred from samples.
  // Gauge layouts include colours and positioning; only ranges are needed here.
  StaticJsonDocument<256> gaugeFilter;
  gaugeFilter["pages"][0]["items"][0]["name"] = true;
  gaugeFilter["pages"][0]["items"][0]["min"] = true;
  gaugeFilter["pages"][0]["items"][0]["max"] = true;
  DynamicJsonDocument gaugeDoc(16384);
  if (deserializeJson(gaugeDoc, readJson("/gauges.json"),
                      DeserializationOption::Filter(gaugeFilter)) == DeserializationError::Ok) {
    for (JsonVariant pageConfig : gaugeDoc["pages"].as<JsonArray>()) {
      for (JsonVariant item : pageConfig["items"].as<JsonArray>()) {
        if (!item["name"].is<const char*>() || !item["min"].is<float>() ||
            !item["max"].is<float>()) continue;
        float low = item["min"].as<float>(), high = item["max"].as<float>();
        if (!isfinite(low) || !isfinite(high) || high <= low) continue;
        for (int i = 0; i < valueCount; ++i) {
          if (values[i].name != item["name"].as<String>()) continue;
          values[i].gaugeMin = low;
          values[i].gaugeMax = high;
          values[i].hasGaugeRange = true;
        }
      }
    }
  }
  if (page >= pages()) page = 0;
  if (graphSelection >= favoriteCount) graphSelection = 0;
  dirty = true;
}

void TEmbedDisplay::setValue(int index, const String& text, const String& unit,
                             float raw, bool numeric) {
  if (index < 0 || index >= valueCount) return;
  Value& v = values[index];
  if (v.text != text || v.unit != unit || !v.updated) dirty = true;
  v.text = text;
  v.unit = unit;
  v.numeric = numeric && isfinite(raw);
  if (v.numeric) {
    v.history[v.historyNext] = raw;
    v.historyNext = (v.historyNext + 1) % HistoryLength;
    if (v.historyCount < HistoryLength) ++v.historyCount;
    dirty = true;
  } else {
    v.historyCount = v.historyNext = 0;
  }
  v.updated = millis();
}

void TEmbedDisplay::invalidate(int index) {
  if (index < 0 || index >= valueCount) return;
  if (values[index].updated) dirty = true;
  values[index].updated = 0;
  values[index].historyCount = values[index].historyNext = 0;
}

bool TEmbedDisplay::live(int index) const {
  return index >= 0 && index < valueCount && values[index].updated &&
         millis() - values[index].updated <= staleMs;
}

String TEmbedDisplay::display(int index) const {
  if (!live(index)) return "--";
  const Value& v = values[index];
  return v.text + (v.unit.length() && v.unit.indexOf('=') < 0 ? " " + v.unit : "");
}

int TEmbedDisplay::pages() const {
  return 3 + max(1, (favoriteCount + 2) / 3);
}

int TEmbedDisplay::graphIndex() const {
  return favoriteCount ? favoriteIndexes[graphSelection] :
         (metricCount ? metricIndexes[0] : -1);
}

bool TEmbedDisplay::range(int index, float& low, float& high) const {
  if (!live(index) || !values[index].numeric || !values[index].historyCount) return false;
  const Value& v = values[index];
  low = high = v.history[0];
  for (int i = 1; i < v.historyCount; ++i) {
    low = min(low, v.history[i]);
    high = max(high, v.history[i]);
  }
  return true;
}

bool TEmbedDisplay::gaugeRange(int index, float& low, float& high,
                               bool& fixed) const {
  fixed = false;
  if (index < 0 || index >= valueCount) return false;
  bool samples = range(index, low, high);
  const Value& value = values[index];
  if (value.hasGaugeRange) {
    low = value.gaugeMin;
    high = value.gaugeMax;
    fixed = true;
  } else if (value.unit == "%" || value.name.equalsIgnoreCase("soc")) {
    low = 0;
    high = 100;
    fixed = true;
  } else if (samples) {
    // Auto ranges always include zero. Once samples cross zero, keep both
    // sides symmetric so signed power/current gauges have a stable midpoint.
    if (low < 0 && high > 0) {
      float extent = max(fabsf(low), fabsf(high));
      low = -extent;
      high = extent;
    } else if (high <= 0) {
      low = min(low * 1.25f, -1.0f);
      high = 0;
    } else {
      low = 0;
      high = max(high * 1.25f, 1.0f);
    }
  }
  return samples && isfinite(low) && isfinite(high) && high > low;
}

bool TEmbedDisplay::gaugeArc(int index, int cx, int cy, int radius,
                             int thickness, uint16_t color,
                             uint16_t background, float& low, float& high,
                             bool& fixed) {
  lcd.drawSmoothArc(cx, cy, radius, radius - thickness, 45, 315,
                    muted, background, true);
  if (!gaugeRange(index, low, high, fixed)) return false;

  float current = values[index].history[
    (values[index].historyNext + HistoryLength - 1) % HistoryLength];
  String metricName = values[index].name;
  String metricUnit = values[index].unit;
  metricName.toLowerCase();
  metricUnit.toLowerCase();
  bool powerMetric = metricName.indexOf("power") >= 0 ||
                     metricUnit == "kw" || metricUnit == "w";
  uint16_t activeColor = powerMetric ? (current < 0 ? green : red) : color;
  float baseline = low <= 0 && high >= 0 ? 0 : low;
  auto angleFor = [low, high](float value) {
    float position = constrain((value - low) / (high - low), 0.0f, 1.0f);
    return 45 + (int)(270 * position);
  };
  int valueAngle = angleFor(current);
  int baselineAngle = angleFor(baseline);
  int startAngle = min(valueAngle, baselineAngle);
  int endAngle = max(valueAngle, baselineAngle);
  if (endAngle > startAngle)
    lcd.drawSmoothArc(cx, cy, radius, radius - thickness,
                      startAngle, endAngle, activeColor, background, true);

  // A small neutral marker makes the centre of a bipolar scale unambiguous.
  if (low < 0 && high > 0) {
    float radians = baselineAngle * DEG_TO_RAD;
    int markerRadius = radius - thickness / 2;
    int markerX = cx + (int)(sinf(radians) * markerRadius);
    int markerY = cy + (int)(cosf(radians) * markerRadius);
    lcd.fillCircle(markerX, markerY, max(1, thickness / 5), white);
  }
  return true;
}

void TEmbedDisplay::sparkline(int index, int x, int y, int w, int h) {
  float low, high;
  if (!range(index, low, high)) {
    lcd.drawFastHLine(x, y + h / 2, w, muted);
    return;
  }
  const Value& v = values[index];
  float spread = high - low;
  if (spread < 0.001f) { low -= 1; high += 1; }
  else { low -= spread * 0.08f; high += spread * 0.08f; }
  int previousX = x, previousY = y + h / 2;
  for (int i = 0; i < v.historyCount; ++i) {
    int pos = (v.historyNext + HistoryLength - v.historyCount + i) % HistoryLength;
    int sx = x + (v.historyCount == 1 ? w - 1 : i * (w - 1) / (v.historyCount - 1));
    int sy = y + h - 1 - (int)((v.history[pos] - low) * (h - 1) / (high - low));
    if (i) lcd.drawLine(previousX, previousY, sx, sy, accent);
    else lcd.drawPixel(sx, sy, accent);
    previousX = sx;
    previousY = sy;
  }
}

void TEmbedDisplay::gauge(int index, int cx, int cy, int radius) {
  float low = 0, high = 0;
  bool fixed = false;
  gaugeArc(index, cx, cy, radius, 8, accent, bg, low, high, fixed);
  lcd.setTextFont(2);
  lcd.setTextColor(muted, bg);
  String label = index >= 0 ? values[index].name.substring(0, 13) : "Value";
  if (index >= 0 && values[index].name.equalsIgnoreCase("soc")) label = "SOC";
  lcd.drawCentreString(label, cx, cy - 32, 2);
  uint16_t valueColor = white;
  if (index >= 0 && index < valueCount && values[index].historyCount) {
    String metricName = values[index].name;
    String metricUnit = values[index].unit;
    metricName.toLowerCase();
    metricUnit.toLowerCase();
    if (metricName.indexOf("power") >= 0 || metricUnit == "kw" || metricUnit == "w") {
      float current = values[index].history[
        (values[index].historyNext + HistoryLength - 1) % HistoryLength];
      valueColor = current < 0 ? green : current > 0 ? red : muted;
    }
  }
  lcd.setTextColor(valueColor, bg);
  lcd.setTextFont(4);
  lcd.drawCentreString(index >= 0 && live(index) ? values[index].text.substring(0, 7) : "--",
                       cx, cy - 10, 4);
  lcd.setTextFont(2);
  lcd.setTextColor(muted, bg);
  lcd.drawCentreString(index >= 0 && live(index) ? values[index].unit.substring(0, 8) : "",
                       cx, cy + 20, 2);
  lcd.setTextFont(1);
  if (fixed) {
    lcd.drawString(shortNumber(low), cx - radius + 7, cy + radius - 8, 1);
    lcd.drawRightString(shortNumber(high), cx + radius - 7, cy + radius - 8, 1);
  } else {
    lcd.drawCentreString("AUTO", cx, cy + radius - 8, 1);
  }
}

void TEmbedDisplay::metricCard(int index, int x, int y, uint16_t color) {
  constexpr int w = 71;
  lcd.setTextFont(1);
  lcd.setTextColor(muted, bg);
  lcd.drawCentreString(index >= 0 ? values[index].name.substring(0, 10) : "--",
                       x + w / 2, y + 1, 1);

  const int cx = x + w / 2, cy = y + 38, radius = 28;
  float low = 0, high = 0;
  bool fixed = false;
  gaugeArc(index, cx, cy, radius, 6, color, bg, low, high, fixed);
  lcd.setTextFont(2);
  uint16_t valueColor = color;
  if (index >= 0 && values[index].historyCount) {
    String metricName = values[index].name;
    String metricUnit = values[index].unit;
    metricName.toLowerCase();
    metricUnit.toLowerCase();
    if (metricName.indexOf("power") >= 0 || metricUnit == "kw" || metricUnit == "w") {
      float current = values[index].history[
        (values[index].historyNext + HistoryLength - 1) % HistoryLength];
      valueColor = current < 0 ? green : current > 0 ? red : muted;
    }
  }
  lcd.setTextColor(valueColor, bg);
  lcd.drawCentreString(index >= 0 ? display(index).substring(0, 8) : "--",
                       cx, cy - 7, 2);
}

void TEmbedDisplay::graph(int index) {
  lcd.setTextFont(2);
  if (index < 0) {
    lcd.setTextColor(muted, bg);
    lcd.drawCentreString("Select a metric in the web UI", 160, 70, 2);
    return;
  }
  lcd.setTextColor(muted, bg);
  lcd.drawString(values[index].name.substring(0, 20), 10, 27);
  lcd.setTextColor(white, bg);
  lcd.drawRightString(display(index).substring(0, 18), 310, 27, 2);
  const int x = 38, y = 57, w = 270, h = 77;
  lcd.drawFastHLine(x, y, w, panel);
  lcd.drawFastHLine(x, y + h - 1, w, muted);
  float low, high;
  if (!range(index, low, high)) {
    lcd.setTextColor(muted, bg);
    lcd.drawCentreString("Waiting for numeric CAN readings", 173, 89, 2);
    return;
  }
  lcd.setTextFont(1);
  lcd.setTextColor(muted, bg);
  lcd.drawRightString(shortNumber(high), 35, y, 1);
  lcd.drawRightString(shortNumber(low), 35, y + h - 8, 1);
  lcd.drawString("older", x, 138, 1);
  lcd.drawRightString("latest", x + w, 138, 1);
  sparkline(index, x, y, w, h);
}

void TEmbedDisplay::draw(bool canReady, int nodeId) {
  lcd.fillSprite(bg);
  if (page == 0) {
    lcd.setTextFont(1);
    lcd.setTextColor(accent, bg);
    lcd.drawString((display(modeIndex) + "  " + display(statusIndex)).substring(0, 25), 8, 6, 1);
    String err = display(errorIndex);
    if (err != "--" && err != "0" && err != "NONE") {
      lcd.setTextColor(amber, bg);
      lcd.drawCentreString(("FAULT " + err).substring(0, 18), 190, 6, 1);
    }
    lcd.setTextColor(canReady ? white : amber, bg);
    lcd.drawRightString(canReady ? "CAN #" + String(nodeId) : "CAN OFFLINE", 312, 6, 1);
  } else {
    lcd.setTextFont(2);
    lcd.setTextColor(accent, bg);
    lcd.drawString(page == 1 ? "METRICS" : page == 2 ? "HISTORY" : "FAVORITES", 10, 5);
    lcd.setTextColor(canReady ? white : amber, bg);
    lcd.drawRightString(canReady ? "CAN #" + String(nodeId) : "CAN OFFLINE", 310, 5, 2);
  }

  if (page == 0) {
    // SOC is the dominant instrument. Fill the four supporting positions from
    // the web-dashboard presets, skipping SOC if it is already among them.
    const uint16_t cardColors[4] = {yellow, green, purple, white};
    int cardIndexes[4] = {-1, -1, -1, -1};
    int cardCount = 0;
    for (int i = 0; i < metricCount && cardCount < 4; ++i) {
      if (metricIndexes[i] != socIndex) cardIndexes[cardCount++] = metricIndexes[i];
    }
    for (int i = 0; i < 4; ++i) {
      int x = 8 + (i % 2) * 75;
      int y = 25 + (i / 2) * 72;
      metricCard(cardIndexes[i], x, y, cardColors[i]);
    }
    gauge(socIndex, 238, 94, 66);

  } else if (page == 1) {
    for (int i = 0; i < metricCount; ++i) {
      int col = i % 2, row = i / 2;
      int x = 8 + col * 155, y = 29 + row * 44;
      lcd.fillRoundRect(x, y, 148, 39, 5, panel);
      lcd.setTextColor(muted, panel);
      lcd.drawString(values[metricIndexes[i]].name.substring(0, 13), x + 7, y + 2);
      lcd.setTextColor(white, panel);
      lcd.drawString(display(metricIndexes[i]).substring(0, 11), x + 7, y + 19);
      sparkline(metricIndexes[i], x + 96, y + 19, 44, 16);
    }
  } else if (page == 2) {
    graph(graphIndex());
  } else if (favoriteCount == 0) {
    lcd.setTextColor(muted, bg);
    lcd.drawCentreString("Star spot values in the web UI", 160, 75, 2);
  } else {
    int start = (page - 3) * 3;
    for (int i = 0; i < 3 && start + i < favoriteCount; ++i) {
      int idx = favoriteIndexes[start + i];
      int y = 30 + i * 44;
      lcd.fillRoundRect(8, y, 304, 36, 6, panel);
      lcd.setTextFont(2);
      lcd.setTextColor(muted, panel);
      lcd.drawString(values[idx].name.substring(0, 13), 16, y + 9);
      sparkline(idx, 131, y + 10, 71, 18);
      lcd.setTextColor(white, panel);
      lcd.drawRightString(display(idx).substring(0, 13), 304, y + 9, 2);
    }
  }
  if (page != 0) {
    lcd.setTextColor(muted, bg);
    lcd.drawCentreString(String(page + 1) + " / " + String(pages()) +
                         (page == 2 ? "  Turn: value  Press: page" : "  Turn or press: page"),
                         160, 154, 2);
  }
  lcd.pushSprite(0, 0);
  dirty = false;
  lastDraw = millis();
}

void TEmbedDisplay::tick(bool canReady, int nodeId) {
  // Poll both contacts; process only valid quadrature transitions.
  static const int8_t steps[16] = {0, -1, 1, 0, 1, 0, 0, -1,
                                    -1, 0, 0, 1, 0, 1, -1, 0};
  static int quarterSteps = 0;
  int now = (digitalRead(encoderA) << 1) | digitalRead(encoderB);
  if (now != lastEncoder) {
    quarterSteps += steps[(lastEncoder << 2) | now];
    lastEncoder = now;
    if (quarterSteps >= 4 || quarterSteps <= -4) {
      if (page == 2 && favoriteCount) {
        graphSelection = (graphSelection + (quarterSteps > 0 ? 1 : favoriteCount - 1)) % favoriteCount;
      } else {
        page = (page + (quarterSteps > 0 ? 1 : pages() - 1)) % pages();
      }
      quarterSteps = 0;
      dirty = true;
    }
  }
  bool pressed = digitalRead(encoderButton) == LOW;
  if (pressed != buttonDown && millis() - buttonChanged > 30) {
    buttonChanged = millis();
    buttonDown = pressed;
    if (pressed) { page = (page + 1) % pages(); dirty = true; }
  }
  if (dirty || millis() - lastDraw > 800) draw(canReady, nodeId);
}
#endif
