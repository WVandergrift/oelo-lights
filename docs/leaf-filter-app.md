# Using the LeafFilter app with the replacement controller

## Supported app mode

The replacement firmware supports the LeafFilter/Oelo app's **Local AP
Control** mode. It does not currently advertise the Oelo BLE service and does
not register with the vendor's `oelo.link` cloud or MQTT service.

The firmware emulates the original local controller values expected by the
app:

```text
Wi-Fi network: OELO_1-23.0
Password:      none (open network)
Controller IP: 172.24.1.1
```

## Connect from Android

1. Power the replacement controller and tap **RESET** if it was just flashed.
2. Open Android Wi-Fi settings and join `OELO_1-23.0`.
3. Android will warn that the network has no internet. Choose **Stay
   connected**, **Use this network**, or the equivalent option.
4. Disable cellular data. The most reliable method is to enable airplane mode
   and then turn Wi-Fi back on.
5. Fully close or force-stop the **Lighting by LeafFilter** app, then reopen it.
6. Wait approximately seven seconds for **Unable to connect** to appear.
7. Tap **Local AP Control** under the "No internet?" message.
8. If the local screen asks to connect, tap **Connect Me**. The app will request
   `OELO_1-23.0` and then load controller data from `172.24.1.1`.

Do not use **Find My Controller** for this workflow. That button performs a BLE
scan for a device named `Oelo`; a failed BLE scan does not automatically open
Local AP Control.

## Configure the zones

1. Open **Controller Settings** in Local AP Control.
2. Configure zone names, enabled state, fixture count, and color order.
3. Select **Update Controller**.
4. The controller saves the settings and reboots. Its AP disappears briefly,
   so Android may switch to another network.
5. Rejoin `OELO_1-23.0`, return to the app, and enter Local AP Control again.

For the current installation the verified settings are:

| App zone | Name | Enabled | Fixtures | Color order | TinyS3 GPIO |
|---:|---|---|---:|---|---:|
| 1 | Lower | Yes | 158 | GBR | 1 |
| 2 | Upper | Yes | 114 | GBR | 2 |
| 3–6 | — | No | — | GBR | 4, 5, 6, 7 |

The count is the number shown by the original LeafFilter app. The firmware
automatically sends two physical UCS1903 pixels for each configured fixture.

## Run Fast Fireworks

1. In Local AP Control, open **Your Patterns**.
2. Select **Fourth of July: Fast Fireworks**.
3. Select the Lower and/or Upper zones.
4. Tap **Set**.
5. Use the app's zone-off control when finished.

Saved offline patterns live on the ESP32 filesystem. The app reads and writes
them through `/getPatterns` and `/savePatterns`.

## Browser fallback

If the app does not expose Local AP Control, stay connected to `OELO_1-23.0`
and open:

```text
http://172.24.1.1
```

The browser interface can configure home Wi-Fi and zones, start Fast
Fireworks, apply test colors, and turn every zone off. After home Wi-Fi is
configured, try `http://leaflights.local` or the LAN address shown on the
status page.

## Troubleshooting

- **App remains in cloud mode:** disable cellular data, force-stop the app, and
  reopen it while connected only to `OELO_1-23.0`.
- **BLE scan finds nothing:** expected; use Local AP Control instead.
- **Phone leaves the AP:** explicitly accept Android's no-internet warning and
  reconnect after controller reboots.
- **No lights or unstable data:** verify common ground and use a 5 V HCT/AHCT
  level shifter. A direct 3.3 V signal is only a temporary test configuration.
- **Browser works but app does not:** confirm the phone, not just the PC, is
  connected to `OELO_1-23.0` and can open `http://172.24.1.1`.

The AP is intentionally open to match the app's hard-coded behavior. Do not
treat it as an authenticated control interface.
