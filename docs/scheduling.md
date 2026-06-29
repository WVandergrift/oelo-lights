# Weekly and holiday scheduling

Leaf Lights evaluates schedules locally on the ESP32. Pattern playback does not
depend on a cloud service, but the TinyS3 has no battery-backed real-time clock,
so it must reach an NTP server over home Wi-Fi after boot before scheduled
events can run.

## Priority model

Only one layer controls the lights at a time:

1. manual override;
2. holiday override;
3. weekly routine;
4. off.

A holiday override owns its entire date window, including the hours before its
on time and after its off time. The weekly routine therefore cannot turn the
lights on during a holiday window unless the holiday rule itself says to do so.
When several rules in the same layer overlap, the rule with the higher numeric
priority wins. The browser creates weekly rules at priority `0` and holiday
rules at priority `100`.

## Weekly routines

A weekly rule stores its days, on/off expressions, saved pattern ID, zone
bitmask, enabled state, and priority. Windows may cross midnight. For example,
a Friday rule from sunset to 1:00 AM remains active into Saturday morning.

## Holiday overrides

A holiday rule uses a recurring month and day plus `daysBefore` and `daysAfter`.
For a July 4 rule with two days before and one day after, the holiday layer owns
July 2 through July 5 every year. Its pattern is evaluated independently on
each day in that window.

## Absolute and solar times

Each on/off expression is either a local clock time, sunrise plus or minus an
offset, or sunset plus or minus an offset. Solar times are calculated on the
controller from the configured latitude, longitude, date, and time-zone rule.
Coordinates stay on the controller. The time-zone choices include US
daylight-saving transitions.

## Manual overrides

Starting a pattern, setting a color, or turning the lights off from the browser
automatically holds that choice until the next scheduled transition. The
Schedules page also provides controls to hold the current output, keep the
lights off, or resume the schedule.

Manual overrides are intentionally held in RAM. A restart clears the override
and returns control to the saved schedule after network time is available.

## Storage and updates

Schedule configuration is stored in `/schedules.json` in LittleFS. Writes use a
temporary file and backup rename. Firmware updates preserve LittleFS, so normal
OTA installation preserves schedules.

Deleting a referenced pattern leaves its rule intact. If that rule becomes
active, the controller turns the selected zones off and reports the missing
pattern until the rule or pattern is corrected.

## Browser API

The project UI uses:

```text
GET  /api/schedules
POST /api/schedules
POST /api/manual-override
```

See [local-api.md](local-api.md) for the JSON shape and override fields.
