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
#include <math.h>
#include <time.h>

#include "github_roots.h"
#include "web_ui.h"

constexpr uint8_t kZoneCount = 6;
constexpr uint8_t kZonePins[kZoneCount] = {1, 2, 4, 5, 6, 7};
constexpr uint16_t kMaxLogicalLeds = 1000;
constexpr uint8_t kPhysicalPixelsPerFixture = 2;
constexpr uint16_t kMaxPhysicalPixels =
    kMaxLogicalLeds * kPhysicalPixelsPerFixture;
constexpr uint8_t kDefaultBrightness = 32;
// The installed 36 V, 5.6 A supply is capped at 80% of nameplate output.
// This is a conservative global output cap, not active current measurement:
// Oelo does not publish a fixture current curve and FastLED's generic current
// estimator is not calibrated for these 36 V fixtures.
constexpr uint8_t kPowerSupplyVolts = 36;
constexpr uint16_t kPowerSupplyMilliamps = 5600;
constexpr uint8_t kPowerBudgetPercent = 80;
constexpr uint16_t kPowerBudgetMilliamps =
    (kPowerSupplyMilliamps * kPowerBudgetPercent) / 100;
constexpr uint8_t kMaximumBrightness =
    (255 * kPowerBudgetPercent) / 100;
constexpr uint8_t kMaxPatternColors = 128;
constexpr uint16_t kPatternLibraryVersion = 1;
constexpr char kAccessPointName[] = "OELO_1-23.0";
constexpr char kDefaultCompatibilityApPassword[] = "LeafLights-Test";
constexpr char kSetupAccessPointName[] = "LeafLights-Setup";
constexpr char kSetupAccessPointPassword[] = "LeafLights-Setup";
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.8.1-dev"
#endif
constexpr char kFirmwareVersion[] = FIRMWARE_VERSION;
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
  int id = 0;
  String name = "Lights off";
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

struct ScheduleDecision {
  bool managed = false;
  bool active = false;
  bool holiday = false;
  int patternId = 0;
  uint8_t zoneMask = 0;
  int priority = -32768;
  String name = "Scheduled off";
};

struct ManualOverride {
  bool active = false;
  bool off = false;
  time_t until = 0;
  String description;
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
String compatibilityApPassword;
String webPasswordSalt;
String webPasswordHash;
String webSessionToken;
bool setupComplete = false;
bool compatibilityApEnabled = false;
bool compatibilityApActive = false;
bool compatibilityApFallback = false;
bool setupApActive = false;
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
String scheduleTimezone = "CST6CDT,M3.2.0/2,M11.1.0/2";
double scheduleLatitude = 41.8781;
double scheduleLongitude = -87.6298;
uint32_t lastScheduleCheckAt = 0;
time_t nextScheduleTransition = 0;
String scheduleRuntimeStatus = "Scheduling not configured";
String scheduleAppliedKey;
ManualOverride manualOverride;

void sendDdpFrame(bool force = false);
void saveNetwork(const String& ssid, const String& password);
void serviceSchedules();
void beginManualUntilNext(const String& description);

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

const char kDefaultSchedules[] PROGMEM = R"JSON({
  "version": 1,
  "location": {
    "timezone": "CST6CDT,M3.2.0/2,M11.1.0/2",
    "latitude": 41.8781,
    "longitude": -87.6298
  },
  "weekly": [],
  "holidays": []
})JSON";

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
  const bool legacyInstallation =
      preferences.isKey("compatAp") || preferences.isKey("cnt0") ||
      preferences.isKey("otaPassword") || preferences.isKey("ssid") ||
      preferences.isKey("patternLibrary");
  setupComplete = preferences.getBool("setupDone", legacyInstallation);
  if (!preferences.isKey("setupDone")) {
    preferences.putBool("setupDone", setupComplete);
  }
  webPasswordSalt = preferences.getString("webSalt", "");
  webPasswordHash = preferences.getString("webHash", "");
  webSessionToken = preferences.getString("webToken", "");
  compatibilityApPassword = preferences.getString(
      "compatPw", kDefaultCompatibilityApPassword);
  if (compatibilityApPassword.length() < 8 ||
      compatibilityApPassword.length() > 63) {
    compatibilityApPassword = kDefaultCompatibilityApPassword;
    preferences.putString("compatPw", compatibilityApPassword);
  }
  compatibilityApEnabled = preferences.getBool("compatAp", false);
  automaticUpdates = preferences.getBool("autoUpdate", false);
  const uint8_t savedBrightness =
      preferences.getUChar("brightness", kDefaultBrightness);
  brightness = constrain(savedBrightness, 1, kMaximumBrightness);
  if (brightness != savedBrightness) {
    preferences.putUChar("brightness", brightness);
  }
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
  activePattern.id = 0;
  activePattern.name = "Lights off";
  activePattern.type = "off";
  activePattern.zoneMask = 0;
  activePattern.colorCount = 1;
  activePattern.colors[0] = CRGB::Black;
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

String randomHex(size_t bytes) {
  static const char digits[] = "0123456789abcdef";
  String output;
  output.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t value = static_cast<uint8_t>(esp_random());
    output += digits[value >> 4];
    output += digits[value & 0x0f];
  }
  return output;
}

String sha256Hex(const String& value) {
  uint8_t hash[32];
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts_ret(&context, 0);
  mbedtls_sha256_update_ret(
      &context, reinterpret_cast<const uint8_t*>(value.c_str()),
      value.length());
  mbedtls_sha256_finish_ret(&context, hash);
  mbedtls_sha256_free(&context);
  char output[65];
  for (uint8_t i = 0; i < sizeof(hash); ++i) {
    snprintf(output + i * 2, 3, "%02x", hash[i]);
  }
  output[64] = '\0';
  return String(output);
}

bool validWebPassword(const String& password) {
  return password.length() >= 8 && password.length() <= 64;
}

bool webPasswordConfigured() {
  return webPasswordSalt.length() == 32 && webPasswordHash.length() == 64;
}

bool webUiAuthorized() {
  if (!webPasswordConfigured()) return true;
  if (webSessionToken.isEmpty() || !server.hasHeader("Cookie")) return false;
  const String expected = String("leaf_session=") + webSessionToken;
  return server.header("Cookie").indexOf(expected) >= 0;
}

bool requireWebUiAuth() {
  if (webUiAuthorized()) return true;
  sendText(401, "Web UI login required");
  return false;
}

bool requestFromCompatibilityNetwork() {
  if (!compatibilityApActive) return false;
  const IPAddress remote = server.client().remoteIP();
  return remote[0] == kAccessPointIp[0] &&
         remote[1] == kAccessPointIp[1] &&
         remote[2] == kAccessPointIp[2];
}

bool requireLegacyAccess() {
  if (requestFromCompatibilityNetwork() || webUiAuthorized()) return true;
  sendText(401, "Web UI login required");
  return false;
}

bool otaRequestAuthorized() {
  return webUiAuthorized();
}

void sendOtaUnauthorized() {
  sendText(401, "Web UI login required");
}

void setWebPassword(const String& password) {
  webPasswordSalt = randomHex(16);
  webPasswordHash = sha256Hex(webPasswordSalt + password);
  webSessionToken = randomHex(24);
  preferences.putString("webSalt", webPasswordSalt);
  preferences.putString("webHash", webPasswordHash);
  preferences.putString("webToken", webSessionToken);
}

void clearWebPassword() {
  webPasswordSalt = "";
  webPasswordHash = "";
  webSessionToken = "";
  preferences.remove("webSalt");
  preferences.remove("webHash");
  preferences.remove("webToken");
}

void sendSessionCookie() {
  server.sendHeader(
      "Set-Cookie", String("leaf_session=") + webSessionToken +
                        "; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict");
}

void scheduleRestart(uint32_t delayMs = 1500) {
  restartAt = millis() + delayMs;
}

bool ensureGithubClock(String& error) {
  if (WiFi.status() != WL_CONNECTED) {
    error = "Connect the controller to home Wi-Fi first";
    return false;
  }
  time_t now = time(nullptr);
  if (now > 1700000000) return true;
  configTzTime(scheduleTimezone.c_str(), "pool.ntp.org", "time.nist.gov");
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
                  uint8_t gap, bool reverse, uint8_t pause, uint8_t other,
                  const String& displayName = "", int patternId = 0) {
  activePattern = PatternState();
  activePattern.id = patternId;
  activePattern.name = displayName.isEmpty() ? requestedType : displayName;
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

String resolvePatternDisplayName(int patternId, const String& requestedName,
                                 const String& type, const CRGB* palette,
                                 uint8_t colorCount, uint8_t speed, uint8_t gap,
                                 bool reverse, uint8_t pause, uint8_t other) {
  if (!requestedName.isEmpty()) return requestedName.substring(0, 48);
  File file = LittleFS.open("/patterns.json", "r");
  if (!file) return type;
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error || !document.is<JsonArray>()) return type;
  for (JsonObjectConst saved : document.as<JsonArrayConst>()) {
    if (patternId > 0 && (saved["id"] | 0) == patternId) {
      return String(saved["name"] | type.c_str());
    }
    const String savedDirection = saved["direction"] | "F";
    if (String(saved["type"] | "") != type ||
        (saved["speed"] | 10) != speed || (saved["gap"] | 0) != gap ||
        (saved["pause"] | 0) != pause || (saved["other"] | 0) != other ||
        (savedDirection == "R") != reverse) {
      continue;
    }
    CRGB savedPalette[kMaxPatternColors];
    const uint8_t savedCount = parsePalette(
        String(saved["colors"] | ""), savedPalette, kMaxPatternColors);
    if (savedCount != colorCount) continue;
    bool matches = true;
    for (uint8_t i = 0; i < colorCount; ++i) {
      if (savedPalette[i] != palette[i]) {
        matches = false;
        break;
      }
    }
    if (matches) return String(saved["name"] | type.c_str());
  }
  return type;
}

void handleSetPattern() {
  String patternType = server.arg("patternType");
  if (patternType.isEmpty()) patternType = "stationary";
  patternType.toLowerCase();
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
  int patternId = server.arg("patternId").toInt();
  if (patternId <= 0) patternId = server.arg("id").toInt();
  String requestedName = server.arg("patternName");
  if (requestedName.isEmpty()) requestedName = server.arg("name");
  const String displayName = patternType == "off"
      ? "Lights off"
      : resolvePatternDisplayName(patternId, requestedName, patternType,
                                  palette, colorCount, speed, gap,
                                  direction == "R", pause, other);
  startPattern(patternType, selectedZoneMask(selectedZones), palette, colorCount,
               speed, gap, direction == "R", pause, other, displayName,
               patternId);
  beginManualUntilNext(displayName);
  sendText(200, patternType == "off" ? "off" : "pattern applied");
}

void startFastFireworks(uint8_t zoneMask) {
  const CRGB colors[] = {
      CRGB(255, 255, 255), CRGB(0, 0, 255), CRGB(0, 0, 255),
      CRGB(255, 255, 255), CRGB(255, 0, 0), CRGB(255, 0, 0),
  };
  startPattern("twinkle", zoneMask, colors, 6, 10, 0, true, 0, 0,
               "Fourth of July: Fast Fireworks", 1);
}

void handleAuthStatus() {
  JsonDocument document;
  document["passwordRequired"] = webPasswordConfigured();
  document["authenticated"] = webUiAuthorized();
  document["setupComplete"] = setupComplete;
  String response;
  serializeJson(document, response);
  sendText(200, response, "application/json");
}

void handleWebLogin() {
  if (!webPasswordConfigured()) {
    sendText(200, "Web UI password is not enabled");
    return;
  }
  const String password = server.arg("password");
  if (sha256Hex(webPasswordSalt + password) != webPasswordHash) {
    sendText(401, "Incorrect password");
    return;
  }
  if (webSessionToken.isEmpty()) {
    webSessionToken = randomHex(24);
    preferences.putString("webToken", webSessionToken);
  }
  sendSessionCookie();
  sendText(200, "Signed in");
}

void handleWebPasswordConfiguration() {
  if (!requireWebUiAuth()) return;
  if (server.arg("enabled") != "1") {
    clearWebPassword();
    server.sendHeader(
        "Set-Cookie",
        "leaf_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    sendText(200, "Web UI password removed");
    return;
  }
  const String password = server.arg("password");
  if (!validWebPassword(password)) {
    sendText(400, "Web UI password must be 8-64 characters");
    return;
  }
  setWebPassword(password);
  sendSessionCookie();
  sendText(200, "Web UI password saved; this browser is signed in");
}

void handleSetupWizard() {
  if (setupComplete && !requireWebUiAuth()) return;

  for (uint8_t i = 0; i < kZoneCount; ++i) {
    zones[i].enabled = server.hasArg(preferenceKey("wen", i));
    zones[i].count = constrain(
        server.arg(preferenceKey("wcnt", i)).toInt(), 1, kMaxLogicalLeds);
    zones[i].name =
        server.arg(preferenceKey("wname", i)).substring(0, 24);
    const String order = server.arg(preferenceKey("word", i));
    zones[i].order = validColorOrder(order) ? order : "GBR";
  }

  const String requestedSsid = server.arg("ssid");
  const String requestedWifiPassword = server.arg("wifiPassword");
  const bool requestedCompatibility = server.hasArg("compatibilityEnabled");
  String requestedCompatibilityPassword =
      server.arg("compatibilityPassword");
  requestedCompatibilityPassword.trim();

  if (!requestedCompatibility && requestedSsid.isEmpty()) {
    sendText(400, "Configure home Wi-Fi or enable the compatibility network");
    return;
  }
  if (requestedCompatibility && requestedCompatibilityPassword.isEmpty() &&
      (!setupComplete || compatibilityApPassword.isEmpty())) {
    sendText(400, "Choose a compatibility Wi-Fi password");
    return;
  }
  if (!requestedCompatibilityPassword.isEmpty() &&
      (requestedCompatibilityPassword.length() < 8 ||
       requestedCompatibilityPassword.length() > 63)) {
    sendText(400, "Compatibility Wi-Fi password must be 8-63 characters");
    return;
  }

  const bool protectWebUi = server.hasArg("protectWebUi");
  const String requestedWebPassword = server.arg("webPassword");
  if (protectWebUi && requestedWebPassword.isEmpty() &&
      !webPasswordConfigured()) {
    sendText(400, "Choose a web UI password or leave protection disabled");
    return;
  }
  if (protectWebUi && !requestedWebPassword.isEmpty() &&
      !validWebPassword(requestedWebPassword)) {
    sendText(400, "Web UI password must be 8-64 characters");
    return;
  }

  saveZoneConfiguration();
  if (requestedSsid != wifiSsid || !requestedWifiPassword.isEmpty()) {
    saveNetwork(requestedSsid, requestedWifiPassword);
  }
  compatibilityApEnabled = requestedCompatibility;
  preferences.putBool("compatAp", compatibilityApEnabled);
  if (!requestedCompatibilityPassword.isEmpty()) {
    compatibilityApPassword = requestedCompatibilityPassword;
    preferences.putString("compatPw", compatibilityApPassword);
  }
  setupComplete = true;
  preferences.putBool("setupDone", true);

  if (protectWebUi && !requestedWebPassword.isEmpty()) {
    setWebPassword(requestedWebPassword);
    sendSessionCookie();
  } else if (!protectWebUi) {
    clearWebPassword();
  }
  sendText(200, "Setup saved; controller is restarting");
  scheduleRestart();
}

void sendStatusJson() {
  JsonDocument document;
  document["setupComplete"] = setupComplete;
  document["chipId"] = chipId;
  document["firmwareVersion"] = kFirmwareVersion;
  document["buildDate"] = __DATE__ " " __TIME__;
  document["brightness"] = brightness;
  JsonObject powerSafety = document["powerSafety"].to<JsonObject>();
  powerSafety["supplyVolts"] = kPowerSupplyVolts;
  powerSafety["supplyMilliamps"] = kPowerSupplyMilliamps;
  powerSafety["budgetMilliamps"] = kPowerBudgetMilliamps;
  powerSafety["budgetPercent"] = kPowerBudgetPercent;
  powerSafety["maximumBrightness"] = kMaximumBrightness;
  powerSafety["activeMeasurement"] = false;
  document["activePattern"] = activePattern.type;
  document["patternRunning"] = activePattern.running;
  JsonObject active = document["activePatternState"].to<JsonObject>();
  active["id"] = activePattern.id;
  active["name"] = activePattern.name;
  active["type"] = activePattern.type;
  active["zones"] = activePattern.zoneMask;
  active["speed"] = activePattern.speed;
  active["gap"] = activePattern.gap;
  active["pause"] = activePattern.pause;
  active["other"] = activePattern.other;
  active["direction"] = activePattern.reverse ? "R" : "F";
  JsonArray activeColors = active["colors"].to<JsonArray>();
  for (uint8_t i = 0; i < activePattern.colorCount; ++i) {
    char color[8];
    snprintf(color, sizeof(color), "#%02x%02x%02x",
             activePattern.colors[i].r, activePattern.colors[i].g,
             activePattern.colors[i].b);
    activeColors.add(color);
  }
  JsonObject wifi = document["wifi"].to<JsonObject>();
  wifi["apSsid"] = setupApActive ? kSetupAccessPointName : kAccessPointName;
  wifi["apIp"] = (compatibilityApActive || setupApActive)
                       ? WiFi.softAPIP().toString()
                       : String();
  wifi["compatibilityApEnabled"] = compatibilityApEnabled;
  wifi["compatibilityApActive"] = compatibilityApActive;
  wifi["compatibilityApFallback"] = compatibilityApFallback;
  wifi["compatibilityApSecured"] = true;
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
  ota["maxImageBytes"] = ESP.getFreeSketchSpace();
  ota["automaticUpdates"] = automaticUpdates;
  ota["automaticUpdateStatus"] = automaticUpdateStatus;
  JsonObject webAccess = document["webAccess"].to<JsonObject>();
  webAccess["passwordConfigured"] = webPasswordConfigured();
  webAccess["authenticated"] = webUiAuthorized();
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
  if (server.hasArg("brightness")) {
    const int requestedBrightness = server.arg("brightness").toInt();
    brightness = constrain(requestedBrightness, 1, kMaximumBrightness);
    FastLED.setBrightness(brightness);
    preferences.putUChar("brightness", brightness);
  }
  const CRGB color(constrain(server.arg("r").toInt(), 0, 255),
                   constrain(server.arg("g").toInt(), 0, 255),
                   constrain(server.arg("b").toInt(), 0, 255));
  startPattern("stationary", 1U << zone, &color, 1, 10, 0, false, 0, 0,
               "Custom color");
  beginManualUntilNext(String("Manual color on ") + zones[zone].name);
  sendText(200, String("Zone ") + String(zone + 1) + " updated");
}

void handleBrightness() {
  const int requested = server.arg("value").toInt();
  if (requested < 1 || requested > kMaximumBrightness) {
    sendText(400, String("Brightness must be between 1 and ") +
                      kMaximumBrightness);
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
  const String requestedSsid = server.arg("ssid");
  const String requestedPassword = server.arg("password");
  const bool requestedCompatibilityAp =
      server.hasArg("compatibilityApEnabled");
  String requestedCompatibilityPassword =
      server.arg("compatibilityApPassword");
  requestedCompatibilityPassword.trim();
  const bool changingCompatibilityPassword =
      !requestedCompatibilityPassword.isEmpty();
  if ((requestedCompatibilityAp != compatibilityApEnabled ||
       changingCompatibilityPassword) &&
      !otaRequestAuthorized()) {
    sendOtaUnauthorized();
    return;
  }
  if (!requestedCompatibilityAp && requestedSsid.isEmpty()) {
    sendText(400, "Configure home Wi-Fi before disabling the compatibility network");
    return;
  }
  if (changingCompatibilityPassword &&
      (requestedCompatibilityPassword.length() < 8 ||
       requestedCompatibilityPassword.length() > 63)) {
    sendText(400, "Compatibility Wi-Fi password must be 8-63 characters");
    return;
  }
  if (requestedSsid != wifiSsid || !requestedPassword.isEmpty()) {
    saveNetwork(requestedSsid, requestedPassword);
  }
  compatibilityApEnabled = requestedCompatibilityAp;
  preferences.putBool("compatAp", compatibilityApEnabled);
  if (changingCompatibilityPassword) {
    compatibilityApPassword = requestedCompatibilityPassword;
    preferences.putString("compatPw", compatibilityApPassword);
  }
  sendText(200, compatibilityApEnabled
                    ? "Wi-Fi saved; compatibility network enabled; rebooting"
                    : "Wi-Fi saved; compatibility network disabled; rebooting");
  scheduleRestart();
}

void handleRestart() {
  if (!otaRequestAuthorized()) {
    sendOtaUnauthorized();
    return;
  }
  sendText(200, "Controller is restarting");
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

void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUploadAuthorized = otaRequestAuthorized();
    otaUploadSucceeded = false;
    otaUploadError = "";
    if (!otaUploadAuthorized) {
      otaUploadError = "Web UI login required";
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
  if (!otaRequestAuthorized()) {
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
  scheduleRestart(6000);
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
  if (!otaRequestAuthorized()) {
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
  scheduleRestart(6000);
}

void handleAutomaticUpdateConfiguration() {
  if (!otaRequestAuthorized()) {
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
  scheduleAppliedKey = "";
  lastScheduleCheckAt = millis() - 15000;
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

int32_t civilDayNumber(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<int32_t>(dayOfEra);
}

int32_t localDayNumber(const tm& value) {
  return civilDayNumber(value.tm_year + 1900, value.tm_mon + 1,
                        value.tm_mday);
}

int timezoneOffsetMinutes(time_t timestamp) {
  tm localValue = {};
  tm utcValue = {};
  localtime_r(&timestamp, &localValue);
  gmtime_r(&timestamp, &utcValue);
  utcValue.tm_isdst = -1;
  return static_cast<int>(difftime(mktime(&localValue), mktime(&utcValue)) /
                          60);
}

double normalizedDegrees(double value) {
  value = fmod(value, 360.0);
  return value < 0 ? value + 360.0 : value;
}

int solarMinutes(const tm& date, bool sunrise, int offsetMinutes,
                 time_t reference) {
  const double longitudeHour = scheduleLongitude / 15.0;
  const double approximate = date.tm_yday + 1 +
      ((sunrise ? 6.0 : 18.0) - longitudeHour) / 24.0;
  const double meanAnomaly = 0.9856 * approximate - 3.289;
  double trueLongitude = meanAnomaly +
      1.916 * sin(meanAnomaly * DEG_TO_RAD) +
      0.020 * sin(2 * meanAnomaly * DEG_TO_RAD) + 282.634;
  trueLongitude = normalizedDegrees(trueLongitude);
  double rightAscension = atan(0.91764 * tan(trueLongitude * DEG_TO_RAD)) /
                          DEG_TO_RAD;
  rightAscension = normalizedDegrees(rightAscension);
  rightAscension += floor(trueLongitude / 90.0) * 90.0 -
                    floor(rightAscension / 90.0) * 90.0;
  rightAscension /= 15.0;
  const double sinDeclination = 0.39782 * sin(trueLongitude * DEG_TO_RAD);
  const double cosDeclination = cos(asin(sinDeclination));
  const double cosHour =
      (cos(90.833 * DEG_TO_RAD) -
       sinDeclination * sin(scheduleLatitude * DEG_TO_RAD)) /
      (cosDeclination * cos(scheduleLatitude * DEG_TO_RAD));
  if (cosHour > 1.0 || cosHour < -1.0) return -1;
  double hour = sunrise ? 360.0 - acos(cosHour) / DEG_TO_RAD
                        : acos(cosHour) / DEG_TO_RAD;
  hour /= 15.0;
  const double localMean = hour + rightAscension -
                           0.06571 * approximate - 6.622;
  double utcHour = fmod(localMean - longitudeHour, 24.0);
  if (utcHour < 0) utcHour += 24.0;
  int result = static_cast<int>(round(utcHour * 60.0)) +
               timezoneOffsetMinutes(reference) + offsetMinutes;
  result %= 1440;
  if (result < 0) result += 1440;
  return result;
}

int resolveTimeExpression(JsonVariantConst expression, const tm& date,
                          time_t reference) {
  const String type = expression["type"] | "clock";
  if (type == "clock") {
    return constrain(expression["minutes"] | 0, 0, 1439);
  }
  const int offset = constrain(expression["offset"] | 0, -720, 720);
  if (type == "sunrise") return solarMinutes(date, true, offset, reference);
  if (type == "sunset") return solarMinutes(date, false, offset, reference);
  return -1;
}

bool weeklyDateEligible(JsonObjectConst rule, const tm& date) {
  const uint8_t days = rule["days"] | 0;
  return (days & (1U << date.tm_wday)) != 0;
}

bool holidayDateEligible(JsonObjectConst rule, const tm& date) {
  const int month = rule["month"] | 1;
  const int day = rule["day"] | 1;
  const int before = constrain(rule["daysBefore"] | 0, 0, 366);
  const int after = constrain(rule["daysAfter"] | 0, 0, 366);
  const int32_t current = localDayNumber(date);
  for (int yearOffset = -1; yearOffset <= 1; ++yearOffset) {
    const int32_t anchor = civilDayNumber(
        date.tm_year + 1900 + yearOffset, month, day);
    if (current >= anchor - before && current <= anchor + after) return true;
  }
  return false;
}

bool ruleWindowActive(JsonObjectConst rule, bool holiday, time_t timestamp) {
  tm current = {};
  tm previous = {};
  localtime_r(&timestamp, &current);
  const time_t previousTimestamp = timestamp - 86400;
  localtime_r(&previousTimestamp, &previous);
  const int minute = current.tm_hour * 60 + current.tm_min;
  const int on = resolveTimeExpression(rule["on"], current, timestamp);
  const int off = resolveTimeExpression(rule["off"], current, timestamp);
  if (on < 0 || off < 0) return false;
  const bool currentEligible = holiday ? holidayDateEligible(rule, current)
                                       : weeklyDateEligible(rule, current);
  if (on < off) return currentEligible && minute >= on && minute < off;
  const bool previousEligible = holiday ? holidayDateEligible(rule, previous)
                                        : weeklyDateEligible(rule, previous);
  return (currentEligible && minute >= on) ||
         (previousEligible && minute < off);
}

uint8_t configuredZoneMask(JsonObjectConst rule) {
  uint8_t requested = rule["zones"] | 0;
  uint8_t enabled = 0;
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (zones[zone].enabled) enabled |= 1U << zone;
  }
  return requested ? requested & enabled : enabled;
}

ScheduleDecision evaluateSchedulesAt(JsonDocument& document,
                                     time_t timestamp) {
  ScheduleDecision result;
  tm current = {};
  tm previous = {};
  localtime_r(&timestamp, &current);
  const time_t previousTimestamp = timestamp - 86400;
  localtime_r(&previousTimestamp, &previous);

  JsonArrayConst holidays = document["holidays"].as<JsonArrayConst>();
  for (JsonObjectConst rule : holidays) {
    if (!(rule["enabled"] | true)) continue;
    const int on = resolveTimeExpression(rule["on"], current, timestamp);
    const int off = resolveTimeExpression(rule["off"], current, timestamp);
    const bool relevant = holidayDateEligible(rule, current) ||
        (on >= off && holidayDateEligible(rule, previous));
    if (!relevant) continue;
    const int priority = rule["priority"] | 100;
    if (!result.holiday || priority > result.priority) {
      result.managed = true;
      result.holiday = true;
      result.priority = priority;
      result.active = ruleWindowActive(rule, true, timestamp);
      result.patternId = rule["patternId"] | 0;
      result.zoneMask = configuredZoneMask(rule);
      result.name = String(rule["name"] | "Holiday override");
    }
  }
  if (result.holiday) return result;

  JsonArrayConst weekly = document["weekly"].as<JsonArrayConst>();
  for (JsonObjectConst rule : weekly) {
    if (!(rule["enabled"] | true)) continue;
    result.managed = true;
    if (!ruleWindowActive(rule, false, timestamp)) continue;
    const int priority = rule["priority"] | 0;
    if (!result.active || priority > result.priority) {
      result.active = true;
      result.priority = priority;
      result.patternId = rule["patternId"] | 0;
      result.zoneMask = configuredZoneMask(rule);
      result.name = String(rule["name"] | "Weekly schedule");
    }
  }
  return result;
}

String scheduleDecisionKey(const ScheduleDecision& decision) {
  return String(decision.managed) + ":" + String(decision.active) + ":" +
         decision.patternId + ":" + decision.zoneMask + ":" + decision.name;
}

bool applyStoredPattern(int patternId, uint8_t zoneMask, String& error) {
  if (patternId <= 0) {
    allOff();
    return true;
  }
  File file = LittleFS.open("/patterns.json", "r");
  if (!file) {
    error = "Pattern library is unavailable";
    return false;
  }
  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, file);
  file.close();
  if (parseError || !document.is<JsonArray>()) {
    error = "Pattern library is invalid";
    return false;
  }
  for (JsonObjectConst pattern : document.as<JsonArrayConst>()) {
    if ((pattern["id"] | -1) != patternId) continue;
    CRGB palette[kMaxPatternColors];
    const uint8_t count = parsePalette(
        String(pattern["colors"] | "255,255,255,"), palette,
        kMaxPatternColors);
    if (count == 0) {
      error = "Scheduled pattern has no colors";
      return false;
    }
    const String direction = pattern["direction"] | "F";
    startPattern(String(pattern["type"] | "stationary"), zoneMask,
                 palette, count, pattern["speed"] | 10,
                 pattern["gap"] | 0, direction == "R",
                 pattern["pause"] | 0, pattern["other"] | 0,
                 String(pattern["name"] | "Scheduled pattern"), patternId);
    return true;
  }
  error = String("Scheduled pattern ") + patternId + " was not found";
  return false;
}

String formatLocalTime(time_t timestamp) {
  if (timestamp <= 0) return "";
  tm value = {};
  localtime_r(&timestamp, &value);
  char buffer[40];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %I:%M %p", &value);
  return String(buffer);
}

time_t localScheduleBoundary(const tm& date, int minutes,
                             int extraDays = 0) {
  tm boundary = date;
  boundary.tm_mday += extraDays;
  boundary.tm_hour = minutes / 60;
  boundary.tm_min = minutes % 60;
  boundary.tm_sec = 0;
  boundary.tm_isdst = -1;
  return mktime(&boundary);
}

void considerScheduleBoundary(JsonDocument& document, time_t now,
                              time_t candidate, time_t& nearest) {
  if (candidate <= now || (nearest && candidate >= nearest)) return;
  const String before = scheduleDecisionKey(
      evaluateSchedulesAt(document, candidate - 60));
  const String after = scheduleDecisionKey(
      evaluateSchedulesAt(document, candidate));
  if (before != after) nearest = candidate;
}

time_t findNextScheduleTransition(JsonDocument& document, time_t now,
                                  const ScheduleDecision&) {
  tm today = {};
  localtime_r(&now, &today);
  today.tm_hour = 12;
  today.tm_min = 0;
  today.tm_sec = 0;
  today.tm_isdst = -1;
  time_t nearest = 0;
  for (int dayOffset = -1; dayOffset <= 370; ++dayOffset) {
    tm date = today;
    date.tm_mday += dayOffset;
    const time_t noon = mktime(&date);
    localtime_r(&noon, &date);

    for (JsonObjectConst rule : document["weekly"].as<JsonArrayConst>()) {
      if (!(rule["enabled"] | true) || !weeklyDateEligible(rule, date)) {
        continue;
      }
      const int on = resolveTimeExpression(rule["on"], date, noon);
      const int off = resolveTimeExpression(rule["off"], date, noon);
      if (on < 0 || off < 0) continue;
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, on), nearest);
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, off, on >= off),
                               nearest);
    }

    for (JsonObjectConst rule : document["holidays"].as<JsonArrayConst>()) {
      if (!(rule["enabled"] | true) || !holidayDateEligible(rule, date)) {
        continue;
      }
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, 0), nearest);
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, 0, 1), nearest);
      const int on = resolveTimeExpression(rule["on"], date, noon);
      const int off = resolveTimeExpression(rule["off"], date, noon);
      if (on < 0 || off < 0) continue;
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, on), nearest);
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, off, on >= off),
                               nearest);
    }
  }
  return nearest;
}

bool readScheduleDocument(JsonDocument& document, String& error) {
  File file = LittleFS.open("/schedules.json", "r");
  if (!file) {
    error = "Schedule storage is unavailable";
    return false;
  }
  const DeserializationError parseError = deserializeJson(document, file);
  file.close();
  if (parseError || !document.is<JsonObject>()) {
    error = String("Invalid schedule data: ") + parseError.c_str();
    return false;
  }
  return true;
}

void loadScheduleLocation() {
  JsonDocument document;
  String error;
  if (!readScheduleDocument(document, error)) return;
  JsonObjectConst location = document["location"].as<JsonObjectConst>();
  scheduleTimezone = String(
      location["timezone"] | "CST6CDT,M3.2.0/2,M11.1.0/2");
  scheduleLatitude = location["latitude"] | 41.8781;
  scheduleLongitude = location["longitude"] | -87.6298;
}

void initializeScheduleStorage() {
  if (!LittleFS.exists("/schedules.json")) {
    File file = LittleFS.open("/schedules.json", "w");
    if (file) {
      file.print(kDefaultSchedules);
      file.close();
    }
  }
  loadScheduleLocation();
}

bool saveScheduleDocument(const String& json, String& error) {
  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, json);
  if (parseError || !document.is<JsonObject>() ||
      !document["weekly"].is<JsonArray>() ||
      !document["holidays"].is<JsonArray>()) {
    error = "Schedule JSON must contain weekly and holidays arrays";
    return false;
  }
  JsonObjectConst location = document["location"].as<JsonObjectConst>();
  const String timezone = location["timezone"] | "";
  const double latitude = location["latitude"] | 999.0;
  const double longitude = location["longitude"] | 999.0;
  if (timezone.isEmpty() || timezone.length() > 96 || latitude < -90 ||
      latitude > 90 || longitude < -180 || longitude > 180) {
    error = "Enter a valid timezone, latitude, and longitude";
    return false;
  }
  File file = LittleFS.open("/schedules.tmp", "w");
  if (!file) {
    error = "Unable to open schedule storage";
    return false;
  }
  serializeJson(document, file);
  file.close();
  LittleFS.remove("/schedules.backup");
  if (LittleFS.exists("/schedules.json")) {
    LittleFS.rename("/schedules.json", "/schedules.backup");
  }
  if (!LittleFS.rename("/schedules.tmp", "/schedules.json")) {
    LittleFS.rename("/schedules.backup", "/schedules.json");
    error = "Unable to activate schedule settings";
    return false;
  }
  LittleFS.remove("/schedules.backup");
  loadScheduleLocation();
  configTzTime(scheduleTimezone.c_str(), "pool.ntp.org", "time.nist.gov");
  scheduleAppliedKey = "";
  nextScheduleTransition = 0;
  lastScheduleCheckAt = millis() - 15000;
  return true;
}

void sendSchedulesJson() {
  JsonDocument document;
  String error;
  if (!readScheduleDocument(document, error)) {
    sendText(500, error);
    return;
  }
  const time_t now = time(nullptr);
  JsonObject runtime = document["runtime"].to<JsonObject>();
  runtime["clockValid"] = now > 1700000000;
  runtime["now"] = formatLocalTime(now);
  runtime["status"] = scheduleRuntimeStatus;
  runtime["nextEvent"] = formatLocalTime(nextScheduleTransition);
  runtime["manualOverride"] = manualOverride.active;
  runtime["manualDescription"] = manualOverride.description;
  runtime["manualUntil"] = formatLocalTime(manualOverride.until);
  String response;
  serializeJson(document, response);
  sendText(200, response, "application/json");
}

void handleSaveSchedules() {
  if (!server.hasArg("plain")) {
    sendText(400, "Missing schedule JSON");
    return;
  }
  String error;
  if (!saveScheduleDocument(server.arg("plain"), error)) {
    sendText(400, error);
    return;
  }
  sendText(200, "Schedules saved");
}

void beginManualUntilNext(const String& description) {
  JsonDocument document;
  String error;
  const time_t now = time(nullptr);
  if (now <= 1700000000 || !readScheduleDocument(document, error)) return;
  const ScheduleDecision current = evaluateSchedulesAt(document, now);
  if (!current.managed) return;
  manualOverride.active = true;
  manualOverride.off = false;
  manualOverride.until = findNextScheduleTransition(document, now, current);
  manualOverride.description = description;
  scheduleRuntimeStatus = String("Manual override: ") + description;
}

void handleManualOverride() {
  const String action = server.arg("action");
  if (action == "clear") {
    manualOverride = ManualOverride();
    scheduleAppliedKey = "";
    lastScheduleCheckAt = millis() - 15000;
    sendText(200, "Manual override cleared");
    return;
  }
  manualOverride.active = true;
  manualOverride.off = action == "off";
  manualOverride.description = manualOverride.off ? "Lights off" :
                               "Current lighting held";
  const String duration = server.arg("duration");
  if (duration == "next") {
    JsonDocument document;
    String error;
    const time_t now = time(nullptr);
    if (readScheduleDocument(document, error)) {
      manualOverride.until = findNextScheduleTransition(
          document, now, evaluateSchedulesAt(document, now));
    }
  } else if (duration == "minutes") {
    manualOverride.until = time(nullptr) +
        constrain(server.arg("minutes").toInt(), 1, 10080) * 60;
  } else {
    manualOverride.until = 0;
  }
  if (manualOverride.off) allOff();
  scheduleRuntimeStatus = String("Manual override: ") +
                          manualOverride.description;
  sendText(200, String("Manual override active: ") +
                    manualOverride.description);
}

void serviceSchedules() {
  if (millis() - lastScheduleCheckAt < 15000) return;
  lastScheduleCheckAt = millis();
  const time_t now = time(nullptr);
  if (now <= 1700000000) {
    scheduleRuntimeStatus = "Waiting for network time";
    return;
  }
  if (manualOverride.active) {
    if (manualOverride.until == 0 || now < manualOverride.until) {
      scheduleRuntimeStatus = String("Manual override: ") +
                              manualOverride.description;
      return;
    }
    manualOverride = ManualOverride();
    scheduleAppliedKey = "";
  }
  JsonDocument document;
  String error;
  if (!readScheduleDocument(document, error)) {
    scheduleRuntimeStatus = error;
    return;
  }
  const ScheduleDecision decision = evaluateSchedulesAt(document, now);
  const String key = scheduleDecisionKey(decision);
  if (!nextScheduleTransition || nextScheduleTransition <= now ||
      key != scheduleAppliedKey) {
    nextScheduleTransition = findNextScheduleTransition(document, now, decision);
  }
  if (!decision.managed) {
    scheduleRuntimeStatus = "No enabled schedule rules";
    scheduleAppliedKey = "";
    return;
  }
  scheduleRuntimeStatus = decision.active
      ? String(decision.holiday ? "Holiday: " : "Weekly: ") + decision.name
      : "Scheduled off";
  if (key == scheduleAppliedKey) return;
  if (!decision.active) {
    allOff();
  } else if (!applyStoredPattern(decision.patternId, decision.zoneMask, error)) {
    allOff();
    scheduleRuntimeStatus = error;
  }
  scheduleAppliedKey = key;
}

void configureWebServer() {
  const char* headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);
  server.on("/", HTTP_GET, []() {
    addCorsHeaders();
    server.send_P(200, "text/html", WEB_UI);
  });
  server.on("/getController", HTTP_GET, []() {
    if (requireLegacyAccess()) sendControllerJson();
  });
  server.on("/saveController", HTTP_GET, []() {
    if (requireLegacyAccess()) handleSaveController();
  });
  server.on("/setPattern", HTTP_GET, []() {
    if (requireLegacyAccess()) handleSetPattern();
  });
  server.on("/getPatterns", HTTP_GET, []() {
    if (requireLegacyAccess()) handleGetPatterns();
  });
  server.on("/savePatterns", HTTP_GET, []() {
    if (requireLegacyAccess()) handleSavePatterns();
  });
  server.on("/api/patterns", HTTP_POST, []() {
    if (requireWebUiAuth()) handleSavePatternsBody();
  });
  server.on("/api/schedules", HTTP_GET, []() {
    if (requireWebUiAuth()) sendSchedulesJson();
  });
  server.on("/api/schedules", HTTP_POST, []() {
    if (requireWebUiAuth()) handleSaveSchedules();
  });
  server.on("/api/manual-override", HTTP_POST, []() {
    if (requireWebUiAuth()) handleManualOverride();
  });
  server.on("/scanNetworksRSSI", HTTP_GET, []() {
    if (requireLegacyAccess()) handleScanNetworks();
  });
  server.on("/saveNetwork", HTTP_GET, []() {
    if (!requireLegacyAccess()) return;
    saveNetwork(server.arg("ssid"), server.arg("pw"));
    sendText(200, "Network saved; rebooting");
    scheduleRestart();
  });
  server.on("/api/auth-status", HTTP_GET, handleAuthStatus);
  server.on("/api/login", HTTP_POST, handleWebLogin);
  server.on("/api/setup", HTTP_POST, handleSetupWizard);
  server.on("/api/web-password", HTTP_POST,
            handleWebPasswordConfiguration);
  server.on("/api/status", HTTP_GET, []() {
    if (requireWebUiAuth()) sendStatusJson();
  });
  server.on("/api/color", HTTP_GET, []() {
    if (requireWebUiAuth()) handleApiColor();
  });
  server.on("/api/brightness", HTTP_GET, []() {
    if (requireWebUiAuth()) handleBrightness();
  });
  server.on("/api/off", HTTP_GET, []() {
    if (!requireWebUiAuth()) return;
    allOff();
    beginManualUntilNext("Lights off");
    sendText(200, "All zones off");
  });
  server.on("/api/preset/fast-fireworks", HTTP_GET, []() {
    if (!requireWebUiAuth()) return;
    uint8_t mask = 0;
    for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
      if (zoneRegistered[zone]) mask |= 1U << zone;
    }
    startFastFireworks(mask);
    beginManualUntilNext("Fourth of July: Fast Fireworks");
    sendText(200, "Fourth of July: Fast Fireworks running");
  });
  server.on("/api/zones", HTTP_POST, []() {
    if (requireWebUiAuth()) handleWebZones();
  });
  server.on("/api/network", HTTP_POST, []() {
    if (requireWebUiAuth()) handleWebNetwork();
  });
  server.on("/api/restart", HTTP_POST, []() {
    if (requireWebUiAuth()) handleRestart();
  });
  server.on("/api/wled-sync", HTTP_POST, []() {
    if (requireWebUiAuth()) handleWledSyncConfiguration();
  });
  server.on("/api/update", HTTP_POST, handleFirmwareUploadComplete,
            handleFirmwareUpload);
  server.on("/api/releases", HTTP_GET, []() {
    if (requireWebUiAuth()) handleGithubReleases();
  });
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
  const bool startConfiguredAp = setupComplete && compatibilityApEnabled;
  const bool startSetupAp = !setupComplete;
  WiFi.mode((startConfiguredAp || startSetupAp) ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (startSetupAp) {
    WiFi.softAPConfig(kAccessPointIp, kAccessPointIp, kAccessPointMask);
    setupApActive = WiFi.softAP(
        kSetupAccessPointName, kSetupAccessPointPassword);
  } else if (startConfiguredAp) {
    WiFi.softAPConfig(kAccessPointIp, kAccessPointIp, kAccessPointMask);
    compatibilityApActive = WiFi.softAP(
        kAccessPointName, compatibilityApPassword.c_str());
  }

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

  if (setupComplete && !compatibilityApEnabled &&
      WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(kAccessPointIp, kAccessPointIp, kAccessPointMask);
    setupApActive = WiFi.softAP(
        kSetupAccessPointName, kSetupAccessPointPassword);
    compatibilityApFallback = setupApActive;
  }

  if (WiFi.status() == WL_CONNECTED && MDNS.begin("leaflights")) {
    MDNS.addService("http", "tcp", 80);
  }
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  zone <0-5> <r> <g> <b>");
  Serial.printf("  brightness <1-%u>\n", kMaximumBrightness);
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
             value >= 1 && value <= kMaximumBrightness) {
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
  initializeScheduleStorage();
  initializeLeds();
  configureWifi();
  configTzTime(scheduleTimezone.c_str(), "pool.ntp.org", "time.nist.gov");
  configureWebServer();
  nextAutomaticUpdateAt = millis() + 60000;
  automaticUpdateStatus = automaticUpdates
                              ? "Enabled; first check in one minute"
                              : "Automatic updates disabled";

  Serial.println("LeafFilter/Oelo UCS1903 test controller ready");
  if (setupApActive) {
    Serial.printf("%sAP: %s at http://%s\n",
                  compatibilityApFallback ? "Recovery " : "Setup ",
                  kSetupAccessPointName, WiFi.softAPIP().toString().c_str());
  } else if (compatibilityApActive) {
    Serial.printf("%sAP: %s at http://%s\n",
                  compatibilityApFallback ? "Recovery " : "Setup ",
                  kAccessPointName, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("Compatibility AP disabled");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("LAN: http://%s or http://leaflights.local\n",
                  WiFi.localIP().toString().c_str());
  }
  printHelp();
}

void loop() {
  server.handleClient();
  updateActivePattern();
  serviceSchedules();
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
