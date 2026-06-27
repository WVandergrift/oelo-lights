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
lightning, march, river, shuffle, split, sprinkle, streak,
storm, takeover, twinkle, off
```

The replacement engines are compatible approximations, not decompiled vendor
source.

## Offline saved patterns

### `GET /getPatterns`

Returns the saved-pattern JSON array from LittleFS.

### `GET /savePatterns?json=...`

Validates and stores a URL-encoded JSON array. A fresh filesystem is seeded
with `Fourth of July: Fast Fireworks`.

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
GET  /api/color
GET  /api/brightness?value=<1-255>
GET  /api/off
GET  /api/preset/fast-fireworks
POST /api/patterns
POST /api/zones
POST /api/network
POST /api/wled-sync
```

`POST /api/wled-sync` accepts form fields `enabled`, `destination`,
`pixelCount`, and `sourceZone`. A source of `-1` automatically uses the first
zone in the active pattern. These settings are stored in NVS.
