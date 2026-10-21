# Historical activation and mailbox test

This records the initial mailbox stage. For the current audio/control app, see [validation](validation.md) and [user guide](user-guide.md).

Date: 21 October 2026. Result: **PASS for the activation and mailbox milestone. Audio is not implemented.**

The Apple Development signed DriverKit extension activated through the normal macOS system-extension path, attached to the Clarett Thunderbolt PCI endpoint, established a mapped response-DMA buffer, and completed six identification queries. No IT involvement or security-policy changes were needed for this test.

## Test environment and security evidence

- Original Focusrite Clarett 2Pre Thunderbolt, PCI identity `1cb5:0002`.
- MacBook Pro, M5 Max, macOS 27.0.1; Xcode 27 / DriverKit 27 SDK.
- Extension `org.codaco.ClarettProbe.driver`, version `0.1/4`, activated and enabled.
- **Full Security:** user ran `sudo bputil -d` and reported Full Security before activation. The agent did not independently authenticate to read boot policy. SIP alone does not prove Full Security.
- **SIP:** directly checked after the successful mailbox run; enabled.
- No changes to boot security, SIP, FileVault, MDM enrollment or endpoint protection. No recovery key was needed by the prototype.
- Normal macOS extension approval was completed by the user. Development DriverKit PCI capability was enabled in the user's existing Apple Developer team with explicit approval. Both embedded development profiles include this Mac.

## Hardware result

The six requests alternate STREAM_INFO band 0 and GET_DATA offset 8, length 8, three times each. All replies passed opcode, sequence, status, response-length and response-landed checks. Repeated firmware replies matched and each geometry reply matched the expected 2Pre pair.

| Property | Observed value |
| --- | --- |
| Status | `mailbox-six-transactions-ok` |
| Completed transactions | 6 |
| Error code | 0 |
| BAR0 size | 65,536 bytes |
| Capability register | `0x032003fd` |
| Playback transport slots | 4 |
| Capture transport slots | 14 |
| Application firmware word | 1016 |
| FPGA firmware word, raw | `0x032003fd` (low 16 bits: 1021) |
| Active driver CDHash | `f2effcca5b71cefc62d21cb8073cfb1db9adefce` |

Transport slots are internal stream geometry, not physical socket counts. Firmware words are raw identification data; no dotted version format is inferred.

The earlier mailbox build rejected a valid 24-byte STREAM_INFO response because it required exactly four bytes. Version 4 accepts the four-byte geometry prefix plus bounded trailing fields. GET_DATA still requires exactly the requested eight bytes. The prior failed session was cleared by power cycling and reconnecting the unit before this passing run.

After the six queries, the driver disables device DMA, clears the advertised response address, closes the PCI session, and releases the mapping. The registered service retains the test results; the prototype does not continue polling or stream audio.

## Software validation

- Sanitized C++ state-machine tests pass with AddressSanitizer and UndefinedBehaviorSanitizer. Cases include delayed payloads, the observed 24-byte geometry reply, short/oversized replies, stale sequences, device errors, timeout, disconnect, terminal failures and the command allowlist.
- The host and DriverKit extension compile and sign with the installed SDK.
- Installed app and driver signatures verify; required signed entitlements and embedded profile grants verify; this Mac is provisioned; profiles are unexpired; PCI personality is restricted to `1cb5:0002`; unrestricted user-client access is absent.
- Xcode omits the ordinary macOS `com.apple.security.app-sandbox` flag from the final DriverKit signature despite the source request. The verifier reports this explicitly; it does not assert that this flag is present.
- Python tooling parses successfully. Git whitespace validation is clean.

## Deliverables and limits

At this milestone the project was local and the host was named Clarett Probe. The current public project is Clarett Bridge. Build artifacts and provisioning material are ignored by Git.

This proves the DriverKit activation, PCI-access and response-DMA mailbox path on this Mac under the user-confirmed Full Security policy. It does not establish audio playback/capture, MIDI, sample-clock control, interrupts, latency, sleep/wake, sustained reliability or support for other devices. Other Macs and future policy/profile changes need their own validation. Shipping a public driver also requires the relevant distribution entitlements and signing workflow.

The next implementation milestone would be AudioDriverKit device/stream exposure and controlled audio DMA. That is a separate hardware-development stage beyond this read-only mailbox prototype.
