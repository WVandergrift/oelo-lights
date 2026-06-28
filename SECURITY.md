# Security notes

## Local AP compatibility

The vendor app hard-codes a Wi-Fi access point named `OELO_1-23.0` and the
controller address `172.24.1.1`. The replacement firmware protects that
network with WPA2 while retaining the expected name and address.

Consequences:

- the compatibility AP requires its configured WPA2 password;
- local HTTP endpoints have no authentication;
- clients that know the WPA2 password can use the legacy unauthenticated local
  endpoints;
- the AP should not be treated as a trusted management network.

The browser interface may also be exposed on the home LAN after provisioning.
Do not port-forward it or expose it directly to the internet.

## Firmware updates

Firmware updates use the same optional web-interface session as settings and
restart. Without a web password, anyone already admitted to the home or WPA2
compatibility network can use management functions. With a web password, the
firmware stores a salted SHA-256 verifier and issues a persistent HttpOnly,
SameSite session cookie after login. Local HTTP is not encrypted, so do not
expose the controller to the internet. Upload only firmware built for the
TinyS3 target. USB remains the recovery path for invalid partition or
bootloader images.

Legacy LeafFilter endpoints remain unauthenticated only for requests arriving
through the WPA2 compatibility interface. Requests to those endpoints from the
home LAN use the optional web session.

GitHub release installs add three checks before ESP32 image validation:

- TLS is validated against embedded USERTrust ECC and ISRG Root X1 trust
  anchors;
- download URLs must be under this repository's GitHub release path;
- the complete image must match the SHA-256 digest in GitHub's release-asset
  metadata.

The embedded trust anchors must be reviewed if GitHub changes certificate
chains. Automatic updates are disabled by default, ignore prereleases, and only
advance to a higher semantic version.

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
