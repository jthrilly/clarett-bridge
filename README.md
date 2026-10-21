# Clarett Bridge

**An independent macOS driver and control app for the original Focusrite Clarett 2Pre Thunderbolt.**

Clarett Bridge connects this interface to Core Audio through DriverKit and AudioDriverKit. It provides playback, recording, hardware direct monitoring and adjustable driver margins while keeping Full Security enabled on the tested Mac.

![Clarett Bridge overview](docs/images/overview.png)

*Representative interface preview. Readiness, active settings and diagnostics in the app come from the connected device; this image uses example values.*

## Current status

This is an **experimental source release**, tested on one Clarett 2Pre Thunderbolt with an Apple silicon Mac running macOS 27.0.1 and Xcode 27. It is independent of Focusrite and is not endorsed by the manufacturer.

| Feature | Result |
| --- | --- |
| Driver activation and PCI attachment | Passed with user-confirmed Full Security; SIP enabled |
| Playback and recording | Clear Spotify playback and Audacity recording/listen-back |
| Software monitoring | Clean, acceptably responsive monitoring in GarageBand at a 256-frame driver margin |
| Hardware direct monitoring | Input 1/2 or stereo mix, selectable output pairs and live/computer blend; manually verified |
| Sample rate | 48 kHz, internal clock |
| Transport channels | 14 capture / 4 playback; these are not physical socket counts |
| Margins below 256 frames | Unreliable: crackling observed at 64 and 128 frames |
| MIDI, other rates, external clocks, Focusrite Control | Not implemented |

The verified software-monitoring test used an **effective 512-frame Core Audio buffer**. Selecting a requested buffer in the app does not guarantee another audio application will use it. Numerical round-trip latency, sustained load and sleep/wake are not yet qualified. See [validation](docs/validation.md).

## Build and install

There is no broadly installable signed binary in this release. A developer build requires Xcode 27 / DriverKit 27, an Apple Developer team, and provisioning with the DriverKit, PCI and audio-family development entitlements. The current driver deployment target is macOS 27.

```sh
git clone https://github.com/jthrilly/clarett-bridge.git
cd clarett-bridge
bash scripts/test.sh
scripts/build.sh audio DEVELOPMENT_TEAM=YOUR_TEAM_ID -allowProvisioningUpdates
python3 scripts/verify-signing.py 'build/audio/Build/Products/Debug/Clarett Bridge.app'
```

New developers must register their own bundle identifiers and capabilities; see the [complete build and signing guide](docs/building.md). Install the verified app in `/Applications`, open **Driver Setup → Activate driver**, and complete the normal macOS approval flow. Keep Full Security and SIP enabled. Device-management approval policy may differ on other Macs.

The app is now named **Clarett Bridge**. Existing driver identifiers and device identity retain the earlier `ClarettProbe` name for installation compatibility. The Xcode project and scheme are still named `ClarettProbe`. The redesigned host app can operate the existing version 12 audio driver.

## Use it

1. Select **Clarett 2Pre Thunderbolt (Experimental)** as the input and output in your audio app.
2. For **software monitoring and effects**, use **Audio Settings → Driver responsiveness → 256 frames** and enable monitoring in your audio app. GarageBand passed the manual listening test. Turn hardware monitoring off for this path.
3. For **hardware direct monitoring**, open **Monitoring**, choose inputs and outputs, adjust the live/computer blend, then enable monitoring. Turn the audio app’s software monitoring off to avoid two delayed copies.
4. Check the **active** buffer value after a request. Audio apps may override it. The driver margin resets to 512 when the device reattaches; choose 256 again if desired.

Audacity 4.0.1’s software-monitoring path requests its audio library’s high input-latency hint, which was 100 ms on the test Mac even with Audacity’s buffer preference at 10 ms. That is separate from driver crackling at smaller margins. See [monitoring and latency](docs/monitoring-controls.md).

![Monitoring controls](docs/images/monitoring.png)

*Representative monitoring preview; changes are applied explicitly and the resulting route/mixer state is read back.*

## Documentation

- [User guide](docs/user-guide.md): monitoring, buffers, driver setup and removal.
- [Build and signing](docs/building.md): requirements, identifiers, entitlements and build stages.
- [Architecture](docs/architecture.md): host, PCI/mailbox, DMA, audio timeline and configuration changes.
- [Monitoring and latency](docs/monitoring-controls.md): mixer preservation, runtime profiles and app buffering.
- [Validation and limitations](docs/validation.md): exactly what has been tested.
- [Troubleshooting](docs/troubleshooting.md): no hardware, silence, crackling, delay and activation issues.
- [Initial feasibility](docs/feasibility.md) and [historical mailbox result](docs/hardware-test-2026-10-21.md).
- [Contributing](CONTRIBUTING.md) and [third-party provenance](THIRD_PARTY.md).

## Command-line tools

```sh
zsh scripts/audio-status.sh                 # Read-only device/client report
zsh scripts/audio-configure.sh margin 256   # Briefly restarts existing I/O
zsh scripts/audio-configure.sh buffer 128   # Request; verify actual result
bash scripts/package-source.sh HEAD         # ZIP and tar.gz of committed source
```

CI runs sanitized C++ protocol/ring/mixer tests, checks generated project files, and type-checks the SwiftUI host. It does not sign, activate a driver or validate audio hardware. The project includes no credentials, provisioning profiles, certificates, private keys or device UDIDs.

## License and provenance

[GPL-2.0-only](LICENSE). Protocol research and the reference Linux driver are credited in [THIRD_PARTY.md](THIRD_PARTY.md). No Focusrite binary was modified or disassembled. Focusrite and Clarett are their owners’ trademarks.
