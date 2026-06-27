# Patterns

## App movement types

The offline LeafFilter app exposes these movement names:

```text
stationary  arcade     blend       bolt       chase
fade        fill       lightning   march      river
shuffle     split      sprinkle    streak     storm
takeover    twinkle
```

The original firmware also accepts `pacman` as an alias associated with
`arcade`, plus `off`.

The replacement firmware implements a non-blocking engine for each name. The
engines preserve the app contract—palette, speed, direction, gap, pause,
`other`, selected zones, and paired fixtures—but exact vendor animation parity
has not yet been established for every effect.

The replacement firmware also provides `fireworks`, an original engine that
launches independent, palette-colored expanding bursts on every selected zone.
Because it renders through the normal pixel pipeline, its output is also sent
to WLED receivers when realtime sync is enabled.

## Fourth of July: Fast Fireworks

The global Oelo catalog contained an exact likely match for the previously used
holiday profile:

| Property | Value |
|---|---|
| Name | Fourth of July: Fast Fireworks |
| Type | twinkle |
| Speed | 10 |
| Direction | R |
| Gap | 0 |
| Pause | 0 |
| Colors | white, blue, blue, white, red, red |

Palette:

```text
(255,255,255)
(0,0,255)
(0,0,255)
(255,255,255)
(255,0,0)
(255,0,0)
```

The source definition is preserved in
[`patterns/fourth-of-july-fast-fireworks.json`](patterns/fourth-of-july-fast-fireworks.json).

The sample firmware seeds this profile into LittleFS and exposes it through
`/getPatterns`, so it appears in the offline app's **Your Patterns** list. The
web interface can start the same configuration with one button.

## Independence Day collection

Fresh controllers receive ten editable presets. Existing controllers merge any
missing preset once when the built-in library version changes; user-created and
edited profiles are not overwritten.

| Preset | Movement | Character |
|---|---|---|
| Fourth of July: Fast Fireworks | Twinkle | Recovered Oelo palette and settings |
| Liberty March | March | Fast, detailed red-white-blue bands |
| Rocket's Red Glare | Bolt | White-hot head with gold, red, and blue tail |
| Fifty Stars | Sprinkle | Sparse white and blue sparks with red accents |
| Freedom River | River | Wide flowing patriotic color bands |
| Grand Finale Fireworks | Fireworks | Staggered expanding bursts per zone |
| American Wave | Blend | Smooth navy-to-white-to-crimson gradient |
| Stars & Stripes Chase | Chase | Color blocks separated by dark gaps |
| United We Split | Split | Symmetric motion radiating from the center |
| Dawn's Early Light | Fade | Slow whole-house sunrise through patriotic tones |

## Saved-profile behavior

Saved profiles are not bundled into the APK. In Local AP Control mode, the app
loads them from the controller using `/getPatterns` and replaces the
controller's saved array using `/savePatterns`.

Therefore:

- an offline replacement controller must implement both routes;
- profiles persist independently of the phone;
- erasing or reformatting the controller filesystem removes user profiles;
- NVS zone settings and LittleFS pattern storage are separate.
