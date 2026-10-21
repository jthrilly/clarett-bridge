# Monitoring and latency

## Tested settings

| Driver margin | Hardware period | Result |
| --- | --- | --- |
| 512 frames | 256 frames | Conservative default; clear playback and monitoring |
| 256 frames | 128 frames | Clear GarageBand monitoring with acceptable perceived delay |
| 128 frames | 64 frames | Crackling and worse perceived delay; returned to 256 |
| 64 frames (earlier build) | 64 frames | Crackling, including after a cold reset |

The successful GarageBand test used an **effective 512-frame Core Audio client buffer**. A 128-frame buffer request was accepted but the active value remained 512; do not call that a successful 128-frame buffer test. Device/app buffering and the driver margin are separate controls.

The normal UI offers margins 256 and 512. The research command-line timing API retains 128 for diagnosis. A reattachment returns to the conservative 512 default. Smaller buffers being accepted does not prove reliable low-latency audio. No numerical round-trip latency was measured.

## Runtime changes

The `cltp` custom dictionary property accepts only `SafetyFrames` values 128, 256 or 512. The driver calls `RequestDeviceConfigurationChange`. Only `ClarettDevice::PerformDeviceConfigurationChange`, after the host stops I/O, updates input/output safety offsets. The next engine start rebuilds descriptors for a period half that margin; the HAL ring remains 4096 frames. Existing I/O restarts through Core Audio.

The applied value is read back independently. Failed/aborted requests do not appear as successful changes. The host’s requested value is not substituted for actual device state. Nothing is saved to device flash.

```sh
zsh scripts/audio-configure.sh margin 512
zsh scripts/audio-configure.sh margin 256
zsh scripts/audio-configure.sh buffer 128
zsh scripts/audio-status.sh
```

The configuration utility matches only the exact prototype device UID. It does not start an audio stream, play, record or change default devices. The status tool is read-only and reports running clients associated with the Clarett.

## Hardware direct monitoring

The monitor dictionary accepts only Enabled, InputMode, OutputPair and Blend with strict types/ranges. Choices are input 1 centered, input 2 centered or stereo inputs 1/2; outputs 1/2, outputs 3/4 (headphone path) or both; and complementary live-input/computer gains. Each created mixer row has total gain at most unity.

Before first enable, the driver snapshots all 48 routes and all 16 × 30 mixer coefficients. It selects two buses unused by existing routes and input slots whose reassignment cannot change any original active mix. It rejects a request if safe resources are unavailable. The analogue input pins on the 2Pre are 0x400 and 0x402.

The driver stages input routes and mix coefficients before switching requested outputs, then verifies routes and both rows. Disable checks ownership of the active plan, restores the original routes and borrowed rows, and verifies that restoration. Changes by another controller are treated as conflicts. Known-complete partial writes can be rolled back; an uncertain transport failure poisons further command submission.

The allowlist permits MUX_READ, GET_MIX, SET_MIX and SET_MUX only with model-specific geometry and band 0. There is no arbitrary configuration or flash-save API. Settings are volatile. Disable monitoring and wait for verified restoration before an upgrade or disconnect: unplugging can prevent restoration, and a warm reconnect can retain volatile routes after the original in-memory snapshot is lost. A cold power cycle restores the saved device configuration.

The user confirmed this hardware monitor works. Keep software monitoring off when using it; two paths produce differently delayed copies. For software effects, disable hardware monitoring and use the recording app’s monitor path. Specific plug-ins have not been separately qualified.

## Audacity-specific delay

The saved Audacity 4.0.1 buffer preference was verified as 10 ms, but its monitoring remained too delayed. Inspection of the official source found a separate path in `au3/libraries/au3-audio-io/AudioIO.cpp`:

- Lines 602–604 select PortAudio’s defaultLowOutputLatency for software playthrough.
- Lines 648–650 select defaultHighInputLatency for that path, rather than the configured latency-duration preference.

Enumerating device info through Audacity’s installed PortAudio library, without opening a stream, reported:

| Default | Value at margin 256 |
| --- | ---: |
| Low input | 10.0 ms |
| High input | 100.0 ms |
| Low output | 7.333 ms |
| High output | 16.667 ms |

Thus Audacity requests a 100 ms input-latency **hint** for software playthrough even with its buffer preference at 10 ms. This is not a measured round-trip latency and does not explain or excuse the driver’s separate small-margin crackling. No installed Audacity binary, library or signature was modified. GarageBand was already installed and provided an independent successful software-monitoring test.

## Scheduling and diagnostic limits

Control commands run on a serialized queue with an atomic ownership handoff and period-queue barrier. Stream period handling continues on its own cause registers during mailbox work. HAL PCM callbacks contain no MMIO, mailbox calls, allocation or locks.

The driver measures maximum poll gap, MMIO read duration, event gap, late polls, input requests beyond the last confirmed hardware frame and output writes behind it. The confirmed position is quantized to hardware events. These diagnostics can remain apparently healthy during audible corruption; zero overruns alone is not a pass. Counters reset on engine restart.

Sanitized tests cover route preservation, mixer resources, 2Pre pins, mono/stereo modes, output pairs, blend headroom, restoration snapshots, exact command serialization and descriptor cadence/bounds. See [validation](validation.md) for hardware results and unqualified cases.

## Sources

- [Audacity 4.0.1 AudioIO.cpp](https://github.com/audacity/audacity/blob/Audacity-4.0.1/au3/libraries/au3-audio-io/AudioIO.cpp#L602-L652).
- [PortAudio Core Audio implementation](https://github.com/PortAudio/portaudio/blob/master/src/hostapi/coreaudio/pa_mac_core.c).
- [Apple AudioDriverKit sample](https://developer.apple.com/documentation/audiodriverkit/creating-an-audio-device-driver) and DriverKit 27 SDK headers.
- [snd-clarett](https://github.com/JustALawnGnome7/snd-clarett) snapshot `fd416d45b74941663f703281666b33c9eccbda6e` and [clarett-sre](https://github.com/JustALawnGnome7/clarett-sre) snapshot `4df059996ecf781f5603f5395d6fe1bf6d56c1f7` for protocol geometry, pins and period servicing.
