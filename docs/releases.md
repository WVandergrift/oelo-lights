# GitHub releases and automatic updates

Firmware releases are the update channel for installed controllers. A release
contains:

- `leaf-lights-tinys3.bin`, the browser/OTA firmware image;
- `SHA256SUMS`, a human-readable checksum file;
- GitHub release notes generated from changes since the previous release.

GitHub also records a `sha256:` digest in the release API for every uploaded
asset. The controller uses that API digest as the required verification value.

## Publishing a release

The workflow in `.github/workflows/release.yml` runs for semantic-version tags
such as `v0.4.0`:

```sh
git tag v0.4.0
git push origin v0.4.0
```

The workflow builds the TinyS3 image with the tag injected as its reported
firmware version, calculates `SHA256SUMS`, creates the GitHub release with
generated notes, and uploads both assets. Normal branch and pull-request builds
continue to run through `.github/workflows/platformio.yml`.

Do not replace an asset on an existing release. Publish a higher patch version
so controllers can compare versions and GitHub produces new immutable asset
metadata.

## Installing a selected release

1. Connect the controller to home Wi-Fi.
2. Open **Settings → Firmware updates**.
3. Press **Check now** if the list has not loaded.
4. Read the notes shown inside the desired release card.
5. Choose **Install**. The current web session authorizes the operation.

The controller obtains release metadata from the public GitHub API, then
downloads the selected `leaf-lights-tinys3.bin`. It validates TLS, repository
path, content length, SHA-256, and ESP32 image structure before selecting the
inactive OTA slot for the next boot.

## Automatic updates

Automatic updates are disabled by default. To enable them, select
**Automatically install stable updates** and save.

The controller waits one minute after boot, then checks every six hours while
home Wi-Fi is connected. It ignores drafts and prereleases and installs only a
tag whose first three numeric components are greater than the running firmware
version. Local development suffixes such as `-dev` do not affect comparison.

Automatic installation has the same TLS, path, size, SHA-256, inactive-slot,
and ESP32 validation as a selected release. If any check fails, the active image
remains selected and the status appears in the firmware settings and diagnostic
JSON.

## Trust-anchor maintenance

The GitHub API and release CDN currently validate through the USERTrust ECC and
ISRG Root X1 trust anchors embedded in `src/github_roots.h`. If GitHub changes
its public certificate chains, release checks fail closed until a firmware
version containing current roots is installed. Never switch production release
downloads to `setInsecure()`.
