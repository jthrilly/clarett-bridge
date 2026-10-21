# Troubleshooting

## No interface or no audio device

Check power and the complete Thunderbolt chain. This project supports the original Thunderbolt Clarett 2Pre, not a USB or Clarett+ model. Extension activation, bus enumeration, PCI matching and Core Audio device creation are separate stages; use Diagnostics to distinguish them.

```sh
systemextensionsctl list
zsh scripts/audio-status.sh
ioreg -r -n ClarettProbe -l -w0
```

If a new driver is activated while the old version owns the device, stop audio, disable hardware monitoring and wait for restoration, then reconnect the powered device. A failed mailbox session can require a cold power cycle. Do not repeatedly submit commands to a poisoned session.

## Silence

Choose Clarett 2Pre Thunderbolt (Experimental) in the audio app and match its project rate to the active rate. Start at 48 kHz when comparing against the previous listening-tested baseline. Check the physical output/headphone controls and the selected channels. The host control app does not create playback itself. “Streaming” indicates an open audio engine, not confirmed audible output.

## Crackling

Use driver margin 256, or 512 for more headroom. Margins 64/128 failed the current tests and are omitted from the normal UI. Increasing an application buffer cannot fix every driver/hardware timing issue. A requested buffer value is not necessarily the active buffer; read the active value in Audio Settings.

Capture the driver report and application name/settings when reporting faults. Zero hardware overruns do not rule out sample corruption or scheduling faults.

## Monitoring delay

Hardware monitoring avoids the software round trip; disable software monitoring when using it. To hear software effects, disable hardware monitoring and monitor through an audio host. GarageBand passed at a 256-frame margin and effective 512-frame Core Audio buffer.

Audacity 4.0.1 software playthrough selects PortAudio’s high input-latency hint; the installed library reported 100 ms on the test Mac. Its 10 ms ordinary buffer preference did not change that code path. See [source findings](monitoring-controls.md#audacity-monitoring-latency-investigation). This explains the application's large delay, not the driver’s separate failure at small margins.

## Driver approval or signing failure

Use your own Apple Developer identifiers/team and the correct DriverKit, PCI and audio-family development capabilities. Check the exact activation error shown in Driver Setup and run the signing verifier. A compile-only unsigned bundle cannot establish Full Security activation.

Keep Full Security and SIP enabled. This app does not change startup security or management policy; another Mac may require an administrator or its management owner to approve the extension. Do not use unrelated system-extension reset commands as a workaround.

## Hardware monitor change failed or uncertain

Read the diagnostic status. A routing conflict may mean the original snapshot cannot safely be restored over another controller’s changes. A transport error may leave the device’s volatile state uncertain; the driver stops further commands for that session. Cold power cycling returns to the device’s saved configuration. No flash-save command is provided.

## Sample-rate control is unavailable or a change fails

Version 12 exposes only 48 kHz; activate driver 13 and reconnect the powered interface to attach it. The new selector reads the live list, so installing only a new host app does not unlock additional rates.

Turn direct monitoring off and wait for verified restoration before a rate change. Stop audio in other apps if a request is rejected. Read the active nominal rate, input/output stream formats and `ClarettHardwareSampleRate`, not just the selected value. A DAW can request its own project rate.

If the driver reports a failed or uncertain clock transaction, it may mark the device unavailable. Reconnect or cold power cycle to recover the session; do not keep submitting commands to a poisoned mailbox. Only internal clock and 44.1/48/88.2/96 kHz are implemented. The 14-channel transport has 10 live input slots at double speed.
