# Architecture

## Components

| Directory | Responsibility |
| --- | --- |
| `Host/` | Native SwiftUI control app, Core Audio requests, extension activation and registry readback |
| `Driver/ClarettAudio.*` | AudioDriverKit driver, PCI ownership, DMA engine, fixed clock and mixer controls |
| `Driver/ClarettDevice.*` | Host-approved configuration-change callbacks |
| `Driver/ClarettSettings.*` | Strict typed custom properties for monitor and timing requests |
| `Shared/` | Testable protocol allowlist, DMA geometry and mixer/routing planner |
| `Tests/` | Sanitized hardware-independent C++ tests |
| `Tools/` | Read-only status, explicit configuration and artwork rendering utilities |

The host was renamed Clarett Bridge; legacy bundle IDs, service name and Core Audio UID remain compatible with the tested installation. App and driver build numbers are separate. The host uses SwiftUI/AppKit without third-party UI dependencies, reports readback separately from a requested value and does not start audio itself.

## PCI and mailbox

The personality matches PCI `1cb5:0002`; that ID is shared across some models, so the driver also checks reported playback/capture geometry. BAR0 must cover 64 KiB. Response and stream allocations are mapped through DriverKit DMA commands and required to fit checked, contiguous, 32-bit device-visible segments. CPU addresses are never treated as bus addresses.

Mailbox commands wait for both completion and response-landed state, then validate opcode, sequence, successful status and bounded length before acknowledgement. Failure poisons the session. The allowlist contains model-specific identification, fixed 48 kHz/internal clock, sync reads and scoped volatile monitor operations. There is no arbitrary command, firmware update or flash-save API.

## Audio memory and clock

The hardware ring has 256 descriptors per direction with 16 frames per fragment: 4096 frames total. Output has four channels in 256-byte slots; input has fourteen channels in 1024-byte slots so the 896-byte fragment does not cross a 4 KiB boundary. HAL-facing buffers are separate packed rings. The realtime callback copies PCM and masks the hardware’s unused low eight bits; it allocates no memory, takes no locks, does no MMIO and sends no mailbox commands.

Both directions share the hardware counter and clock. A bounded polled period servicer acknowledges events and accumulates the modulo-256 counter in 16-frame units. Consumed output is retired to silence. A gap approaching a complete counter wrap stops the engine rather than guessing position. This is not an interrupt-based low-latency implementation.

The HAL zero-timestamp period is the full 4096-frame shared ring, not the smaller hardware event period. Confusing them in the first audio build caused a silent device and timeline resets. Full-ring timestamp publication corrected that initial failure.

## Control serialization and runtime timing

A serialized control queue owns mailbox work. An atomic flag plus a period-queue barrier transfers ownership of mailbox cause registers; period handling continues on the stream cause blocks during commands. Teardown closes PCI/disables device DMA before unmapping allocations and drains outstanding work.

The timing dictionary accepts only the supported `SafetyFrames` values. The driver requests a Core Audio configuration change; only the device’s `PerformDeviceConfigurationChange` callback, after I/O has stopped, updates safety offsets. The next engine start builds descriptors for half that margin as its hardware period. The HAL ring size stays fixed. Driver objects are retained for queued requests, with closing-state checks and explicit failure/abort status.

The monitor planner snapshots all routes and mixer rows, chooses unused buses and input slots that cannot disturb active original mixes, stages the new mix, then switches selected outputs. It verifies readback and checks ownership/conflicts before restoring the snapshot. See [monitoring controls](monitoring-controls.md) for protocol geometry and failure handling.

## Validation boundaries

Tests model ordering, response validation, descriptor bounds, page containment, counter wrap, route preservation and command scope. They do not emulate the interface or prove audible quality. Hardware evidence and its limits are tracked in [validation](validation.md). Signed activation does not prove attachment; attachment does not prove audio; counters do not replace a listening or loopback test.
