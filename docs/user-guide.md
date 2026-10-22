# Using Clarett Bridge

## Overview

The overview shows the detected Core Audio sample rate, transport channel counts and active buffer. “Streaming” means an audio client has opened the engine, not that a listening test has passed. The app does not generate a tone, start a recording or change your default audio device.

Select **Clarett 2Pre Thunderbolt (Experimental)** in the recording or playback application. The format uses 32-bit signed interleaved PCM carrying 24-bit hardware samples, at 44.1, 48, 88.2 or 96 kHz with driver 13 or later. The 14 input and 4 output transport channels include digital/routing slots; they do not mean fourteen analogue sockets.

## Sample rate

In **Audio Settings → Sample rate**, choose 44.1, 48, 88.2 or 96 kHz and click **Apply rate**. Turn hardware direct monitoring off first and wait for verified restoration. The hardware clock and both Core Audio stream formats change together during a stopped-I/O configuration change. Check the active value after the request; an audio app can request a different rate when it opens the device. A running app may need its project rate changed or audio stopped before accepting the switch.

The default is 48 kHz after reattachment. The four choices are read from the active driver's advertised capabilities; the earlier version 12 driver offers only 48 kHz. Activate driver 13 to use the new selector. An activation request alone does not replace an attached old driver until it detaches.

The transport remains 14 input / 4 output at every supported rate. At 88.2/96 kHz, four ADAT channels disappear, leaving 10 live capture slots. The driver zeros the unused tail in each capture callback to suppress hardware residue. Direct monitoring uses the matching single/double-speed routing table when enabled again. External clocks and 176.4/192 kHz are not currently offered.

## Hardware direct monitoring

In Monitoring, choose input 1 centered, input 2 centered, or stereo inputs 1/2. Choose outputs 1/2, outputs 3/4 (headphone path), or both output pairs. The blend sets complementary live-input and computer gains. Click Enable monitoring or Apply mix to submit the change. The slider does not write hardware continuously.

The status shows the driver’s verified applied result. An accepted request alone is not enough. The original route and mixer state is read before enable and restored/read back on disable. Settings are volatile; they are not saved to device flash. Input gain and phantom power are not controlled by this app.

Turn off your recording app’s software input monitoring when using this path. Otherwise you may hear two copies at different delays. Disable direct monitoring and wait for restoration before disconnecting the device: unplugging can prevent the old driver from restoring its in-memory snapshot.

## Software monitoring and effects

Turn direct monitoring off, select the Clarett for input/output in the audio app and enable its monitoring. GarageBand passed a manual test with a 256-frame driver margin and an effective 512-frame Core Audio buffer. Specific plug-ins and numerical round-trip latency were not separately measured.

In Audio Settings, Driver responsiveness offers:

- **256 frames — Tested:** the lower margin that passed clear, acceptably responsive GarageBand monitoring.
- **512 frames — More headroom:** the conservative default.

Changing the margin briefly restarts existing audio through Core Audio’s configuration-change mechanism. It resets to 512 when the driver reattaches. Margins of 64/128 frames produced crackling, so the normal interface does not offer them. The research command-line API retains 128 for diagnosis.

The Audio app buffer control is a **request** to Core Audio. Check the active value shown separately. A running audio app can choose another value. The tested setup continued to report 512 even after a 128-frame request; that is not a validated 128-frame buffer test.

Audacity 4.0.1 software playthrough uses a separate high input-latency hint, observed as 100 ms on the test Mac, rather than its ordinary buffer preference. Reducing that preference to 10 ms did not make its monitoring sufficiently responsive. See [monitoring details](monitoring-controls.md).

## Diagnostics and driver setup

Diagnostics provides the current report, an explicit refresh and Copy report. Counters indicate driver activity, not audible integrity. The app distinguishes connection/readiness from the result of an activation request.

Driver Setup activates or deactivates the installed extension using the normal macOS flow. It does not change startup security, SIP or device-management policy. If activation fails, retain the exact domain/code shown and consult [troubleshooting](troubleshooting.md).

For removal, use Deactivate driver and follow any macOS restart instruction. Do not run `systemextensionsctl reset`, which affects unrelated extensions.

## Output levels

Open **Outputs** in app 0.3.2 after activating driver 17. Choose a per-pair maximum (default −30 dB), then explicitly enable software control. The main volume slider and macOS volume keys adjust hardware gains within those maxima. Raising a maximum preserves the current level until you turn the volume up separately. Turn physical levels down for the first test and before returning to knob control. Limits apply only to software-owned pairs; minimum volume is −127 dB attenuation, not mute. Saved limits are applied when you enable control again after a reconnect. See [output volume](output-volume.md) for complete ownership and failure behavior.

Driver 16 spreads normal volume changes over 48 dB, approximately 3 dB per normal Mac key step. Its zero endpoint selects the −127 dB hardware floor. This makes the useful adjustment range wider than driver 15's full 127 dB exposure. See [slider range](output-volume.md#software-slider-range).

## Mute

The Mac mute key and **Outputs → Mute** silence all four analogue outputs/headphones, including direct monitoring. Mute works in either gain-control mode. Unmute keeps the current chosen volume, checking software gain ownership and capped gains before releasing the hardware master mute. Recording continues while outputs are muted. Turn physical levels down for the first test; use the same button/key again to unmute. See [output volume and mute](output-volume.md#mute) for reconnect and failure handling.
