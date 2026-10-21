# Validation and supported scope

Test date: **21 October 2026**. Hardware: original Clarett 2Pre Thunderbolt, Apple silicon M5 Max MacBook Pro, macOS 27.0.1, Xcode 27 / DriverKit 27. One device/Mac combination was tested.

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

This is a source/development release, not a generally provisioned binary driver. Only 48 kHz/internal clock and the original Thunderbolt 2Pre geometry are implemented. MIDI, other sample rates, external clocking, general device configuration and Focusrite Control compatibility are absent.

The user performed manual listening/recording tests. No agent-generated test tone, connected cable loopback or numerical round-trip measurement was used. Individual software effects were not separately qualified. Sleep/wake, sustained stress, arbitrary hot-unplug scenarios, other models/Macs and public distribution signing remain unqualified.

The first mailbox/audio implementations and corrected failures remain documented in the historical notes. A successful compile/signature or zero overrun count alone must not be described as an audio pass.

## Sample-rate update (0.2.0 / driver 13)

Implemented rates: 44.1, 48, 88.2 and 96 kHz, internal clock. New hardware validation is pending activation. The historical listening results above cover the version 12 / 48 kHz baseline, not all new rates.

The signed local build and sanitizer checks pass. New tests cover strict rate/clock-source allowlists, speed-band read bounds and routing preservation, correct double-speed packet lengths/padding, S/MUX residue suppression, rate-scaled polling, ring-wrap watchdogs and timestamp conversion. The red-accent previews are generated from the real SwiftUI views with example data.
