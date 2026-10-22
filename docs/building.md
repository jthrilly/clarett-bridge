# Building and signing Clarett Bridge

## Requirements

- macOS and hardware compatible with the current DriverKit 27 deployment target.
- Xcode 27 with the DriverKit 27 SDK selected using `xcode-select`.
- An Apple Developer team able to provision development DriverKit, DriverKit PCI and DriverKit audio-family capabilities.
- An original Clarett 2Pre **Thunderbolt**, powered and connected through a working Thunderbolt chain. USB and Clarett+ models use different transports.

The SwiftUI host targets macOS 14; the driver currently targets macOS 27. Earlier driver deployment targets and other hardware have not been validated. Successful source compilation does not authorize driver activation.

## Identifiers and capabilities

The checked-in bundle identifiers support the original development installation. Other developer teams must register and use identifiers they control.

Choose a reverse-DNS prefix you control when registering a new app/driver pair:

```sh
export CLARETT_BUNDLE_PREFIX=com.example.ClarettBridge
scripts/build.sh audio DEVELOPMENT_TEAM=YOUR_TEAM_ID -allowProvisioningUpdates
```

The generator derives the host and driver bundle IDs from that prefix and embeds the driver ID in the host’s Info.plist. The legacy Core Audio UID and I/O Registry service name remain stable. The generator uses Python’s standard library, commits deterministic project files, and sets no default development team. Select a team in Xcode or pass `DEVELOPMENT_TEAM` to the build command. Re-run `python3 scripts/generate-project.py audio` without the prefix environment variable to restore the default generated files before contributing.

Required signed/profile capabilities:

| Component | Capabilities |
| --- | --- |
| Host | `com.apple.developer.system-extension.install` |
| Driver | `com.apple.developer.driverkit`, `com.apple.developer.driverkit.transport.pci`, `com.apple.developer.driverkit.family.audio` |

Development PCI uses Apple’s development wildcard grant; the actual IOKit personality is restricted to PCI `1cb5:0002`. The driver checks the 2Pre geometry before creating audio streams. The framework-provided audio user client is used; unrestricted user-client access is not enabled.

Xcode may omit the ordinary macOS app-sandbox flag from the final DriverKit signature. The verifier explicitly reports that fact and checks the required DriverKit capabilities rather than claiming the ordinary flag is present.

## Build commands

```sh
bash scripts/test.sh
scripts/build.sh audio CODE_SIGNING_ALLOWED=NO  # Compilation only
scripts/build.sh audio DEVELOPMENT_TEAM=YOUR_TEAM_ID -allowProvisioningUpdates
python3 scripts/verify-signing.py 'build/audio/Build/Products/Debug/Clarett Bridge.app'
```

`verify-signing.py` checks the real signatures, required signed and profile capabilities, profile expiry, eligibility of this Mac and restricted PCI matching. It does not print the profile’s full contents or device identifiers.

The default build stage is `audio`. Other stages remain useful during hardware research:

- `attach`: PCI attachment and capability read, no device DMA.
- `mailbox`: six identification queries, then quiesce.
- `audio`: 44.1–96 kHz playback/capture and monitor controls.

Build with `scripts/build.sh attach ...` or `scripts/build.sh mailbox ...`. The script regenerates the correct driver source list and entitlements for the selected stage. Changing only a build-mode string does not choose the implementation.

The host build number and driver build number are independent. `CLARETT_HOST_VERSION` controls the host number; `CLARETT_BUILD_VERSION` overrides the selected stage’s driver number when an actual driver update needs a new version. A cosmetic host update does not require incrementing the already tested driver. The output-volume update uses host build 18 (0.3.2) and audio driver build 17 (0.3.2); activate the new driver for hardware mute and the existing 48 dB volume range. Output ownership and ceiling control were introduced in driver 15. Sample-rate control was introduced in driver 13.

## Install and update

Copy the verified app to `/Applications/Clarett Bridge.app`, open Driver Setup and activate it. Complete the normal approval prompts yourself. Full Security and SIP remain enabled. Enrollment and device-management policies are not modified; another Mac may require administrator or management approval.

Extension activation and PCI attachment are separate. An older attached driver can remain until a powered Thunderbolt reconnect. Disable direct monitoring and wait for verified restoration before disconnecting or upgrading. A failed mailbox session can require a cold device power cycle.

Public binary distribution requires its own signing, entitlements and distribution workflow. The source archives intentionally exclude local signed builds and provisioning material.

## Artwork and source packages

```sh
scripts/render-previews.sh
bash scripts/package-source.sh HEAD
```

The renderer creates the app icon from source and produces representative SwiftUI previews with an explicit example-data controller. It does not read the interface, play audio or record. `package-source.sh` archives a resolved Git commit into `build/releases`; ignored and untracked files are excluded. Source archives cannot carry an uncommitted change.
