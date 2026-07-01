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
#include "preset_library.h"
#include "web_ui.h"

#ifdef GLEDOPTO_ESP32
constexpr uint8_t kZoneCount = 2;
constexpr uint8_t kZonePins[kZoneCount] = {16, 2};
#else
constexpr uint8_t kZoneCount = 6;
constexpr uint8_t kZonePins[kZoneCount] = {1, 2, 4, 5, 6, 7};
#endif
constexpr uint16_t kMaxLogicalLeds = 1000;
constexpr uint8_t kMaxPhysicalPixelsPerFixture = 2;
constexpr uint16_t kMaxPhysicalPixels =
    kMaxLogicalLeds * kMaxPhysicalPixelsPerFixture;
constexpr uint16_t kAnimationFrameIntervalMs = 25;
constexpr uint16_t kDdpFrameIntervalMs = 25;
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
constexpr uint16_t kPatternLibraryVersion = 3;
constexpr char kAccessPointName[] = "OELO_1-23.0";
constexpr char kDefaultCompatibilityApPassword[] = "LeafLights-Test";
constexpr char kSetupAccessPointName[] = "LeafLights-Setup";
constexpr char kSetupAccessPointPassword[] = "LeafLights-Setup";
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.10.0-dev"
#endif
constexpr char kFirmwareVersion[] = FIRMWARE_VERSION;
constexpr char kGithubApiUrl[] =
    "https://api.github.com/repos/WVandergrift/oelo-lights/releases?per_page=5";
constexpr char kGithubDownloadPrefix[] =
    "https://github.com/WVandergrift/oelo-lights/releases/download/";
constexpr char kSportsFeedBaseUrl[] =
    "https://wvandergrift.github.io/oelo-lights/data/sports/teams/";
#ifdef GLEDOPTO_ESP32
constexpr char kReleaseAssetName[] = "leaf-lights-gledopto-esp32.bin";
#else
constexpr char kReleaseAssetName[] = "leaf-lights-tinys3.bin";
#endif
const IPAddress kAccessPointIp(172, 24, 1, 1);
const IPAddress kAccessPointMask(255, 255, 255, 0);

struct ZoneConfig {
  bool enabled = false;
  uint16_t count = 1;
  String name;
  String order = "GBR";
  String protocol = "UCS1903";
  uint8_t gpio = 0;
  uint8_t physicalPixelsPerFixture = 2;
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
  bool oneOff = false;
  bool holiday = false;
  bool sports = false;
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
// Keep the remote-update transfer buffer out of the Arduino loop-task stack.
// TLS, HTTP redirects, SHA-256, and Update already consume substantial stack.
uint8_t firmwareDownloadBuffer[2048];
ZoneConfig zones[kZoneCount];
bool zoneRegistered[kZoneCount] = {};
uint8_t brightness = kDefaultBrightness;
bool automaticPowerLimit = false;
uint8_t configuredLedVolts = kPowerSupplyVolts;
uint16_t configuredPowerMilliamps = kPowerSupplyMilliamps;
uint8_t configuredMilliampsPerPixel = 55;
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
IPAddress resolvedDdpDestination;
String resolvedDdpDestinationName;
bool resolvedDdpDestinationValid = false;
uint16_t lastDdpStatusPixelCount = 0;
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
uint32_t lastSportsFeedCheckAt = 0;
String sportsFeedStatus = "Not refreshed";

void sendDdpFrame(bool force = false);
uint32_t estimateLedMilliamps();
void showLeds();
void saveNetwork(const String& ssid, const String& password);
void serviceSchedules();
void beginManualUntilNext(const String& description);
bool saveScheduleDocument(const String& json, String& error);
bool refreshSportsScheduleFeed(String& error);


const char kDefaultSchedules[] PROGMEM = R"JSON({
  "version": 3,
  "location": {
    "timezone": "CST6CDT,M3.2.0/2,M11.1.0/2",
    "latitude": 41.8781,
    "longitude": -87.6298
  },
  "weekly": [],
  "holidays": [],
  "oneOff": [],
  "sports": {
    "enabled": false,
    "nightBefore": false,
    "gameDay": true,
    "includePreseason": false,
    "on": {"type": "sunset", "offset": -20},
    "off": {"type": "clock", "minutes": 1380},
    "zones": 0,
    "teams": [],
    "events": [],
    "lastUpdated": 0
  }
})JSON";

String preferenceKey(const char* prefix, uint8_t zone) {
  return String(prefix) + zone;
}

bool validColorOrder(const String& value) {
  return value == "RGB" || value == "RBG" || value == "GRB" ||
         value == "GBR" || value == "BRG" || value == "BGR";
}

bool validLedProtocol(const String& value) {
  return value == "UCS1903" || value == "WS281x";
}

bool validZoneGpio(uint8_t gpio) {
#ifdef GLEDOPTO_ESP32
  return gpio == 2 || gpio == 16;
#else
  return gpio == 1 || gpio == 2 || gpio == 4 || gpio == 5 || gpio == 6 ||
         gpio == 7;
#endif
}

bool enabledZoneGpiosAreUnique() {
  for (uint8_t i = 0; i < kZoneCount; ++i) {
    if (!zones[i].enabled) continue;
    for (uint8_t j = i + 1; j < kZoneCount; ++j) {
      if (zones[j].enabled && zones[i].gpio == zones[j].gpio) return false;
    }
  }
  return true;
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
  automaticPowerLimit = preferences.getBool("pwrLimit", false);
  configuredLedVolts = constrain(
      preferences.getUChar("pwrVolts", kPowerSupplyVolts), 5, 36);
  configuredPowerMilliamps = constrain(
      preferences.getUShort("pwrMa", kPowerSupplyMilliamps), 100, 30000);
  configuredMilliampsPerPixel = constrain(
      preferences.getUChar("pixelMa", 55), 1, 100);
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
    const String protocolKey = preferenceKey("proto", i);
    const String gpioKey = preferenceKey("gpio", i);
    const String physicalKey = preferenceKey("phys", i);
    zones[i].count = constrain(
        preferences.getUShort(countKey.c_str(), 1), 1, kMaxLogicalLeds);
    zones[i].enabled = preferences.getBool(enabledKey.c_str(), i == 0);
    zones[i].name = preferences.getString(
        nameKey.c_str(), String("Zone ") + String(i + 1));
    zones[i].order = preferences.getString(orderKey.c_str(), "GBR");
    if (!validColorOrder(zones[i].order)) {
      zones[i].order = "GBR";
    }
    zones[i].protocol = preferences.getString(protocolKey.c_str(), "UCS1903");
    if (!validLedProtocol(zones[i].protocol)) zones[i].protocol = "UCS1903";
    zones[i].gpio = preferences.getUChar(gpioKey.c_str(), kZonePins[i]);
    if (!validZoneGpio(zones[i].gpio)) zones[i].gpio = kZonePins[i];
    zones[i].physicalPixelsPerFixture = constrain(
        preferences.getUChar(physicalKey.c_str(), 2), 1,
        kMaxPhysicalPixelsPerFixture);
  }
}

void saveZoneConfiguration() {
  for (uint8_t i = 0; i < kZoneCount; ++i) {
    preferences.putUShort(preferenceKey("cnt", i).c_str(), zones[i].count);
    preferences.putBool(preferenceKey("en", i).c_str(), zones[i].enabled);
    preferences.putString(preferenceKey("name", i).c_str(), zones[i].name);
    preferences.putString(preferenceKey("ord", i).c_str(), zones[i].order);
    preferences.putString(preferenceKey("proto", i).c_str(), zones[i].protocol);
    preferences.putUChar(preferenceKey("gpio", i).c_str(), zones[i].gpio);
    preferences.putUChar(preferenceKey("phys", i).c_str(),
                         zones[i].physicalPixelsPerFixture);
  }
}

template <uint8_t Pin, EOrder Order>
void registerZoneWithOrder(uint8_t zone, uint16_t physicalCount) {
  if (zones[zone].protocol == "WS281x") {
    FastLED.addLeds<WS2812B, Pin, Order>(zonePixels[zone], physicalCount);
  } else {
    FastLED.addLeds<UCS1903, Pin, Order>(zonePixels[zone], physicalCount);
  }
}

template <uint8_t Pin>
void registerZoneForPin(uint8_t zone) {
  const uint16_t physicalCount =
      zones[zone].count * zones[zone].physicalPixelsPerFixture;
  const String& order = zones[zone].order;

  if (order == "RGB") {
    registerZoneWithOrder<Pin, RGB>(zone, physicalCount);
  } else if (order == "RBG") {
    registerZoneWithOrder<Pin, RBG>(zone, physicalCount);
  } else if (order == "GRB") {
    registerZoneWithOrder<Pin, GRB>(zone, physicalCount);
  } else if (order == "BRG") {
    registerZoneWithOrder<Pin, BRG>(zone, physicalCount);
  } else if (order == "BGR") {
    registerZoneWithOrder<Pin, BGR>(zone, physicalCount);
  } else {
    registerZoneWithOrder<Pin, GBR>(zone, physicalCount);
  }
  zoneRegistered[zone] = true;
}

void registerZone(uint8_t zone) {
  if (!zones[zone].enabled) {
    return;
  }
#ifdef GLEDOPTO_ESP32
  switch (zones[zone].gpio) {
    case 2: registerZoneForPin<2>(zone); break;
    case 16: registerZoneForPin<16>(zone); break;
  }
#else
  switch (zones[zone].gpio) {
    case 1: registerZoneForPin<1>(zone); break;
    case 2: registerZoneForPin<2>(zone); break;
    case 4: registerZoneForPin<4>(zone); break;
    case 5: registerZoneForPin<5>(zone); break;
    case 6: registerZoneForPin<6>(zone); break;
    case 7: registerZoneForPin<7>(zone); break;
  }
#endif
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
  const uint16_t physical =
      fixture * zones[zone].physicalPixelsPerFixture;
  for (uint8_t copy = 0; copy < zones[zone].physicalPixelsPerFixture; ++copy) {
    zonePixels[zone][physical + copy] = color;
  }
}

void fillZone(uint8_t zone, const CRGB& color) {
  if (zone >= kZoneCount || !zoneRegistered[zone]) {
    return;
  }
  fill_solid(zonePixels[zone],
             zones[zone].count * zones[zone].physicalPixelsPerFixture, color);
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
  showLeds();
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
  // Release assets redirect to GitHub's asset CDN and may pause between TLS
  // records while flash sectors are erased. These timeouts are deliberately
  // longer than the small metadata requests above.
  client.setTimeout(30);
  http.setTimeout(60000);
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
  size_t written = 0;
  uint32_t lastDataAt = millis();
  while (written < release.size) {
    const size_t available = stream->available();
    if (available > 0) {
      const size_t wanted = min<size_t>(sizeof(firmwareDownloadBuffer),
          min<size_t>(available, release.size - written));
      const size_t received =
          stream->readBytes(firmwareDownloadBuffer, wanted);
      if (received == 0) continue;
      if (Update.write(firmwareDownloadBuffer, received) != received) {
        error = Update.errorString();
        break;
      }
      mbedtls_sha256_update_ret(&hashContext, firmwareDownloadBuffer,
                                received);
      written += received;
      lastDataAt = millis();
      delay(1);
    } else {
      if (!http.connected() || millis() - lastDataAt > 60000) {
        error = String("Firmware download stopped after ") + written +
                " of " + release.size + " bytes";
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
  if (resolvedDdpDestinationValid &&
      resolvedDdpDestinationName == wledSync.destination) {
    address = resolvedDdpDestination;
    return true;
  }
  if (wledSync.destination == "255.255.255.255" &&
      WiFi.status() == WL_CONNECTED) {
    const IPAddress local = WiFi.localIP();
    const IPAddress mask = WiFi.subnetMask();
    address = IPAddress(local[0] | static_cast<uint8_t>(~mask[0]),
                        local[1] | static_cast<uint8_t>(~mask[1]),
                        local[2] | static_cast<uint8_t>(~mask[2]),
                        local[3] | static_cast<uint8_t>(~mask[3]));
  } else if (!address.fromString(wledSync.destination) &&
      WiFi.hostByName(wledSync.destination.c_str(), address) != 1) {
    return false;
  }
  resolvedDdpDestination = address;
  resolvedDdpDestinationName = wledSync.destination;
  resolvedDdpDestinationValid = true;
  return true;
}

void sendDdpFrame(bool force) {
  if (!wledSync.enabled) {
    lastDdpStatus = "Disabled";
    return;
  }
  const uint32_t now = millis();
  if (!force && now - lastDdpFrameAt < kDdpFrameIntervalMs) return;

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
  constexpr uint16_t kDdpHeaderLength = 10;
  constexpr uint16_t kDdpPixelsPerPacket = kDdpChannelsPerPacket / 3;
  uint8_t packet[kDdpHeaderLength + kDdpChannelsPerPacket];
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
    packet[0] = static_cast<uint8_t>(0x40 | (push ? 0x01 : 0x00));
    packet[1] = ddpSequence++ & 0x0f;
    if ((ddpSequence & 0x0f) == 0) ++ddpSequence;
    packet[2] = 0x0b;  // RGB, 8 bits/channel.
    packet[3] = 0x01;  // Default display output.
    packet[4] = static_cast<uint8_t>(channelOffset >> 24);
    packet[5] = static_cast<uint8_t>(channelOffset >> 16);
    packet[6] = static_cast<uint8_t>(channelOffset >> 8);
    packet[7] = static_cast<uint8_t>(channelOffset);
    packet[8] = static_cast<uint8_t>(dataLength >> 8);
    packet[9] = static_cast<uint8_t>(dataLength);

    uint16_t packetOffset = kDdpHeaderLength;
    for (uint16_t pixel = firstPixel;
         pixel < firstPixel + packetPixels; ++pixel) {
      const uint16_t sourceFixture =
          min<uint16_t>((static_cast<uint32_t>(pixel) * sourceCount) /
                            outputCount,
                        sourceCount - 1);
      const CRGB& color =
          zonePixels[source][sourceFixture *
                             zones[source].physicalPixelsPerFixture];
      packet[packetOffset++] = scale8_video(color.r, brightness);
      packet[packetOffset++] = scale8_video(color.g, brightness);
      packet[packetOffset++] = scale8_video(color.b, brightness);
    }
    const size_t written = ddpUdp.write(packet, packetOffset);
    const bool packetSent = ddpUdp.endPacket();
    if (written != packetOffset || !packetSent) {
      lastDdpStatus = "UDP send failed";
      return;
    }
  }
  lastDdpFrameAt = now;
  if (!lastDdpStatus.startsWith("Streaming") ||
      lastDdpStatusPixelCount != outputCount) {
    lastDdpStatus = String("Streaming ") + outputCount + " pixels";
    lastDdpStatusPixelCount = outputCount;
  }
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
                  zones[zone].count * zones[zone].physicalPixelsPerFixture,
                  amount);
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
  showLeds();
  sendDdpFrame(true);
}

void updateActivePattern() {
  if (!activePattern.running || activePattern.zoneMask == 0) return;
  const uint32_t now = millis();
  if (now - activePattern.lastFrameAt < kAnimationFrameIntervalMs) return;
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
    showLeds();
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
    const String protocol = server.arg(preferenceKey("wproto", i));
    if (!protocol.isEmpty()) {
      zones[i].protocol = validLedProtocol(protocol) ? protocol : "UCS1903";
    }
    const int gpio = server.arg(preferenceKey("wgpio", i)).toInt();
    if (server.hasArg(preferenceKey("wgpio", i))) {
      if (!validZoneGpio(gpio)) {
        sendText(400, String("Invalid GPIO for zone ") + (i + 1));
        return;
      }
      zones[i].gpio = gpio;
    }
    if (server.hasArg(preferenceKey("wphys", i))) {
      zones[i].physicalPixelsPerFixture = constrain(
          server.arg(preferenceKey("wphys", i)).toInt(), 1,
          kMaxPhysicalPixelsPerFixture);
    }
  }

  if (!enabledZoneGpiosAreUnique()) {
    sendText(400, "Enabled zones must use different GPIOs");
    return;
  }
  const int requestedVolts = server.arg("powerVolts").toInt();
  const int requestedMilliamps = server.arg("powerMilliamps").toInt();
  const int requestedPixelMilliamps = server.arg("pixelMilliamps").toInt();
  if (requestedVolts < 5 || requestedVolts > 36 ||
      requestedMilliamps < 100 || requestedMilliamps > 30000 ||
      requestedPixelMilliamps < 1 || requestedPixelMilliamps > 100) {
    sendText(400, "Power settings are outside the supported range");
    return;
  }
  automaticPowerLimit = server.hasArg("powerLimitEnabled");
  configuredLedVolts = requestedVolts;
  configuredPowerMilliamps = requestedMilliamps;
  configuredMilliampsPerPixel = requestedPixelMilliamps;

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
  preferences.putBool("pwrLimit", automaticPowerLimit);
  preferences.putUChar("pwrVolts", configuredLedVolts);
  preferences.putUShort("pwrMa", configuredPowerMilliamps);
  preferences.putUChar("pixelMa", configuredMilliampsPerPixel);
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

uint32_t estimateLedMilliamps() {
  uint64_t milliampUnits = 0;
  uint32_t physicalPixels = 0;
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    if (!zoneRegistered[zone]) continue;
    const uint16_t count =
        zones[zone].count * zones[zone].physicalPixelsPerFixture;
    physicalPixels += count;
    for (uint16_t pixel = 0; pixel < count; ++pixel) {
      const CRGB& color = zonePixels[zone][pixel];
      milliampUnits += static_cast<uint16_t>(color.r) + color.g + color.b;
    }
  }
  const uint64_t activeMilliamps =
      milliampUnits * configuredMilliampsPerPixel * brightness /
      (765ULL * 255ULL);
  return physicalPixels + activeMilliamps;  // Approx. 1 mA idle per pixel.
}

void showLeds() {
  uint8_t appliedBrightness = brightness;
  if (automaticPowerLimit) {
    const uint32_t estimatedMilliamps = estimateLedMilliamps();
    if (estimatedMilliamps > configuredPowerMilliamps) {
      appliedBrightness = max<uint8_t>(
          1, static_cast<uint32_t>(brightness) * configuredPowerMilliamps /
                 estimatedMilliamps);
    }
  }
  FastLED.setBrightness(appliedBrightness);
  FastLED.show();
  FastLED.setBrightness(brightness);
}

void sendStatusJson() {
  JsonDocument document;
  document["setupComplete"] = setupComplete;
  document["chipId"] = chipId;
  document["firmwareVersion"] = kFirmwareVersion;
  document["buildDate"] = __DATE__ " " __TIME__;
  document["brightness"] = brightness;
  JsonObject powerSafety = document["powerSafety"].to<JsonObject>();
  powerSafety["automaticLimiter"] = automaticPowerLimit;
  powerSafety["supplyVolts"] = configuredLedVolts;
  powerSafety["supplyMilliamps"] = configuredPowerMilliamps;
  powerSafety["budgetMilliamps"] = automaticPowerLimit
                                        ? configuredPowerMilliamps
                                        : kPowerBudgetMilliamps;
  powerSafety["budgetPercent"] = kPowerBudgetPercent;
  powerSafety["maximumBrightness"] = kMaximumBrightness;
  powerSafety["milliampsPerPixel"] = configuredMilliampsPerPixel;
  const uint32_t estimatedMilliamps = estimateLedMilliamps();
  powerSafety["estimatedMilliamps"] = estimatedMilliamps;
  powerSafety["estimatedWatts"] =
      estimatedMilliamps * configuredLedVolts / 1000.0;
  powerSafety["utilizationPercent"] =
      min<uint32_t>(999, estimatedMilliamps * 100UL /
                             max<uint16_t>(configuredPowerMilliamps, 1));
  powerSafety["limiting"] =
      automaticPowerLimit && estimatedMilliamps > configuredPowerMilliamps;
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
    zone["protocol"] = zones[i].protocol;
    zone["gpio"] = zones[i].gpio;
    zone["physicalPixelsPerFixture"] = zones[i].physicalPixelsPerFixture;
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
  showLeds();
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
    const String protocol = server.arg(preferenceKey("proto", i));
    zones[i].protocol = validLedProtocol(protocol) ? protocol : "UCS1903";
    const int gpio = server.arg(preferenceKey("gpio", i)).toInt();
    if (!validZoneGpio(gpio)) {
      sendText(400, String("Invalid GPIO for zone ") + (i + 1));
      return;
    }
    zones[i].gpio = gpio;
    zones[i].physicalPixelsPerFixture = constrain(
        server.arg(preferenceKey("phys", i)).toInt(), 1,
        kMaxPhysicalPixelsPerFixture);
  }
  if (!enabledZoneGpiosAreUnique()) {
    sendText(400, "Enabled zones must use different GPIOs");
    return;
  }
  const int requestedVolts = server.arg("powerVolts").toInt();
  const int requestedMilliamps = server.arg("powerMilliamps").toInt();
  const int requestedPixelMilliamps = server.arg("pixelMilliamps").toInt();
  if (requestedVolts < 5 || requestedVolts > 36 ||
      requestedMilliamps < 100 || requestedMilliamps > 30000 ||
      requestedPixelMilliamps < 1 || requestedPixelMilliamps > 100) {
    sendText(400, "Power settings are outside the supported range");
    return;
  }
  automaticPowerLimit = server.hasArg("powerLimitEnabled");
  configuredLedVolts = requestedVolts;
  configuredPowerMilliamps = requestedMilliamps;
  configuredMilliampsPerPixel = requestedPixelMilliamps;
  preferences.putBool("pwrLimit", automaticPowerLimit);
  preferences.putUChar("pwrVolts", configuredLedVolts);
  preferences.putUShort("pwrMa", configuredPowerMilliamps);
  preferences.putUChar("pixelMa", configuredMilliampsPerPixel);
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
  resolvedDdpDestinationValid = false;
  resolvedDdpDestinationName = "";
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
    const String presetKey = builtIn["presetKey"] | "";
    JsonObject existing;
    for (JsonObject saved : savedPatterns) {
      const String savedKey = saved["presetKey"] | "";
      if ((!presetKey.isEmpty() && presetKey == savedKey) ||
          name == String(saved["name"] | "")) {
        existing = saved;
        break;
      }
    }
    if (!existing.isNull()) {
      // Add library metadata without replacing a user's edits to the effect.
      existing["presetKey"] = presetKey;
      existing["category"] = builtIn["category"] | "Other";
      existing["tags"].set(builtIn["tags"]);
      existing["builtin"] = true;
      changed = true;
      continue;
    }

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
  File file = LittleFS.open("/patterns.tmp", "w");
  if (!file) {
    sendText(500, "Unable to open pattern storage");
    return;
  }
  if (serializeJson(document, file) == 0) {
    file.close();
    LittleFS.remove("/patterns.tmp");
    sendText(500, "Unable to write pattern storage");
    return;
  }
  file.close();
  LittleFS.remove("/patterns.backup");
  if (LittleFS.exists("/patterns.json") &&
      !LittleFS.rename("/patterns.json", "/patterns.backup")) {
    LittleFS.remove("/patterns.tmp");
    sendText(500, "Unable to prepare pattern storage");
    return;
  }
  if (!LittleFS.rename("/patterns.tmp", "/patterns.json")) {
    LittleFS.rename("/patterns.backup", "/patterns.json");
    sendText(500, "Unable to activate restored patterns");
    return;
  }
  LittleFS.remove("/patterns.backup");
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

bool sportsEventMatchesDate(JsonObjectConst event, JsonObjectConst sports,
                            JsonObjectConst team, const tm& date) {
  if (event["cancelled"] | false) return false;
  if (String(event["teamKey"] | "") != String(team["key"] | "")) {
    return false;
  }
  const String seasonType = event["seasonType"] | "";
  if (!(sports["includePreseason"] | false) &&
      seasonType.indexOf("pre") >= 0) {
    return false;
  }
  const String homeAway = event["homeAway"] | "";
  if (homeAway == "home" && !(team["includeHome"] | true)) return false;
  if (homeAway == "away" && !(team["includeAway"] | true)) return false;
  const time_t start = event["startEpoch"] | 0;
  if (start <= 0) return false;
  tm gameDate = {};
  localtime_r(&start, &gameDate);
  const int32_t difference = localDayNumber(gameDate) - localDayNumber(date);
  return ((sports["gameDay"] | true) && difference == 0) ||
         ((sports["nightBefore"] | false) && difference == 1);
}

JsonObjectConst sportsEventForDate(JsonObjectConst sports,
                                   JsonObjectConst team, const tm& date) {
  JsonObjectConst selected;
  time_t selectedStart = 0;
  for (JsonObjectConst event : sports["events"].as<JsonArrayConst>()) {
    if (!sportsEventMatchesDate(event, sports, team, date)) continue;
    const time_t start = event["startEpoch"] | 0;
    if (selected.isNull() || start < selectedStart) {
      selected = event;
      selectedStart = start;
    }
  }
  return selected;
}

bool sportsWindowActive(JsonObjectConst sports, bool currentEligible,
                        bool previousEligible, time_t timestamp) {
  tm current = {};
  localtime_r(&timestamp, &current);
  const int minute = current.tm_hour * 60 + current.tm_min;
  const int on = resolveTimeExpression(sports["on"], current, timestamp);
  const int off = resolveTimeExpression(sports["off"], current, timestamp);
  if (on < 0 || off < 0) return false;
  if (on < off) return currentEligible && minute >= on && minute < off;
  return (currentEligible && minute >= on) ||
         (previousEligible && minute < off);
}

String localDateKey(const tm& date) {
  char value[11];
  snprintf(value, sizeof(value), "%04d-%02d-%02d", date.tm_year + 1900,
           date.tm_mon + 1, date.tm_mday);
  return String(value);
}

bool oneOffDateEligible(JsonObjectConst rule, const tm& date) {
  return String(rule["date"] | "") == localDateKey(date);
}

bool oneOffWindowActive(JsonObjectConst rule, time_t timestamp) {
  tm current = {};
  tm previous = {};
  localtime_r(&timestamp, &current);
  const time_t previousTimestamp = timestamp - 86400;
  localtime_r(&previousTimestamp, &previous);
  const int minute = current.tm_hour * 60 + current.tm_min;
  const int on = resolveTimeExpression(rule["on"], current, timestamp);
  const int off = resolveTimeExpression(rule["off"], current, timestamp);
  if (on < 0 || off < 0) return false;
  if (on < off) {
    return oneOffDateEligible(rule, current) && minute >= on && minute < off;
  }
  return (oneOffDateEligible(rule, current) && minute >= on) ||
         (oneOffDateEligible(rule, previous) && minute < off);
}

ScheduleDecision evaluateSchedulesAt(JsonDocument& document,
                                     time_t timestamp) {
  ScheduleDecision result;
  tm current = {};
  tm previous = {};
  localtime_r(&timestamp, &current);
  const time_t previousTimestamp = timestamp - 86400;
  localtime_r(&previousTimestamp, &previous);

  for (JsonObjectConst rule : document["oneOff"].as<JsonArrayConst>()) {
    if (!(rule["enabled"] | true)) continue;
    const int on = resolveTimeExpression(rule["on"], current, timestamp);
    const int off = resolveTimeExpression(rule["off"], current, timestamp);
    const bool relevant = oneOffDateEligible(rule, current) ||
        (on >= off && oneOffDateEligible(rule, previous));
    if (!relevant) continue;
    result.managed = true;
    result.oneOff = true;
    result.priority = 2000;
    result.active = oneOffWindowActive(rule, timestamp);
    result.patternId = rule["patternId"] | 0;
    result.zoneMask = configuredZoneMask(rule);
    result.name = String(rule["name"] | "One-time override");
    return result;
  }

  JsonObjectConst sports = document["sports"].as<JsonObjectConst>();
  if ((sports["enabled"] | false) &&
      sports["teams"].is<JsonArray>() && sports["events"].is<JsonArray>()) {
    for (JsonObjectConst team : sports["teams"].as<JsonArrayConst>()) {
      if (!(team["enabled"] | true)) continue;
      const JsonObjectConst currentEvent =
          sportsEventForDate(sports, team, current);
      const JsonObjectConst previousEvent =
          sportsEventForDate(sports, team, previous);
      if (currentEvent.isNull() && previousEvent.isNull()) continue;
      result.managed = true;
      result.sports = true;
      result.priority = 1000;
      result.active = sportsWindowActive(
          sports, !currentEvent.isNull(), !previousEvent.isNull(), timestamp);
      result.patternId = team["patternId"] | 0;
      result.zoneMask = configuredZoneMask(sports);
      const JsonObjectConst event =
          !currentEvent.isNull() ? currentEvent : previousEvent;
      result.name = String(team["name"] | "Sports") + " · " +
                    String(event["gameName"] | "Game day");
      return result;
    }
  }

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
         String(decision.oneOff) + ":" + String(decision.sports) + ":" +
         String(decision.holiday) + ":" +
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

    for (JsonObjectConst rule : document["oneOff"].as<JsonArrayConst>()) {
      if (!(rule["enabled"] | true) || !oneOffDateEligible(rule, date)) {
        continue;
      }
      const int on = resolveTimeExpression(rule["on"], date, noon);
      const int off = resolveTimeExpression(rule["off"], date, noon);
      if (on < 0 || off < 0) continue;
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, 0), nearest);
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, on), nearest);
      considerScheduleBoundary(document, now,
                               localScheduleBoundary(date, off, on >= off),
                               nearest);
    }

    JsonObjectConst sports = document["sports"].as<JsonObjectConst>();
    if ((sports["enabled"] | false) && sports["teams"].is<JsonArray>()) {
      bool eligible = false;
      for (JsonObjectConst team : sports["teams"].as<JsonArrayConst>()) {
        if ((team["enabled"] | true) &&
            !sportsEventForDate(sports, team, date).isNull()) {
          eligible = true;
          break;
        }
      }
      if (eligible) {
        const int on = resolveTimeExpression(sports["on"], date, noon);
        const int off = resolveTimeExpression(sports["off"], date, noon);
        considerScheduleBoundary(document, now,
                                 localScheduleBoundary(date, 0), nearest);
        considerScheduleBoundary(document, now,
                                 localScheduleBoundary(date, 0, 1), nearest);
        if (on >= 0 && off >= 0) {
          considerScheduleBoundary(document, now,
                                   localScheduleBoundary(date, on), nearest);
          considerScheduleBoundary(document, now,
                                   localScheduleBoundary(date, off,
                                                         on >= off), nearest);
        }
      }
    }

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
  JsonDocument existing;
  String error;
  if (readScheduleDocument(existing, error) && !existing["sports"].is<JsonObject>()) {
    JsonDocument defaults;
    if (!deserializeJson(defaults, kDefaultSchedules)) {
      existing["sports"].set(defaults["sports"]);
      String migrated;
      serializeJson(existing, migrated);
      saveScheduleDocument(migrated, error);
    }
  }
  loadScheduleLocation();
}

bool validLayoutDocument(JsonDocument& document, String& error) {
  if (!document.is<JsonObject>() || !document["paths"].is<JsonArray>()) {
    error = "Layout JSON must contain a paths array";
    return false;
  }
  const double width = document["width"] | 0.0;
  const double height = document["height"] | 0.0;
  JsonArrayConst paths = document["paths"].as<JsonArrayConst>();
  if (width <= 0 || width > 10000 || height <= 0 || height > 10000 ||
      paths.size() > 64) {
    error = "Layout dimensions or path count are invalid";
    return false;
  }
  for (JsonObjectConst path : paths) {
    const String id = path["id"] | "";
    const String deviceId = path["deviceId"] | "";
    const int zone = path["zone"] | -1;
    const int count = path["count"] | 0;
    const double scale = path["spatialScale"] | 0.0;
    JsonArrayConst points = path["points"].as<JsonArrayConst>();
    if (id.isEmpty() || id.length() > 40 || deviceId.isEmpty() ||
        deviceId.length() > 40 || zone < 0 || zone > 31 || count < 1 ||
        count > 10000 || scale < 0.1 || scale > 10 || points.size() < 2 ||
        points.size() > 16) {
      error = String("Invalid layout path: ") + id;
      return false;
    }
    for (JsonArrayConst point : points) {
      if (point.size() != 2) {
        error = String("Invalid point in layout path: ") + id;
        return false;
      }
      const double x = point[0] | -10001.0;
      const double y = point[1] | -10001.0;
      if (x < 0 || x > width || y < 0 || y > height) {
        error = String("Layout point is outside the canvas: ") + id;
        return false;
      }
    }
  }
  return true;
}

bool saveLayoutDocument(const String& json, String& error) {
  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, json);
  if (parseError || !validLayoutDocument(document, error)) {
    if (error.isEmpty()) error = String("Invalid layout JSON: ") + parseError.c_str();
    return false;
  }
  File file = LittleFS.open("/layout.tmp", "w");
  if (!file || serializeJson(document, file) == 0) {
    if (file) file.close();
    LittleFS.remove("/layout.tmp");
    error = "Unable to write layout storage";
    return false;
  }
  file.close();
  LittleFS.remove("/layout.backup");
  if (LittleFS.exists("/layout.json")) {
    LittleFS.rename("/layout.json", "/layout.backup");
  }
  if (!LittleFS.rename("/layout.tmp", "/layout.json")) {
    LittleFS.rename("/layout.backup", "/layout.json");
    error = "Unable to activate layout";
    return false;
  }
  LittleFS.remove("/layout.backup");
  return true;
}

void initializeLayoutStorage() {
  if (LittleFS.exists("/layout.json")) return;
  JsonDocument document;
  document["version"] = 1;
  document["width"] = 100;
  document["height"] = 100;
  document["units"] = "ft";
  JsonArray paths = document["paths"].to<JsonArray>();
  for (uint8_t zone = 0; zone < kZoneCount; ++zone) {
    JsonObject path = paths.add<JsonObject>();
    path["id"] = String("local-zone-") + zone;
    path["deviceId"] = chipId;
    path["zone"] = zone;
    path["name"] = zones[zone].name;
    path["count"] = zones[zone].count;
    path["reverse"] = false;
    path["spatialScale"] = 1.0;
    path["layer"] = zone;
    JsonArray points = path["points"].to<JsonArray>();
    JsonArray start = points.add<JsonArray>();
    start.add(10);
    start.add(15 + zone * 15);
    JsonArray end = points.add<JsonArray>();
    end.add(90);
    end.add(15 + zone * 15);
  }
  File file = LittleFS.open("/layout.json", "w");
  if (file) {
    serializeJson(document, file);
    file.close();
  }
}

void handleGetLayout() {
  File file = LittleFS.open("/layout.json", "r");
  if (!file) {
    sendText(404, "Layout storage is unavailable");
    return;
  }
  addCorsHeaders();
  server.streamFile(file, "application/json");
  file.close();
}

void handleSaveLayout() {
  if (!server.hasArg("plain")) {
    sendText(400, "Missing layout JSON body");
    return;
  }
  String error;
  if (!saveLayoutDocument(server.arg("plain"), error)) {
    sendText(400, error);
    return;
  }
  sendText(200, "Layout saved");
}

bool saveScheduleDocument(const String& json, String& error) {
  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, json);
  if (parseError || !document.is<JsonObject>() ||
      !document["weekly"].is<JsonArray>() ||
      !document["holidays"].is<JsonArray>() ||
      (!document["oneOff"].isNull() && !document["oneOff"].is<JsonArray>())) {
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

void streamLittleFsFile(const char* path, const char* fallback) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    server.sendContent(fallback);
    return;
  }
  char buffer[1024];
  while (file.available()) {
    const size_t count = file.readBytes(buffer, sizeof(buffer));
    if (count == 0) break;
    server.sendContent(buffer, count);
  }
  file.close();
}

void handleBackupDownload() {
  JsonDocument settingsDocument;
  JsonObject settings = settingsDocument.to<JsonObject>();
  settings["brightness"] = brightness;
  settings["automaticUpdates"] = automaticUpdates;
  settings["compatibilityApEnabled"] = compatibilityApEnabled;
  JsonObject power = settings["powerLimit"].to<JsonObject>();
  power["enabled"] = automaticPowerLimit;
  power["volts"] = configuredLedVolts;
  power["milliamps"] = configuredPowerMilliamps;
  power["milliampsPerPixel"] = configuredMilliampsPerPixel;
  JsonObject wled = settings["wledSync"].to<JsonObject>();
  wled["enabled"] = wledSync.enabled;
  wled["destination"] = wledSync.destination;
  wled["pixelCount"] = wledSync.pixelCount;
  wled["sourceZone"] = wledSync.sourceZone;
  JsonArray savedZones = settings["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < kZoneCount; ++i) {
    JsonObject zone = savedZones.add<JsonObject>();
    zone["enabled"] = zones[i].enabled;
    zone["count"] = zones[i].count;
    zone["name"] = zones[i].name;
    zone["order"] = zones[i].order;
    zone["protocol"] = zones[i].protocol;
    zone["gpio"] = zones[i].gpio;
    zone["physicalPixelsPerFixture"] =
        zones[i].physicalPixelsPerFixture;
  }
  String serializedSettings;
  serializeJson(settingsDocument, serializedSettings);

  addCorsHeaders();
  server.sendHeader("Content-Disposition",
                    "attachment; filename=leaflights-backup.json");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent(String("{\"format\":\"leaflights-backup\","
                            "\"version\":1,\"firmware\":\"") +
                     kFirmwareVersion + "\",\"settings\":");
  server.sendContent(serializedSettings);
  server.sendContent(",\"patterns\":");
  streamLittleFsFile("/patterns.json", "[]");
  server.sendContent(",\"schedules\":");
  streamLittleFsFile("/schedules.json", kDefaultSchedules);
  server.sendContent(",\"layout\":");
  streamLittleFsFile("/layout.json",
                     "{\"version\":1,\"width\":100,\"height\":100,"
                     "\"units\":\"ft\",\"paths\":[]}");
  server.sendContent("}");
  server.sendContent("");
}

void handleRestoreSettings() {
  if (!server.hasArg("plain")) {
    sendText(400, "Missing restore settings JSON");
    return;
  }
  JsonDocument document;
  const DeserializationError parseError =
      deserializeJson(document, server.arg("plain"));
  if (parseError || !document.is<JsonObject>() ||
      !document["zones"].is<JsonArray>() ||
      !document["wledSync"].is<JsonObject>()) {
    sendText(400, "Backup settings are invalid");
    return;
  }

  const int restoredBrightness = document["brightness"] | -1;
  JsonObjectConst restoredPower = document["powerLimit"].as<JsonObjectConst>();
  const int restoredPowerVolts = restoredPower.isNull()
                                      ? kPowerSupplyVolts
                                      : (restoredPower["volts"] | -1);
  const int restoredPowerMilliamps = restoredPower.isNull()
                                           ? kPowerSupplyMilliamps
                                           : (restoredPower["milliamps"] | -1);
  const int restoredPixelMilliamps = restoredPower.isNull()
                                         ? 55
                                         : (restoredPower["milliampsPerPixel"] | 55);
  JsonObjectConst restoredWled = document["wledSync"].as<JsonObjectConst>();
  String restoredDestination = restoredWled["destination"] | "";
  restoredDestination.trim();
  const int restoredPixels = restoredWled["pixelCount"] | -1;
  const int restoredSource = restoredWled["sourceZone"] | -2;
  JsonArrayConst restoredZones = document["zones"].as<JsonArrayConst>();
  if (restoredBrightness < 1 || restoredBrightness > kMaximumBrightness ||
      restoredDestination.isEmpty() || restoredDestination.length() > 64 ||
      restoredDestination.indexOf("://") >= 0 ||
      restoredDestination.indexOf(' ') >= 0 || restoredPixels < 1 ||
      restoredPixels > kMaxLogicalLeds || restoredSource < -1 ||
      restoredSource >= kZoneCount || restoredZones.size() == 0 ||
      restoredPowerVolts < 5 || restoredPowerVolts > 36 ||
      restoredPowerMilliamps < 100 || restoredPowerMilliamps > 30000 ||
      restoredPixelMilliamps < 1 || restoredPixelMilliamps > 100) {
    sendText(400, "Backup settings contain invalid controller values");
    return;
  }
  for (uint8_t i = 0; i < kZoneCount && i < restoredZones.size(); ++i) {
    JsonObjectConst restored = restoredZones[i].as<JsonObjectConst>();
    const int count = restored["count"] | -1;
    const String name = restored["name"] | "";
    const String order = restored["order"] | "";
    const String protocol = restored["protocol"] | "UCS1903";
    const int gpio = restored["gpio"] | kZonePins[i];
    const int physicalPixels = restored["physicalPixelsPerFixture"] | 2;
    if (count < 1 || count > kMaxLogicalLeds || name.isEmpty() ||
        name.length() > 24 || !validColorOrder(order) ||
        !validLedProtocol(protocol) || !validZoneGpio(gpio) ||
        physicalPixels < 1 ||
        physicalPixels > kMaxPhysicalPixelsPerFixture) {
      sendText(400, String("Backup contains invalid zone ") + (i + 1));
      return;
    }
  }
  for (uint8_t i = 0; i < kZoneCount && i < restoredZones.size(); ++i) {
    JsonObjectConst first = restoredZones[i].as<JsonObjectConst>();
    if (!(first["enabled"] | false)) continue;
    for (uint8_t j = i + 1; j < kZoneCount && j < restoredZones.size(); ++j) {
      JsonObjectConst second = restoredZones[j].as<JsonObjectConst>();
      if ((second["enabled"] | false) &&
          (first["gpio"] | kZonePins[i]) ==
              (second["gpio"] | kZonePins[j])) {
        sendText(400, "Backup enables multiple zones on the same GPIO");
        return;
      }
    }
  }

  brightness = restoredBrightness;
  automaticUpdates = document["automaticUpdates"] | false;
  compatibilityApEnabled = document["compatibilityApEnabled"] | false;
  automaticPowerLimit = restoredPower.isNull()
                            ? false
                            : (restoredPower["enabled"] | false);
  configuredLedVolts = restoredPowerVolts;
  configuredPowerMilliamps = restoredPowerMilliamps;
  configuredMilliampsPerPixel = restoredPixelMilliamps;
  wledSync.enabled = restoredWled["enabled"] | false;
  wledSync.destination = restoredDestination;
  wledSync.pixelCount = restoredPixels;
  wledSync.sourceZone = restoredSource;
  for (uint8_t i = 0; i < kZoneCount && i < restoredZones.size(); ++i) {
    JsonObjectConst restored = restoredZones[i].as<JsonObjectConst>();
    zones[i].enabled = restored["enabled"] | false;
    zones[i].count = restored["count"];
    zones[i].name = String(restored["name"] | "");
    zones[i].order = String(restored["order"] | "GBR");
    zones[i].protocol = String(restored["protocol"] | "UCS1903");
    zones[i].gpio = restored["gpio"] | kZonePins[i];
    zones[i].physicalPixelsPerFixture =
        restored["physicalPixelsPerFixture"] | 2;
  }

  preferences.putUChar("brightness", brightness);
  preferences.putBool("autoUpdate", automaticUpdates);
  preferences.putBool("compatAp", compatibilityApEnabled);
  preferences.putBool("pwrLimit", automaticPowerLimit);
  preferences.putUChar("pwrVolts", configuredLedVolts);
  preferences.putUShort("pwrMa", configuredPowerMilliamps);
  preferences.putUChar("pixelMa", configuredMilliampsPerPixel);
  preferences.putBool("ddpEnabled", wledSync.enabled);
  preferences.putString("ddpDest", wledSync.destination);
  preferences.putUShort("ddpPixels", wledSync.pixelCount);
  preferences.putChar("ddpSource", wledSync.sourceZone);
  preferences.putUShort("patternLibrary", kPatternLibraryVersion);
  saveZoneConfiguration();
  sendText(200, "Backup restored; rebooting in 2 seconds");
  scheduleRestart(2000);
}

bool refreshSportsScheduleFeed(String& error) {
  JsonDocument schedules;
  if (!readScheduleDocument(schedules, error)) return false;
  JsonObject sports = schedules["sports"].as<JsonObject>();
  if (sports.isNull() || !(sports["enabled"] | false)) {
    sportsFeedStatus = "Sports automation disabled";
    return true;
  }
  JsonArray teams = sports["teams"].as<JsonArray>();
  if (teams.isNull() || teams.size() == 0) {
    error = "Select at least one sports team";
    return false;
  }

  JsonArray cached = sports["events"].to<JsonArray>();
  uint16_t added = 0;
  const time_t now = time(nullptr);
  for (JsonObjectConst subscription : teams) {
    if (!(subscription["enabled"] | true)) continue;
    const String teamKey = subscription["key"] | "";
    if (teamKey.isEmpty() || teamKey.length() > 80 ||
        teamKey.indexOf("..") >= 0 || teamKey.indexOf('/') >= 0) {
      continue;
    }
    WiFiClientSecure client;
    HTTPClient http;
    const String url = String(kSportsFeedBaseUrl) + teamKey + ".json";
    if (!beginGithubRequest(http, client, url, error)) return false;
    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
      error = status > 0 ? String("Schedule feed returned HTTP ") + status +
                               " for " + String(subscription["name"] | teamKey)
                         : String("Schedule feed connection failed: ") +
                               HTTPClient::errorToString(status);
      http.end();
      return false;
    }
    JsonDocument feed;
    const DeserializationError parseError =
        deserializeJson(feed, http.getStream());
    http.end();
    if (parseError || !feed["events"].is<JsonArray>()) {
      error = String("Invalid schedule feed for ") +
              String(subscription["name"] | teamKey);
      return false;
    }
    for (JsonObjectConst event : feed["events"].as<JsonArrayConst>()) {
      if (added >= 500) break;
      const time_t eventStart = event["startEpoch"] | 0;
      if (eventStart < now - 86400 || eventStart > now + 120 * 86400) {
        continue;
      }
      JsonObject item = cached.add<JsonObject>();
      item["id"] = event["id"] | "";
      item["teamKey"] = teamKey;
      item["gameName"] = event["name"] | "Game";
      item["start"] = event["start"] | "";
      item["startEpoch"] = event["startEpoch"] | 0;
      item["seasonType"] = event["seasonType"] | "";
      item["status"] = event["status"] | "";
      item["cancelled"] = event["cancelled"] | false;
      for (JsonObjectConst participant : event["teams"].as<JsonArrayConst>()) {
        if (String(participant["key"] | "") == teamKey) {
          item["homeAway"] = participant["homeAway"] | "";
          break;
        }
      }
      ++added;
    }
  }

  sports["lastUpdated"] = now;
  String serialized;
  serializeJson(schedules, serialized);
  if (!saveScheduleDocument(serialized, error)) return false;
  preferences.putULong64("sportsFetch", static_cast<uint64_t>(now));
  sportsFeedStatus = String("Updated ") + added + " upcoming team events";
  return true;
}

void serviceSportsScheduleFeed() {
  if (millis() < 20000 || millis() - lastSportsFeedCheckAt < 3600000) return;
  lastSportsFeedCheckAt = millis();
  if (WiFi.status() != WL_CONNECTED || time(nullptr) <= 1700000000) return;
  JsonDocument schedules;
  String error;
  if (!readScheduleDocument(schedules, error)) return;
  JsonObjectConst sports = schedules["sports"].as<JsonObjectConst>();
  if (!(sports["enabled"] | false)) return;
  const time_t lastUpdated = sports["lastUpdated"] | 0;
  if (lastUpdated > 0 && time(nullptr) - lastUpdated < 21600) return;
  if (!refreshSportsScheduleFeed(error)) sportsFeedStatus = error;
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
  runtime["sportsFeedStatus"] = sportsFeedStatus;
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
      ? String(decision.oneOff ? "One-time: " :
               decision.sports ? "Sports: " :
               decision.holiday ? "Holiday: " : "Weekly: ") + decision.name
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
  server.on("/api/layout", HTTP_GET, []() {
    if (requireWebUiAuth()) handleGetLayout();
  });
  server.on("/api/layout", HTTP_POST, []() {
    if (requireWebUiAuth()) handleSaveLayout();
  });
  server.on("/api/sports/refresh", HTTP_POST, []() {
    if (!requireWebUiAuth()) return;
    String error;
    if (!refreshSportsScheduleFeed(error)) {
      sportsFeedStatus = error;
      sendText(502, error);
      return;
    }
    sendText(200, sportsFeedStatus);
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
  server.on("/api/backup", HTTP_GET, []() {
    if (requireWebUiAuth()) handleBackupDownload();
  });
  server.on("/api/restore-settings", HTTP_POST, []() {
    if (requireWebUiAuth()) handleRestoreSettings();
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

  // Realtime DDP depends on predictable packet latency. ESP32 modem sleep can
  // batch outgoing UDP traffic and produce visible pauses followed by bursts.
  if (WiFi.status() == WL_CONNECTED) WiFi.setSleep(false);
  resolvedDdpDestinationValid = false;
  resolvedDdpDestinationName = "";

  if (WiFi.status() == WL_CONNECTED && MDNS.begin("leaflights")) {
    MDNS.addService("http", "tcp", 80);
  }
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  zone <0-5> <r> <g> <b>");
  Serial.printf("  brightness <1-%u>\n", kMaximumBrightness);
  Serial.println("  wled on|off|status");
  Serial.println("  wifi");
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
  } else if (line == "wifi") {
    Serial.printf("WiFi status=%d mode=%d LAN=%s RSSI=%d AP=%s clients=%u\n",
                  static_cast<int>(WiFi.status()),
                  static_cast<int>(WiFi.getMode()),
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPgetStationNum());
  } else if (line == "wled status") {
    Serial.printf("WLED sync %s: %s\n",
                  wledSync.enabled ? "enabled" : "disabled",
                  lastDdpStatus.c_str());
  } else if (line == "wled off" || line == "wled on") {
    wledSync.enabled = line.endsWith("on");
    preferences.putBool("ddpEnabled", wledSync.enabled);
    lastDdpStatus = wledSync.enabled ? "Waiting for frame" : "Disabled";
    lastDdpFrameAt = 0;
    Serial.printf("WLED sync %s and saved\n",
                  wledSync.enabled ? "enabled" : "disabled");
  } else if (sscanf(line.c_str(), "zone %d %d %d %d",
                    &zone, &red, &green, &blue) == 4) {
    if (zone < 0 || zone >= kZoneCount || !zoneRegistered[zone]) {
      Serial.println("Zone is disabled or invalid");
      return;
    }
    fillZone(zone, CRGB(constrain(red, 0, 255), constrain(green, 0, 255),
                        constrain(blue, 0, 255)));
    showLeds();
    Serial.println("Zone updated");
  } else if (sscanf(line.c_str(), "brightness %d", &value) == 1 &&
             value >= 1 && value <= kMaximumBrightness) {
    brightness = value;
    FastLED.setBrightness(brightness);
    preferences.putUChar("brightness", brightness);
    showLeds();
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
  initializeLayoutStorage();
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
  serviceSportsScheduleFeed();
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
