# Validation and supported scope

Initial audio test date: **21 October 2026**. Sample-rate switching checked **22 October 2026**. Hardware: original Clarett 2Pre Thunderbolt, Apple silicon M5 Max MacBook Pro, macOS 27.0.1, Xcode 27 / DriverKit 27. One device/Mac combination was tested.

| Check | Evidence |
| --- | --- |
| Full Security | User independently ran `sudo bputil -d` and reported Full Security |
| SIP | Read directly with `csrutil status`; enabled |
| Activation / attachment | Normal signed system-extension activation and PCI matching; version 12 baseline |
| Mailbox | Six alternating identification queries, validated replies and repeated firmware words |
| Playback | User confirmed clear Spotify playback |
| Capture | User confirmed clear Audacity recording/listen-back |
| Hardware monitoring | User confirmed live monitoring; route restoration read back on disable |
| Software monitoring | User confirmed clean GarageBand monitoring with acceptable perceived delay at margin 256 |
| Effective Core Audio buffer | 512 frames in that successful test; a 128-frame request was not shown as applied |
| Smaller margins | 64 and 128 produced crackling; cold reset did not clear the regression |
| Recovery comparison | Restoring the 512 margin/256 period cleared playback; margin 256/period 128 also passed |

No boot-policy, FileVault, enrollment or endpoint-protection settings were changed. No IT contact was needed on the test Mac. This does not guarantee another managed Mac permits development-driver approval.

## Automated checks

- AddressSanitizer/UndefinedBehaviorSanitizer C++ protocol, ring and mixer tests.
- Generated Xcode project consistency and Python syntax.
- SwiftUI host type-check and native rendering inspection.
- Complete local app/driver build; real signatures and embedded development profile capability/expiry/device-eligibility checks.
- Read-only Core Audio settings/client reports and driver diagnostic properties.

CI runs hardware-independent checks only. Local provisioning and hardware activation are not part of CI.

## Clarett Bridge control app

Host build 13 introduces the native SwiftUI sidebar and separate Overview, Monitoring, Audio Settings, Diagnostics and Driver Setup pages, plus a red app icon. The user opened the installed app and confirmed the new UI showed the connected interface. A read-only controller check also confirmed the device was alive at 48 kHz with actual buffer and margin values. The existing version 12 audio driver remains unchanged; the UI update did not require reactivation.

Representative previews were rendered from the actual SwiftUI views with example data, including dark and disconnected states. These complement the live connection check; they are not hardware screenshots or a new audio listening test.

## Limits

This is a source/development release, not a generally provisioned binary driver. The original Thunderbolt 2Pre geometry and 44.1, 48, 88.2 and 96 kHz internal-clock operation are implemented. MIDI, 176.4/192 kHz, external clocking, general device configuration and Focusrite Control compatibility are absent.

The user performed manual listening/recording tests. No agent-generated test tone, connected cable loopback or numerical round-trip measurement was used. Individual software effects were not separately qualified. Sleep/wake, sustained stress, arbitrary hot-unplug scenarios, other models/Macs and public distribution signing remain unqualified.

The first mailbox/audio implementations and corrected failures remain documented in the historical notes. A successful compile/signature or zero overrun count alone must not be described as an audio pass.

## Sample-rate update (0.2.0 / driver 13)

The user activated driver 13 and reconnected the powered interface. The system reports version 0.2/13 activated and enabled; the attached device advertises 44.1, 48, 88.2 and 96 kHz. Its startup identification queries validated both rate bands, including 10 live capture channels at double speed.

On 22 October 2026, with audio stopped and hardware monitoring off, these changes passed:

| Transition | Hardware clock | Core Audio nominal / input / output | Live capture slots |
| --- | --- | --- | --- |
| 48 → 44.1 kHz | 44,100 Hz | All 44,100 Hz | 14 |
| 44.1 → 88.2 kHz | 88,200 Hz | All 88,200 Hz | 10 |
| 88.2 → 96 kHz | 96,000 Hz | All 96,000 Hz | 10 |
| 96 → 44.1 kHz | 44,100 Hz | All 44,100 Hz | 14 |
| 44.1 → 48 kHz | 48,000 Hz | All 48,000 Hz | 14 |

Every readback reported a stable rate, zero rate error and an alive, idle device. The buffer and driver margin remained 512 frames, and the transport stayed 14 input / 4 output channels. The interface was restored to 48 kHz. No playback, recording or monitoring was started during these checks.

These results validate idle clock changes and matching stream metadata, not audible playback/capture or live rate changes during I/O. Manual listening/recording at the new rates is pending. The historical listening results above cover the version 12 / 48 kHz baseline.

The signed local build and sanitizer checks pass. New tests cover strict rate/clock-source allowlists, speed-band read bounds and routing preservation, correct double-speed packet lengths/padding, S/MUX residue suppression, rate-scaled polling, ring-wrap watchdogs and timestamp conversion. The red-accent previews are generated from the real SwiftUI views with example data.
