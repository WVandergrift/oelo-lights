// SPDX-License-Identifier: MIT
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <mbedtls/sha256.h>

#include "github_roots.h"
#include "web_ui.h"

constexpr uint8_t kZoneCount = 6;
constexpr uint8_t kZonePins[kZoneCount] = {1, 2, 4, 5, 6, 7};
constexpr uint16_t kMaxLogicalLeds = 1000;
constexpr uint8_t kPhysicalPixelsPerFixture = 2;
constexpr uint16_t kMaxPhysicalPixels =
    kMaxLogicalLeds * kPhysicalPixelsPerFixture;
constexpr uint8_t kDefaultBrightness = 32;
constexpr uint8_t kMaxPatternColors = 128;
constexpr uint16_t kPatternLibraryVersion = 1;
constexpr char kAccessPointName[] = "OELO_1-23.0";
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.5.1-dev"
#endif
constexpr char kFirmwareVersion[] = FIRMWARE_VERSION;
constexpr char kOtaUsername[] = "leaflights";
constexpr char kGithubApiUrl[] =
    "https://api.github.com/repos/WVandergrift/oelo-lights/releases?per_page=5";
constexpr char kGithubDownloadPrefix[] =
    "https://github.com/WVandergrift/oelo-lights/releases/download/";
constexpr char kReleaseAssetName[] = "leaf-lights-tinys3.bin";
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
  Fireworks,
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

struct WledSyncConfig {
  bool enabled = false;
  String destination = "255.255.255.255";
  uint16_t pixelCount = 300;
  int8_t sourceZone = -1;
};

struct FireworkBurst {
  bool active = false;
  uint16_t center = 0;
  uint8_t radius = 0;
  uint8_t paletteIndex = 0;
  uint32_t nextLaunchAt = 0;
};

struct GithubRelease {
  String tag;
  String name;
  String notes;
  String publishedAt;
  String downloadUrl;
  String digest;
  size_t size = 0;
  bool prerelease = false;
};

CRGB zonePixels[kZoneCount][kMaxPhysicalPixels];
ZoneConfig zones[kZoneCount];
bool zoneRegistered[kZoneCount] = {};
uint8_t brightness = kDefaultBrightness;
PatternState activePattern;
FireworkBurst fireworkBursts[kZoneCount];

Preferences preferences;
WebServer server(80);
WiFiUDP ddpUdp;
String wifiSsid;
String wifiPassword;
String chipId;
String otaPassword;
uint32_t restartAt = 0;
WledSyncConfig wledSync;
uint8_t ddpSequence = 1;
uint32_t lastDdpFrameAt = 0;
String lastDdpStatus = "Disabled";
bool otaUploadAuthorized = false;
bool otaUploadSucceeded = false;
String otaUploadError;
bool automaticUpdates = false;
uint32_t nextAutomaticUpdateAt = 0;
String automaticUpdateStatus = "Automatic updates disabled";

void sendDdpFrame(bool force = false);

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
  },
  {
    "id": 2,
    "name": "Liberty March",
    "type": "march",
    "num_colors": 24,
    "direction": "F",
    "speed": 18,
    "gap": 0,
    "pause": 0,
    "other": 0,
    "colors": "255,18,30,230,0,18,176,0,14,96,0,8,0,0,0,0,0,0,255,255,255,255,255,255,232,236,255,190,202,230,0,0,0,0,0,0,24,64,255,10,38,225,4,20,176,0,10,108,0,0,0,0,0,0,255,255,255,255,255,255,255,18,30,230,0,18,24,64,255,10,38,225,"
  },
  {
    "id": 3,
    "name": "Rocket's Red Glare",
    "type": "bolt",
    "num_colors": 8,
    "direction": "F",
    "speed": 20,
    "gap": 0,
    "pause": 0,
    "other": 0,
    "colors": "255,255,255,255,232,178,255,142,38,255,28,24,205,0,18,118,0,15,25,52,255,4,15,126,"
  },
  {
    "id": 4,
    "name": "Fifty Stars",
    "type": "sprinkle",
    "num_colors": 10,
    "direction": "R",
    "speed": 14,
    "gap": 0,
    "pause": 0,
    "other": 0,
    "colors": "255,255,255,255,255,255,218,232,255,255,255,255,34,82,255,18,48,220,6,18,130,255,28,34,210,0,22,255,255,255,"
  },
  {
    "id": 5,
    "name": "Freedom River",
    "type": "river",
    "num_colors": 9,
    "direction": "R",
    "speed": 15,
    "gap": 0,
    "pause": 0,
    "other": 4,
    "colors": "255,20,32,178,0,18,255,255,255,220,232,255,255,255,255,30,72,255,8,34,205,2,12,112,255,255,255,"
  },
  {
    "id": 6,
    "name": "Grand Finale Fireworks",
    "type": "fireworks",
    "num_colors": 9,
    "direction": "F",
    "speed": 19,
    "gap": 0,
    "pause": 0,
    "other": 0,
    "colors": "255,255,255,255,255,255,255,220,155,255,28,32,220,0,20,255,255,255,35,75,255,8,26,200,255,255,255,"
  },
  {
    "id": 7,
    "name": "American Wave",
    "type": "blend",
    "num_colors": 12,
    "direction": "F",
    "speed": 13,
    "gap": 0,
    "pause": 0,
    "other": 0,
    "colors": "118,0,14,205,0,22,255,28,36,255,160,150,255,255,255,220,235,255,112,160,255,30,82,255,8,34,205,2,12,112,30,82,255,220,235,255,"
  },
  {
    "id": 8,
    "name": "Stars & Stripes Chase",
    "type": "chase",
    "num_colors": 6,
    "direction": "F",
    "speed": 17,
    "gap": 3,
    "pause": 0,
    "other": 5,
    "colors": "255,22,34,214,0,22,255,255,255,230,238,255,28,68,255,6,24,178,"
  },
  {
    "id": 9,
    "name": "United We Split",
    "type": "split",
    "num_colors": 11,
    "direction": "F",
    "speed": 16,
    "gap": 0,
    "pause": 0,
    "other": 0,
    "colors": "255,20,32,190,0,20,80,0,10,0,0,0,255,255,255,225,235,255,0,0,0,5,20,120,18,52,225,35,80,255,255,255,255,"
  },
  {
    "id": 10,
    "name": "Dawn's Early Light",
    "type": "fade",
    "num_colors": 8,
    "direction": "F",
    "speed": 6,
    "gap": 0,
    "pause": 0,
    "other": 0,
    "colors": "1,8,60,8,30,145,30,78,255,198,220,255,255,248,225,255,142,116,235,24,34,125,0,16,"
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
  otaPassword = preferences.getString("otaPassword", "");
  automaticUpdates = preferences.getBool("autoUpdate", false);
  brightness = preferences.getUChar("brightness", kDefaultBrightness);
  wledSync.enabled = preferences.getBool("ddpEnabled", false);
  wledSync.destination =
      preferences.getString("ddpDest", "255.255.255.255");
  wledSync.pixelCount = constrain(
      preferences.getUShort("ddpPixels", 300), 1, kMaxLogicalLeds);
  wledSync.sourceZone = constrain(
      static_cast<int>(preferences.getChar("ddpSource", -1)), -1,
      static_cast<int>(kZoneCount - 1));

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
  sendDdpFrame(true);
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers",
                    "Content-Type, Authorization");
  server.sendHeader("Cache-Control", "no-store");
}

void sendText(int status, const String& text,
              const char* contentType = "text/plain") {
  addCorsHeaders();
  server.send(status, contentType, text);
}

bool otaAuthenticated() {
  return !otaPassword.isEmpty() &&
         server.authenticate(kOtaUsername, otaPassword.c_str());
}

void sendOtaUnauthorized() {
  server.sendHeader("WWW-Authenticate",
                    "Basic realm=\"Leaf Lights firmware update\"");
  sendText(401, "Firmware update password required");
}

bool validOtaPassword(const String& password) {
  if (password.length() < 10 || password.length() > 64) return false;
  for (size_t i = 0; i < password.length(); ++i) {
    const char value = password[i];
    if (value < 33 || value > 126 || value == ':') return false;
  }
  return true;
}

void scheduleRestart() {
  restartAt = millis() + 1500;
}

bool ensureGithubClock(String& error) {
  if (WiFi.status() != WL_CONNECTED) {
    error = "Connect the controller to home Wi-Fi first";
    return false;
  }
  time_t now = time(nullptr);
  if (now > 1700000000) return true;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  const uint32_t deadline = millis() + 8000;
  while (time(nullptr) <= 1700000000 && millis() < deadline) delay(100);
  if (time(nullptr) <= 1700000000) {
    error = "Unable to set the clock for secure GitHub access";
    return false;
  }
  return true;
}

bool beginGithubRequest(HTTPClient& http, WiFiClientSecure& client,
                        const String& url, String& error) {
  if (!ensureGithubClock(error)) return false;
  client.setCACert(GITHUB_ROOT_CA);
  client.setHandshakeTimeout(15);
  client.setTimeout(15);
  http.setConnectTimeout(10000);
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    error = "Unable to initialize the GitHub HTTPS request";
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("User-Agent", "WVandergrift-oelo-lights/" +
                                   String(kFirmwareVersion));
  http.addHeader("X-GitHub-Api-Version", "2026-03-10");
  return true;
}

void parseVersion(const String& value, int parts[3]) {
  parts[0] = parts[1] = parts[2] = 0;
  uint8_t part = 0;
  bool reading = false;
  for (size_t i = 0; i < value.length() && part < 3; ++i) {
    const char character = value[i];
    if (character >= '0' && character <= '9') {
      parts[part] = parts[part] * 10 + character - '0';
      reading = true;
    } else if (reading) {
      ++part;
      reading = false;
    }
  }
}

int compareVersions(const String& first, const String& second) {
  int firstParts[3], secondParts[3];
  parseVersion(first, firstParts);
  parseVersion(second, secondParts);
  for (uint8_t i = 0; i < 3; ++i) {
    if (firstParts[i] != secondParts[i]) {
      return firstParts[i] > secondParts[i] ? 1 : -1;
    }
  }
  return 0;
}

uint8_t fetchGithubReleases(GithubRelease* output, uint8_t capacity,
                            String& error) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginGithubRequest(http, client, kGithubApiUrl, error)) return 0;
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    error = status > 0 ? String("GitHub API returned HTTP ") + status
                       : String("GitHub connection failed: ") +
                             HTTPClient::errorToString(status);
    http.end();
    return 0;
  }

  JsonDocument filter;
  JsonObject releaseFilter = filter[0].to<JsonObject>();
  releaseFilter["tag_name"] = true;
  releaseFilter["name"] = true;
  releaseFilter["body"] = true;
  releaseFilter["published_at"] = true;
  releaseFilter["draft"] = true;
  releaseFilter["prerelease"] = true;
  JsonObject assetFilter = releaseFilter["assets"][0].to<JsonObject>();
  assetFilter["name"] = true;
  assetFilter["browser_download_url"] = true;
  assetFilter["size"] = true;
  assetFilter["digest"] = true;

  JsonDocument document;
  const DeserializationError parseError = deserializeJson(
      document, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (parseError || !document.is<JsonArray>()) {
    error = String("Unable to parse GitHub releases: ") + parseError.c_str();
    return 0;
  }

  uint8_t count = 0;
  for (JsonObject release : document.as<JsonArray>()) {
    if (count >= capacity) break;
    if (release["draft"] | false) continue;
    for (JsonObject asset : release["assets"].as<JsonArray>()) {
      if (String(asset["name"] | "") != kReleaseAssetName) continue;
      const String digest = asset["digest"] | "";
      const String url = asset["browser_download_url"] | "";
      if (!digest.startsWith("sha256:") || digest.length() != 71 ||
          !url.startsWith(kGithubDownloadPrefix)) {
        continue;
      }
      GithubRelease& item = output[count++];
      item.tag = String(release["tag_name"] | "").substring(0, 32);
      item.name = String(release["name"] | item.tag).substring(0, 80);
      item.notes = String(release["body"] | "").substring(0, 6000);
      item.publishedAt =
          String(release["published_at"] | "").substring(0, 32);
      item.downloadUrl = url;
      item.digest = digest;
      item.size = asset["size"] | 0;
      item.prerelease = release["prerelease"] | false;
      break;
    }
  }
  if (count == 0) error = "No compatible firmware releases were found";
  return count;
}

bool installGithubRelease(const GithubRelease& release, String& error) {
  if (!release.downloadUrl.startsWith(kGithubDownloadPrefix) ||
      !release.digest.startsWith("sha256:") ||
      release.digest.length() != 71 || release.size == 0) {
    error = "Release metadata failed validation";
    return false;
  }
  if (release.size > ESP.getFreeSketchSpace()) {
    error = "Release image is larger than the inactive firmware slot";
    return false;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginGithubRequest(http, client, release.downloadUrl, error)) {
    return false;
  }
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    error = status > 0 ? String("Firmware download returned HTTP ") + status
                       : String("Firmware download failed: ") +
                             HTTPClient::errorToString(status);
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength > 0 && static_cast<size_t>(contentLength) != release.size) {
    error = "Downloaded firmware size does not match the GitHub release";
    http.end();
    return false;
  }

  allOff();
  if (!Update.begin(release.size, U_FLASH)) {
    error = Update.errorString();
    http.end();
    return false;
  }

  mbedtls_sha256_context hashContext;
  mbedtls_sha256_init(&hashContext);
  mbedtls_sha256_starts_ret(&hashContext, 0);
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[4096];
  size_t written = 0;
  uint32_t lastDataAt = millis();
  while (written < release.size) {
    const size_t available = stream->available();
    if (available > 0) {
      const size_t wanted = min<size_t>(sizeof(buffer),
          min<size_t>(available, release.size - written));
      const size_t received = stream->readBytes(buffer, wanted);
      if (received == 0) continue;
      if (Update.write(buffer, received) != received) {
        error = Update.errorString();
        break;
      }
      mbedtls_sha256_update_ret(&hashContext, buffer, received);
      written += received;
      lastDataAt = millis();
    } else {
      if (!http.connected() || millis() - lastDataAt > 20000) {
        error = "Firmware download ended before the complete image arrived";
        break;
      }
      delay(1);
    }
  }

  uint8_t hash[32];
  mbedtls_sha256_finish_ret(&hashContext, hash);
  mbedtls_sha256_free(&hashContext);
  http.end();
  if (written != release.size || !error.isEmpty()) {
    Update.abort();
    return false;
  }

  char hashHex[65];
  for (uint8_t i = 0; i < 32; ++i) {
    snprintf(hashHex + i * 2, 3, "%02x", hash[i]);
  }
  hashHex[64] = '\0';
  String expected = release.digest.substring(7);
  expected.toLowerCase();
  if (expected != hashHex) {
    Update.abort();
    error = "Firmware SHA-256 does not match the GitHub release";
    return false;
  }
  if (!Update.end(true)) {
    error = Update.errorString();
    return false;
  }
  return true;
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

int8_t ddpSourceZone() {
  if (wledSync.sourceZone >= 0 && wledSync.sourceZone < kZoneCount &&
      zoneRegistered[wledSync.sourceZone]) {
    return wledSync.sourceZone;
  }
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if ((activePattern.zoneMask & (1U << zone)) && zoneRegistered[zone]) {
      return zone;
    }
  }
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (zoneRegistered[zone]) return zone;
  }
  return -1;
}

bool resolveDdpDestination(IPAddress& address) {
  if (address.fromString(wledSync.destination)) return true;
  return WiFi.hostByName(wledSync.destination.c_str(), address) == 1;
}

void sendDdpFrame(bool force) {
  if (!wledSync.enabled) {
    lastDdpStatus = "Disabled";
    return;
  }
  const uint32_t now = millis();
  if (!force && now - lastDdpFrameAt < 30) return;

  IPAddress destination;
  if (!resolveDdpDestination(destination)) {
    lastDdpStatus = "Invalid destination";
    return;
  }
  const int8_t source = ddpSourceZone();
  if (source < 0) {
    lastDdpStatus = "No enabled source zone";
    return;
  }

  constexpr uint16_t kDdpPort = 4048;
  constexpr uint16_t kDdpChannelsPerPacket = 1440;
  constexpr uint16_t kDdpPixelsPerPacket = kDdpChannelsPerPacket / 3;
  const uint16_t outputCount =
      constrain(wledSync.pixelCount, 1, kMaxLogicalLeds);
  const uint16_t sourceCount = max<uint16_t>(zones[source].count, 1);

  for (uint16_t firstPixel = 0; firstPixel < outputCount;
       firstPixel += kDdpPixelsPerPacket) {
    const uint16_t packetPixels =
        min<uint16_t>(kDdpPixelsPerPacket, outputCount - firstPixel);
    const uint16_t dataLength = packetPixels * 3;
    const uint32_t channelOffset = static_cast<uint32_t>(firstPixel) * 3;
    const bool push = firstPixel + packetPixels >= outputCount;

    if (!ddpUdp.beginPacket(destination, kDdpPort)) {
      lastDdpStatus = "UDP send failed";
      return;
    }
    ddpUdp.write(static_cast<uint8_t>(0x40 | (push ? 0x01 : 0x00)));
    ddpUdp.write(ddpSequence++ & 0x0f);
    if ((ddpSequence & 0x0f) == 0) ++ddpSequence;
    ddpUdp.write(static_cast<uint8_t>(0x0b));  // RGB, 8 bits/channel.
    ddpUdp.write(static_cast<uint8_t>(0x01));  // Default display output.
    ddpUdp.write(static_cast<uint8_t>(channelOffset >> 24));
    ddpUdp.write(static_cast<uint8_t>(channelOffset >> 16));
    ddpUdp.write(static_cast<uint8_t>(channelOffset >> 8));
    ddpUdp.write(static_cast<uint8_t>(channelOffset));
    ddpUdp.write(static_cast<uint8_t>(dataLength >> 8));
    ddpUdp.write(static_cast<uint8_t>(dataLength));

    for (uint16_t pixel = firstPixel;
         pixel < firstPixel + packetPixels; ++pixel) {
      const uint16_t sourceFixture =
          min<uint16_t>((static_cast<uint32_t>(pixel) * sourceCount) /
                            outputCount,
                        sourceCount - 1);
      const CRGB& color =
          zonePixels[source][sourceFixture * kPhysicalPixelsPerFixture];
      ddpUdp.write(scale8_video(color.r, brightness));
      ddpUdp.write(scale8_video(color.g, brightness));
      ddpUdp.write(scale8_video(color.b, brightness));
    }
    if (!ddpUdp.endPacket()) {
      lastDdpStatus = "UDP send failed";
      return;
    }
  }
  lastDdpFrameAt = now;
  lastDdpStatus = String("Streaming ") + outputCount + " pixels";
}

void serviceDdpSync() {
  if (wledSync.enabled && millis() - lastDdpFrameAt >= 750) {
    sendDdpFrame(true);
  }
}

PatternKind patternKindFromName(String type) {
  type.toLowerCase();
  if (type == "arcade" || type == "pacman") return PatternKind::Arcade;
  if (type == "blend") return PatternKind::Blend;
  if (type == "bolt") return PatternKind::Bolt;
  if (type == "chase") return PatternKind::Chase;
  if (type == "fade") return PatternKind::Fade;
  if (type == "fill") return PatternKind::Fill;
  if (type == "fireworks") return PatternKind::Fireworks;
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
  if (activePattern.kind == PatternKind::March) {
    return 2100 - (speed * 100);  // Recovered vendor scale: speed 20 = 100 ms.
  }
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

void renderFireworks(uint32_t now, bool advance) {
  fadePatternZones(12);
  if (!advance) return;

  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(activePattern.zoneMask & (1U << zone))) continue;
    FireworkBurst& burst = fireworkBursts[zone];
    const uint16_t count = zones[zone].count;
    if (!burst.active) {
      if (static_cast<int32_t>(now - burst.nextLaunchAt) < 0) continue;
      burst.active = true;
      burst.center = random(count);
      burst.radius = 0;
      burst.paletteIndex = random(activePattern.colorCount);
    }

    const uint8_t maximumRadius =
        min<uint16_t>(18, max<uint16_t>(count / 10, 5));
    const uint8_t intensity =
        255 - (burst.radius * 190U / maximumRadius);
    CRGB spark = patternColor(burst.paletteIndex);
    spark.nscale8_video(intensity);
    CRGB hotSpark = blend(spark, CRGB::White, burst.radius < 2 ? 180 : 35);

    auto place = [&](int32_t fixture, const CRGB& color) {
      if (fixture >= 0 && fixture < count) {
        setLogicalFixture(zone, fixture, color);
      }
    };
    if (burst.radius == 0) {
      place(burst.center, hotSpark);
    } else {
      place(static_cast<int32_t>(burst.center) - burst.radius, hotSpark);
      place(static_cast<int32_t>(burst.center) + burst.radius, hotSpark);

      CRGB ember = spark;
      ember.nscale8_video(150);
      const uint8_t innerRadius = max<uint8_t>(1, burst.radius / 2);
      place(static_cast<int32_t>(burst.center) - innerRadius, ember);
      place(static_cast<int32_t>(burst.center) + innerRadius, ember);
      if ((burst.radius & 1U) == 0) place(burst.center, ember);
    }

    if (++burst.radius > maximumRadius) {
      burst.active = false;
      burst.nextLaunchAt = now + random(180, 850);
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
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    fireworkBursts[zone] = FireworkBurst();
    fireworkBursts[zone].nextLaunchAt = millis() + zone * 135UL;
  }

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
  sendDdpFrame(true);
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
    case PatternKind::Fireworks:
      renderFireworks(now, stepDue); changed = true; break;
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
  if (changed) {
    FastLED.show();
    sendDdpFrame();
  }
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
  document["firmwareVersion"] = kFirmwareVersion;
  document["buildDate"] = __DATE__ " " __TIME__;
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
  JsonObject sync = document["wledSync"].to<JsonObject>();
  sync["enabled"] = wledSync.enabled;
  sync["destination"] = wledSync.destination;
  sync["pixelCount"] = wledSync.pixelCount;
  sync["sourceZone"] = wledSync.sourceZone;
  sync["status"] = lastDdpStatus;
  JsonObject ota = document["ota"].to<JsonObject>();
  ota["configured"] = !otaPassword.isEmpty();
  ota["maxImageBytes"] = ESP.getFreeSketchSpace();
  ota["automaticUpdates"] = automaticUpdates;
  ota["automaticUpdateStatus"] = automaticUpdateStatus;
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
  startPattern("stationary", 1U << zone, &color, 1, 10, 0, false, 0, 0);
  sendText(200, String("Zone ") + String(zone + 1) + " updated");
}

void handleBrightness() {
  const int requested = server.arg("value").toInt();
  if (requested < 1 || requested > 255) {
    sendText(400, "Brightness must be between 1 and 255");
    return;
  }
  brightness = requested;
  FastLED.setBrightness(brightness);
  preferences.putUChar("brightness", brightness);
  FastLED.show();
  sendDdpFrame(true);
  sendText(200, "Brightness updated");
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

void handleWledSyncConfiguration() {
  const String enabled = server.arg("enabled");
  wledSync.enabled = enabled == "1" || enabled == "true" || enabled == "on";
  String destination = server.arg("destination");
  destination.trim();
  if (destination.isEmpty()) destination = "255.255.255.255";
  if (destination.length() > 64 || destination.indexOf("://") >= 0 ||
      destination.indexOf(' ') >= 0) {
    sendText(400, "Enter an IP address or hostname without http://");
    return;
  }
  const int pixels = server.arg("pixelCount").toInt();
  if (pixels < 1 || pixels > kMaxLogicalLeds) {
    sendText(400, "WLED pixel count must be between 1 and 1000");
    return;
  }
  const int source = server.arg("sourceZone").toInt();
  if (source < -1 || source >= kZoneCount) {
    sendText(400, "Invalid WLED source zone");
    return;
  }

  wledSync.destination = destination;
  wledSync.pixelCount = pixels;
  wledSync.sourceZone = source;
  preferences.putBool("ddpEnabled", wledSync.enabled);
  preferences.putString("ddpDest", wledSync.destination);
  preferences.putUShort("ddpPixels", wledSync.pixelCount);
  preferences.putChar("ddpSource", wledSync.sourceZone);

  if (wledSync.enabled) {
    sendDdpFrame(true);
  } else {
    lastDdpStatus = "Disabled";
  }
  sendText(200, wledSync.enabled ? "WLED realtime sync enabled"
                                 : "WLED realtime sync disabled");
}

void handleOtaPasswordConfiguration() {
  if (!otaPassword.isEmpty() && !otaAuthenticated()) {
    sendOtaUnauthorized();
    return;
  }
  const String newPassword = server.arg("newPassword");
  if (!validOtaPassword(newPassword)) {
    sendText(400,
             "Use 10-64 printable ASCII characters without spaces or colons");
    return;
  }
  otaPassword = newPassword;
  preferences.putString("otaPassword", otaPassword);
  sendText(200, "Firmware update password saved");
}

void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUploadAuthorized = otaAuthenticated();
    otaUploadSucceeded = false;
    otaUploadError = "";
    if (!otaUploadAuthorized) {
      otaUploadError = "Firmware update password required";
      return;
    }
    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".bin")) {
      otaUploadError = "Select a compiled .bin firmware image";
      return;
    }
    allOff();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      otaUploadError = Update.errorString();
    }
  } else if (!otaUploadAuthorized || !otaUploadError.isEmpty()) {
    return;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      const String error = Update.errorString();
      Update.abort();
      otaUploadError = error;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      otaUploadSucceeded = true;
    } else {
      otaUploadError = Update.errorString();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaUploadError = "Firmware upload was interrupted";
  }
}

void handleFirmwareUploadComplete() {
  if (!otaAuthenticated()) {
    sendOtaUnauthorized();
    return;
  }
  server.sendHeader("Connection", "close");
  if (!otaUploadSucceeded) {
    sendText(500, otaUploadError.isEmpty() ? "Firmware update failed"
                                           : otaUploadError);
    return;
  }
  sendText(200, "Firmware verified; rebooting into the update");
  scheduleRestart();
}

void handleGithubReleases() {
  GithubRelease releases[5];
  String error;
  const uint8_t count = fetchGithubReleases(releases, 5, error);
  if (count == 0) {
    sendText(502, error);
    return;
  }
  JsonDocument document;
  document["currentVersion"] = kFirmwareVersion;
  JsonArray output = document["releases"].to<JsonArray>();
  for (uint8_t i = 0; i < count; ++i) {
    JsonObject release = output.add<JsonObject>();
    release["tag"] = releases[i].tag;
    release["name"] = releases[i].name;
    release["notes"] = releases[i].notes;
    release["publishedAt"] = releases[i].publishedAt;
    release["url"] = releases[i].downloadUrl;
    release["digest"] = releases[i].digest;
    release["size"] = releases[i].size;
    release["prerelease"] = releases[i].prerelease;
    release["newer"] =
        compareVersions(releases[i].tag, kFirmwareVersion) > 0;
  }
  String response;
  serializeJson(document, response);
  sendText(200, response, "application/json");
}

void handleInstallGithubRelease() {
  if (!otaAuthenticated()) {
    sendOtaUnauthorized();
    return;
  }
  GithubRelease release;
  release.tag = server.arg("tag").substring(0, 32);
  release.name = release.tag;
  release.downloadUrl = server.arg("url");
  release.digest = server.arg("digest");
  release.size = strtoul(server.arg("size").c_str(), nullptr, 10);
  String error;
  if (!installGithubRelease(release, error)) {
    sendText(502, error);
    return;
  }
  sendText(200, String("Release ") + release.tag +
                    " verified; rebooting into the update");
  scheduleRestart();
}

void handleAutomaticUpdateConfiguration() {
  if (!otaAuthenticated()) {
    sendOtaUnauthorized();
    return;
  }
  const String enabled = server.arg("enabled");
  automaticUpdates = enabled == "1" || enabled == "true" || enabled == "on";
  preferences.putBool("autoUpdate", automaticUpdates);
  nextAutomaticUpdateAt = millis() + 15000;
  automaticUpdateStatus = automaticUpdates
                              ? "Enabled; checking stable releases soon"
                              : "Automatic updates disabled";
  sendText(200, automaticUpdateStatus);
}

void serviceAutomaticUpdates() {
  if (!automaticUpdates || WiFi.status() != WL_CONNECTED) return;
  if (static_cast<int32_t>(millis() - nextAutomaticUpdateAt) < 0) return;
  nextAutomaticUpdateAt = millis() + 6UL * 60UL * 60UL * 1000UL;

  automaticUpdateStatus = "Checking GitHub releases";
  GithubRelease releases[5];
  String error;
  const uint8_t count = fetchGithubReleases(releases, 5, error);
  if (count == 0) {
    automaticUpdateStatus = String("Check failed: ") + error;
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    if (releases[i].prerelease ||
        compareVersions(releases[i].tag, kFirmwareVersion) <= 0) {
      continue;
    }
    automaticUpdateStatus = String("Installing ") + releases[i].tag;
    if (!installGithubRelease(releases[i], error)) {
      automaticUpdateStatus = String("Install failed: ") + error;
      return;
    }
    delay(100);
    ESP.restart();
  }
  automaticUpdateStatus = "Up to date";
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
      preferences.putUShort("patternLibrary", kPatternLibraryVersion);
    }
    return;
  }

  if (preferences.getUShort("patternLibrary", 0) >=
      kPatternLibraryVersion) {
    return;
  }

  File file = LittleFS.open("/patterns.json", "r");
  JsonDocument savedDocument;
  const DeserializationError savedError = deserializeJson(savedDocument, file);
  file.close();
  if (savedError || !savedDocument.is<JsonArray>()) {
    Serial.printf("Unable to merge built-in patterns: %s\n",
                  savedError.c_str());
    return;
  }

  JsonDocument libraryDocument;
  const DeserializationError libraryError =
      deserializeJson(libraryDocument, kSeedPatterns);
  if (libraryError || !libraryDocument.is<JsonArray>()) {
    Serial.printf("Built-in pattern library is invalid: %s\n",
                  libraryError.c_str());
    return;
  }

  JsonArray savedPatterns = savedDocument.as<JsonArray>();
  uint32_t nextId = 0;
  for (JsonObjectConst pattern : savedPatterns) {
    nextId = max(nextId, pattern["id"] | 0U);
  }

  bool changed = false;
  for (JsonObjectConst builtIn : libraryDocument.as<JsonArrayConst>()) {
    const String name = builtIn["name"] | "";
    bool exists = false;
    for (JsonObjectConst saved : savedPatterns) {
      if (name == String(saved["name"] | "")) {
        exists = true;
        break;
      }
    }
    if (exists) continue;

    JsonObject added = savedPatterns.add<JsonObject>();
    added.set(builtIn);
    added["id"] = ++nextId;
    changed = true;
  }

  if (changed) {
    File temporary = LittleFS.open("/patterns.tmp", "w");
    if (!temporary || serializeJson(savedDocument, temporary) == 0) {
      if (temporary) temporary.close();
      LittleFS.remove("/patterns.tmp");
      Serial.println("Unable to write merged built-in patterns");
      return;
    }
    temporary.close();

    LittleFS.remove("/patterns.backup");
    if (!LittleFS.rename("/patterns.json", "/patterns.backup")) {
      LittleFS.remove("/patterns.tmp");
      Serial.println("Unable to back up saved patterns");
      return;
    }
    if (!LittleFS.rename("/patterns.tmp", "/patterns.json")) {
      LittleFS.remove("/patterns.json");
      LittleFS.rename("/patterns.backup", "/patterns.json");
      LittleFS.remove("/patterns.tmp");
      Serial.println("Unable to activate merged built-in patterns");
      return;
    }
    LittleFS.remove("/patterns.backup");
  }

  preferences.putUShort("patternLibrary", kPatternLibraryVersion);
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

void savePatternsJson(const String& json) {
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

void handleSavePatterns() {
  if (!server.hasArg("json")) {
    sendText(400, "Missing json parameter");
    return;
  }
  savePatternsJson(server.arg("json"));
}

void handleSavePatternsBody() {
  if (!server.hasArg("plain")) {
    sendText(400, "Missing JSON body");
    return;
  }
  savePatternsJson(server.arg("plain"));
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
  server.on("/api/patterns", HTTP_POST, handleSavePatternsBody);
  server.on("/scanNetworksRSSI", HTTP_GET, handleScanNetworks);
  server.on("/saveNetwork", HTTP_GET, []() {
    saveNetwork(server.arg("ssid"), server.arg("pw"));
    sendText(200, "Network saved; rebooting");
    scheduleRestart();
  });
  server.on("/api/status", HTTP_GET, sendStatusJson);
  server.on("/api/color", HTTP_GET, handleApiColor);
  server.on("/api/brightness", HTTP_GET, handleBrightness);
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
  server.on("/api/wled-sync", HTTP_POST, handleWledSyncConfiguration);
  server.on("/api/update-password", HTTP_POST,
            handleOtaPasswordConfiguration);
  server.on("/api/update", HTTP_POST, handleFirmwareUploadComplete,
            handleFirmwareUpload);
  server.on("/api/releases", HTTP_GET, handleGithubReleases);
  server.on("/api/install-release", HTTP_POST,
            handleInstallGithubRelease);
  server.on("/api/automatic-updates", HTTP_POST,
            handleAutomaticUpdateConfiguration);
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
    if (WiFi.status() != WL_CONNECTED) {
      // Stop background station scans so the compatibility AP remains stable
      // when saved home-network credentials are stale or out of range. The
      // credentials stay in our NVS and will be tried again after a reboot.
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false, false);
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
  nextAutomaticUpdateAt = millis() + 60000;
  automaticUpdateStatus = automaticUpdates
                              ? "Enabled; first check in one minute"
                              : "Automatic updates disabled";

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
  serviceDdpSync();
  serviceAutomaticUpdates();
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
