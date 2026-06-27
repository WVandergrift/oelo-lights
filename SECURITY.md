# Security notes

## Local AP compatibility

The vendor app hard-codes an open Wi-Fi access point named `OELO_1-23.0` and
the controller address `172.24.1.1`. The sample firmware reproduces those
values so the unmodified app can use Local AP Control.

Consequences:

- the setup AP has no password;
- local HTTP endpoints have no authentication;
- any nearby client that joins the AP can inspect configuration and control
  the lights;
- the AP should not be treated as a trusted management network.

The browser interface may also be exposed on the home LAN after provisioning.
Do not port-forward it or expose it directly to the internet.

## Firmware updates

Remote firmware updates are disabled until a separate update password is
configured. The OTA endpoint requires HTTP Basic authentication and writes only
to the ESP32's inactive application slot before verification and reboot.

Basic authentication protects the operation from unauthenticated clients, but
it does not encrypt traffic. Configure updates from a trusted local network,
use a unique password, and never expose the controller's HTTP server to the
internet. Upload only `firmware.bin` built for the TinyS3 target in this
repository. A power failure during update should leave the active image intact,
but USB access remains the recovery path for invalid partition or bootloader
images.

GitHub release installs add three checks before ESP32 image validation:

- TLS is validated against embedded USERTrust ECC and ISRG Root X1 trust
  anchors;
- download URLs must be under this repository's GitHub release path;
- the complete image must match the SHA-256 digest in GitHub's release-asset
  metadata.

The embedded trust anchors must be reviewed if GitHub changes certificate
chains. Automatic updates are disabled by default, require the update password
to enable, ignore prereleases, and only advance to a higher semantic version.

## Electrical safety

- ESP32 GPIOs are not 5 V tolerant.
- Use a 5 V HCT/AHCT non-inverting buffer for installed operation.
- Disconnect the original controller's data output before attaching a
  replacement output.
- Power the LEDs from their intended external supply, not the ESP32.
- Confirm supply voltage, polarity, current capacity, data, and ground with a
  meter before energizing the installation.
- Start at low brightness with one zone and one fixture count.

## Sensitive and third-party material

Do not commit:

- Wi-Fi credentials or generated credential headers;
- flash dumps or device-specific identifiers;
- account tokens, cookies, or user pattern libraries;
- vendor APKs, decompiled application trees, or vendor firmware binaries.

## Reporting problems

Open a GitHub issue for bugs in this project's original code. Do not post
private credentials, proprietary binaries, or vulnerabilities affecting a
vendor-hosted service. Report vendor-service vulnerabilities to the relevant
vendor through an appropriate private channel.
