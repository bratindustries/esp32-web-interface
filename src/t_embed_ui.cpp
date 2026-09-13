#include "t_embed_ui.h"

#ifdef T_EMBED_DISPLAY

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <TFT_eSPI.h>
#include <math.h>
#include "t_embed_display.h"

namespace {

constexpr int ENCODER_A = 2;
constexpr int ENCODER_B = 1;
constexpr int ENCODER_BUTTON = 0;
constexpr uint8_t MAX_PAGES = 10;
constexpr uint8_t MAX_ITEMS = 28;
constexpr uint8_t MAX_DASH_METRICS = 5;
constexpr uint8_t MAX_CACHE = 18;

constexpr uint16_t BG = 0x0841;
constexpr uint16_t PANEL = 0x10A2;
constexpr uint16_t PANEL_2 = 0x18E3;
constexpr uint16_t TEXT = 0xFFFF;
constexpr uint16_t MUTED = 0x9CD3;
constexpr uint16_t GREEN = 0x4E6B;
constexpr uint16_t AMBER = 0xFD20;
constexpr uint16_t RED = 0xF986;
constexpr uint16_t CYAN = 0x2D7F;

struct Condition {
  String name;
  float minValue = 0;
  float maxValue = 0;
  bool hasMin = false;
  bool hasMax = false;
  bool invert = false;
};

struct Gauge {
  String name;
  String name2;
  String param;
  String label;
  String type = "radial";
  String unit;
  String onLabel = "ON";
  String offLabel = "OFF";
  String action;
  float minValue = 0;
  float maxValue = 100;
  float value = 0;
  float onValue = 1;
  float offValue = 0;
  int decimals = 1;
  uint16_t color = CYAN;
  uint32_t canId = 0;
  uint8_t canData[8] = {0};
  uint8_t canLength = 0;
  uint8_t canOnData[8] = {0};
  uint8_t canOnLength = 0;
  uint8_t canOffData[8] = {0};
  uint8_t canOffLength = 0;
  int presetId = -1;
  bool confirm = true;
  bool invertScale = false;
  bool localOn = false;
};

struct Page {
  String name;
  Condition condition;
  Gauge items[MAX_ITEMS];
  uint8_t count = 0;
};

struct CacheEntry {
  String name;
  float value = NAN;
  uint32_t at = 0;
  bool valid = false;
};

TEmbedUi::Backend io = {};
Page pages[MAX_PAGES];
uint8_t pageCount = 0;
String dashMetrics[MAX_DASH_METRICS] = {"udc", "tmphs"};
uint8_t dashMetricCount = 2;
CacheEntry cache[MAX_CACHE];
uint8_t cacheCount = 0;

volatile int32_t encoderTransitions = 0;
volatile uint8_t encoderState = 0;
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
int32_t consumedTransitions = 0;

int activePage = -1;
int screenIndex = 0; // 0 dashboard, 1..n page gauges
bool editing = false;
bool confirming = false;
float editValue = 0;
uint32_t lastDraw = 0;
uint32_t lastConfigCheck = 0;
uint32_t gaugesStamp = 0;
uint32_t prefsStamp = 0;
uint32_t buttonChangedAt = 0;
bool lastButton = true;
bool stableButton = true;
String toast;
uint32_t toastUntil = 0;
float lineHistory[64] = {0};
uint8_t lineHead = 0;
bool lineFull = false;

uint16_t parseColor(const char* text, uint16_t fallback) {
  if (!text || text[0] != '#' || strlen(text) != 7) return fallback;
  uint32_t rgb = strtoul(text + 1, nullptr, 16);
  return ((rgb & 0xF80000) >> 8) | ((rgb & 0x00FC00) >> 5) | ((rgb & 0x0000F8) >> 3);
}

uint32_t fileStamp(const char* path) {
  File file = SPIFFS.open(path, "r");
  if (!file) return 0;
  uint32_t hash = 2166136261UL ^ file.size();
  uint8_t buffer[96];
  while (file.available()) {
    size_t n = file.read(buffer, sizeof(buffer));
    for (size_t i = 0; i < n; ++i) hash = (hash ^ buffer[i]) * 16777619UL;
  }
  file.close();
  return hash;
}

bool parseCanBytes(const String& source, uint8_t* output, uint8_t* length) {
  *length = 0;
  int pos = 0;
  while (pos < (int)source.length() && *length < 8) {
    while (pos < (int)source.length() && (source[pos] == ' ' || source[pos] == ',' || source[pos] == '-')) pos++;
    if (pos >= (int)source.length()) break;
    char* end = nullptr;
    long value = strtol(source.c_str() + pos, &end, 16);
    if (end == source.c_str() + pos || value < 0 || value > 255) return false;
    output[(*length)++] = (uint8_t)value;
    pos = end - source.c_str();
  }
  return *length > 0;
}

void parseGauge(JsonObject obj, Gauge& gauge) {
  gauge = Gauge();
  gauge.name = (const char*)(obj["name"] | "");
  gauge.name2 = (const char*)(obj["name2"] | "");
  gauge.param = (const char*)(obj["param"] | "");
  gauge.label = (const char*)(obj["label"] | "");
  gauge.type = (const char*)(obj["type"] | "radial");
  gauge.unit = (const char*)(obj["unit"] | "");
  gauge.onLabel = (const char*)(obj["onLabel"] | "ON");
  gauge.offLabel = (const char*)(obj["offLabel"] | "OFF");
  gauge.action = (const char*)(obj["act"] | "set");
  gauge.minValue = obj["min"] | 0.0f;
  gauge.maxValue = obj.containsKey("max") ? obj["max"].as<float>() : 100.0f;
  if (gauge.maxValue == gauge.minValue) gauge.maxValue = gauge.minValue + 100.0f;
  gauge.value = obj["value"] | 0.0f;
  gauge.onValue = obj["onValue"] | 1.0f;
  gauge.offValue = obj["offValue"] | 0.0f;
  gauge.decimals = constrain((int)(obj["decimals"] | 1), 0, 4);
  gauge.color = parseColor(obj["color"] | "", CYAN);
  gauge.confirm = obj["confirm"] | true;
  gauge.invertScale = obj["invertScale"] | false;
  gauge.presetId = obj["presetId"] | -1;
  if (gauge.type == "toggle") {
    gauge.minValue = min(gauge.onValue, gauge.offValue);
    gauge.maxValue = max(gauge.onValue, gauge.offValue);
    if (gauge.maxValue == gauge.minValue) gauge.maxValue = gauge.minValue + 1.0f;
  }
  if (gauge.action == "can") {
    String id = obj["canId"].is<const char*>() ? String(obj["canId"].as<const char*>()) : String(obj["canId"] | 0);
    gauge.canId = strtoul(id.c_str(), nullptr, 0);
    if (gauge.type == "toggle") {
      parseCanBytes(String((const char*)(obj["onData"] | "")), gauge.canOnData, &gauge.canOnLength);
      parseCanBytes(String((const char*)(obj["offData"] | "")), gauge.canOffData, &gauge.canOffLength);
    } else {
      parseCanBytes(String((const char*)(obj["canData"] | "")), gauge.canData, &gauge.canLength);
    }
  }
}

void parsePage(JsonObject object, Page& page) {
  page = Page();
  page.name = (const char*)(object["name"] | "Gauges");
  if (object["cond"].is<JsonObject>()) {
    JsonObject cond = object["cond"].as<JsonObject>();
    page.condition.name = (const char*)(cond["name"] | "");
    page.condition.hasMin = cond.containsKey("min");
    page.condition.hasMax = cond.containsKey("max");
    page.condition.minValue = cond["min"] | 0.0f;
    page.condition.maxValue = cond["max"] | 0.0f;
    page.condition.invert = cond["invert"] | false;
  }
  JsonArray items = object["items"].as<JsonArray>();
  for (JsonObject item : items) {
    if (page.count >= MAX_ITEMS) break;
    parseGauge(item, page.items[page.count++]);
  }
}

bool loadGauges() {
  pageCount = 0;
  File file = SPIFFS.open("/gauges.json", "r");
  if (!file) return false;
  size_t capacity = max((size_t)8192, file.size() * 2 + 2048);
  DynamicJsonDocument document(capacity);
  DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error) return false;
  if (document["pages"].is<JsonArray>()) {
    for (JsonObject page : document["pages"].as<JsonArray>()) {
      if (pageCount >= MAX_PAGES) break;
      parsePage(page, pages[pageCount++]);
    }
  } else if (document["items"].is<JsonArray>()) {
    JsonObject root = document.as<JsonObject>();
    parsePage(root, pages[pageCount++]);
  }
  return pageCount > 0;
}

void loadPrefs() {
  dashMetricCount = 2;
  dashMetrics[0] = "udc";
  dashMetrics[1] = "tmphs";
  File file = SPIFFS.open("/uiprefs.json", "r");
  if (!file) return;
  DynamicJsonDocument document(2048);
  if (!deserializeJson(document, file) && document["dashMetrics"].is<JsonArray>()) {
    dashMetricCount = 0;
    for (JsonVariant metric : document["dashMetrics"].as<JsonArray>()) {
      if (dashMetricCount >= MAX_DASH_METRICS) break;
      const char* name = metric.as<const char*>();
      if (name && *name) dashMetrics[dashMetricCount++] = name;
    }
    if (!dashMetricCount) {
      dashMetrics[0] = "udc";
      dashMetrics[1] = "tmphs";
      dashMetricCount = 2;
    }
  }
  file.close();
}

bool readValue(const String& name, float* value, uint32_t maxAge = 350) {
  if (!name.length() || !io.readValue) return false;
  uint32_t now = millis();
  for (uint8_t i = 0; i < cacheCount; ++i) {
    if (cache[i].name == name) {
      if (cache[i].valid && now - cache[i].at <= maxAge) {
        *value = cache[i].value;
        return true;
      }
      bool ok = io.readValue(name.c_str(), value);
      cache[i].at = now;
      cache[i].valid = ok;
      if (ok) cache[i].value = *value;
      return ok;
    }
  }
  uint8_t slot = cacheCount < MAX_CACHE ? cacheCount++ : (now / 379) % MAX_CACHE;
  cache[slot].name = name;
  bool ok = io.readValue(name.c_str(), value);
  cache[slot].at = now;
  cache[slot].valid = ok;
  if (ok) cache[slot].value = *value;
  return ok;
}

String valueText(const String& name, float value, int decimals) {
  if (io.formatValue) {
    String formatted = io.formatValue(name.c_str(), value);
    if (formatted.length()) return formatted;
  }
  return String(value, decimals);
}

String shortUnit(const Gauge& gauge) {
  if (gauge.unit.length()) return gauge.unit;
  if (!io.unitFor) return "";
  String unit = io.unitFor((gauge.param.length() ? gauge.param : gauge.name).c_str());
  if (unit.indexOf('=') >= 0) return "";
  return unit;
}

bool conditionMatches(const Condition& condition) {
  if (!condition.name.length()) return false;
  float value;
  if (!readValue(condition.name, &value, 700)) return false;
  bool match = (!condition.hasMin || value >= condition.minValue) &&
               (!condition.hasMax || value <= condition.maxValue);
  return condition.invert ? !match : match;
}

int choosePage() {
  if (!pageCount) return -1;
  for (uint8_t i = 0; i < pageCount; ++i)
    if (pages[i].condition.name.length() && conditionMatches(pages[i].condition)) return i;
  for (uint8_t i = 0; i < pageCount; ++i)
    if (!pages[i].condition.name.length()) return i;
  return 0;
}

void IRAM_ATTR encoderIsr() {
  static const int8_t transitions[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  uint8_t next = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);
  portENTER_CRITICAL_ISR(&encoderMux);
  encoderTransitions += transitions[(encoderState << 2) | next];
  encoderState = next;
  portEXIT_CRITICAL_ISR(&encoderMux);
}

int encoderDelta() {
  int32_t raw;
  portENTER_CRITICAL(&encoderMux);
  raw = encoderTransitions;
  portEXIT_CRITICAL(&encoderMux);
  int32_t detents = raw / 4;
  int32_t consumed = consumedTransitions / 4;
  if (detents == consumed) return 0;
  int delta = detents - consumed;
  consumedTransitions = detents * 4;
  return delta;
}

void text(TFT_eSPI& tft, const String& value, int x, int y, uint8_t size, uint16_t color, uint8_t datum = TL_DATUM) {
  tft.setTextDatum(datum);
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.drawString(value, x, y);
}

void drawHeader(TFT_eSPI& tft, const String& title, const String& right, uint16_t accent = CYAN) {
  tft.fillRect(0, 0, 320, 28, PANEL);
  tft.fillRect(0, 27, 320, 2, accent);
  text(tft, title, 10, 7, 2, TEXT);
  text(tft, right, 310, 8, 1, MUTED, TR_DATUM);
}

void drawDashboard(TFT_eSPI& tft) {
  bool online = io.isOnline && io.isOnline();
  float opmode = NAN, status = NAN, lastError = NAN;
  readValue("opmode", &opmode);
  readValue("status", &status);
  readValue("lasterr", &lastError);
  String mode = isnan(opmode) ? "--" : valueText("opmode", opmode, 0);
  String node = io.nodeId ? String(io.nodeId()) : "?";
  drawHeader(tft, "DASHBOARD", "CAN " + node, online ? GREEN : RED);

  tft.fillRoundRect(8, 37, 116, 37, 6, PANEL);
  tft.fillCircle(22, 55, 5, online ? GREEN : RED);
  text(tft, online ? "ONLINE" : "OFFLINE", 34, 48, 2, TEXT);
  tft.fillRoundRect(132, 37, 180, 37, 6, PANEL);
  text(tft, "MODE", 143, 43, 1, MUTED);
  text(tft, mode, 300, 46, mode.length() > 12 ? 1 : 2, TEXT, TR_DATUM);

  int cardWidth = (304 - (dashMetricCount - 1) * 6) / max(1, (int)dashMetricCount);
  for (uint8_t i = 0; i < dashMetricCount; ++i) {
    int x = 8 + i * (cardWidth + 6);
    tft.fillRoundRect(x, 82, cardWidth, 60, 6, PANEL_2);
    float value;
    bool ok = readValue(dashMetrics[i], &value);
    String shown = ok ? valueText(dashMetrics[i], value, 1) : "--";
    String unit = io.unitFor ? io.unitFor(dashMetrics[i].c_str()) : "";
    if (unit.indexOf('=') >= 0) unit = "";
    text(tft, dashMetrics[i], x + cardWidth / 2, 90, 1, MUTED, TC_DATUM);
    text(tft, shown, x + cardWidth / 2, 106, shown.length() > 7 ? 1 : 2, TEXT, TC_DATUM);
    if (unit.length()) text(tft, unit, x + cardWidth / 2, 128, 1, MUTED, TC_DATUM);
  }
  String foot = "rotate: gauges";
  if (!isnan(lastError) && (int)lastError != 0) foot = "ERROR " + valueText("lasterr", lastError, 0);
  else if (!isnan(status)) foot = "status " + valueText("status", status, 0);
  text(tft, foot, 160, 153, 1, (!isnan(lastError) && lastError != 0) ? RED : MUTED, TC_DATUM);
}

void drawBar(TFT_eSPI& tft, const Gauge& gauge, float value) {
  float lo = min(gauge.minValue, gauge.maxValue), hi = max(gauge.minValue, gauge.maxValue);
  float ratio = constrain((value - lo) / (hi - lo), 0.0f, 1.0f);
  if ((gauge.maxValue < gauge.minValue) != gauge.invertScale) ratio = 1.0f - ratio;
  tft.fillRoundRect(30, 104, 260, 20, 9, PANEL_2);
  tft.fillRoundRect(32, 106, max(2, (int)(256 * ratio)), 16, 7, gauge.color);
  text(tft, String(gauge.minValue, gauge.decimals), 30, 130, 1, MUTED);
  text(tft, String(gauge.maxValue, gauge.decimals), 290, 130, 1, MUTED, TR_DATUM);
}

void drawRadial(TFT_eSPI& tft, const Gauge& gauge, float value) {
  float lo = min(gauge.minValue, gauge.maxValue), hi = max(gauge.minValue, gauge.maxValue);
  float ratio = constrain((value - lo) / (hi - lo), 0.0f, 1.0f);
  if ((gauge.maxValue < gauge.minValue) != gauge.invertScale) ratio = 1.0f - ratio;
  int cx = 78, cy = 101, radius = 48;
  for (int deg = -135; deg <= 135; deg += 5) {
    float rad = deg * DEG_TO_RAD;
    uint16_t c = deg <= -135 + ratio * 270 ? gauge.color : PANEL_2;
    tft.drawLine(cx + cos(rad) * 39, cy + sin(rad) * 39, cx + cos(rad) * radius, cy + sin(rad) * radius, c);
  }
  float angle = (-135 + ratio * 270) * DEG_TO_RAD;
  tft.drawLine(cx, cy, cx + cos(angle) * 34, cy + sin(angle) * 34, TEXT);
  tft.fillCircle(cx, cy, 4, gauge.color);
}

void drawLineGauge(TFT_eSPI& tft, const Gauge& gauge, float value) {
  lineHistory[lineHead++] = value;
  if (lineHead >= 64) { lineHead = 0; lineFull = true; }
  int count = lineFull ? 64 : lineHead;
  tft.drawRoundRect(20, 62, 280, 78, 5, PANEL_2);
  if (count < 2) return;
  float lo = min(gauge.minValue, gauge.maxValue), hi = max(gauge.minValue, gauge.maxValue);
  for (int i = 1; i < count; ++i) {
    int a = lineFull ? (lineHead + i - 1) % 64 : i - 1;
    int b = lineFull ? (lineHead + i) % 64 : i;
    int x1 = 23 + (i - 1) * 274 / 63, x2 = 23 + i * 274 / 63;
    int y1 = 136 - constrain((lineHistory[a] - lo) / (hi - lo), 0.0f, 1.0f) * 70;
    int y2 = 136 - constrain((lineHistory[b] - lo) / (hi - lo), 0.0f, 1.0f) * 70;
    tft.drawLine(x1, y1, x2, y2, gauge.color);
  }
}

void drawGauge(TFT_eSPI& tft, Page& page, Gauge& gauge) {
  String position = String(screenIndex) + "/" + String(page.count);
  drawHeader(tft, page.name, position, editing || confirming ? AMBER : gauge.color);
  String label = gauge.label.length() ? gauge.label : (gauge.param.length() ? gauge.param : gauge.name);
  text(tft, label, 160, 35, label.length() > 22 ? 1 : 2, TEXT, TC_DATUM);

  String source = gauge.param.length() ? gauge.param : gauge.name;
  float live = NAN;
  bool ok;
  if (gauge.type == "toggle" && gauge.action == "can") {
    live = gauge.localOn ? gauge.onValue : gauge.offValue;
    ok = true;
  } else {
    ok = source.length() && readValue(source, &live);
  }
  if (editing || confirming) live = editValue;
  String unit = shortUnit(gauge);
  String shown = ok || editing || confirming ? valueText(source, live, gauge.decimals) : "--";

  if (gauge.type == "indicator") {
    bool on = ok && live >= max(gauge.minValue, gauge.maxValue);
    tft.fillCircle(105, 101, 31, on ? gauge.color : PANEL_2);
    text(tft, on ? "ON" : "OFF", 105, 94, 2, on ? BG : MUTED, TC_DATUM);
  } else if (gauge.type == "line") {
    if (ok) drawLineGauge(tft, gauge, live);
  } else if (gauge.type == "bar" || gauge.type == "slider" || gauge.type == "toggle") {
    if (gauge.type == "toggle") {
      bool on = fabs(live - gauge.onValue) <= fabs(live - gauge.offValue);
      shown = on ? gauge.onLabel : gauge.offLabel;
    }
    if (ok || editing) drawBar(tft, gauge, live);
  } else if (gauge.type == "action") {
    tft.fillRoundRect(35, 70, 250, 55, 10, confirming ? AMBER : gauge.color);
    text(tft, confirming ? "PRESS TO CONFIRM" : "PRESS TO RUN", 160, 89, 2, confirming ? BG : TEXT, TC_DATUM);
    shown = gauge.action == "preset" ? "preset" : gauge.action == "can" ? "raw CAN" : String(gauge.value, gauge.decimals);
  } else {
    if (ok) drawRadial(tft, gauge, live);
  }

  if (gauge.type != "line" && gauge.type != "action") {
    int x = (gauge.type == "radial" || gauge.type.length() == 0) ? 220 : 160;
    text(tft, shown, x, 76, shown.length() > 9 ? 2 : 3, editing ? AMBER : TEXT, TC_DATUM);
    if (unit.length()) text(tft, unit, x, 111, 1, MUTED, TC_DATUM);
  }
  String hint = editing ? "rotate: adjust   press: save" : confirming ? "press: confirm   hold: cancel" :
    (gauge.type == "slider" || gauge.type == "toggle" ? "press: adjust" : gauge.type == "action" ? "press: select" : "press: read only");
  text(tft, hint, 160, 154, 1, editing || confirming ? AMBER : MUTED, TC_DATUM);
}

bool applyPreset(int presetId) {
  File file = SPIFFS.open("/presets.json", "r");
  if (!file) return false;
  DynamicJsonDocument document(max((size_t)4096, file.size() * 2 + 1024));
  bool parsed = !deserializeJson(document, file);
  file.close();
  if (!parsed || !document["presets"].is<JsonArray>()) return false;
  for (JsonObject preset : document["presets"].as<JsonArray>()) {
    if ((int)(preset["id"] | -1) != presetId) continue;
    JsonObject params = preset["params"].as<JsonObject>();
    bool ok = true;
    for (JsonPair pair : params) {
      if (!io.writeValue || !io.writeValue(pair.key().c_str(), pair.value().as<float>())) ok = false;
    }
    return ok;
  }
  return false;
}

bool executeAction(const Gauge& gauge) {
  if (gauge.action == "can")
    return io.sendCan && gauge.canLength && io.sendCan(gauge.canId, gauge.canData, gauge.canLength);
  if (gauge.action == "preset") return applyPreset(gauge.presetId);
  return io.writeValue && gauge.param.length() && io.writeValue(gauge.param.c_str(), gauge.value);
}

bool saveEditedGauge(Gauge& gauge) {
  if (gauge.type == "toggle" && gauge.action == "can") {
    bool nextOn = editValue == gauge.onValue;
    const uint8_t* data = nextOn ? gauge.canOnData : gauge.canOffData;
    uint8_t length = nextOn ? gauge.canOnLength : gauge.canOffLength;
    bool ok = io.sendCan && length && io.sendCan(gauge.canId, data, length);
    if (ok) gauge.localOn = nextOn;
    return ok;
  }
  return io.writeValue && gauge.param.length() && io.writeValue(gauge.param.c_str(), editValue);
}

void showToast(const String& message) {
  toast = message;
  toastUntil = millis() + 1400;
}

void handlePress(bool longPress) {
  if (longPress) {
    editing = confirming = false;
    screenIndex = 0;
    return;
  }
  if (screenIndex == 0 || activePage < 0) return;
  Gauge& gauge = pages[activePage].items[screenIndex - 1];
  if (editing) {
    bool ok = saveEditedGauge(gauge);
    editing = false;
    cacheCount = 0;
    showToast(ok ? "SAVED" : "WRITE FAILED");
    return;
  }
  if (confirming) {
    bool ok = executeAction(gauge);
    confirming = false;
    cacheCount = 0;
    showToast(ok ? "DONE" : "ACTION FAILED");
    return;
  }
  if (gauge.type == "slider" || gauge.type == "toggle") {
    if (gauge.type == "toggle" && gauge.action == "can") {
      editValue = gauge.localOn ? gauge.onValue : gauge.offValue;
      editing = true;
      return;
    }
    if (!gauge.param.length() || !readValue(gauge.param, &editValue, 0)) {
      showToast("VALUE UNAVAILABLE");
      return;
    }
    editing = true;
    return;
  }
  if (gauge.type == "action") {
    if (gauge.confirm) confirming = true;
    else showToast(executeAction(gauge) ? "DONE" : "ACTION FAILED");
    return;
  }
  showToast("READ ONLY");
}

void handleEncoder(int delta) {
  if (!delta) return;
  if (editing && activePage >= 0 && screenIndex > 0) {
    Gauge& gauge = pages[activePage].items[screenIndex - 1];
    if (gauge.type == "toggle") {
      editValue = delta > 0 ? gauge.onValue : gauge.offValue;
    } else {
      float step = powf(10.0f, -gauge.decimals);
      editValue += delta * step;
      editValue = constrain(editValue, min(gauge.minValue, gauge.maxValue), max(gauge.minValue, gauge.maxValue));
    }
    return;
  }
  if (confirming) return;
  int count = activePage >= 0 ? pages[activePage].count : 0;
  int total = count + 1;
  screenIndex = ((screenIndex + delta) % total + total) % total;
  lineHead = 0;
  lineFull = false;
}

void draw() {
  TFT_eSPI& tft = TEmbedDisplay::device();
  tft.fillScreen(BG);
  if (screenIndex == 0 || activePage < 0 || !pages[activePage].count) drawDashboard(tft);
  else drawGauge(tft, pages[activePage], pages[activePage].items[screenIndex - 1]);
  if (toast.length() && millis() < toastUntil) {
    uint16_t color = toast.indexOf("FAILED") >= 0 || toast.indexOf("UNAVAILABLE") >= 0 ? RED : GREEN;
    tft.fillRoundRect(78, 64, 164, 42, 8, color);
    text(tft, toast, 160, 78, toast.length() > 15 ? 1 : 2, BG, TC_DATUM);
  }
}

} // namespace

namespace TEmbedUi {

void reload() {
  loadGauges();
  loadPrefs();
  cacheCount = 0;
  activePage = choosePage();
  int count = activePage >= 0 ? pages[activePage].count : 0;
  if (screenIndex > count) screenIndex = 0;
  gaugesStamp = fileStamp("/gauges.json");
  prefsStamp = fileStamp("/uiprefs.json");
  lastDraw = 0;
}

void begin(const Backend& backend) {
  io = backend;
  pinMode(ENCODER_A, INPUT_PULLUP);
  pinMode(ENCODER_B, INPUT_PULLUP);
  pinMode(ENCODER_BUTTON, INPUT_PULLUP);
  encoderState = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), encoderIsr, CHANGE);
  reload();
  draw();
}

void loop() {
  uint32_t now = millis();
  handleEncoder(encoderDelta());

  bool button = digitalRead(ENCODER_BUTTON);
  static uint32_t pressedAt = 0;
  if (button != lastButton) { lastButton = button; buttonChangedAt = now; }
  if (button != stableButton && now - buttonChangedAt >= 25) {
    stableButton = button;
    if (!button) pressedAt = now;
    else handlePress(now - pressedAt >= 800);
  }

  int nextPage = choosePage();
  if (nextPage != activePage && !editing && !confirming) {
    activePage = nextPage;
    if (activePage < 0 || screenIndex > pages[activePage].count) screenIndex = 0;
    lineHead = 0;
    lineFull = false;
  }

  if (now - lastConfigCheck >= 2500) {
    lastConfigCheck = now;
    uint32_t nextGauges = fileStamp("/gauges.json");
    uint32_t nextPrefs = fileStamp("/uiprefs.json");
    if (nextGauges != gaugesStamp || nextPrefs != prefsStamp) reload();
  }
  if (toast.length() && now >= toastUntil) toast = "";
  if (now - lastDraw >= 350) {
    lastDraw = now;
    draw();
  }
}

} // namespace TEmbedUi

#endif
