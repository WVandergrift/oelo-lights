# Firmware update path

## Android package behavior

The analyzed `Lighting by LeafFilter` Android package is a Cordova application.
Its packaged client uses:

```text
Socket.IO host: https://oelo.link
Local AP IP:    172.24.1.1
```

The client emits `getControllerFirmware` to obtain firmware information. The
administrative web client contains a firmware catalog and emits
`updateControllerFirmware` with a controller chip ID and selected URL.

## Firmware 1.78

The catalog entry analyzed during this research identified version 1.78 at:

```text
http://glowlabs.co/oelo/fw178.bin
```

HTTPS was also available. The analyzed image SHA-256 was:

```text
60f9b1bd24e48a28a96f8876431045f2f0a698d0b50fa3bb990f4c2794faa0a3
```

The image is an ESP32 application containing six loadable segments and reports
ESP-IDF 4.4.5-era components. It contains strings and code paths for OTA,
MQTT-delivered update commands, FastLED, RMT, local patterns, and SPIFFS.

## Observed update flow

```text
app/admin UI
    -> oelo.link Socket.IO command
    -> controller-specific cloud command
    -> controller downloads supplied firmware URL
    -> ESP32 OTA update and reboot
```

The exact cloud authorization and broker configuration are intentionally not
replicated by this project. The replacement firmware uses PlatformIO upload
for development and does not currently implement OTA.

## Distribution boundary

This repository records the URL, version, and hash for reproducibility but
does not redistribute the vendor firmware image.
