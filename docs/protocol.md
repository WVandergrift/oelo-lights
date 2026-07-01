# Recovered LED protocol

## Summary

Firmware version 1.78 drives six independent strings of clockless UCS1903 RGB
pixels. The replacement firmware additionally allows each zone to select a
FastLED WS281x-compatible controller, GPIO, color order, and physical mapping.

| Property | Recovered value |
|---|---|
| Chipset | UCS1903 |
| Bit rate | 400 kbit/s |
| Bit period | 2.5 microseconds |
| Zero bit | ~0.5 us high, ~2.0 us low |
| One bit | ~2.0 us high, ~0.5 us low |
| Pixel payload | 3 bytes, most-significant bit first |
| Reset/latch | At least 50 us low |
| Default color order | GBR |
| Logical-to-physical mapping | UCS1903 default: 1 fixture to 2 consecutive RGB pixels |

The recovered FastLED timing tuple is:

```text
T1 = 500 ns
T2 = 1500 ns
T3 = 500 ns
```

FastLED interprets this as:

```cpp
FastLED.addLeds<UCS1903, DATA_PIN, GBR>(pixels, physicalCount);
```

## Original output mapping

| Logical zone | Original ESP32 GPIO | Maximum logical fixtures | Maximum physical pixels |
|---:|---:|---:|---:|
| 0 | 25 | 1,000 | 2,000 |
| 1 | 13 | 1,000 | 2,000 |
| 2 | 33 | 1,000 | 2,000 |
| 3 | 14 | 1,000 | 2,000 |
| 4 | 15 | 1,000 | 2,000 |
| 5 | 26 | 1,000 | 2,000 |

The original firmware allocates 6,000 bytes per zone, corresponding to 2,000
three-byte `CRGB` entries.

## Fixture pairing

The configured `ledCnt` is treated as a logical fixture count. The controller
registers twice that many physical pixels and writes each logical color twice:

```text
fixture 0 -> physical pixels 0 and 1
fixture 1 -> physical pixels 2 and 3
fixture 2 -> physical pixels 4 and 5
...
```

The replacement firmware preserves this behavior by default. WS281x runs
normally use one physical pixel per logical fixture, producing a direct 1:1
mapping. Either mapping remains selectable per zone. Application and web
settings use logical counts; callers do not manually double them.

## Color ordering

The original firmware contains specializations for all six byte orders:

```text
RGB, RBG, GRB, GBR, BRG, BGR
```

If saved configuration is missing or invalid, firmware 1.78 defaults to `GBR`.
The replacement firmware provides the same choices and default.

## Confidence

The timing, pin mapping, buffer sizes, color-order branches, and fixture
doubling were recovered from the ESP32 firmware image and matched against
FastLED's public UCS1903 implementation. External connector numbering and
electrical levels were confirmed separately from PCB inspection.
