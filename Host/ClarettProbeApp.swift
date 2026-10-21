// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI
import SystemExtensions
import IOKit
import AppKit

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
                Text("Build mode: \(Bundle.main.object(forInfoDictionaryKey: "ClarettProbeMode") as? String ?? "unknown")")
                HStack {
                    Button("Activate Driver") { controller.activate() }
                    Button("Deactivate Driver") { controller.deactivate() }
                    Button("Refresh Status") { controller.refresh() }
                }.disabled(controller.pending)
                Text(controller.message).textSelection(.enabled)
                GroupBox("Device results") {
                    ScrollView {
                        Text(controller.deviceStatus)
                            .font(.system(.body, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .textSelection(.enabled)
                    }.frame(minHeight: 180)
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
            let lines = properties.keys.filter { $0.hasPrefix("Clarett") }.sorted().map {
                "\($0): \(properties[$0]!)"
            }
            reports.append(lines.joined(separator: "\n"))
        }
        deviceStatus = reports.isEmpty
            ? "No ClarettProbe service. Activation, device connection, and PCI attachment are separate checks."
            : reports.joined(separator: "\n\n")
    }
}
