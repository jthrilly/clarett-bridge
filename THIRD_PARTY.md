# License and research provenance

Clarett Bridge is GPL-2.0-only; see [LICENSE](LICENSE) and source SPDX identifiers.

The implementation was developed from public protocol research, a public Linux reference driver, Apple's documented APIs and the locally installed SDK. No Focusrite binary was modified or disassembled, and no vendor firmware or driver binary is included.

| Source | Use / pinned research snapshot |
| --- | --- |
| [snd-clarett](https://github.com/JustALawnGnome7/snd-clarett) | GPL-2.0-only Linux reference for PCI/mailbox ordering, DMA geometry, stream servicing and scoped mixer commands; `fd416d45b74941663f703281666b33c9eccbda6e` |
| [clarett-sre](https://github.com/JustALawnGnome7/clarett-sre) | Public protocol/device-map facts; `4df059996ecf781f5603f5395d6fe1bf6d56c1f7` |
| [Apple AudioDriverKit documentation/sample](https://developer.apple.com/documentation/audiodriverkit/creating-an-audio-device-driver) | Framework object model, operation handlers and configuration-change lifecycle; sample source is not vendored |
| [Audacity 4.0.1](https://github.com/audacity/audacity/tree/Audacity-4.0.1) | Read-only inspection of monitoring latency selection; not linked, modified or shipped |
| [PortAudio](https://github.com/PortAudio/portaudio) | Read-only investigation of latency defaults; not linked or shipped |

The native host uses system SwiftUI, AppKit, IOKit, Core Audio and SystemExtensions frameworks. The artwork generator and icon are included as source. System symbols are rendered through Apple's APIs. This source release contains no downloaded reference repositories or third-party binary libraries.

Focusrite and Clarett are trademarks of their respective owners. This is an independent project without manufacturer affiliation or endorsement.

The output-volume implementation uses the same pinned protocol research for attenuation bytes 32/33 and 36/37, hardware-gain ownership bytes 52/56, live knob byte 112, and volatile commit codes 1 and 3. It does not copy a vendor driver binary or enable flash persistence. The volume safety policy and its tests are implemented in this repository.

Hardware master mute follows the same pinned Clarett protocol research: byte 24 (1 = muted), commit code 2, and the per-output master-mute assignment bitmap at byte 72, commit code 3. Other assignment bits are preserved; the four original 2Pre analogue outputs use bits 0–3. The implementation uses Apple's standard AudioDriverKit Boolean mute control.
