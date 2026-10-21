# AudioDriverKit streaming candidate

*Historical milestone notes. For the current rate-aware driver and control app, see [architecture](architecture.md), [user guide](user-guide.md) and [validation](validation.md).*

## Scope and current result

The audio stage exposes **Clarett 2Pre Thunderbolt (Experimental)** to Core Audio at **48,000 Hz**, with **4 playback channels and 14 capture channels**. The implementation is specific to the original Thunderbolt 2Pre's transport geometry. The host application does not play or record audio itself.

Version `0.1/6` passed playback/capture tests on the test Mac, with valid development DriverKit, PCI and audio-family grants. Core Audio reported the expected format and channels. The six mailbox identification checks passed again at attachment, and the larger audio allocation mapped successfully. The user confirmed clear Spotify playback and clear recording/listen-back in Audacity 4.0.1. Version 8 is now activated with monitoring controls: the user confirmed hardware direct monitoring works, but software monitoring crackles at multiple buffer sizes. The crackle persisted after a cold reset. Version 10 attached and collected timing diagnostics; the user confirmed both ordinary playback and software monitoring crackle. Version 11 restores version 6 timing settings; it is activated and attached, and the user confirmed ordinary playback and software monitoring are clear. See [monitor controls and current validation](monitoring-controls.md). No loopback test or test tone is run by the agent.

SIP remains enabled. Full Security was confirmed by the user with `sudo bputil -d` before this work, and no boot-policy or management settings were changed. No IT involvement was needed for development signing or activation.

## Architecture

- `ClarettAudio` subclasses Apple's `IOUserAudioDriver`. Its framework-provided HAL user client is enabled with `com.apple.developer.driverkit.family.audio`; unrestricted user-client access is absent.
- A standard `IOUserAudioDevice` owns two streams with separate packed, interleaved 32-bit signed PCM buffers. Hardware sample words are 24-bit MSB-justified; playback masks the low eight bits and capture reports the same alignment.
- A separate, bidirectional, 32-bit-addressable PCI DMA allocation contains 256 playback descriptors, playback fragments, 256 capture descriptors and page-aligned capture fragments. Each fragment contains 16 frames. Playback slots are 256 bytes; capture slots are 1,024 bytes for 896 bytes of audio, so a 14-channel fragment never crosses a 4 KiB boundary. The allocation is required to map as one device-visible segment with checked bounds and alignment.
- Both hardware rings are armed together. Playback starts with zeroed samples, including when a capture-only client opens the device. Played samples are retired to zero so an input-only session does not replay stale output at ring wrap.
- The service uses bounded polling from a DriverKit timer: 500 microseconds in version 6, 250 microseconds from version 8. It reads the period causes, accumulates the modulo-256 hardware counter in 16-frame units and keeps both stream directions on the same clock. This is a prototype polling implementation; reliable low-latency streaming has not been established.
- The real-time HAL callback only copies PCM between the packed HAL buffers and padded device fragments. It allocates no memory, takes no locks, performs no MMIO and issues no mailbox commands. Frame count is checked before copying; buffer indexing wraps within the known allocation.
- One fixed 48 kHz/internal-clock command and three read-only sync queries run before the engine is armed. The monitoring controls add a separate, model-specific allowlist for volatile mixer/routing changes and readback. Unknown commands, flash updates and arbitrary clock payloads remain unavailable. Clock replies must succeed and the device must report a locked 48 kHz clock before arming.
- A failed mailbox command poisons further command submission. A disconnected device or a scheduling/clock gap approaching the 85 ms counter modulus stops the audio engine rather than guessing its position. Device teardown stops the rings, disables DMA and closes the PCI session before unmapping device-visible memory.

## Timestamp correction from the first manual test

Version 5 registered and ran the hardware engine, but Spotify was silent. Diagnostics showed zero audio-copy callbacks and Core Audio repeatedly resetting its timeline. The driver advertised a 256-frame zero-timestamp period, although the HAL stream buffers contain 4,096 frames; Core Audio interpreted this as a 256-frame ring.

Version 6 uses the full **4,096-frame HAL ring** as its zero-timestamp period, separate from the **256-frame hardware period**. Hardware causes continue to be serviced every period; clock timestamps are published only at complete HAL-ring boundaries, interpolating a delayed poll back to the boundary. The initial reattachment produced 178 write callbacks with 512-frame blocks and no reported hardware overruns. The initial callbacks contained silence. During the subsequent Spotify test, diagnostics showed 7,136 write callbacks, 7,112,702 nonzero output sample words and zero reported overruns. The user separately confirmed that playback was clear. No Core Audio timeline re-anchoring messages appeared in a subsequent one-minute observation window. During the capture test, telemetry showed 268 read callbacks and 1,218,050 nonzero input sample words while playback continued, with zero reported overruns. This confirms both callback paths are active; the user subsequently confirmed the Audacity recording plays back clearly. Monitoring latency was the remaining issue. Audacity was buffering 100 ms; reducing that to 10 ms did not sufficiently reduce perceived monitoring delay. Lower driver buffers and a direct-monitoring path are now being implemented.

## Manual test

1. Select **Clarett 2Pre Thunderbolt (Experimental)** as the output for Spotify or your audio app. Use the Clarett's normal output/headphone controls. Report whether audio is audible, clear and continuous.
2. For capture, select the same device as the input in a recording app or DAW with microphone permission. Use a 48 kHz project and initially a 256- or 512-frame app buffer. Check an analogue input meter, record a short clip, and listen back. A meter alone does not verify recorded sample integrity.
3. Test playback and capture together, then stop and start the app's audio engine. Report silence, distortion, dropouts or device disappearance, including the application and selected channels.

Focusrite Control compatibility is not implemented; use Core Audio applications for this test. Hardware monitoring changes volatile mixer routing and restores its snapshot when disabled; it does not save those changes to flash. Input gain and phantom power are not controlled. Internal transport channel counts must not be interpreted as physical socket counts.

## Read-only diagnostics

```sh
scripts/audio-status.sh
ioreg -r -n ClarettProbe -l -w0
systemextensionsctl list
csrutil status
```

`audio-status.sh` only enumerates Core Audio properties. It never starts I/O, changes the default device, plays a tone or records. Driver properties expose the current status, hardware frames/events/overruns, HAL read/write callback counts and nonzero sample counts. Nonzero samples alone do not prove correct routing or intelligible audio.

`audio-device-ready` means the device was created and is idle. `audio-streaming` means the hardware was armed, not that the audible test passed. `audio-clock-command-failed`, `audio-clock-stalled` and `audio-device-disconnected` identify startup/runtime failures. Use a powered Thunderbolt reconnect after an installed update; a failed mailbox session may require a power cycle.

## Validation and limits

Sanitized tests cover mailbox completion ordering and command scope, descriptor-table bounds and wrap flags, 4 KiB fragment containment, padded capture addressing, hardware counter rollover/overrun handling and HAL-ring timestamp boundaries. The original attach-only build also compiles after the changes, and the generated project is restored to audio mode. Both the host and driver build against the installed Xcode 27/DriverKit 27 SDK. Real signatures and embedded provisioning profiles are checked for the required capabilities and eligibility of this Mac.

This candidate does not support sample-rate selection, external clocks, multiple Claretts, MIDI, general device mixer controls or Focusrite Control. The scoped direct-monitor controls are described separately. The initial 64/128-frame margins crackle. Version 12 software monitoring in GarageBand is user-confirmed clear with acceptable delay at margin 256 and an effective 512-frame Core Audio buffer. Sleep/wake, hot-unplug during active I/O, sustained load, latency and sample integrity still need hardware testing. The existing mailbox stage remains available as a separate build mode. No public distribution signing or entitlement work has been performed.

## Sources

- [Apple AudioDriverKit](https://developer.apple.com/documentation/audiodriverkit): audio driver/device/stream model and PCI integration.
- [Apple audio-device-driver sample](https://developer.apple.com/documentation/audiodriverkit/creating-an-audio-device-driver): framework setup and real-time operation handler.
- [Apple audio-family entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.family.audio).
- Public [snd-clarett](https://github.com/JustALawnGnome7/snd-clarett) snapshot `fd416d45b74941663f703281666b33c9eccbda6e` and [clarett-sre](https://github.com/JustALawnGnome7/clarett-sre) snapshot `4df059996ecf781f5603f5395d6fe1bf6d56c1f7`: protocol layout, startup, DMA geometry and counter semantics. No Focusrite binary was modified or disassembled.
