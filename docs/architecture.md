# Architecture

## Components

| Directory | Responsibility |
| --- | --- |
| `Host/` | Native SwiftUI control app, Core Audio requests, extension activation and registry readback |
| `Driver/ClarettAudio.*` | AudioDriverKit driver, PCI ownership, DMA engine, sample-rate transitions and mixer controls |
| `Driver/ClarettDevice.*` | Host-approved configuration-change callbacks |
| `Driver/ClarettSettings.*` | Strict typed custom properties for monitor and timing requests |
| `Shared/` | Testable protocol allowlist, DMA geometry and mixer/routing planner |
| `Tests/` | Sanitized hardware-independent C++ tests |
| `Tools/` | Read-only status, explicit configuration and artwork rendering utilities |

The host was renamed Clarett Bridge; legacy bundle IDs, service name and Core Audio UID remain compatible with the tested installation. App and driver build numbers are separate. The host uses SwiftUI/AppKit without third-party UI dependencies, reports readback separately from a requested value and does not start audio itself.

## PCI and mailbox

The personality matches PCI `1cb5:0002`; that ID is shared across some models, so the driver also checks reported playback/capture geometry. BAR0 must cover 64 KiB. Response and stream allocations are mapped through DriverKit DMA commands and required to fit checked, contiguous, 32-bit device-visible segments. CPU addresses are never treated as bus addresses.

Mailbox commands wait for both completion and response-landed state, then validate opcode, sequence, successful status and bounded length before acknowledgement. Failure poisons the session. The allowlist contains model-specific identification, four allowed rates/internal clock, sync reads and scoped volatile monitor operations. There is no arbitrary command, firmware update or flash-save API.

## Audio memory and clock

The hardware ring has 256 descriptors per direction with 16 frames per fragment: 4096 frames total. Output has four channels in 256-byte slots; input has fourteen channels in 1024-byte slots so the 896-byte fragment does not cross a 4 KiB boundary. HAL-facing buffers are separate packed rings. The realtime callback copies PCM and masks the hardware’s unused low eight bits; it allocates no memory, takes no locks, does no MMIO and sends no mailbox commands.

Both directions share the hardware counter and clock. A bounded polled period servicer acknowledges events and accumulates the modulo-256 counter in 16-frame units. Consumed output is retired to silence. A gap approaching a complete counter wrap stops the engine rather than guessing position. This is not an interrupt-based low-latency implementation.

The HAL zero-timestamp period is the full 4096-frame shared ring, not the smaller hardware event period. Confusing them in the first audio build caused a silent device and timeline resets. Full-ring timestamp publication corrected that initial failure.

## Sample-rate transitions

The device advertises 44.1, 48, 88.2 and 96 kHz and both streams advertise matching formats. `ClarettDevice::HandleChangeSampleRate` handles normal Core Audio rate requests, including the control app and Audio MIDI Setup. It rejects invalid/fractional rates, active I/O, an overlapping control/configuration request, or a live/uncertain direct-monitor snapshot. During the framework's stopped-I/O callback it serializes mailbox ownership, sends the internal-clock handshake, waits for lock and exact rate readback, and then updates the device and both stream formats. A known completed failure attempts to restore the old clock/formats. An uncertain transport or failed rollback marks the device unavailable instead of reporting success.

The fixed 14-channel input DMA stride is retained. An additional `STREAM_INFO{band 1}` query validates the 2Pre's 10 live double-speed capture channels before advertisement. Capture callbacks silence the four unused tail slots at 88.2/96 kHz. Poll intervals, lateness diagnostics, timestamp interpolation and the counter-wrap watchdog use the active rate; a 75 ms fixed watchdog would be unsafe at 96 kHz, where the ring wraps in about 42.7 ms.

Direct-monitor snapshots carry their speed band. The router has 48 destinations at single speed and 44 at double speed; reads, writes, pin allowlists, padding and restoration verification all use that band. No base-band snapshot is carried across a rate change: disable/restore monitoring before switching and enable it again afterward. Mixer geometry remains 30 inputs × 16 buses.

## Control serialization and runtime timing

A serialized control queue owns mailbox work. An atomic flag plus a period-queue barrier transfers ownership of mailbox cause registers; period handling continues on the stream cause blocks during commands. Teardown closes PCI/disables device DMA before unmapping allocations and drains outstanding work.

The timing dictionary accepts only the supported `SafetyFrames` values. The driver requests a Core Audio configuration change; only the device’s `PerformDeviceConfigurationChange` callback, after I/O has stopped, updates safety offsets. The next engine start builds descriptors for half that margin as its hardware period. The HAL ring size stays fixed. Driver objects are retained for queued requests, with closing-state checks and explicit failure/abort status.

The monitor planner snapshots all routes and mixer rows, chooses unused buses and input slots that cannot disturb active original mixes, stages the new mix, then switches selected outputs. It verifies readback and checks ownership/conflicts before restoring the snapshot. See [monitoring controls](monitoring-controls.md) for protocol geometry and failure handling.

## Validation boundaries

Tests model ordering, response validation, descriptor bounds, page containment, counter wrap, route preservation and command scope. They do not emulate the interface or prove audible quality. Hardware evidence and its limits are tracked in [validation](validation.md). Signed activation does not prove attachment; attachment does not prove audio; counters do not replace a listening or loopback test.

## Hardware-backed output volume

`ClarettVolume` is an output/main AudioDriverKit level control. Scalar requests use the framework's own scalar-to-dB transfer curve over a 48 dB normal range, then quantize toward attenuation and enqueue hardware work. The zero endpoint maps to the −127 dB hardware floor. Quiet takeover and preserved levels below that normal range are published at the standard control's minimum without increasing their physical gain; the exact attenuation remains in diagnostics. The `clvo` custom dictionary accepts exactly two enable booleans and two ceiling attenuation codes (0–60). Configuration preserves the driver’s current logical level; only the standard level control changes it. `Shared/Volume.hpp` enforces ceilings, quiet takeover and no increase when raising a ceiling. Returning ownership requires a sufficiently quiet live knob position. Both paths share the driver's exclusive control guard and serialized mailbox queue; no driver callbacks wait synchronously for the level control's own work queue.

The app polls a completion revision and independent registry status rather than treating property acceptance as application. It uses the standard Core Audio scalar for both slider display and setting, so system volume changes appear in the app. Ceiling preferences are saved after confirmation. Ownership is never automatically assumed on a new driver attachment; boot config readback is not authoritative. See [output-volume.md](output-volume.md) for the exact allowlist, volatile lifetime and limits of this protection.
