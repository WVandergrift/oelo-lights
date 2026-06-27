# Research method and confidence

## Inputs examined privately

- Lighting by LeafFilter Android XAPK/APK;
- the app's packaged HTML and JavaScript;
- publicly served Oelo web assets;
- Oelo ESP32 firmware version 1.78;
- photographs of the original controller PCB;
- a live TinyS3 replacement board.

Those third-party binaries and decompiled trees are not included in this
repository.

## Tools and approach

- XAPK extraction and Apktool decoding;
- JavaScript inspection for Socket.IO events and local HTTP routes;
- ESP32 image segment mapping;
- `strings`, radare2, and Xtensa disassembly;
- comparison with FastLED's public chipset implementations;
- PCB component and trace inspection;
- on-device PlatformIO builds and HTTP/serial smoke tests.

## Verified from the vendor firmware

- six output GPIOs on the original ESP32;
- UCS1903/400 kbit/s timing tuple;
- per-zone maximum allocation;
- all six RGB byte-order branches and GBR default;
- logical fixture count doubled to physical pixels;
- local pattern/configuration endpoint names and parameters;
- saved pattern storage and pattern engine names.

## Verified from the app

- `OELO_1-23.0` and `172.24.1.1` are hard-coded for local control;
- BLE discovery searches for a device named `Oelo`;
- BLE failure does not automatically enter AP mode;
- Local AP Control uses `/getController`, `/saveController`, `/setPattern`,
  `/getPatterns`, `/savePatterns`, `/scanNetworksRSSI`, and `/saveNetwork`;
- the app's fixture-count limit is 1,000;
- saved offline profiles are controller-hosted.

## Verified on the sample firmware

- TinyS3 build and upload;
- AP and web UI operation;
- NVS configuration persistence across reboot;
- LittleFS pattern persistence and Fast Fireworks seed;
- app-shaped JSON and exact app-style HTTP requests;
- Lower 158 / Upper 114 zone configuration;
- all exposed movement names start and remain HTTP-responsive;
- outputs return to off and brightness returns to the configured safe value.

## Known gaps

- exact visual parity for every vendor animation;
- long-duration tests at maximum fixture count across all six zones;
- production enclosure, fused power distribution, and EMC testing;
- authenticated LAN/AP control;
- BLE provisioning compatibility;
- vendor-cloud/MQTT compatibility;
- OTA and signed update support;
- a native WLED UCS1903 timing type.

Claims in this repository should be read according to these boundaries. The
replacement effect implementations are original approximations; they are not
vendor source code.
