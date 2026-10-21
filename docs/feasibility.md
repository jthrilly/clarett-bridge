# Clarett 2Pre Thunderbolt: DriverKit feasibility

Assessment dated 21 October 2026. Scope: a replacement for the original Thunderbolt Clarett 2Pre driver on an Apple silicon Mac, with Full Security and SIP enabled. No driver was built, loaded, or tested during this investigation. No security settings, device configuration, or firmware were changed.

## Assessment

A replacement is technically plausible and worth a bounded prototype. Newly available public protocol research and a working Linux implementation substantially reduce the reverse-engineering work. The main uncertainties are now DriverKit activation on this managed Mac, DMA mapping on Apple silicon, and reliable audio timing. This remains a new hardware-driver implementation, rather than an update to Focusrite's installed binary.

There is a credible development-signing route that does not depend on obtaining Focusrite's signing identity or waiting for Apple to approve a public-distribution entitlement. Whether that route works on this particular managed Mac must be tested first. Full Security support does not exempt a driver from system-extension approval or management restrictions.

## Evidence examined

- Apple's AudioDriverKit and PCI driver documentation, real-time audio sample, and Apple DTS guidance on current DriverKit signing.
- `snd-clarett`, cloned and inspected at commit `fd416d45b74941663f703281666b33c9eccbda6e` (20 October 2026).
- `clarett-sre`, cloned and inspected at commit `4df059996ecf781f5603f5395d6fe1bf6d56c1f7` (20 October 2026), particularly `spec/clarett-interface.md`.
- The installed DriverKit 27.0 SDK: PCI register access, interrupt configuration, DMA mappings and address-width specifications are present.
- Earlier local checks established an M5 Max Mac, macOS 27.0.1, Automated Device Enrollment, FileVault, and the original Focusrite 4.116.2 kernel extension. The Clarett appeared on Thunderbolt during the initial troubleshooting. It was absent from the Thunderbolt and PCI queries at the end of this investigation, so there was no live endpoint validation for this assessment.

The public Linux project's hardware-test claims are the maintainer's reports. I inspected the implementation and protocol documentation but did not independently reproduce its audio results.

## Why the platform is suitable

Apple's AudioDriverKit documentation explicitly permits integration with PCIDriverKit. The intended arrangement is a user-space driver that owns the PCI endpoint and publishes Core Audio streams. AudioDriverKit provides real-time I/O callbacks; DriverKit provides the supporting PCI, interrupt, memory and DMA interfaces. RME has shipped Thunderbolt DriverKit drivers and documents a path that does not require Reduced Security. This demonstrates that Thunderbolt audio is not categorically restricted to kernel extensions.

An older Apple article says Thunderbolt audio requires a kernel extension. That conflicts with the newer AudioDriverKit documentation and actual vendor implementations; it should not be treated as a current blanket prohibition.

Sources: [AudioDriverKit](https://developer.apple.com/documentation/audiodriverkit), [audio driver sample](https://developer.apple.com/documentation/audiodriverkit/creating-an-audio-device-driver), [RME's Thunderbolt DriverKit announcement](https://forum.rme-audio.de/viewtopic.php?id=36046), [RME installation paths](https://rme-audio.de/first-steps-macos.html).

## The hardware protocol is substantially documented

The public Linux driver supports the Clarett Thunderbolt family, including the 2Pre. Its maintainer reports capture, playback, MIDI and control operation on hardware. The code separates PCI attachment, the Focusrite Control Protocol mailbox, PCM streaming, MIDI and user-space control transport.

The research identifies:

- PCI endpoint `1cb5:0002`, a 64 KiB BAR0 register interface and MSI interrupts.
- A mailbox for Focusrite Control Protocol commands, with response data delivered by DMA.
- Descriptor rings for full-duplex audio, 16-frame fragments, sample layout, hardware period counters and clock commands.
- Hardware quirks, including asynchronous mailbox responses and a 4 KiB fragment-boundary constraint.
- Routing, preamp, metering and persistent-setting maps, with separate per-model definitions.

The Linux driver is GPL-2.0-only. Any source reuse needs to preserve the applicable terms. The research repository has no root license file in the inspected checkout; copying its text, tools or maps requires separate consideration. A protocol reference and an implementation's reusable source are distinct resources.

Sources: [driver and status](https://github.com/JustALawnGnome7/snd-clarett), [implementation notes](https://github.com/JustALawnGnome7/snd-clarett/blob/main/DEVELOPMENT.md), [protocol specification](https://github.com/JustALawnGnome7/clarett-sre/blob/main/spec/clarett-interface.md).

## What must be rewritten

The Linux module cannot be compiled directly for macOS. Its ALSA interfaces, Linux PCI/DMA APIs, kernel threads, work queues and interrupt handling must be replaced.

| Component | Proposed macOS implementation | Main uncertainty |
|---|---|---|
| Installation and activation | Small host app containing a DriverKit extension | Provisioning, user approval and management policy |
| PCI attachment | PCIDriverKit, matched narrowly to the Clarett endpoint | Binding the actual endpoint under Full Security |
| Control protocol | C++ mailbox implementation on a serialized control queue | Correct DMA response ordering and timeouts |
| DMA and rings | DriverKit memory descriptors and IODMACommand mappings | IOMMU addresses, segment layout, cache visibility and device quirks |
| Audio | AudioDriverKit device, streams, timestamps and real-time buffer service | Clock stability, scheduling jitter and underruns |
| Controls | Small replacement control app through a restricted user client | Porting device maps and validating routing |
| MIDI | Separate Core MIDI integration | Additional scope; not required for first audio |
| Lifecycle | Explicit stop, detach, sleep/wake and failure handling | Safely stopping DMA before releasing mapped memory |

Focusrite Control compatibility must not be assumed. A driver publishing Core Audio streams does not automatically implement Focusrite's proprietary user-client interface. A small independent control utility is the simpler initial scope.

## Signing and Full Security

Apple DTS documents development variants of DriverKit entitlements available to paid developer accounts without special approval. With current Xcode, these support development-signed PCI drivers. The development PCI entitlement can authorize broad hardware access while the driver's matching personality remains restricted to the single Clarett endpoint. Development provisioning is tied to registered test machines.

This is an important correction to the idea that all DriverKit work must wait for Apple to approve a vendor-specific transport entitlement. Development and public distribution have different gates.

For public distribution, a Developer ID build needs the appropriate approved PCI entitlement and distribution packaging/signing, including notarization as applicable. An independent developer's approval for Focusrite's PCI identity is not guaranteed. We should validate that route separately if distributing the driver becomes a goal.

For this Mac, the first test should use ordinary development signing with Full Security and SIP retained. If activation is blocked by policy, the prototype has not met the user's requirement, even if it builds successfully. Disabling SIP or locally ad-hoc signing would not demonstrate success against that requirement.

Sources: [Apple DTS: How to sign a DEXT](https://developer.apple.com/forums/thread/809202), [PCI signing and device matching details](https://developer.apple.com/forums/thread/806986), [entitlement requests](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development).

## Most important engineering risks

**DMA portability.** The Linux code allocates coherent buffers and requests a 32-bit DMA mask. DriverKit supports DMA address-width specifications and returns device-visible mapped segments, but the replacement must use those mappings correctly rather than assume virtual or physical addresses are device addresses. The hardware exposes high-address registers; the Linux mask alone is not proof of a hardware 32-bit limit. Descriptor-table continuity and the documented 4 KiB fragment rules need explicit tests on Apple silicon.

**Real-time operation.** The Linux implementation services the engine from a real-time kernel thread. AudioDriverKit supplies a different execution model. We need to measure mailbox interference, interrupt latency, stream timestamps, ring occupancy and error counts. Begin at 48 kHz with conservative buffers. Low-latency parity with Focusrite's driver is a later measured target, not an assumption.

**Device state and controls.** Routing can make a working stream silent. The device preserves configuration in NVRAM. Initial work should read existing configuration and avoid automatic persistent writes or firmware operations. Native Core Audio exposure should use verified physical channels; internal transport slots are not automatically physical inputs.

**Maturity.** The Linux project is recent and documents remaining quirks. It is valuable implementation evidence, not a production specification guaranteed by Focusrite. Cross-check the source and protocol document where they differ, and resolve uncertainty using hardware tests.

**Managed installation.** A DriverKit driver still needs system-extension activation. We have not established whether this Mac's management policies permit a new development extension.

Source: [inspected Linux streaming implementation](https://github.com/JustALawnGnome7/snd-clarett/blob/fd416d45b74941663f703281666b33c9eccbda6e/clarett_pcm.c), [PCI/DMA initialization](https://github.com/JustALawnGnome7/snd-clarett/blob/fd416d45b74941663f703281666b33c9eccbda6e/clarett_main.c), [Apple real-time audio guidance](https://developer.apple.com/documentation/audiodriverkit/creating-an-audio-device-driver).

## Recommended experiment and decision gates

1. **Activation and attachment.** Build the smallest host app plus development-signed PCI extension. Confirm it activates under Full Security/SIP, matches only the Clarett and reads a known register. Pass requires successful activation and attachment on this Mac, not merely compilation. Requires a paid developer account with usable provisioning.
2. **Mailbox and response DMA.** Map a response buffer and obtain repeatable firmware/model responses. Validate addresses, ordering, bounds and timeouts. No configuration or flash writes.
3. **One audio configuration.** Implement a minimal 48 kHz full-duplex engine with conservative buffers, silence on unused outputs, two verified analogue inputs and the main output pair. Measure audio integrity and sustained operation. Any necessary routing change should be explicit and reversible.
4. **Useful local driver.** Add routing controls, remaining verified channels, sample-rate changes, clean hot unplug, and sleep/wake. Test actual audio apps and repeated restart/attach cycles.
5. **Broader release.** Add MIDI and other models if wanted; obtain distribution permissions, package the driver and broaden hardware/macOS testing.

Very rough effort estimates for one experienced low-level macOS developer, assuming usable development signing and no new hardware-protocol blocker: 1–3 engineering days for the initial activation/transport decision; 1–3 engineering weeks to first useful audio; 1–3 engineering months for a dependable local driver. A broadly supported replacement could take longer. These are planning estimates, not measured delivery commitments or guarantees.

**Recommendation:** proceed only as far as activation and mailbox DMA initially. Those tests are small enough to make the next investment concrete. Successful activation plus reliable responses under Full Security would justify the audio implementation. Failure at that gate would identify an installation-policy or signing obstacle before substantial audio work.
