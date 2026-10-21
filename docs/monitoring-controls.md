# Monitoring controls and current validation

## Result on 21 October 2026

The user confirmed that hardware direct monitoring works on activated version 8. The registry also reports successful monitor disable and restoration (`off-restored`, error 0, uncertain 0). This is separate from the earlier version 6 tests confirming clear Spotify playback and clear Audacity recording/listen-back.

The initial low-margin software monitoring test crackled at multiple Core Audio buffer settings. The user also increased Audacity's own buffer to 100 ms without resolving it. Zero hardware-overrun flags do not establish sample continuity. The crackle persisted after a cold reset on the same version 8 build, making stale volatile hardware state less likely. Version 10 passed compilation, signature/profile checks, sanitized tests and hardware attachment. The user confirmed both live monitoring and ordinary playback crackle on version 10. Version 11 restores the previously clear version 6 timing settings while preserving the controls and diagnostics; macOS accepted it, and the user confirmed both ordinary playback and software monitoring are clear. Hardware attachment reports safety 512 frames and period 256 frames, with zero late output writes, input-ahead callbacks or hardware overruns in the observed test.

Full Security was confirmed by the user earlier with `sudo bputil -d`; SIP was checked independently. No startup-security or management setting was changed.

## Runtime margin control (version 12)

Version 12 is activated and attached. It boots with the clear version 11 baseline (512-frame safety, 256-frame hardware period). Its new `cltp` custom dictionary property accepts only `SafetyFrames` values 128, 256 or 512. The old crackling 64-frame setting is deliberately excluded.

The driver requests `RequestDeviceConfigurationChange`; only `ClarettDevice::PerformDeviceConfigurationChange`, after the host stops I/O, may update the input/output safety offsets. The hardware period becomes half the selected margin; new descriptors are built only on the following engine start. The HAL ring remains 4096 frames. Core Audio automatically restarts existing I/O. Failed and aborted requests are reported separately from the applied value. Nothing is saved to flash.

A runtime change from 512 to 256 completed successfully on the test hardware: Core Audio and the driver both read back safety 256, the hardware period became 128, the pending flag cleared, and streaming resumed. The user confirmed clean playback and monitoring with the 256-frame margin, but excessive monitoring delay remained. A subsequent 128-frame margin test produced crackling and worse perceived delay, so the driver was returned to 256. An attempted app-buffer request of 128 was accepted, but the running clients retained a 512-frame effective buffer; this must not be reported as a successful 128-frame test.

The host now has a separate Driver safety margin picker. 512 is the initial verified baseline. The 256-frame setting also passed the user’s clarity test; 128 has now failed and should not be used for normal monitoring. A scrollable window keeps the additional controls accessible. The native automation connection failed during installation; the user opened version 12, activated it and reconnected the hardware. Subsequent verification used Core Audio APIs and I/O Registry readback.

For equivalent strict command-line configuration:

```sh
zsh scripts/audio-configure.sh margin 512  # known clear fallback
zsh scripts/audio-configure.sh margin 256  # experimental
zsh scripts/audio-configure.sh margin 128  # experimental
zsh scripts/audio-configure.sh buffer 128  # app may override; verify actual value
zsh scripts/audio-status.sh
```

The utility matches the exact prototype device UID. It does not start I/O, play or record audio, or change default devices. The existing status utility remains read-only. Tests cover all permitted descriptor cadences and rejection of invalid periods and margins. Audio/attachment builds and signature/profile checks pass.

## Controls

The host offers Core Audio buffer sizes 64, 128, 256, 512, 1024 and 2048 frames, filtered against the live supported range. On this Mac Core Audio reported 15–1536, so 2048 is omitted. The active value is read back after setting. Audio applications can select a different size when opening their engine; this control does not replace an application's own additional buffering.

Hardware monitoring offers input 1 centered, input 2 centered, or stereo inputs 1/2; outputs 1/2, outputs 3/4, or both; and complementary live-input/computer blend from 0 to 100 percent. The total row gain is at most unity. Turn off software input monitoring when evaluating this direct path to avoid hearing two differently delayed copies.

The monitor request uses a typed AudioDriverKit custom dictionary property. Only Enabled, InputMode, OutputPair and Blend are accepted, with strict type/range checks. An accepted request is distinct from verified device state. UI status comes from the driver's apply/readback result.

## Device changes and restoration

Before the first enable, the driver reads all 48 routes and all 16 mixer rows. It reserves two mixer buses unused by existing routes and finds input slots whose reassignment cannot affect any existing active mix. If safe resources are unavailable, it rejects the request. The 2Pre analogue source pins are 0x400 and 0x402.

Enable stages mixer rows and input routes before switching the selected output destinations. Every completed change is read back. Disable first checks that the owned routes and rows still match the active plan, then restores and verifies the original routes and the two borrowed mixer rows. Changes from another controller are treated as conflicts. A transport failure poisons the session and is reported as uncertain when writes may have taken effect.

The fixed allowlist permits MUX_READ, GET_MIX, SET_MIX and SET_MUX only with model-specific geometry and band 0. There is no flash-save command or arbitrary configuration API. Settings are volatile. Disable monitoring and wait for verified restoration before an upgrade or disconnect: removal can prevent the old driver from restoring routes, and a warm reconnect can retain the volatile device state without the original in-memory snapshot. A cold power cycle is the recovery path for lost volatile state.

## Scheduling and diagnostics

Mailbox controls run on their own serialized queue. An atomic ownership handoff and a period-queue barrier keep mailbox cause registers separate from audio period ACKs. The period timer continues servicing the two audio cause blocks during a command. HAL PCM callbacks contain no mailbox calls, MMIO, allocations or locks. Queues survive until driver free, and queued property requests retain the driver until completion.

Version 8 uses a 64-frame hardware period and a 64-frame input/output safety offset, while the HAL ring remains 4096 frames. The timer targets 250 microseconds. Those settings are experimental; a small buffer being accepted does not prove reliable low-latency audio.

Version 10 additionally measures maximum poll gap, MMIO read-block duration, event gap, late polls, input requests extending beyond the last confirmed capture frame, and output writes beginning behind the confirmed hardware frame. The confirmed frame position is quantized to completed hardware events, so an input-ahead count within one period is diagnostic evidence rather than proof of invalid samples. Counters reset on an engine restart. The reference driver explicitly warns that cumulative hardware counters can remain smooth despite audible timing faults.

## Checks and remaining work

Sanitized tests cover route preservation, available mixer resources, 2Pre pins, mono/stereo modes, output pairs, blend headroom, snapshot retention, exact command serialization and command permissions. Protocol and DMA/ring tests continue to pass. Audio and attachment targets compile against Xcode 27.

Version 10 initial telemetry showed zero late polls, zero input-ahead callbacks, zero late output writes, a maximum poll gap of 516 microseconds and maximum MMIO read-block duration of 46 microseconds. A later extended observation reached one late poll and two input-ahead callbacks (maximum 24 frames, below the 64-frame event quantization). Both runs reported zero hardware overruns. These counters do not identify the audible fault.

Version 11 restores a 512-frame input/output safety offset, 256-frame hardware period and 500-microsecond timer target. These are the version 6 settings that passed clear playback and capture tests. Client buffer selection remains available, but the conservative driver margin adds latency; reliable reduced-latency streaming is not yet claimed.

Pending: acceptable software-monitoring delay in a separate audio host, reliable margins below 256 frames, sustained-load/sleep-wake tests, and measured round-trip latency. No test tone or agent recording is used; the user performs audible tests. Rates other than 48 kHz, external clocking, MIDI and Focusrite Control compatibility remain unsupported.

## Primary references

- [Apple audio-driver sample](https://developer.apple.com/documentation/audiodriverkit/creating-an-audio-device-driver) and the installed AudioDriverKit 27 SDK headers.
- [snd-clarett](https://github.com/JustALawnGnome7/snd-clarett), pinned research snapshot `fd416d45b74941663f703281666b33c9eccbda6e`, particularly period-servicer timing diagnostics and mixer command serialization.
- [clarett-sre](https://github.com/JustALawnGnome7/clarett-sre), snapshot `4df059996ecf781f5603f5395d6fe1bf6d56c1f7`, 2Pre route geometry and pins.

## Audacity monitoring latency investigation

On 21 October 2026 the user reported that version 12 at margin 256 and a software buffer selection of 128 was clean, but monitoring delay remained excessive. Core Audio readback during the investigation still showed an effective 512-frame device buffer; a picker selection alone does not prove the requested buffer is active. The only running client identified by the read-only Core Audio process API at that moment was Audacity (`org.audacityteam.audacity4`). Its saved AudioIO.LatencyDuration value was 10.0 ms, with a 48 kHz default project rate.

The official Audacity 4.0.1 source explains why that preference does not directly control this monitoring path. In `au3/libraries/au3-audio-io/AudioIO.cpp`, lines 602–604 select PortAudio’s defaultLowOutputLatency for software playthrough, and lines 648–650 select defaultHighInputLatency. The configured latency duration is used in the other branch. Loading the installed Audacity PortAudio library and enumerating device info, without opening a stream, reported these Clarett defaults at margin 256:

| Default | Reported value |
| --- | ---: |
| Low input | 10.0 ms |
| High input | 100.0 ms |
| Low output | 7.333 ms |
| High output | 16.667 ms |

Thus Audacity requests a 100 ms input latency hint for this path even with its buffer preference set to 10 ms. This is a requested hint, not a measured round-trip latency, and it does not explain or excuse the driver’s separate crackling at margins 64/128. No installed Audacity binary, library or signature was modified.

GarageBand is already installed. The next manual comparison uses it with the Clarett selected for both directions and input monitoring on an audio track, keeping hardware monitoring disabled. This checks a separate software monitoring path, including the ability to hear software effects, without changing driver security or installing another application.

Sources: [Audacity 4.0.1 AudioIO.cpp](https://github.com/audacity/audacity/blob/Audacity-4.0.1/au3/libraries/au3-audio-io/AudioIO.cpp#L602-L652), [PortAudio Core Audio implementation](https://github.com/PortAudio/portaudio/blob/master/src/hostapi/coreaudio/pa_mac_core.c), and [Apple GarageBand input-monitoring guide](https://support.apple.com/guide/garageband/turn-on-input-monitoring-for-audio-tracks-gbnd4bc15233/mac). Downloaded source was inspected only; it was not built or executed. The read-only process diagnostics enumerate only running clients associated with the Clarett device.
