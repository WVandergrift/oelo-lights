// SPDX-License-Identifier: MIT
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "web_ui.h"

constexpr uint8_t kZoneCount = 6;
constexpr uint8_t kZonePins[kZoneCount] = {1, 2, 4, 5, 6, 7};
constexpr uint16_t kMaxLogicalLeds = 1000;
constexpr uint8_t kPhysicalPixelsPerFixture = 2;
constexpr uint16_t kMaxPhysicalPixels =
    kMaxLogicalLeds * kPhysicalPixelsPerFixture;
constexpr uint8_t kDefaultBrightness = 32;
constexpr uint8_t kMaxPatternColors = 128;
constexpr char kAccessPointName[] = "OELO_1-23.0";
const IPAddress kAccessPointIp(172, 24, 1, 1);
const IPAddress kAccessPointMask(255, 255, 255, 0);

struct ZoneConfig {
  bool enabled = false;
  uint16_t count = 1;
  String name;
  String order = "GBR";
};

enum class PatternKind : uint8_t {
  Off,
  Stationary,
  Arcade,
  Blend,
  Bolt,
  Chase,
  Fade,
  Fill,
  Lightning,
  March,
  River,
  Shuffle,
  Split,
  Sprinkle,
  Streak,
  Storm,
  Takeover,
  Twinkle,
};

struct PatternState {
  bool running = false;
  PatternKind kind = PatternKind::Off;
  String type = "off";
  uint8_t zoneMask = 0;
  uint8_t colorCount = 1;
  CRGB colors[kMaxPatternColors] = {CRGB::Black};
  uint8_t speed = 10;
  uint8_t gap = 0;
  uint8_t pause = 0;
  uint8_t other = 0;
  bool reverse = false;
  uint32_t startedAt = 0;
  uint32_t lastFrameAt = 0;
  uint32_t lastStepAt = 0;
  uint32_t nextEventAt = 0;
  uint32_t step = 0;
  uint8_t phase = 0;
};

CRGB zonePixels[kZoneCount][kMaxPhysicalPixels];
ZoneConfig zones[kZoneCount];
bool zoneRegistered[kZoneCount] = {};
uint8_t brightness = kDefaultBrightness;
PatternState activePattern;

Preferences preferences;
WebServer server(80);
String wifiSsid;
String wifiPassword;
String chipId;
uint32_t restartAt = 0;

const char kSeedPatterns[] PROGMEM = R"JSON([
  {
    "id": 1,
    "name": "Fourth of July: Fast Fireworks",
    "type": "twinkle",
    "num_colors": 6,
    "direction": "R",
    "speed": 10,
    "gap": 0,
    "pause": 0,
    "other": 0,
    "colors": "255,255,255,0,0,255,0,0,255,255,255,255,255,0,0,255,0,0,"
  }
])JSON";

String preferenceKey(const char* prefix, uint8_t zone) {
  return String(prefix) + zone;
}

bool validColorOrder(const String& value) {
  return value == "RGB" || value == "RBG" || value == "GRB" ||
         value == "GBR" || value == "BRG" || value == "BGR";
}

void loadConfiguration() {
  preferences.begin("leaflights", false);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  brightness = preferences.getUChar("brightness", kDefaultBrightness);

  for (uint8_t i = 0; i < kZoneCount; ++i) {
    const String countKey = preferenceKey("cnt", i);
    const String enabledKey = preferenceKey("en", i);
    const String nameKey = preferenceKey("name", i);
    const String orderKey = preferenceKey("ord", i);
    zones[i].count = constrain(
        preferences.getUShort(countKey.c_str(), 1), 1, kMaxLogicalLeds);
    zones[i].enabled = preferences.getBool(enabledKey.c_str(), i == 0);
    zones[i].name = preferences.getString(
        nameKey.c_str(), String("Zone ") + String(i + 1));
    zones[i].order = preferences.getString(orderKey.c_str(), "GBR");
    if (!validColorOrder(zones[i].order)) {
      zones[i].order = "GBR";
    }
  }
}

void saveZoneConfiguration() {
  for (uint8_t i = 0; i < kZoneCount; ++i) {
    preferences.putUShort(preferenceKey("cnt", i).c_str(), zones[i].count);
    preferences.putBool(preferenceKey("en", i).c_str(), zones[i].enabled);
    preferences.putString(preferenceKey("name", i).c_str(), zones[i].name);
    preferences.putString(preferenceKey("ord", i).c_str(), zones[i].order);
  }
}

template <uint8_t Pin>
void registerZoneForPin(uint8_t zone) {
  const uint16_t physicalCount =
      zones[zone].count * kPhysicalPixelsPerFixture;
  const String& order = zones[zone].order;

  if (order == "RGB") {
    FastLED.addLeds<UCS1903, Pin, RGB>(zonePixels[zone], physicalCount);
  } else if (order == "RBG") {
    FastLED.addLeds<UCS1903, Pin, RBG>(zonePixels[zone], physicalCount);
  } else if (order == "GRB") {
    FastLED.addLeds<UCS1903, Pin, GRB>(zonePixels[zone], physicalCount);
  } else if (order == "BRG") {
    FastLED.addLeds<UCS1903, Pin, BRG>(zonePixels[zone], physicalCount);
  } else if (order == "BGR") {
    FastLED.addLeds<UCS1903, Pin, BGR>(zonePixels[zone], physicalCount);
  } else {
    FastLED.addLeds<UCS1903, Pin, GBR>(zonePixels[zone], physicalCount);
  }
  zoneRegistered[zone] = true;
}

void registerZone(uint8_t zone) {
  if (!zones[zone].enabled) {
    return;
  }
  switch (zone) {
    case 0: registerZoneForPin<1>(zone); break;
    case 1: registerZoneForPin<2>(zone); break;
    case 2: registerZoneForPin<4>(zone); break;
    case 3: registerZoneForPin<5>(zone); break;
    case 4: registerZoneForPin<6>(zone); break;
    case 5: registerZoneForPin<7>(zone); break;
  }
}

void initializeLeds() {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    registerZone(zone);
  }
  FastLED.setBrightness(brightness);
  FastLED.clear(true);
}

void setLogicalFixture(uint8_t zone, uint16_t fixture, const CRGB& color) {
  if (zone >= kZoneCount || !zoneRegistered[zone] ||
      fixture >= zones[zone].count) {
    return;
  }
  const uint16_t physical = fixture * kPhysicalPixelsPerFixture;
  zonePixels[zone][physical] = color;
  zonePixels[zone][physical + 1] = color;
}

void fillZone(uint8_t zone, const CRGB& color) {
  if (zone >= kZoneCount || !zoneRegistered[zone]) {
    return;
  }
  fill_solid(zonePixels[zone],
             zones[zone].count * kPhysicalPixelsPerFixture, color);
}

void allOff() {
  activePattern.running = false;
  activePattern.kind = PatternKind::Off;
  activePattern.type = "off";
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    fillZone(zone, CRGB::Black);
  }
  FastLED.show();
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Cache-Control", "no-store");
}

void sendText(int status, const String& text,
              const char* contentType = "text/plain") {
  addCorsHeaders();
  server.send(status, contentType, text);
}

void scheduleRestart() {
  restartAt = millis() + 1500;
}

void sendControllerJson() {
  JsonDocument document;
  JsonArray array = document.to<JsonArray>();
  for (uint8_t i = 0; i < kZoneCount; ++i) {
    JsonObject zone = array.add<JsonObject>();
    zone["chipID"] = chipId;
    zone["fw"] = 1.78;
    zone["num"] = i;
    zone["name"] = zones[i].name;
    zone["enabled"] = zones[i].enabled;
    zone["ledCnt"] = zones[i].count;
    zone["rgbOrder"] = zones[i].order;
    zone["slaveTo"] = "none";
    zone["pattern"] = "off";
  }
  String response;
  serializeJson(document, response);
  sendText(200, response, "application/json");
}

void handleSaveController() {
  if (!server.hasArg("json")) {
    sendText(400, "Missing json parameter");
    return;
  }

  JsonDocument document;
  const DeserializationError error =
      deserializeJson(document, server.arg("json"));
  if (error || !document.is<JsonArray>()) {
    sendText(400, String("Invalid controller JSON: ") + error.c_str());
    return;
  }

  JsonArray array = document.as<JsonArray>();
  uint8_t i = 0;
  for (JsonObject input : array) {
    if (i >= kZoneCount) break;
    zones[i].enabled = input["enabled"] | zones[i].enabled;
    zones[i].count = constrain(input["ledCnt"] | zones[i].count,
                               1, kMaxLogicalLeds);
    const String name = input["name"] | zones[i].name;
    const String order = input["rgbOrder"] | zones[i].order;
    zones[i].name = name.substring(0, 24);
    zones[i].order = validColorOrder(order) ? order : "GBR";
    ++i;
  }
  saveZoneConfiguration();
  sendText(200, "Controller settings saved; rebooting");
  scheduleRestart();
}

uint8_t parsePalette(const String& encoded, CRGB* palette,
                     uint8_t maxColors) {
  int values[3] = {};
  uint8_t component = 0;
  uint8_t colorCount = 0;
  int start = 0;

  while (start < encoded.length() && colorCount < maxColors) {
    const int amp = encoded.indexOf('&', start);
    const int comma = encoded.indexOf(',', start);
    int end = encoded.length();
    if (amp >= 0 && amp < end) end = amp;
    if (comma >= 0 && comma < end) end = comma;
    values[component++] = constrain(encoded.substring(start, end).toInt(), 0, 255);
    if (component == 3) {
      palette[colorCount++] = CRGB(values[0], values[1], values[2]);
      component = 0;
    }
    if (end >= encoded.length()) break;
    start = end + 1;
  }
  return colorCount;
}

bool zoneSelected(uint8_t zone, const String& selectedZones) {
  if (selectedZones.isEmpty()) return zone == 0;
  int start = 0;
  while (start <= selectedZones.length()) {
    int end = selectedZones.indexOf(',', start);
    if (end < 0) end = selectedZones.length();
    if (selectedZones.substring(start, end).toInt() == zone) return true;
    if (end >= selectedZones.length()) break;
    start = end + 1;
  }
  return false;
}

uint8_t selectedZoneMask(const String& selectedZones) {
  uint8_t mask = 0;
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (zoneSelected(zone, selectedZones) && zoneRegistered[zone]) {
      mask |= 1U << zone;
    }
  }
  return mask;
}

PatternKind patternKindFromName(String type) {
  type.toLowerCase();
  if (type == "arcade" || type == "pacman") return PatternKind::Arcade;
  if (type == "blend") return PatternKind::Blend;
  if (type == "bolt") return PatternKind::Bolt;
  if (type == "chase") return PatternKind::Chase;
  if (type == "fade") return PatternKind::Fade;
  if (type == "fill") return PatternKind::Fill;
  if (type == "lightning") return PatternKind::Lightning;
  if (type == "march") return PatternKind::March;
  if (type == "river") return PatternKind::River;
  if (type == "shuffle") return PatternKind::Shuffle;
  if (type == "split") return PatternKind::Split;
  if (type == "sprinkle") return PatternKind::Sprinkle;
  if (type == "streak") return PatternKind::Streak;
  if (type == "storm") return PatternKind::Storm;
  if (type == "takeover") return PatternKind::Takeover;
  if (type == "twinkle") return PatternKind::Twinkle;
  if (type == "off") return PatternKind::Off;
  return PatternKind::Stationary;
}

uint16_t patternStepInterval() {
  const uint8_t speed = constrain(activePattern.speed, 1, 20);
  if (activePattern.kind == PatternKind::Twinkle ||
      activePattern.kind == PatternKind::Sprinkle) {
    return 270 - (speed * 11);  // 259 ms at 1, 50 ms at 20.
  }
  return 430 - (speed * 19);  // 411 ms at 1, 50 ms at 20.
}

uint16_t fixtureForDirection(uint8_t zone, uint16_t fixture) {
  return activePattern.reverse ? zones[zone].count - 1 - fixture : fixture;
}

void clearPatternZones() {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (activePattern.zoneMask & (1U << zone)) fillZone(zone, CRGB::Black);
  }
}

void fadePatternZones(uint8_t amount) {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    fadeToBlackBy(zonePixels[zone],
                  zones[zone].count * kPhysicalPixelsPerFixture, amount);
  }
}

CRGB patternColor(uint32_t index) {
  return activePattern.colors[index % activePattern.colorCount];
}

void renderStationary() {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    for (uint16_t fixture = 0; fixture < zones[zone].count; ++fixture) {
      setLogicalFixture(zone, fixture, patternColor(fixture));
    }
  }
}

void renderMarch(bool river) {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    const uint8_t group = river ? max<uint8_t>(activePattern.other, 2) : 1;
    for (uint16_t fixture = 0; fixture < zones[zone].count; ++fixture) {
      const uint16_t directed = fixtureForDirection(zone, fixture);
      const uint32_t paletteIndex =
          ((directed / group) + activePattern.step) % activePattern.colorCount;
      setLogicalFixture(zone, fixture, patternColor(paletteIndex));
    }
  }
}

void renderChase() {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    const uint8_t lit = max<uint8_t>(activePattern.other, 1);
    const uint8_t blank = max<uint8_t>(activePattern.gap, 3);
    const uint16_t period = lit + blank;
    for (uint16_t fixture = 0; fixture < zones[zone].count; ++fixture) {
      const uint16_t directed = fixtureForDirection(zone, fixture);
      const uint16_t position = (directed + activePattern.step) % period;
      const CRGB color = position < lit
                             ? patternColor((directed + activePattern.step) /
                                            period)
                             : CRGB::Black;
      setLogicalFixture(zone, fixture, color);
    }
  }
}

void renderSplit() {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    const uint16_t count = zones[zone].count;
    for (uint16_t fixture = 0; fixture < count; ++fixture) {
      const uint16_t distance = min<uint16_t>(fixture, count - 1 - fixture);
      setLogicalFixture(zone, fixture,
                        patternColor(distance + activePattern.step));
    }
  }
}

void renderBlend(uint32_t now) {
  const uint32_t duration = 1450 - activePattern.speed * 55UL;
  const uint32_t totalElapsed = now - activePattern.startedAt;
  const uint32_t elapsed = totalElapsed % duration;
  const uint8_t amount = (elapsed * 255UL) / duration;
  const uint8_t first = (totalElapsed / duration) % activePattern.colorCount;
  const uint8_t second = (first + 1) % activePattern.colorCount;
  const CRGB color = blend(activePattern.colors[first],
                           activePattern.colors[second], amount);
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (activePattern.zoneMask & (1U << zone)) fillZone(zone, color);
  }
}

void renderGradientBlend() {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    const uint16_t count = max<uint16_t>(zones[zone].count, 1);
    for (uint16_t fixture = 0; fixture < count; ++fixture) {
      const uint32_t scaled =
          (((uint32_t)fixture * activePattern.colorCount * 256UL) / count) +
          activePattern.step * 24UL;
      const uint8_t first = (scaled >> 8) % activePattern.colorCount;
      const uint8_t second = (first + 1) % activePattern.colorCount;
      const CRGB color = blend(activePattern.colors[first],
                               activePattern.colors[second], scaled & 0xff);
      setLogicalFixture(zone, fixture, color);
    }
  }
}

void renderFill(bool takeover) {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    const uint16_t count = zones[zone].count;
    const uint16_t position = activePattern.step % count;
    if (position == 0 && !takeover) fillZone(zone, CRGB::Black);
    const uint32_t cycle = activePattern.step / count;
    setLogicalFixture(zone, fixtureForDirection(zone, position),
                      patternColor(cycle));
  }
}

void renderShuffle() {
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    for (uint16_t fixture = 0; fixture < zones[zone].count; ++fixture) {
      setLogicalFixture(zone, fixture,
                        patternColor(random(activePattern.colorCount)));
    }
  }
}

void renderArcade() {
  clearPatternZones();
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    const uint16_t count = zones[zone].count;
    const uint16_t head = fixtureForDirection(zone, activePattern.step % count);
    setLogicalFixture(zone, head, patternColor(activePattern.step / count));
    const uint16_t tail = fixtureForDirection(zone,
        (activePattern.step + count - 1) % count);
    CRGB dim = patternColor(activePattern.step / count);
    dim.nscale8_video(80);
    setLogicalFixture(zone, tail, dim);
  }
}

void renderStreak(bool bolt) {
  fadePatternZones(bolt ? 72 : 38);
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    const uint16_t count = zones[zone].count;
    const uint16_t head = activePattern.step % count;
    const uint8_t length = bolt ? min<uint8_t>(activePattern.colorCount, 8) : 1;
    for (uint8_t tail = 0; tail < length; ++tail) {
      const uint16_t raw = (head + count - tail) % count;
      CRGB color = patternColor(tail);
      color.nscale8_video(255 - tail * (200 / max<uint8_t>(length, 1)));
      setLogicalFixture(zone, fixtureForDirection(zone, raw), color);
    }
  }
}

void renderSparkles(bool twinkle) {
  const uint8_t divisor = twinkle ? 55 : 90;
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    const uint8_t sparks = max<uint8_t>(1, zones[zone].count / divisor);
    for (uint8_t i = 0; i < sparks; ++i) {
      const uint16_t fixture = random(zones[zone].count);
      setLogicalFixture(zone, fixture,
                        patternColor(random(activePattern.colorCount)));
    }
  }
}

bool renderWeather(uint32_t now, bool storm) {
  if (static_cast<int32_t>(now - activePattern.nextEventAt) < 0) return false;
  if (activePattern.phase == 0) {
    const CRGB flash = storm ? CRGB(210, 220, 255)
                             : patternColor(random(activePattern.colorCount));
    for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
      if (activePattern.zoneMask & (1U << zone)) fillZone(zone, flash);
    }
    activePattern.phase = 1;
    activePattern.nextEventAt = now + random(35, 95);
  } else {
    clearPatternZones();
    activePattern.phase = 0;
    const uint16_t base = storm ? 450 : 180;
    activePattern.nextEventAt =
        now + random(base, base + patternStepInterval() * 3);
  }
  return true;
}

void startPattern(const String& requestedType, uint8_t zoneMask,
                  const CRGB* palette, uint8_t colorCount, uint8_t speed,
                  uint8_t gap, bool reverse, uint8_t pause, uint8_t other) {
  activePattern = PatternState();
  activePattern.type = requestedType;
  activePattern.type.toLowerCase();
  activePattern.kind = patternKindFromName(activePattern.type);
  activePattern.zoneMask = zoneMask;
  activePattern.colorCount = constrain(colorCount, 1, kMaxPatternColors);
  for (uint8_t i = 0; i < activePattern.colorCount; ++i) {
    activePattern.colors[i] = palette[i];
  }
  activePattern.speed = constrain(speed, 1, 20);
  activePattern.gap = gap;
  activePattern.pause = pause;
  activePattern.other = other;
  activePattern.reverse = reverse;
  activePattern.startedAt = millis();
  activePattern.lastFrameAt = 0;
  activePattern.lastStepAt = millis();
  activePattern.nextEventAt = millis();

  if (activePattern.kind == PatternKind::Off) {
    clearPatternZones();
    activePattern.running = false;
  } else if (activePattern.kind == PatternKind::Stationary) {
    renderStationary();
    activePattern.running = false;
  } else if (activePattern.kind == PatternKind::Blend) {
    renderGradientBlend();
    activePattern.running = true;
  } else {
    clearPatternZones();
    activePattern.running = true;
  }
  FastLED.show();
}

void updateActivePattern() {
  if (!activePattern.running || activePattern.zoneMask == 0) return;
  const uint32_t now = millis();
  if (now - activePattern.lastFrameAt < 35) return;
  activePattern.lastFrameAt = now;
  const bool stepDue = now - activePattern.lastStepAt >= patternStepInterval();
  bool changed = false;

  switch (activePattern.kind) {
    case PatternKind::Arcade:
      if (stepDue) { renderArcade(); changed = true; } break;
    case PatternKind::Blend:
      renderGradientBlend();
      if (stepDue) activePattern.step++;
      changed = stepDue;
      break;
    case PatternKind::Bolt:
      if (stepDue) { renderStreak(true); changed = true; } break;
    case PatternKind::Chase:
      if (stepDue) { renderChase(); changed = true; } break;
    case PatternKind::Fade:
      renderBlend(now); changed = true; break;
    case PatternKind::Fill:
      if (stepDue) { renderFill(false); changed = true; } break;
    case PatternKind::Lightning:
      changed = renderWeather(now, false); break;
    case PatternKind::March:
      if (stepDue) { renderMarch(false); changed = true; } break;
    case PatternKind::River:
      if (stepDue) { renderMarch(true); changed = true; } break;
    case PatternKind::Shuffle:
      if (stepDue) { renderShuffle(); changed = true; } break;
    case PatternKind::Split:
      if (stepDue) { renderSplit(); changed = true; } break;
    case PatternKind::Sprinkle:
      fadePatternZones(7);
      if (stepDue) renderSparkles(false);
      changed = true;
      break;
    case PatternKind::Streak:
      if (stepDue) { renderStreak(false); changed = true; } break;
    case PatternKind::Storm:
      changed = renderWeather(now, true); break;
    case PatternKind::Takeover:
      if (stepDue) { renderFill(true); changed = true; } break;
    case PatternKind::Twinkle:
      fadePatternZones(16);
      if (stepDue) renderSparkles(true);
      changed = true;
      break;
    default: break;
  }

  if (stepDue) {
    activePattern.step++;
    activePattern.lastStepAt = now;
  }
  if (changed) FastLED.show();
}

void handleSetPattern() {
  String patternType = server.arg("patternType");
  if (patternType.isEmpty()) patternType = "stationary";
  const String selectedZones = server.arg("zones");
  CRGB palette[kMaxPatternColors];
  uint8_t colorCount =
      parsePalette(server.arg("colors"), palette, kMaxPatternColors);
  if (colorCount == 0) {
    palette[0] = CRGB::Black;
    colorCount = 1;
  }
  const uint8_t speed = constrain(server.arg("speed").toInt(), 1, 20);
  const uint8_t gap = constrain(server.arg("gap").toInt(), 0, 255);
  const uint8_t pause = constrain(server.arg("pause").toInt(), 0, 255);
  const uint8_t other = constrain(server.arg("other").toInt(), 0, 255);
  const String direction = server.arg("direction");
  startPattern(patternType, selectedZoneMask(selectedZones), palette, colorCount,
               speed, gap, direction == "R", pause, other);
  sendText(200, patternType == "off" ? "off" : "pattern applied");
}

void startFastFireworks(uint8_t zoneMask) {
  const CRGB colors[] = {
      CRGB(255, 255, 255), CRGB(0, 0, 255), CRGB(0, 0, 255),
      CRGB(255, 255, 255), CRGB(255, 0, 0), CRGB(255, 0, 0),
  };
  startPattern("twinkle", zoneMask, colors, 6, 10, 0, true, 0, 0);
}

void sendStatusJson() {
  JsonDocument document;
  document["chipId"] = chipId;
  document["brightness"] = brightness;
  document["activePattern"] = activePattern.type;
  document["patternRunning"] = activePattern.running;
  JsonObject wifi = document["wifi"].to<JsonObject>();
  wifi["apSsid"] = kAccessPointName;
  wifi["apIp"] = WiFi.softAPIP().toString();
  wifi["ssid"] = wifiSsid;
  wifi["connected"] = WiFi.status() == WL_CONNECTED;
  wifi["lanIp"] = WiFi.status() == WL_CONNECTED
                        ? WiFi.localIP().toString()
                        : String();
  JsonArray outputZones = document["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < kZoneCount; ++i) {
    JsonObject zone = outputZones.add<JsonObject>();
    zone["enabled"] = zones[i].enabled;
    zone["name"] = zones[i].name;
    zone["count"] = zones[i].count;
    zone["order"] = zones[i].order;
    zone["gpio"] = kZonePins[i];
  }
  String response;
  serializeJson(document, response);
  sendText(200, response, "application/json");
}

void handleApiColor() {
  const int zone = server.arg("zone").toInt();
  if (zone < 0 || zone >= kZoneCount || !zoneRegistered[zone]) {
    sendText(400, "Zone is disabled or invalid");
    return;
  }
  const int requestedBrightness = server.arg("brightness").toInt();
  if (requestedBrightness >= 1 && requestedBrightness <= 255) {
    brightness = requestedBrightness;
    FastLED.setBrightness(brightness);
    preferences.putUChar("brightness", brightness);
  }
  const CRGB color(constrain(server.arg("r").toInt(), 0, 255),
                   constrain(server.arg("g").toInt(), 0, 255),
                   constrain(server.arg("b").toInt(), 0, 255));
  activePattern.running = false;
  activePattern.type = "stationary";
  fillZone(zone, color);
  FastLED.show();
  sendText(200, String("Zone ") + String(zone + 1) + " updated");
}

void handleWebZones() {
  for (uint8_t i = 0; i < kZoneCount; ++i) {
    zones[i].enabled = server.hasArg(preferenceKey("en", i));
    zones[i].count = constrain(
        server.arg(preferenceKey("cnt", i)).toInt(), 1, kMaxLogicalLeds);
    zones[i].name = server.arg(preferenceKey("name", i)).substring(0, 24);
    const String order = server.arg(preferenceKey("ord", i));
    zones[i].order = validColorOrder(order) ? order : "GBR";
  }
  saveZoneConfiguration();
  sendText(200, "Zone settings saved; rebooting in 1.5 seconds");
  scheduleRestart();
}

void saveNetwork(const String& ssid, const String& password) {
  wifiSsid = ssid;
  wifiPassword = password;
  preferences.putString("ssid", wifiSsid);
  preferences.putString("password", wifiPassword);
}

void handleWebNetwork() {
  if (!server.hasArg("ssid")) {
    sendText(400, "Missing Wi-Fi name");
    return;
  }
  saveNetwork(server.arg("ssid"), server.arg("password"));
  sendText(200, "Wi-Fi saved; rebooting in 1.5 seconds");
  scheduleRestart();
}

void handleScanNetworks() {
  const int count = WiFi.scanNetworks();
  String response;
  for (int i = 0; i < count; ++i) {
    if (!response.isEmpty()) response += ',';
    response += WiFi.SSID(i) + "***" + String(WiFi.RSSI(i));
  }
  WiFi.scanDelete();
  sendText(200, response);
}

void initializePatternStorage() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS initialization failed");
    return;
  }
  if (!LittleFS.exists("/patterns.json")) {
    File file = LittleFS.open("/patterns.json", "w");
    if (file) {
      file.print(kSeedPatterns);
      file.close();
    }
  }
}

void handleGetPatterns() {
  File file = LittleFS.open("/patterns.json", "r");
  if (!file) {
    sendText(200, "[]", "application/json");
    return;
  }
  addCorsHeaders();
  server.streamFile(file, "application/json");
  file.close();
}

void handleSavePatterns() {
  if (!server.hasArg("json")) {
    sendText(400, "Missing json parameter");
    return;
  }
  const String json = server.arg("json");
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, json);
  if (error || !document.is<JsonArray>()) {
    sendText(400, String("Invalid patterns JSON: ") + error.c_str());
    return;
  }
  File file = LittleFS.open("/patterns.json", "w");
  if (!file) {
    sendText(500, "Unable to open pattern storage");
    return;
  }
  file.print(json);
  file.close();
  sendText(200, "Patterns saved");
}

void configureWebServer() {
  server.on("/", HTTP_GET, []() {
    addCorsHeaders();
    server.send_P(200, "text/html", WEB_UI);
  });
  server.on("/getController", HTTP_GET, sendControllerJson);
  server.on("/saveController", HTTP_GET, handleSaveController);
  server.on("/setPattern", HTTP_GET, handleSetPattern);
  server.on("/getPatterns", HTTP_GET, handleGetPatterns);
  server.on("/savePatterns", HTTP_GET, handleSavePatterns);
  server.on("/scanNetworksRSSI", HTTP_GET, handleScanNetworks);
  server.on("/saveNetwork", HTTP_GET, []() {
    saveNetwork(server.arg("ssid"), server.arg("pw"));
    sendText(200, "Network saved; rebooting");
    scheduleRestart();
  });
  server.on("/api/status", HTTP_GET, sendStatusJson);
  server.on("/api/color", HTTP_GET, handleApiColor);
  server.on("/api/off", HTTP_GET, []() {
    allOff();
    sendText(200, "All zones off");
  });
  server.on("/api/preset/fast-fireworks", HTTP_GET, []() {
    uint8_t mask = 0;
    for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
      if (zoneRegistered[zone]) mask |= 1U << zone;
    }
    startFastFireworks(mask);
    sendText(200, "Fourth of July: Fast Fireworks running");
  });
  server.on("/api/zones", HTTP_POST, handleWebZones);
  server.on("/api/network", HTTP_POST, handleWebNetwork);
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      sendText(204, "");
    } else {
      server.sendHeader("Location", "/", true);
      sendText(302, "");
    }
  });
  server.begin();
}

void configureWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.softAPConfig(kAccessPointIp, kAccessPointIp, kAccessPointMask);
  WiFi.softAP(kAccessPointName);

  if (!wifiSsid.isEmpty()) {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    const uint32_t deadline = millis() + 10000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
      delay(100);
    }
  }

  if (WiFi.status() == WL_CONNECTED && MDNS.begin("leaflights")) {
    MDNS.addService("http", "tcp", 80);
  }
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  zone <0-5> <r> <g> <b>");
  Serial.println("  brightness <1-255>");
  Serial.println("  off");
  Serial.println("  status");
}

void handleSerialCommand(String line) {
  line.trim();
  int zone = 0, red = 0, green = 0, blue = 0, value = 0;
  if (line == "off") {
    allOff();
    Serial.println("All zones off");
  } else if (line == "help") {
    printHelp();
  } else if (line == "status") {
    Serial.printf("AP http://%s  LAN http://%s  leaflights.local\n",
                  WiFi.softAPIP().toString().c_str(),
                  WiFi.localIP().toString().c_str());
  } else if (sscanf(line.c_str(), "zone %d %d %d %d",
                    &zone, &red, &green, &blue) == 4) {
    if (zone < 0 || zone >= kZoneCount || !zoneRegistered[zone]) {
      Serial.println("Zone is disabled or invalid");
      return;
    }
    fillZone(zone, CRGB(constrain(red, 0, 255), constrain(green, 0, 255),
                        constrain(blue, 0, 255)));
    FastLED.show();
    Serial.println("Zone updated");
  } else if (sscanf(line.c_str(), "brightness %d", &value) == 1 &&
             value >= 1 && value <= 255) {
    brightness = value;
    FastLED.setBrightness(brightness);
    preferences.putUChar("brightness", brightness);
    FastLED.show();
    Serial.println("Brightness updated");
  } else {
    Serial.println("Unknown command; enter help");
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);
  const uint64_t mac = ESP.getEfuseMac();
  char chipBuffer[13];
  snprintf(chipBuffer, sizeof(chipBuffer), "%04X%08X",
           static_cast<uint16_t>(mac >> 32),
           static_cast<uint32_t>(mac));
  chipId = chipBuffer;

  loadConfiguration();
  initializePatternStorage();
  initializeLeds();
  configureWifi();
  configureWebServer();

  Serial.println("LeafFilter/Oelo UCS1903 test controller ready");
  Serial.printf("Setup AP: %s at http://%s\n", kAccessPointName,
                WiFi.softAPIP().toString().c_str());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("LAN: http://%s or http://leaflights.local\n",
                  WiFi.localIP().toString().c_str());
  }
  printHelp();
}

void loop() {
  server.handleClient();
  updateActivePattern();
  if (Serial.available()) {
    handleSerialCommand(Serial.readStringUntil('\n'));
  }
  if (restartAt != 0 && static_cast<int32_t>(millis() - restartAt) >= 0) {
    allOff();
    delay(50);
    ESP.restart();
  }
  delay(2);
}
