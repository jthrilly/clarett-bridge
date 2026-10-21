// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI
import SystemExtensions
import IOKit
import AppKit
import CoreAudio

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
    @Published var hardwarePeriod: UInt32 = 0
    @Published var hardwareOverruns: UInt64 = 0
    @Published var bufferMessage = "Choose a requested buffer, then check the active value."
    private var lastMonitorConfiguration = ""
    private var lastSafety: UInt32 = 0
    private var audioDevice: AudioDeviceID = 0
    private static let monitorSelector: AudioObjectPropertySelector = 0x636c6d63
    private static let timingSelector: AudioObjectPropertySelector = 0x636c7470
    private var timer: Timer?

    override convenience init() { self.init(preview: false) }
    init(preview: Bool) {
        super.init()
        if preview {
            attached = true; streaming = true; audioDeviceAvailable = true
            monitorAvailable = true; timingAvailable = true
            sampleRate = 48000; actualBuffer = 512; selectedBuffer = 128
            actualSafety = 256; selectedSafety = 256; hardwarePeriod = 128
            summary = "Audio streaming at 48 kHz."
            message = "Driver is available."
            monitorMessage = "Hardware monitoring is off."
            timingMessage = "256-frame driver margin is active."
            deviceStatus = "ClarettProbeStatus: audio-streaming\nClarettAudioSampleRate: 48000\nClarettAudioSafetyFrames: 256\nClarettAudioPeriodFrames: 128\nClarettAudioOverruns: 0"
            return
        }
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        refresh()
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
        guard audioDeviceAvailable else { monitorAvailable = false; timingAvailable = false; actualBuffer = 0; actualSafety = 0; sampleRate = 0; return }
        var property = address(kAudioDevicePropertyBufferFrameSize)
        var frames: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &frames) == noErr {
            if actualBuffer == 0 { selectedBuffer = frames }
            actualBuffer = frames
        }
        property = address(kAudioDevicePropertyNominalSampleRate)
        var rate: Double = 0; size = UInt32(MemoryLayout<Double>.size)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &rate) == noErr { sampleRate = rate }
        var range = AudioValueRange()
        size = UInt32(MemoryLayout<AudioValueRange>.size)
        property = address(kAudioDevicePropertyBufferFrameSizeRange)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &range) == noErr {
            bufferOptions = [64, 128, 256, 512, 1024, 2048].map(UInt32.init).filter { Double($0) >= range.mMinimum && Double($0) <= range.mMaximum }
            if !bufferOptions.contains(selectedBuffer), let first = bufferOptions.first { selectedBuffer = first }
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
            summary = state == "audio-device-ready" ? "Audio ready at 48 kHz." : state == "audio-streaming" ? "Audio streaming at 48 kHz." : "Driver status: \(state)"
            monitorBusy = (properties["ClarettControlPending"] as? NSNumber)?.boolValue ?? false
            timingBusy = (properties["ClarettTimingPending"] as? NSNumber)?.boolValue ?? false
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
        deviceStatus = reports.isEmpty
            ? "No Clarett driver service. Activation, device connection, and PCI attachment are separate checks."
            : reports.joined(separator: "\n\n")
    }
}
