# WLED realtime synchronization

The replacement controller can mirror its rendered Oelo/LeafFilter output to
WLED devices on the same network. It sends DDP RGB frames to UDP port 4048,
which current WLED releases listen on automatically.

## Why DDP instead of WLED effect sync

WLED's normal sync notifier distributes WLED state: effect number, palette,
speed, intensity, brightness, and related settings. That works because every
receiver has the same WLED effect engine. Oelo movements such as March, River,
Takeover, and Sprinkle are different engines with no reliable WLED effect-ID
equivalent.

This firmware therefore synchronizes pixels. A WLED receiver sees the colors
already rendered by the TinyS3, so an Oelo March or Twinkle does not need to be
translated to a different WLED animation.

## Setup

1. Configure the TinyS3 and WLED controllers on the same home Wi-Fi network.
2. On each WLED controller, open **Config → Sync Interfaces**. Under realtime
   input, enable **Force max brightness**. Then open **Config → WiFi Setup** and
   enable **Disable WiFi sleep** to reduce realtime packet jitter. DDP reception
   itself is initialized by WLED on UDP port 4048.
3. Open the TinyS3 UI and choose **Settings → WLED realtime sync**.
4. Leave the destination at `255.255.255.255` to broadcast to all WLED devices
   on the LAN. For one receiver, use its specific IPv4 address for smoother,
   lower-jitter unicast delivery.
5. Set **Virtual pixel count** to the receiving strip's LED count.
6. Choose **Auto (first active)** or a fixed Oelo source zone, enable sync, and
   save.

The sender scales the frame by the TinyS3 brightness value. Force-max-brightness
mode prevents WLED from multiplying that by its previous local brightness and
making the synchronized result too dim.

## Behavior and limits

- All broadcast receivers get the same DDP frame. Receivers with the same LED
  count show the same full-span animation.
- A single broadcast cannot independently scale for WLED devices with different
  strip lengths. Use a count matching the intended receivers, or use a unicast
  destination for one differently sized controller.
- The chosen Oelo source zone is resampled to the configured virtual count.
- The maximum virtual output is 1,000 RGB pixels. Frames above 480 pixels are
  split into multiple DDP packets and pushed after the last packet.
- Animated output is sent when a rendered frame changes, with a 25 ms minimum
  interval. A 750 ms heartbeat keeps stationary colors in WLED realtime mode.
- The TinyS3 disables its own Wi-Fi modem sleep while connected and batches
  each DDP packet into one UDP write to reduce sender-side latency variance.
- DDP is UDP and has no acknowledgement. “Streaming” means packets were handed
  to the network stack, not that a receiver confirmed display.
- The limited broadcast address depends on the local router allowing LAN
  broadcast traffic. The sender converts `255.255.255.255` to the home
  network's directed broadcast address so DDP is not also emitted through the
  compatibility access point. If broadcast is filtered, configure a WLED
  device's IP.
- USB serial recovery commands `wled status`, `wled off`, and `wled on` remain
  available if a network configuration ever makes the browser unreachable.
- When sync is disabled or the TinyS3 disappears, each WLED receiver returns to
  its prior state after its own realtime timeout.
