// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI
import SystemExtensions
import IOKit
import AppKit
import CoreAudio

@main
struct ClarettProbeApp: App {
    @StateObject private var controller = ProbeController()
    var body: some Scene {
        WindowGroup("Clarett Probe") {
            VStack(alignment: .leading, spacing: 16) {
                Text("Clarett Thunderbolt Probe").font(.title)
                Text(Bundle.main.object(forInfoDictionaryKey: "ClarettProbeMode") as? String == "audio"
                    ? "Experimental audio driver: 48 kHz, 4 playback channels and 14 capture channels. Select it in your audio app to start streaming."
                    : "This prototype tests driver activation and device identification. It does not provide audio.")
                Text("Build mode: \(Bundle.main.object(forInfoDictionaryKey: "ClarettProbeMode") as? String ?? "unknown") · version \(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "unknown")")
                HStack {
                    Button("Activate Driver") { controller.activate() }
                    Button("Deactivate Driver") { controller.deactivate() }
                    Button("Refresh Status") { controller.refresh() }
                }.disabled(controller.pending)
                Text(controller.message).textSelection(.enabled)
                Text(controller.summary).textSelection(.enabled)
                if Bundle.main.object(forInfoDictionaryKey: "ClarettProbeMode") as? String == "audio" {
                    GroupBox("Software audio buffer") {
                        HStack {
                            Picker("Frames", selection: $controller.selectedBuffer) {
                                ForEach(controller.bufferOptions, id: \.self) { frames in
                                    Text("\(frames)").tag(frames)
                                }
                            }.frame(width: 180)
                            Button("Set Buffer") { controller.applyBuffer() }
                            Text("Active: \(controller.actualBuffer) frames")
                        }.disabled(!controller.audioDeviceAvailable || controller.pending)
                    }
                    GroupBox("Hardware direct monitoring") {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Picker("Input", selection: $controller.monitorInput) {
                                    Text("Input 1 (mono)").tag(UInt32(1))
                                    Text("Input 2 (mono)").tag(UInt32(2))
                                    Text("Inputs 1–2 (stereo)").tag(UInt32(3))
                                }
                                Picker("Output", selection: $controller.monitorOutput) {
                                    Text("Outputs 1–2").tag(UInt32(1))
                                    Text("Outputs 3–4 / headphones").tag(UInt32(2))
                                    Text("Both output pairs").tag(UInt32(3))
                                }
                            }
                            Text("Live input \(Int(controller.monitorBlend))% / Computer \(100 - Int(controller.monitorBlend))%")
                            Slider(value: $controller.monitorBlend, in: 0...100, step: 1)
                            HStack {
                                Button("Enable / Update Monitor") { controller.applyMonitor(enabled: true) }
                                Button("Disable Monitor") { controller.applyMonitor(enabled: false) }
                            }
                            Text(controller.monitorMessage).textSelection(.enabled)
                            Text("Turn off software input monitoring in your recording app when using this hardware mix.")
                                .font(.caption).foregroundStyle(.secondary)
                        }.disabled(!controller.monitorAvailable || controller.monitorBusy || controller.pending)
                    }
                }
                DisclosureGroup("Diagnostics") {
                    ScrollView {
                        Text(controller.deviceStatus)
                            .font(.system(.body, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .textSelection(.enabled)
                    }.frame(height: 180)
                }
                Text("Full Security and SIP should remain enabled. Approval of this system extension may be requested by macOS.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(24).frame(minWidth: 650, minHeight: 440)
            .onAppear { controller.refresh() }
        }
    }
}

final class ProbeController: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    static let identifier = "org.codaco.ClarettProbe.driver"
    @Published var message = "Ready. Activate the driver to begin."
    @Published var deviceStatus = "No driver service detected."
    @Published var pending = false
    @Published var summary = "Waiting for the driver."
    @Published var audioDeviceAvailable = false
    @Published var selectedBuffer: UInt32 = 128
    @Published var actualBuffer: UInt32 = 0
    @Published var bufferOptions: [UInt32] = [64, 128, 256, 512, 1024, 2048]
    @Published var monitorAvailable = false
    @Published var monitorInput: UInt32 = 1
    @Published var monitorOutput: UInt32 = 2
    @Published var monitorBlend: Double = 50
    @Published var monitorBusy = false
    @Published var monitorMessage = "Hardware monitor controls require the updated audio driver."
    private var audioDevice: AudioDeviceID = 0
    private static let monitorSelector: AudioObjectPropertySelector = 0x636c6d63
    private var timer: Timer?

    override init() {
        super.init()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.refresh()
        }
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
        message = "macOS requires your approval of Clarett Probe. Complete the system prompt or the extension approval in System Settings."
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
        guard audioDeviceAvailable else { monitorAvailable = false; return }
        var property = address(kAudioDevicePropertyBufferFrameSize)
        var frames: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &frames) == noErr { actualBuffer = frames }
        var range = AudioValueRange()
        size = UInt32(MemoryLayout<AudioValueRange>.size)
        property = address(kAudioDevicePropertyBufferFrameSizeRange)
        if AudioObjectGetPropertyData(audioDevice, &property, 0, nil, &size, &range) == noErr {
            bufferOptions = [64, 128, 256, 512, 1024, 2048].map(UInt32.init).filter { Double($0) >= range.mMinimum && Double($0) <= range.mMaximum }
            if !bufferOptions.contains(selectedBuffer), let first = bufferOptions.first { selectedBuffer = first }
        }
        property = address(Self.monitorSelector)
        monitorAvailable = AudioObjectHasProperty(audioDevice, &property)
    }
    func applyBuffer() {
        audioDevice = findAudioDevice()
        guard audioDevice != 0 else { message = "The Clarett audio device is unavailable."; return }
        var requested = selectedBuffer
        var property = address(kAudioDevicePropertyBufferFrameSize)
        let status = AudioObjectSetPropertyData(audioDevice, &property, 0, nil, UInt32(MemoryLayout<UInt32>.size), &requested)
        refreshAudioSettings()
        message = status == noErr ? "Core Audio buffer is \(actualBuffer) frames. Audio apps may select their own buffer when they open the device."
                                 : "Could not change the audio buffer (\(status)). Stop audio in other apps and try again."
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
        while case let entry = IOIteratorNext(iterator), entry != 0 {
            defer { IOObjectRelease(entry) }
            var unmanaged: Unmanaged<CFMutableDictionary>?
            guard IORegistryEntryCreateCFProperties(entry, &unmanaged, kCFAllocatorDefault, 0) == KERN_SUCCESS,
                  let properties = unmanaged?.takeRetainedValue() as? [String: Any] else { continue }
            let state = properties["ClarettProbeStatus"] as? String ?? "unknown"
            summary = state == "audio-device-ready" ? "Audio ready at 48 kHz." : state == "audio-streaming" ? "Audio streaming at 48 kHz." : "Driver status: \(state)"
            monitorBusy = (properties["ClarettControlPending"] as? NSNumber)?.boolValue ?? false
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
        if reports.isEmpty { summary = "No Clarett driver service is attached." }
        refreshAudioSettings()
        deviceStatus = reports.isEmpty
            ? "No ClarettProbe service. Activation, device connection, and PCI attachment are separate checks."
            : reports.joined(separator: "\n\n")
    }
}
