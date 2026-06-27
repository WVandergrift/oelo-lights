# Oelo / LeafFilter lights research and replacement controller

This repository documents the protocol used by an Oelo/Lighting by LeafFilter
controller and provides an open ESP32-S3 sample controller for existing
UCS1903 lighting installations.

The sample firmware currently runs on an Unexpected Maker TinyS3 and provides:

- six configurable UCS1903 outputs at 400 kbit/s;
- Oelo's two-physical-pixels-per-fixture mapping;
- persistent zone names, fixture counts, enable state, and color order;
- a phone-first browser interface with animated pattern previews, daily controls,
  pattern editing, and advanced installation settings;
- compatibility with the LeafFilter app's offline Local AP Control mode;
- controller-hosted offline pattern profiles and a ten-preset Independence Day
  collection;
- non-blocking approximations of all 17 movement types exposed by the app plus
  a custom multi-burst fireworks engine;
- optional realtime DDP broadcast of rendered frames to WLED controllers;
- password-protected browser firmware updates using the ESP32's inactive OTA
  application slot;
- GitHub release discovery, release-note review, selected-version installation,
  and opt-in automatic stable updates;
- the recovered **Fourth of July: Fast Fireworks** profile and nine original
  red, white, and blue presets.

## Current status

The following installation has been exercised on-device:

| Zone | Name | Fixtures | Color order | TinyS3 GPIO |
|---:|---|---:|---|---:|
| 1 | Lower | 158 | GBR | 1 |
| 2 | Upper | 114 | GBR | 2 |
| 3–6 | Disabled | — | GBR | 4, 5, 6, 7 |

The web server, persistent settings, Local AP API, pattern persistence, and all
17 pattern names have passed runtime smoke tests. Exact visual parity with the
vendor's animation engines is not claimed; Fast Fireworks uses the recovered
name, palette, type, direction, and speed with our non-blocking twinkle engine.

## Quick start

Requirements:

- Unexpected Maker TinyS3;
- PlatformIO;
- 5 V `74HCT244` or `74AHCT244` level shifting for installed use;
- correctly sized external LED power supply;
- common ground between the ESP32, level shifter, and lights.

Build and upload:

```sh
cd firmware/tinys3
pio run -e um_tinys3
pio run -e um_tinys3 -t upload
```

After flashing, connect to the open `OELO_1-23.0` access point and open:

```text
http://172.24.1.1
```

Select one or more zones and press **Play** on a saved pattern, or use the
LeafFilter app workflow in [docs/leaf-filter-app.md](docs/leaf-filter-app.md).
WLED receiver setup is covered in [docs/wled-sync.md](docs/wled-sync.md).

## First electrical test

Test only one data output before connecting the full installation:

```text
TinyS3 GPIO1 -> 220–470 ohm resistor -> LED DATA 1
TinyS3 GND  --------------------------> LED GND
```

A direct 3.3 V data signal is acceptable only as a short-cable experiment. The
original PCB uses a 5 V HCT244 non-inverting buffer, so a replacement should do
the same. Never power the lights from the TinyS3.

## Documentation

- [Recovered LED protocol](docs/protocol.md)
- [Original and replacement hardware](docs/hardware.md)
- [Local controller HTTP API](docs/local-api.md)
- [LeafFilter app offline workflow](docs/leaf-filter-app.md)
- [Patterns and Fast Fireworks](docs/patterns.md)
- [WLED realtime synchronization](docs/wled-sync.md)
- [GitHub releases and automatic updates](docs/releases.md)
- [Firmware update path](docs/firmware-update-path.md)
- [Research method and confidence](docs/research.md)
- [Sample firmware details](firmware/tinys3/README.md)
- [Security considerations](SECURITY.md)

## Repository boundaries

This repository contains original documentation and replacement firmware. It
does **not** redistribute the vendor APK, decompiled application, vendor
firmware binaries, flash backups, account data, credentials, or device-specific
identifiers.

Oelo and LeafFilter are trademarks of their respective owners. This project is
independent and is not affiliated with or endorsed by either company.

## License

Original code and documentation in this repository are available under the
[MIT License](LICENSE). This license does not apply to third-party products,
firmware, applications, trademarks, or protocols discussed by the project.
