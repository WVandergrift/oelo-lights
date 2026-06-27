# Hardware notes

## Original controller

Components identified from the controller PCB:

- ESP32-WROVER-IE module;
- W6100 Ethernet controller;
- TI HCT244 octal non-inverting buffer;
- six-position terminal block labeled `LED DATA`, channels 1 through 6;
- separate `LED GND` connection;
- UART and programming headers.

The HCT244 is important: it converts the ESP32's 3.3 V output to a robust 5 V
logic signal. This explains why a direct 3.3 V test may work on a short cable
but is not the preferred installed design.

## TinyS3 replacement mapping

The original GPIOs are not all exposed on the TinyS3. The sample uses:

| App zone | TinyS3 GPIO | Original connector |
|---:|---:|---|
| 1 | 1 | LED DATA 1 |
| 2 | 2 | LED DATA 2 |
| 3 | 4 | LED DATA 3 |
| 4 | 5 | LED DATA 4 |
| 5 | 6 | LED DATA 5 |
| 6 | 7 | LED DATA 6 |

GPIO0 and GPIO3 are deliberately avoided because they are ESP32-S3 strapping
pins.

## Recommended level shifter wiring

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

For an SN74HCT244-style pinout:

- pin 20: 5 V;
- pin 10: ground;
- pins 1 and 19: active-low output enables, connect low;
- add 100 nF ceramic decoupling directly between pins 20 and 10.

Use `HCT` or `AHCT`, not plain `HC`. HCT/AHCT inputs reliably recognize a
3.3 V ESP32 high while powered from 5 V.

## Bring-up sequence

1. Power everything off.
2. Disconnect the selected data wire from the original controller.
3. Connect common ground.
4. Connect one buffered data channel.
5. Power the ESP32 by USB and the lights from their normal supply.
6. Start at brightness 16 or 32 with a dim primary color.
7. Validate red, green, blue, off, fixture count, and direction before adding
   more zones.
