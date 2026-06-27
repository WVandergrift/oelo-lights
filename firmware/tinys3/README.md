# ESP32 UCS1903 proof of concept

This Arduino sketch targets the Unexpected Maker TinyS3 and reproduces the LED-output layer recovered from Oelo firmware 1.78.

## Recovered configuration

| Zone | Original ESP32 GPIO | TinyS3 GPIO | Protocol |
|---:|---:|---:|---|
| 0 | 25 | 1 | UCS1903, 400 kHz |
| 1 | 13 | 2 | UCS1903, 400 kHz |
| 2 | 33 | 4 | UCS1903, 400 kHz |
| 3 | 14 | 5 | UCS1903, 400 kHz |
| 4 | 15 | 6 | UCS1903, 400 kHz |
| 5 | 26 | 7 | UCS1903, 400 kHz |

The original GPIOs are not all exposed on the TinyS3. The replacement mapping uses exposed GPIOs 1, 2, 4, 5, 6, and 7. It avoids GPIOs 0 and 3 because they are ESP32-S3 boot-strapping pins.

The firmware uses FastLED with timing parameters equivalent to `UCS1903` (`500 ns, 1500 ns, 500 ns`). Each configured logical Oelo LED is written twice as two consecutive physical RGB pixels. The firmware allows up to 1,000 logical LEDs per zone and allocates 2,000 `CRGB` entries per zone.

The configured color order can be `RGB`, `RBG`, `GRB`, `GBR`, `BRG`, or `BGR`; missing or invalid configuration defaults to `GBR`.

## Before connecting hardware

- Power the lighting from its correctly sized external supply, not the ESP32.
- Connect the ESP32 ground to the lighting/controller ground.
- Use a 5 V `74HCT244` or `74AHCT244` between the TinyS3 and the lighting data wires. The original PCB contains TI `HCT244` non-inverting buffers, confirming that it level-shifts the ESP32 signals from 3.3 V to 5 V.
- Start with one logical fixture and brightness 32, as configured in the sketch.
- Confirm the connector pinout with a meter or logic analyzer before attaching an ESP32 GPIO. The firmware identifies GPIO assignments, but not the external connector's physical pin numbering.

## Replacement wiring

The original six-position terminal block is labelled `LED DATA`, with channels 1 through 6 running top-to-bottom in the supplied controller-board photo. A separate terminal is labelled `LED GND`.

```text
TinyS3 GPIO 1 -> HCT244 input -> HCT244 output -> LED DATA 1
TinyS3 GPIO 2 -> HCT244 input -> HCT244 output -> LED DATA 2
TinyS3 GPIO 4 -> HCT244 input -> HCT244 output -> LED DATA 3
TinyS3 GPIO 5 -> HCT244 input -> HCT244 output -> LED DATA 4
TinyS3 GPIO 6 -> HCT244 input -> HCT244 output -> LED DATA 5
TinyS3 GPIO 7 -> HCT244 input -> HCT244 output -> LED DATA 6

TinyS3 GND -----+---------------------------> LED GND
                +-> HCT244 GND
5 V logic rail ----> HCT244 VCC
```

For an SN74HCT244-style pinout, connect pin 20 to 5 V and pin 10 to ground. Both active-low output enables (pins 1 and 19) must be low. Add a 100 nF ceramic capacitor directly between pins 20 and 10. The part is non-inverting, so the firmware requires no signal inversion.

Use an `HCT`/`AHCT` device, not a plain `HC244`: the TTL-compatible HCT input threshold is the reason a 3.3 V ESP32 can reliably drive a buffer powered at 5 V.

For the first test, enable only Zone 1, connect only LED DATA 1 through one
buffer channel, and leave the other outputs disconnected. Then send
`zone 0 16 0 0` at 115200 baud. At boot, the firmware registers every zone
marked enabled in persisted configuration.

## Browser and LeafFilter app control

Detailed Android workflow: [../../docs/leaf-filter-app.md](../../docs/leaf-filter-app.md)

The firmware always creates the open access point `OELO_1-23.0` at
`172.24.1.1`, matching the values hard-coded in the LeafFilter/Oelo Android
app. Connect a phone or computer to that network and open:

```text
http://172.24.1.1
```

The web page provides a phone-first daily-control screen, animated saved-pattern
previews, a pattern editor, multi-zone selection, per-zone hardware settings,
home Wi-Fi provisioning, diagnostics, and optional WLED realtime sync. Settings
are stored in ESP32 NVS and survive power loss. Saved patterns live in
LittleFS. When home Wi-Fi is configured, the same page is available at the
displayed LAN address and, where mDNS is supported,
`http://leaflights.local`.

The following original local-controller endpoints are implemented for app
compatibility:

```text
GET /getController
GET /saveController?json=...
GET /setPattern?...
GET /getPatterns
GET /savePatterns?json=...
GET /scanNetworksRSSI
GET /saveNetwork?ssid=...&pw=...
```

Saved patterns are persisted in LittleFS and exposed to the app's offline
"Your Patterns" screen. A fresh filesystem is seeded with the recovered
`Fourth of July: Fast Fireworks` preset.

The non-blocking demo engine accepts every movement type exposed by the
offline app: `stationary`, `arcade`/`pacman`, `blend`, `bolt`, `chase`, `fade`,
`fill`, `lightning`, `march`, `river`, `shuffle`, `split`, `sprinkle`, `streak`,
`storm`, `takeover`, `twinkle`, and `off`. These are compatible approximations;
the UCS1903 transport, fixture pairing, palette, direction, and speed inputs
match the recovered controller contract, while exact visual parity remains a
future reverse-engineering task for each movement engine.

## WLED realtime sync

The controller can broadcast the actual rendered RGB frames to WLED devices
using DDP on UDP port 4048. This is deliberately different from WLED's effect
state notifier: Oelo movement engines and WLED effect IDs do not have a
one-to-one mapping, so sending pixels preserves the Oelo colors and motion.

Configure home Wi-Fi first, then open **Settings → WLED realtime sync**:

- use `255.255.255.255` to broadcast to receivers on the local network, or a
  specific WLED IP address for unicast;
- set the virtual pixel count to the receiving strip length;
- use **Auto** to copy the first zone in the active pattern, or choose a fixed
  source zone;
- enable **Force max brightness** on each WLED receiver under its realtime/sync
  settings because the outgoing RGB frame already includes TinyS3 brightness.

Static colors are refreshed every 750 ms so WLED remains in realtime mode.
Animated patterns send each rendered frame, capped at roughly 33 frames per
second. Disabling sync stops the stream; WLED resumes its previous state after
its configured realtime timeout. See
[../../docs/wled-sync.md](../../docs/wled-sync.md) for limitations.

## Serial use

Build the TinyS3 PlatformIO environment and open a 115200-baud serial
terminal. Outputs remain off until a command or HTTP request is received.

```text
zone 0 255 0 0
brightness 16
off
status
```

If colors are swapped, select the correct color order in the browser or app
zone configuration and reboot.

The `um_tinys3_diagnostic` PlatformIO environment builds a serial-only image
for separating USB/boot faults from LED-driver faults.
