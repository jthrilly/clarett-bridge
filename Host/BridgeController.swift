// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI
import SystemExtensions
import IOKit
import AppKit
import CoreAudio

struct OutputPairSettings: Identifiable {
    let id: Int
    var softwareEnabled = false
    var ownershipKnown = false
    var ceiling: Double = -30
    var appliedCeiling: UInt32 = 30
    var attenuation: UInt32 = 127
    var title: String { id == 0 ? "Line outputs 1–2" : "Line outputs 3–4 / headphones" }
}

final class BridgeController: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    static let identifier = Bundle.main.object(forInfoDictionaryKey: "ClarettDriverIdentifier") as? String ?? "org.codaco.ClarettProbe.driver"
    @Published var message = "Choose Driver Setup to install or manage the driver."
    @Published var deviceStatus = "No driver service detected."
    @Published var pending = false
    @Published var summary = "Waiting for the driver."
    @Published var audioDeviceAvailable = false
    @Published var selectedBuffer: UInt32 = 512
    @Published var actualBuffer: UInt32 = 0
    @Published var bufferOptions: [UInt32] = [64, 128, 256, 512, 1024, 2048]
    @Published var monitorAvailable = false
    @Published var monitorInput: UInt32 = 1
    @Published var monitorOutput: UInt32 = 2
    @Published var monitorBlend: Double = 50
    @Published var monitorBusy = false
    @Published var monitorMessage = "Hardware monitor controls require the updated audio driver."
    @Published var selectedSafety: UInt32 = 512
    @Published var actualSafety: UInt32 = 0
    @Published var timingAvailable = false
    @Published var timingBusy = false
    @Published var timingMessage = "Adjustable driver margin requires version 12."
    @Published var attached = false
    @Published var streaming = false
    @Published var monitorEnabled = false
    @Published var monitorUncertain = false
    @Published var sampleRate: Double = 0
    @Published var selectedRate: Double = 48000
    @Published var rateOptions: [Double] = []
    @Published var rateAvailable = false
    @Published var rateBusy = false
    @Published var rateMessage = "Sample-rate control requires the updated audio driver."
    @Published var hardwarePeriod: UInt32 = 0
    @Published var hardwareOverruns: UInt64 = 0
    @Published var bufferMessage = "Choose a requested buffer, then check the active value."
    @Published var outputPairs = [OutputPairSettings(id: 0), OutputPairSettings(id: 1)]
    @Published var volumeAvailable = false
    @Published var volumeBusy = false
    @Published var volumeUncertain = false
    @Published var outputVolume: Double = 0
    @Published var volumeMessage = "Output controls require driver version 15 or later."
    var volumeEditing = false
    private var volumeDeadline: Date?
    private var volumeRequest: (UInt32, UInt32, Bool, Bool)?
    private var volumeSavePairs: [Int] = []
    private var volumeRevision: UInt64 = 0
    private var volumeBaseline: UInt64 = 0
    private static let volumeSelector: AudioObjectPropertySelector = 0x636c766f
    private var lastMonitorConfiguration = ""
    private var lastSafety: UInt32 = 0
    private var lastRate: Double = 0
    private var requestedRate: Double?
    private var rateDeadline: Date?
    private var audioDevice: AudioDeviceID = 0
    private static let monitorSelector: AudioObjectPropertySelector = 0x636c6d63
    private static let timingSelector: AudioObjectPropertySelector = 0x636c7470
    private var timer: Timer?

    override convenience init() { self.init(preview: false) }
    init(preview: Bool) {
        super.init()
        for i in 0..<2 {
            let key = "OutputCeiling\(i)"
            if let saved = UserDefaults.standard.object(forKey: key) as? Double, saved >= -60 && saved <= 0 {
                outputPairs[i].ceiling = saved
            }
        }
        if preview {
            attached = true; streaming = true; audioDeviceAvailable = true
            monitorAvailable = true; timingAvailable = true; volumeAvailable = true
            outputPairs[0].softwareEnabled = true; outputPairs[0].ownershipKnown = true
            outputPairs[0].attenuation = 42
            outputVolume = 90.55
            volumeMessage = "Output gains and software ownership verified."

            sampleRate = 48000; actualBuffer = 512; selectedBuffer = 128
            rateOptions = [44100, 48000, 88200, 96000]; rateAvailable = true
            rateMessage = "48 kHz is active. Internal clock."
            actualSafety = 256; selectedSafety = 256; hardwarePeriod = 128
            summary = "Audio streaming at 48 kHz."
            message = "Driver is available."
            monitorMessage = "Hardware monitoring is off."
            timingMessage = "256-frame driver margin is active."
            deviceStatus = "ClarettAudioRingFrames: 4096\nClarettAudioSampleRate: 48000\nClarettAudioSafetyFrames: 256\nClarettAudioPeriodFrames: 128\nClarettAudioOverruns: 0"
            return
        }
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        refresh()
    }
    static func rateLabel(_ rate: Double) -> String {
        (rate / 1000).formatted(.number.precision(.fractionLength(0...1))) + " kHz"
    }
    func activate() {
        pending = true
        message = "Requesting activation…"
        let request = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: Self.identifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }
    func deactivate() {
        pending = true
        message = "Requesting deactivation…"
        let request = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: Self.identifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        // This is a pending request, not proof of activation or device attachment.
        message = "macOS requires your approval of Clarett Bridge. Complete the system prompt or the extension approval in System Settings."
    }
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        .replace
    }
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        pending = false
        message = result == .completed ? "Extension request completed. Device attachment is checked separately below."
                                      : "Request accepted; macOS requires a restart before completion."
        refresh()
    }
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        pending = false
        let nsError = error as NSError
        message = "Activation/deactivation failed: \(nsError.domain) code \(nsError.code): \(nsError.localizedDescription)"
        refresh()
    }
    private func address(_ selector: AudioObjectPropertySelector) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    }
    private func findAudioDevice() -> AudioDeviceID {
        var property = address(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &property, 0, nil, &size) == noErr else { return 0 }
        var devices = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        guard !devices.isEmpty else { return 0 }
        let status = devices.withUnsafeMutableBytes { bytes in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &property, 0, nil, &size, bytes.baseAddress!)
        }
        guard status == noErr else { return 0 }
        for device in devices {
            var uid: Unmanaged<CFString>?
            var uidSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
            var uidProperty = address(kAudioDevicePropertyDeviceUID)
            if AudioObjectGetPropertyData(device, &uidProperty, 0, nil, &uidSize, &uid) == noErr,
               let value = uid?.takeRetainedValue(), value as String == "org.codaco.clarett.2pre.experimental" { return device }
        }
        return 0
    }
    private func refreshAudioSettings() {
        audioDevice = findAudioDevice()
        audioDeviceAvailable = audioDevice != 0
        if audioDeviceAvailable {
            var alive: UInt32 = 0
            var size = UInt32(MemoryLayout<UInt32>.size)
            var property = address(kAudioDevicePropertyDeviceIsAlive)
            audioDeviceAvailable = AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &alive) == noErr && alive != 0
        }
        guard audioDeviceAvailable else {
            monitorAvailable = false; timingAvailable = false; rateAvailable = false; volumeAvailable = false
            volumeBusy = false; volumeDeadline = nil; volumeRequest = nil
            volumeMessage = "Connect the interface to use output controls."

            actualBuffer = 0; actualSafety = 0; sampleRate = 0; lastRate = 0
            rateOptions = []; rateBusy = false; requestedRate = nil; rateDeadline = nil
            rateMessage = "Connect the interface to change its sample rate."
            return
        }
        var property = address(kAudioDevicePropertyBufferFrameSize)
        var frames: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &frames) == noErr {
            if actualBuffer == 0 { selectedBuffer = frames }
            actualBuffer = frames
        }
        property = address(kAudioDevicePropertyNominalSampleRate)
        var rate: Double = 0; size = UInt32(MemoryLayout<Double>.size)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &rate) == noErr {
            sampleRate = rate
            if rate != lastRate {
                selectedRate = rate; lastRate = rate
                if requestedRate == nil { rateMessage = "\(Self.rateLabel(rate)) is active. Internal clock." }
            }
        }
        var settable = DarwinBoolean(false)
        let canSetRate = AudioObjectIsPropertySettable(audioDevice, &property, &settable) == noErr && settable.boolValue
        property = address(kAudioDevicePropertyAvailableNominalSampleRates)
        size = 0
        rateOptions = []
        if AudioObjectGetPropertyDataSize(audioDevice, &property, 0, nil, &size) == noErr,
           size > 0 && size % UInt32(MemoryLayout<AudioValueRange>.size) == 0 {
            var ranges = [AudioValueRange](repeating: AudioValueRange(), count: Int(size) / MemoryLayout<AudioValueRange>.size)
            let status = ranges.withUnsafeMutableBytes { bytes in
                AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, bytes.baseAddress!)
            }
            if status == noErr {
                rateOptions = [44100, 48000, 88200, 96000].filter { candidate in
                    ranges.contains { candidate >= $0.mMinimum && candidate <= $0.mMaximum }
                }
            }
        }
        rateAvailable = canSetRate && rateOptions.count > 1
        if let target = requestedRate {
            if sampleRate == target {
                rateMessage = "\(Self.rateLabel(sampleRate)) is active. Internal clock."
                rateBusy = false; requestedRate = nil; rateDeadline = nil
            } else if let deadline = rateDeadline, Date() > deadline {
                rateMessage = "Rate change was not confirmed. Active: \(Self.rateLabel(sampleRate)). Check Diagnostics."
                rateBusy = false; requestedRate = nil; rateDeadline = nil
            }
        } else if !rateAvailable {
            rateMessage = "Activate driver version 13 or later to enable sample-rate changes."
        }
        var range = AudioValueRange()
        size = UInt32(MemoryLayout<AudioValueRange>.size)
        property = address(kAudioDevicePropertyBufferFrameSizeRange)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &range) == noErr {
            bufferOptions = [64, 128, 256, 512, 1024, 2048].map(UInt32.init).filter { Double($0) >= range.mMinimum && Double($0) <= range.mMaximum }
            if !bufferOptions.contains(selectedBuffer), let first = bufferOptions.first { selectedBuffer = first }
        }
        property = address(Self.volumeSelector)
        volumeAvailable = AudioObjectHasProperty(audioDevice, &property)
        if volumeAvailable && !volumeEditing && !volumeBusy {
            var value: Float32 = 0
            var volumeSize = UInt32(MemoryLayout<Float32>.size)
            var volumeProperty = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyVolumeScalar,
                mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
            if AudioObjectGetPropertyData(audioDevice, &volumeProperty, 0, nil, &volumeSize, &value) == noErr {
                outputVolume = Double(value) * 100
            }
        }
        property = address(Self.monitorSelector)
        monitorAvailable = AudioObjectHasProperty(audioDevice, &property)
        property = address(Self.timingSelector)
        timingAvailable = AudioObjectHasProperty(audioDevice, &property)
        property = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertySafetyOffset, mScope: kAudioObjectPropertyScopeInput, mElement: kAudioObjectPropertyElementMain)
        var safety: UInt32 = 0
        size = UInt32(MemoryLayout<UInt32>.size)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &safety) == noErr {
            actualSafety = safety
            if safety != lastSafety { selectedSafety = [256, 512].contains(safety) ? safety : 256; lastSafety = safety }
        }
    }
    func applyRate() {
        guard rateAvailable && rateOptions.contains(selectedRate) && !monitorEnabled && !monitorUncertain else {
            rateMessage = "Turn direct monitoring off before changing sample rate."
            return
        }
        var property = address(kAudioDevicePropertyNominalSampleRate)
        var rate = selectedRate
        let status = AudioObjectSetPropertyData(audioDevice, &property, 0, nil, UInt32(MemoryLayout<Double>.size), &rate)
        if status == noErr {
            requestedRate = rate; rateDeadline = Date(timeIntervalSinceNow: 20); rateBusy = true
            rateMessage = "Waiting for Core Audio and hardware readback…"
            refresh()
        } else {
            rateMessage = "Rate request failed (\(status)). Stop audio in other apps, turn direct monitoring off and retry."
        }
    }
    func applyTiming() {
        audioDevice = findAudioDevice()
        guard audioDevice != 0 else { timingMessage = "The Clarett audio device is unavailable."; return }
        var property = address(Self.timingSelector)
        let request = ["SafetyFrames": selectedSafety] as CFDictionary
        var reference = Unmanaged.passUnretained(request).toOpaque()
        let status = withExtendedLifetime(request) {
            AudioObjectSetPropertyData(audioDevice, &property, 0, nil, UInt32(MemoryLayout<UnsafeMutableRawPointer>.size), &reference)
        }
        timingBusy = status == noErr
        timingMessage = status == noErr ? "Waiting for Core Audio to apply the margin…" : "Margin request failed (\(status))."
    }
    func applyBuffer() {
        audioDevice = findAudioDevice()
        guard audioDevice != 0 else { message = "The Clarett audio device is unavailable."; return }
        var requested = selectedBuffer
        var property = address(kAudioDevicePropertyBufferFrameSize)
        let status = AudioObjectSetPropertyData(audioDevice, &property, 0, nil, UInt32(MemoryLayout<UInt32>.size), &requested)
        refreshAudioSettings()
        if status != noErr { bufferMessage = "Could not change the buffer (\(status)). Stop audio in other apps and try again." }
        else if actualBuffer == requested { bufferMessage = "Applied \(actualBuffer) frames. Audio apps can change this when they open the device." }
        else { bufferMessage = "Requested \(requested) frames; Core Audio is using \(actualBuffer). A running audio app may choose its own buffer." }
    }
    func applyMonitor(enabled: Bool) {
        audioDevice = findAudioDevice()
        guard audioDevice != 0 else { message = "The Clarett audio device is unavailable."; return }
        var property = address(Self.monitorSelector)
        let request = ["Enabled": enabled, "InputMode": monitorInput, "OutputPair": monitorOutput, "Blend": UInt32(monitorBlend)] as CFDictionary
        var reference = Unmanaged.passUnretained(request).toOpaque()
        let status = withExtendedLifetime(request) {
            AudioObjectSetPropertyData(audioDevice, &property, 0, nil, UInt32(MemoryLayout<UnsafeMutableRawPointer>.size), &reference)
        }
        if status == noErr {
            monitorBusy = true
            monitorMessage = "Applying hardware monitoring and checking the device readback…"
        } else {
            monitorMessage = "Hardware monitor request failed (\(status)). Check Diagnostics."
        }
    }
    func applyOutputVolume() {
        guard volumeAvailable && !volumeBusy && !volumeUncertain && outputPairs.contains(where: { $0.softwareEnabled }) else { return }
        // Standard Core Audio control: system volume keys use this same driver
        // path. The driver, rather than the UI, applies the per-pair ceilings.
        var property = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyVolumeScalar,
            mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
        var scalar = Float32(outputVolume / 100)
        let status = AudioObjectSetPropertyData(audioDevice, &property, 0, nil, UInt32(MemoryLayout<Float32>.size), &scalar)
        volumeSubmitted(status)
    }
    func configureOutputs(togglePair: Int? = nil) {
        guard volumeAvailable && !volumeBusy && !volumeUncertain else { return }
        var enabled = outputPairs.map(\.softwareEnabled)
        var caps = outputPairs.map(\.appliedCeiling)
        if let pair = togglePair {
            guard (0..<2).contains(pair) else { return }
            enabled[pair].toggle()
            if enabled[pair] { caps[pair] = UInt32((-outputPairs[pair].ceiling).rounded(.up)) }
        } else {
            caps = outputPairs.map { UInt32((-$0.ceiling).rounded(.up)) }
        }
        var property = address(Self.volumeSelector)
        let request = ["Enabled12": enabled[0], "Enabled34": enabled[1],
                       "Ceiling12": caps[0], "Ceiling34": caps[1]] as CFDictionary
        var reference = Unmanaged.passUnretained(request).toOpaque()
        let status = withExtendedLifetime(request) {
            AudioObjectSetPropertyData(audioDevice, &property, 0, nil, UInt32(MemoryLayout<UnsafeMutableRawPointer>.size), &reference)
        }
        if status == noErr {
            volumeRequest = (caps[0], caps[1], enabled[0], enabled[1])
            volumeSavePairs = togglePair.map { [$0] } ?? [0, 1]
        }
        volumeSubmitted(status)
    }
    private func volumeSubmitted(_ status: OSStatus) {
        if status == noErr {
            volumeBaseline = volumeRevision
            volumeBusy = true; volumeDeadline = Date(timeIntervalSinceNow: 15)
            volumeMessage = "Applying output controls and checking hardware readback…"
            // Poll promptly after the short transaction; the regular poll also
            // handles system-volume changes made while this app is closed.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.refresh() }
        } else {
            volumeMessage = "Output request rejected (\(status)). Wait for other controls to finish and try again."
            refresh()
        }
    }
    private func refreshOutputs(_ properties: [String: Any]) {
        guard let state = properties["ClarettVolumeStatus"] as? String else { return }
        let revision = (properties["ClarettVolumeRevision"] as? NSNumber)?.uint64Value ?? 0
        volumeRevision = revision
        volumeUncertain = (properties["ClarettVolumeUncertain"] as? NSNumber)?.boolValue ?? false
        let pending = (properties["ClarettVolumePending"] as? NSNumber)?.boolValue ?? false
        if pending { volumeBusy = true }
        else if volumeBusy && revision > volumeBaseline {
            volumeBusy = false; volumeDeadline = nil
            if let request = volumeRequest, state == "verified",
               (properties["ClarettVolumeCeiling12"] as? NSNumber)?.uint32Value == request.0,
               (properties["ClarettVolumeCeiling34"] as? NSNumber)?.uint32Value == request.1,
               (properties["ClarettVolumeEnabled12"] as? NSNumber)?.boolValue == request.2,
               (properties["ClarettVolumeEnabled34"] as? NSNumber)?.boolValue == request.3 {
                if volumeSavePairs.contains(0) { UserDefaults.standard.set(-Double(request.0), forKey: "OutputCeiling0") }
                if volumeSavePairs.contains(1) { UserDefaults.standard.set(-Double(request.1), forKey: "OutputCeiling1") }
            }
            volumeRequest = nil
        }
        for i in 0..<2 {
            let suffix = i == 0 ? "12" : "34"
            outputPairs[i].softwareEnabled = (properties["ClarettVolumeEnabled" + suffix] as? NSNumber)?.boolValue ?? false
            outputPairs[i].ownershipKnown = (properties["ClarettVolumeKnown" + suffix] as? NSNumber)?.boolValue ?? false
            outputPairs[i].appliedCeiling = (properties["ClarettVolumeCeiling" + suffix] as? NSNumber)?.uint32Value ?? 30
            outputPairs[i].attenuation = (properties["ClarettVolumeHardware" + suffix] as? NSNumber)?.uint32Value ?? 127
        }
        if volumeUncertain { volumeMessage = "Output state is uncertain. Turn physical levels down and reconnect before using software control again." }
        else if state == "turn-knob-down" { volumeMessage = "Turn the front-panel monitor knob fully down, then try returning control to the knob again." }
        else if volumeBusy { volumeMessage = "Waiting for hardware output readback…" }
        else if state == "verified" {
            volumeMessage = outputPairs.contains(where: { $0.softwareEnabled }) ?
                "Output gains and software ownership verified. Raising a limit keeps the current level unchanged until you turn the volume up." :
                "Limits applied. Enable software control to use them; otherwise device controls keep their current ownership."
        }
        else if state == "unchanged" { volumeMessage = "Output controls are unchanged. Choose a limit, then enable software control for the pair you use." }
        else if pending { volumeMessage = "Applying output controls…" }
        else { volumeMessage = "Output change failed. Check Diagnostics before retrying." }
    }
    func refresh() {
        var iterator: io_iterator_t = 0
        guard let match = IOServiceNameMatching("ClarettProbe"),
              IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator) == KERN_SUCCESS else {
            deviceStatus = "Could not query the I/O Registry."
            return
        }
        defer { IOObjectRelease(iterator) }
        var reports: [String] = []
        attached = false; streaming = false; monitorEnabled = false; monitorUncertain = false
        while case let entry = IOIteratorNext(iterator), entry != 0 {
            defer { IOObjectRelease(entry) }
            var unmanaged: Unmanaged<CFMutableDictionary>?
            guard IORegistryEntryCreateCFProperties(entry, &unmanaged, kCFAllocatorDefault, 0) == KERN_SUCCESS,
                  let properties = unmanaged?.takeRetainedValue() as? [String: Any] else { continue }
            refreshOutputs(properties)
            let state = properties["ClarettProbeStatus"] as? String ?? "unknown"
            attached = true; streaming = state == "audio-streaming"
            monitorEnabled = (properties["ClarettMonitorEnabled"] as? NSNumber)?.boolValue ?? false
            monitorUncertain = (properties["ClarettMonitorUncertain"] as? NSNumber)?.boolValue ?? false
            hardwarePeriod = (properties["ClarettAudioPeriodFrames"] as? NSNumber)?.uint32Value ?? 0
            hardwareOverruns = (properties["ClarettAudioOverruns"] as? NSNumber)?.uint64Value ?? 0
            if monitorEnabled {
                let input = (properties["ClarettMonitorInputMode"] as? NSNumber)?.uint32Value ?? 1
                let output = (properties["ClarettMonitorOutputPair"] as? NSNumber)?.uint32Value ?? 2
                let blend = (properties["ClarettMonitorBlend"] as? NSNumber)?.doubleValue ?? 50
                let signature = "\(input)/\(output)/\(blend)"
                if signature != lastMonitorConfiguration {
                    monitorInput = input; monitorOutput = output; monitorBlend = blend; lastMonitorConfiguration = signature
                }
            }
            let rate = (properties["ClarettAudioSampleRate"] as? NSNumber)?.doubleValue ?? 48000
            summary = state == "audio-device-ready" ? "Audio ready at \(Self.rateLabel(rate))." : state == "audio-streaming" ? "Audio streaming at \(Self.rateLabel(rate))." : "Driver status: \(state)"
            monitorBusy = (properties["ClarettControlPending"] as? NSNumber)?.boolValue ?? false
            timingBusy = (properties["ClarettTimingPending"] as? NSNumber)?.boolValue ?? false
            if let target = requestedRate,
               (properties["ClarettRequestedSampleRate"] as? NSNumber)?.doubleValue == target,
               properties["ClarettRateStatus"] as? String == "request-failed" {
                rateMessage = "Hardware rate change failed. Check Diagnostics; the active value is read separately."
                requestedRate = nil; rateDeadline = nil; rateBusy = false
            }
            if let timingState = properties["ClarettTimingStatus"] as? String {
                timingMessage = timingState == "stable" ? "Driver margin applied. Choose an app buffer separately above." :
                    timingState == "applying" ? "Waiting for Core Audio configuration change…" : "Margin change failed. Check Diagnostics."
            }
            if let monitorState = properties["ClarettMonitorStatus"] as? String {
                let uncertain = (properties["ClarettMonitorUncertain"] as? NSNumber)?.boolValue ?? false
                if uncertain { monitorMessage = "Hardware monitor state is uncertain. Check Diagnostics before continuing." }
                else if monitorState == "on-verified" { monitorMessage = "Hardware monitoring is on. Routing and mixer readback verified." }
                else if monitorState == "off" || monitorState == "off-restored" { monitorMessage = "Hardware monitoring is off." }
                else if monitorState == "applying" { monitorMessage = "Applying hardware monitoring…" }
                else { monitorMessage = "Hardware monitor change failed. Check Diagnostics." }
            }
            let lines = properties.keys.filter { $0.hasPrefix("Clarett") }.sorted().map {
                "\($0): \(properties[$0]!)"
            }
            reports.append(lines.joined(separator: "\n"))
        }
        if reports.isEmpty { summary = "Connect and power on your Clarett, or open Driver Setup."; monitorBusy = false; timingBusy = false; hardwarePeriod = 0; hardwareOverruns = 0; monitorMessage = "Connect your Clarett to use direct monitoring." }
        refreshAudioSettings()
        if let deadline = volumeDeadline, Date() > deadline {
            volumeBusy = false; volumeDeadline = nil; volumeRequest = nil
            volumeMessage = "Output change was not confirmed. Check Diagnostics before continuing."
        }
        if !volumeAvailable { volumeMessage = "Output controls require driver version 15 or later." }
        deviceStatus = reports.isEmpty
            ? "No Clarett driver service. Activation, device connection, and PCI attachment are separate checks."
            : reports.joined(separator: "\n\n")
    }
}
