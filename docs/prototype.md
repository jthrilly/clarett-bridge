# Prototype decisions

*Historical milestone notes. For the current rate-aware driver and control app, see [architecture](architecture.md), [user guide](user-guide.md) and [validation](validation.md).*

## Initial scope

The first executable driver is attachment-only. Mailbox support is a separate build mode and version so activation/PCI matching can be diagnosed before permitting device DMA. No Core Audio or MIDI framework is required at this stage.

The extension has no user client. Its mailbox commands are a fixed allowlist compiled into the driver. The host app requests activation/deactivation through Apple's SystemExtensions framework and reads diagnostic properties through the I/O Registry.

## Matching and signing

The development PCI entitlement uses Apple's development-only wildcard value. The IOKit personality matches only the external PCI identity `1cb5:0002`, not all PCI devices. This identity is shared by the Clarett and some Red interfaces; stream geometry narrows the mailbox experiment to the 2Pre.

The prototype uses ordinary Apple Development signing and device provisioning. Locally ad-hoc signing a driver or disabling SIP would not validate the Full Security requirement. The build scripts do not change those settings.

Xcode 27's DriverKit packaging omits the ordinary macOS `com.apple.security.app-sandbox` entitlement from the final driver signature, even when requested in the source entitlement file and build setting. The signature verifier reports this explicitly and does not claim that this macOS App Sandbox flag is present. The required DriverKit and PCI grants must be present in both the real signature and the embedded profile; unrestricted user-client access must be absent. DriverKit execution and Full Security acceptance are tested through the normal OS activation path.

## DMA and ordering

The response allocation is 16 KiB with 16 KiB alignment. The DMA mapper must return one segment covering the allocation, aligned to 4 KiB and within a 32-bit device-visible address range. This deliberately matches the reference Linux driver's conservative DMA mask. A mapping that fails these constraints is rejected instead of treating a CPU pointer as a device address.

The driver advertises the mapped address, waits for its acknowledgement, then sends at most six small queries. Each transaction uses a fresh response buffer header and checks opcode, sequence, status, response size and device response-landed phase. A timeout poisons the session. It does not guess that a response landed, acknowledge a partial response, or submit a new command after an uncertain result.

The hardware test of version 3 returned a successful STREAM_INFO reply with sequence 0, length 24, phase 3 and DONE set. Version 2/3 incorrectly required exactly four bytes, although the reference driver consumes only the four-byte geometry prefix and accepts longer replies. Version 4 accepts a STREAM_INFO payload of at least four bytes within the response allocation; GET_DATA still requires exactly eight. Unknown trailing geometry fields are ignored. Tests cover the observed 24-byte response, truncated and oversized replies, and oversized GET_DATA rejection.

The initial transport uses the reference implementation's polled/no-MSI fallback. Legacy INTx delivery is disabled. No interrupt vectors or streaming engine are configured. Interrupt-driven transport can be investigated later if hardware evidence requires it.

The response address is cleared during cleanup while the allocation remains mapped. Bus mastering is disabled before clearing the address. DriverKit's PCI Close disables Bus Lead and Memory Space before the DMA mapping is released. Cleanup is idempotent.

## Verification gates

1. Sanitized state-machine tests pass.
2. Both app and extension compile against the real installed SDK.
3. Real signatures and embedded development profiles authorize the required capabilities.
4. Full Security and SIP are verified independently.
5. The system extension activates with the normal approval flow.
6. PCI attachment and the capability register read succeed.
7. Six identification transactions complete with consistent 2Pre geometry and firmware words.

Build success alone is not hardware success. Any pending user approval, reboot requirement, signing failure or policy denial must remain explicit in the recorded result.

## Audio stage

The separate audio build now subclasses IOUserAudioDriver and exposes a fixed 48 kHz duplex device. It retains the PCI/mailbox session, maps a separate audio DMA ring and adds a fixed internal-clock handshake. Audio buffers and callbacks are separate from mailbox protocol handling. See [audio streaming](audio-streaming.md) for the implementation, current hardware evidence, timestamp correction, manual test and remaining limitations.
