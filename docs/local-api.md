# Local controller HTTP API

The original app uses unauthenticated HTTP GET requests against the controller
at `172.24.1.1`. The sample firmware implements the routes required for local
control and configuration.

## Controller configuration

### `GET /getController`

Returns a JSON array with one object per zone. Important properties:

```json
{
  "chipID": "controller identifier",
  "fw": 1.78,
  "num": 0,
  "name": "Lower",
  "enabled": true,
  "ledCnt": 158,
  "rgbOrder": "GBR",
  "slaveTo": "none",
  "pattern": "off"
}
```

### `GET /saveController?json=...`

Accepts the URL-encoded array returned by the app's controller settings UI,
saves zone configuration in NVS, and reboots.

## Pattern control

### `GET /setPattern?...`

Known parameters:

```text
command=setPattern
patternType=<movement name>
speed=<1-20>
gap=<number>
direction=<F-or-R>
num_colors=<number>
colors=<r,g,b,... or URL-encoded r&g&b...>
pause=<number>
num_zones=<number>
zones=<comma-separated zero-based zone numbers>
other=<movement-specific number>
```

Supported movement names:

```text
stationary, arcade, pacman, blend, bolt, chase, fade, fill,
fireworks, lightning, march, river, shuffle, split, sprinkle, streak,
storm, takeover, twinkle, off
```

The replacement engines are compatible approximations, not decompiled vendor
source.

## Offline saved patterns

### `GET /getPatterns`

Returns the saved-pattern JSON array from LittleFS.

### `GET /savePatterns?json=...`

Validates and stores a URL-encoded JSON array. A fresh filesystem receives the
built-in Independence Day collection, including the recovered
`Fourth of July: Fast Fireworks` profile. Versioned firmware upgrades merge
missing built-ins by name and preserve the existing array.

### `POST /api/patterns`

Accepts the same JSON array in the request body. The replacement web UI uses
this route so larger palettes do not need to fit in a URL. The original GET
route remains available for LeafFilter app compatibility.

## Wi-Fi setup

### `GET /scanNetworksRSSI`

Returns comma-separated `SSID***RSSI` entries expected by the app.

### `GET /saveNetwork?ssid=...&pw=...`

Stores home Wi-Fi credentials in NVS and reboots. The compatibility AP remains
available in AP+STA mode.

## Project browser API

The sample's own web UI additionally uses:

```text
GET  /api/status
GET  /api/auth-status
POST /api/login
POST /api/setup
POST /api/web-password
GET  /api/color
GET  /api/brightness?value=<1-204>
GET  /api/off
GET  /api/preset/fast-fireworks
POST /api/patterns
POST /api/zones
POST /api/network
POST /api/restart
POST /api/wled-sync
POST /api/update
GET  /api/releases
POST /api/install-release
POST /api/automatic-updates
```

`POST /api/wled-sync` accepts form fields `enabled`, `destination`,
`pixelCount`, and `sourceZone`. A source of `-1` automatically uses the first
zone in the active pattern. These settings are stored in NVS.

`GET /api/auth-status` reports whether the optional web login is configured and
whether the current browser session is authenticated. `POST /api/login`
accepts `password` and returns a persistent HttpOnly, SameSite session cookie.

`POST /api/setup` completes first-time onboarding. It accepts six zone records,
home Wi-Fi credentials, optional compatibility-network settings, and the
optional web password. `POST /api/web-password` changes or removes that web
login from an authenticated session.

`POST /api/network` stores home-network settings, the optional
`compatibilityApEnabled` flag, and an optional 8–63-character
`compatibilityApPassword`. A blank password preserves the current WPA2
passphrase. Disabling the compatibility AP requires a configured home network.
If that network fails at boot, the firmware starts a protected temporary
recovery AP rather than leaving the controller unreachable.

`POST /api/restart` schedules a controlled controller restart.

`POST /api/update` accepts a multipart `.bin` firmware image. The image is
streamed to the inactive OTA slot, verified by the ESP32 Update library,
activated, and followed by a controlled reboot.

Project management endpoints use the optional web-interface session. No
separate firmware-update password or HTTP Basic prompt is used.

`GET /api/releases` retrieves the five newest public GitHub releases through a
certificate-validated connection and returns only compatible
`leaf-lights-tinys3.bin` assets with a GitHub-provided SHA-256 digest.

`POST /api/install-release` accepts
the selected release metadata returned by `/api/releases`, restricts downloads
to this repository's release path, downloads through validated HTTPS, verifies
size and SHA-256, and activates the inactive OTA slot.

`POST /api/automatic-updates` configures the opt-in update check.
When enabled, the controller checks every six hours while connected to home
Wi-Fi and installs only newer, non-prerelease semantic versions.
