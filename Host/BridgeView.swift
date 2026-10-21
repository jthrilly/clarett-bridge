// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI
import AppKit

enum BridgePage: String, CaseIterable, Identifiable {
    case overview = "Overview", monitoring = "Monitoring", audio = "Audio Settings", diagnostics = "Diagnostics", setup = "Driver Setup"
    var id: String { rawValue }
    var icon: String {
        switch self {
        case .overview: return "square.grid.2x2"
        case .monitoring: return "headphones"
        case .audio: return "slider.horizontal.3"
        case .diagnostics: return "waveform.path.ecg"
        case .setup: return "puzzlepiece.extension"
        }
    }
    var subtitle: String {
        switch self {
        case .overview: return "Your Clarett, connected to your Mac."
        case .monitoring: return "Hear your inputs directly through the interface."
        case .audio: return "Balance responsiveness and reliable playback."
        case .diagnostics: return "Live driver status and troubleshooting details."
        case .setup: return "Install and manage the macOS driver extension."
        }
    }
}

enum BridgeStyle {
    static let accent = Color(red: 0.85, green: 0.16, blue: 0.20)
    static let iconShadow = Color(red: 0.40, green: 0.035, blue: 0.055)
    static let canvas = Color(nsColor: .windowBackgroundColor)
    static let card = Color(nsColor: .controlBackgroundColor)
    static let line = Color.primary.opacity(0.08)
}

struct BridgeView: View {
    @ObservedObject var controller: BridgeController
    @State private var page: BridgePage
    init(controller: BridgeController, initialPage: BridgePage = .overview) {
        self.controller = controller
        _page = State(initialValue: initialPage)
    }
    private var stateLabel: String {
        controller.streaming ? "Streaming" : controller.audioDeviceAvailable ? "Ready" : "Disconnected"
    }
    var body: some View {
        HStack(spacing: 0) {
            sidebar
            Divider()
            VStack(spacing: 0) {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 5) {
                        Text(page.rawValue).font(.system(size: 25, weight: .semibold, design: .rounded))
                        Text(page.subtitle).font(.callout).foregroundStyle(.secondary)
                    }
                    Spacer()
                    StatusBadge(text: stateLabel, active: controller.audioDeviceAvailable)
                }
                .padding(.horizontal, 30).padding(.top, 28).padding(.bottom, 22)
                ScrollView {
                    VStack(alignment: .leading, spacing: 20) {
                        switch page {
                        case .overview: overview
                        case .monitoring: monitoring
                        case .audio: audio
                        case .diagnostics: diagnostics
                        case .setup: setup
                        }
                    }.padding(.horizontal, 30).padding(.bottom, 28).frame(maxWidth: 900, alignment: .leading)
                }
            }.frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(BridgeStyle.canvas)
        .tint(BridgeStyle.accent)
        .frame(minWidth: 900, minHeight: 650)
    }
    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                BridgeMark().frame(width: 34, height: 34)
                VStack(alignment: .leading, spacing: 1) {
                    Text("Clarett").font(.system(size: 17, weight: .bold, design: .rounded))
                    Text("BRIDGE").font(.system(size: 10, weight: .semibold)).tracking(2.8).foregroundStyle(.secondary)
                }
            }.padding(.horizontal, 21).padding(.top, 30).padding(.bottom, 35)
            ForEach(BridgePage.allCases) { item in
                Button { page = item } label: {
                    HStack(spacing: 11) {
                        Image(systemName: item.icon).font(.system(size: 15)).frame(width: 18)
                        Text(item.rawValue).font(.system(size: 13, weight: page == item ? .semibold : .regular))
                        Spacer(minLength: 0)
                    }
                    .foregroundStyle(page == item ? BridgeStyle.accent : Color.primary.opacity(0.72))
                    .padding(.horizontal, 13).padding(.vertical, 12)
                    .background(page == item ? BridgeStyle.accent.opacity(0.10) : Color.clear, in: RoundedRectangle(cornerRadius: 9))
                    .contentShape(Rectangle())
                }.buttonStyle(.plain).padding(.horizontal, 10).padding(.bottom, 4)
                    .accessibilityAddTraits(page == item ? .isSelected : [])
            }
            Spacer()
            VStack(alignment: .leading, spacing: 9) {
                HStack(spacing: 6) {
                    Circle().fill(controller.audioDeviceAvailable ? BridgeStyle.accent : Color.secondary).frame(width: 6, height: 6)
                    Text(controller.audioDeviceAvailable ? "Clarett 2Pre" : "No interface connected").font(.caption)
                }
                Text("Independent, experimental driver").font(.system(size: 10)).foregroundStyle(.secondary)
                Text("Clarett Bridge · \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.2.0")")
                    .font(.system(size: 10)).foregroundStyle(.tertiary)
            }.padding(20)
        }.frame(width: 194).background(Color(nsColor: .underPageBackgroundColor).opacity(0.45))
    }
    private var overview: some View {
        Group {
            BridgeCard {
                HStack(alignment: .center, spacing: 22) {
                    ZStack {
                        RoundedRectangle(cornerRadius: 18).fill(BridgeStyle.accent.opacity(0.09)).frame(width: 84, height: 84)
                        Image(systemName: "waveform").font(.system(size: 32, weight: .light)).foregroundStyle(BridgeStyle.accent)
                    }.accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Clarett 2Pre").font(.system(size: 25, weight: .semibold, design: .rounded))
                        Text("THUNDERBOLT").font(.system(size: 10, weight: .semibold)).tracking(2).foregroundStyle(.secondary)
                        Text(controller.audioDeviceAvailable ? "Ready for playback, recording and monitoring." : "Connect and power on your interface to get started.")
                            .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer(minLength: 0)
                }
                Divider().padding(.vertical, 15)
                HStack {
                    Metric(label: "SAMPLE RATE", value: controller.sampleRate > 0 ? BridgeController.rateLabel(controller.sampleRate) : "—")
                    Spacer()
                    Metric(label: "TRANSPORT CHANNELS", value: "14 in / 4 out")
                    Spacer()
                    Metric(label: "ACTIVE BUFFER", value: controller.actualBuffer > 0 ? "\(controller.actualBuffer) frames" : "—")
                }
            }
            HStack(alignment: .top, spacing: 16) {
                ActionCard(icon: "headphones", title: "Direct monitoring", detail: "A hardware mix of your live input and computer audio.", button: "Open monitoring") { page = .monitoring }
                ActionCard(icon: "slider.horizontal.3", title: "Software monitoring", detail: "Set driver responsiveness, then monitor in your audio app.", button: "Open audio settings") { page = .audio }
            }
            BridgeCard {
                SectionHeading(icon: "checkmark.circle", title: "A tested starting point")
                Text("Use a 256-frame driver margin for clear, responsive monitoring in GarageBand. Choose 512 frames for more headroom.")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                HStack {
                    Text("Driver margin").font(.callout)
                    Spacer()
                    Text(controller.actualSafety > 0 ? "\(controller.actualSafety) frames active" : "Interface unavailable").font(.callout.weight(.medium))
                }.padding(.top, 9)
            }
        }
    }
    private var monitoring: some View {
        Group {
            BridgeCard {
                HStack {
                    SectionHeading(icon: "headphones", title: "Direct monitoring")
                    Spacer()
                    StatusBadge(text: controller.monitorUncertain ? "Check diagnostics" : controller.monitorEnabled ? "On · verified" : "Off", active: controller.monitorEnabled && !controller.monitorUncertain)
                }
                Text("Listen through the Clarett without the round trip through a recording app.")
                    .font(.callout).foregroundStyle(.secondary)
                Divider().padding(.vertical, 12)
                HStack(alignment: .top, spacing: 24) {
                    VStack(alignment: .leading, spacing: 8) {
                        FieldLabel(text: "INPUT")
                        Picker("Monitor input", selection: $controller.monitorInput) {
                            Text("Input 1 · Mono").tag(UInt32(1))
                            Text("Input 2 · Mono").tag(UInt32(2))
                            Text("Inputs 1–2 · Stereo").tag(UInt32(3))
                        }.labelsHidden().frame(maxWidth: .infinity)
                    }
                    VStack(alignment: .leading, spacing: 8) {
                        FieldLabel(text: "LISTEN ON")
                        Picker("Monitor output", selection: $controller.monitorOutput) {
                            Text("Outputs 1–2").tag(UInt32(1))
                            Text("Outputs 3–4 / Headphones").tag(UInt32(2))
                            Text("Both output pairs").tag(UInt32(3))
                        }.labelsHidden().frame(maxWidth: .infinity)
                    }
                }
                HStack {
                    BlendValue(title: "LIVE INPUT", value: Int(controller.monitorBlend))
                    Spacer()
                    BlendValue(title: "COMPUTER", value: 100 - Int(controller.monitorBlend))
                }.padding(.top, 24)
                Slider(value: $controller.monitorBlend, in: 0...100, step: 1).accessibilityLabel("Live input blend percentage")
                HStack {
                    Text("Computer only").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Text("Input only").font(.caption).foregroundStyle(.secondary)
                }
                Divider().padding(.vertical, 12)
                HStack(spacing: 10) {
                    Button(controller.monitorEnabled ? "Apply mix" : "Enable monitoring") { controller.applyMonitor(enabled: true) }
                        .buttonStyle(.borderedProminent)
                    if controller.monitorEnabled {
                        Button("Turn off") { controller.applyMonitor(enabled: false) }.buttonStyle(.bordered)
                    }
                    if controller.monitorBusy { ProgressView().controlSize(.small) }
                }
                .disabled(!controller.monitorAvailable || controller.monitorBusy || controller.rateBusy || controller.pending)
                Text(controller.monitorMessage).font(.caption).foregroundStyle(.secondary).padding(.top, 6).textSelection(.enabled)
            }.disabled(!controller.monitorAvailable || controller.rateBusy || controller.pending)
            NoteCard(icon: "waveform", title: "One monitoring path at a time", text: "Turn off software input monitoring in your audio app when using this hardware mix. To hear software effects, turn direct monitoring off and monitor in the app instead.")
        }
    }
    private var audio: some View {
        Group {
            BridgeCard {
                SectionHeading(icon: "waveform", title: "Sample rate")
                Text("One internal clock serves playback and capture. Changing rate briefly stops audio; recording apps may choose their own rate.")
                    .font(.callout).foregroundStyle(.secondary)
                HStack(spacing: 22) {
                    Picker("Sample rate", selection: $controller.selectedRate) {
                        if controller.rateOptions.isEmpty { Text("Interface unavailable").tag(controller.selectedRate) }
                        ForEach(controller.rateOptions, id: \.self) { Text(BridgeController.rateLabel($0)).tag($0) }
                    }.labelsHidden().frame(maxWidth: 220)
                    Spacer()
                    Button("Apply rate") { controller.applyRate() }.buttonStyle(.borderedProminent)
                }.padding(.top, 12)
                    .disabled(!controller.rateAvailable || controller.rateBusy || controller.timingBusy || controller.monitorBusy || controller.pending || controller.monitorEnabled || controller.monitorUncertain)
                HStack {
                    Text("Active sample rate").font(.callout).foregroundStyle(.secondary)
                    Spacer()
                    if controller.rateBusy { ProgressView().controlSize(.small) }
                    Text(controller.sampleRate > 0 ? BridgeController.rateLabel(controller.sampleRate) : "Unavailable").font(.callout.weight(.semibold))
                }.padding(12).background(BridgeStyle.accent.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
                Text(controller.rateMessage).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                if controller.monitorEnabled || controller.monitorUncertain {
                    Text("Turn direct monitoring off in Monitoring before switching rate.").font(.caption).foregroundStyle(.secondary)
                }
                Text("At 88.2/96 kHz, 10 capture channels are live; the four unused ADAT transport slots are silenced.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            BridgeCard {
                SectionHeading(icon: "slider.horizontal.3", title: "Driver responsiveness")
                Text("The driver margin gives audio time to reach the hardware. Changing it briefly restarts audio.")
                    .font(.callout).foregroundStyle(.secondary)
                HStack(spacing: 22) {
                    Picker("Driver margin", selection: $controller.selectedSafety) {
                        Text("256 frames · Tested").tag(UInt32(256))
                        Text("512 frames · More headroom").tag(UInt32(512))
                    }.labelsHidden().frame(maxWidth: 340)
                    Spacer()
                    Button("Apply margin") { controller.applyTiming() }.buttonStyle(.borderedProminent)
                }.padding(.top, 12).disabled(!controller.timingAvailable || controller.timingBusy || controller.rateBusy || controller.pending)
                AppliedValue(value: controller.actualSafety, label: "Driver margin", busy: controller.timingBusy).padding(.top, 12)
                Text(controller.timingMessage).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                if controller.actualSafety > 0 && controller.actualSafety < 256 {
                    Text("The active margin produced crackling in testing. Apply 256 or 512 frames.").font(.caption).foregroundStyle(.orange)
                }
            }
            BridgeCard {
                SectionHeading(icon: "square.stack.3d.up", title: "Audio app buffer")
                Text("Request a Core Audio buffer size. Running apps can choose a different size; the active value below is the one actually in use.")
                    .font(.callout).foregroundStyle(.secondary)
                HStack(spacing: 22) {
                    Picker("Requested audio buffer", selection: $controller.selectedBuffer) {
                        ForEach(controller.bufferOptions, id: \.self) { Text("\($0) frames").tag($0) }
                    }.labelsHidden().frame(maxWidth: 220)
                    Spacer()
                    Button("Request buffer") { controller.applyBuffer() }.buttonStyle(.bordered)
                }.padding(.top, 12).disabled(!controller.audioDeviceAvailable || controller.rateBusy || controller.pending)
                AppliedValue(value: controller.actualBuffer, label: "Active Core Audio buffer", busy: false).padding(.top, 12)
                if controller.actualBuffer > 0 && controller.selectedBuffer != controller.actualBuffer {
                    Label("Selected \(controller.selectedBuffer) · Active \(controller.actualBuffer)", systemImage: "info.circle")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Text(controller.bufferMessage).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            }
            NoteCard(icon: "music.note", title: "Monitoring in your audio app", text: "GarageBand monitoring passed at a 256-frame margin. Audacity’s monitoring path requests a separate high input latency, even when its buffer preference is reduced. Margins below 256 frames produced crackling in testing.")
            Text("The margin returns to 512 frames when the driver reattaches. Choose 256 again after reconnecting if desired.").font(.caption).foregroundStyle(.secondary)
        }
    }
    private var diagnostics: some View {
        Group {
            HStack(spacing: 16) {
                DiagnosticMetric(title: "Driver", value: controller.attached ? "Attached" : "Not attached", icon: "puzzlepiece.extension")
                DiagnosticMetric(title: "Period", value: controller.hardwarePeriod > 0 ? "\(controller.hardwarePeriod) frames" : "—", icon: "waveform.path")
                DiagnosticMetric(title: "Reported overruns", value: controller.attached ? "\(controller.hardwareOverruns)" : "—", icon: "clock")
            }
            BridgeCard {
                HStack {
                    SectionHeading(icon: "terminal", title: "Driver report")
                    Spacer()
                    Button("Refresh", systemImage: "arrow.clockwise") { controller.refresh() }
                    Button("Copy report", systemImage: "doc.on.doc") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(controller.deviceStatus, forType: .string)
                    }
                }
                Text(controller.summary).font(.callout).foregroundStyle(.secondary)
                Divider().padding(.vertical, 10)
                Text(controller.deviceStatus).font(.system(size: 11, design: .monospaced))
                    .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
            }
            Text("Counters show driver activity; they do not measure audible quality or round-trip latency.").font(.caption).foregroundStyle(.secondary)
        }
    }
    private var setup: some View {
        Group {
            BridgeCard {
                SectionHeading(icon: "puzzlepiece.extension", title: "Driver extension")
                Text("Clarett Bridge uses a macOS DriverKit extension for the original Clarett 2Pre Thunderbolt.")
                    .font(.callout).foregroundStyle(.secondary)
                Divider().padding(.vertical, 12)
                HStack(spacing: 12) {
                    Button("Activate driver") { controller.activate() }.buttonStyle(.borderedProminent)
                    Button("Deactivate driver") { controller.deactivate() }.buttonStyle(.bordered)
                    if controller.pending { ProgressView().controlSize(.small) }
                }.disabled(controller.pending)
                Text(controller.message).font(.callout).textSelection(.enabled).padding(.top, 10)
            }
            NoteCard(icon: "checkmark.shield", title: "Keep Full Security enabled", text: "Use the normal macOS extension approval flow. This app does not require Recovery Mode or change boot security, SIP or device-management settings. Approval policies can differ on other Macs.")
            BridgeCard {
                SectionHeading(icon: "info.circle", title: "About this project")
                Text("Independent open-source software. Not affiliated with or endorsed by Focusrite. Playback and capture support 44.1, 48, 88.2 and 96 kHz with the internal clock. MIDI, external clocks and Focusrite Control compatibility are not implemented.")
                    .font(.callout).foregroundStyle(.secondary)
                Link("Source and documentation ↗", destination: URL(string: "https://github.com/jthrilly/clarett-bridge")!).padding(.top, 8)
            }
        }
    }
}

struct BridgeMark: View {
    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10).fill(LinearGradient(colors: [BridgeStyle.accent, BridgeStyle.iconShadow], startPoint: .topTrailing, endPoint: .bottomLeading))
            Image(systemName: "waveform.path").font(.system(size: 21, weight: .semibold)).foregroundStyle(.white)
        }.accessibilityHidden(true)
    }
}
struct BridgeCard<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 12) { content }
            .padding(22).frame(maxWidth: .infinity, alignment: .leading)
            .background(BridgeStyle.card, in: RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(BridgeStyle.line, lineWidth: 1))
    }
}
struct SectionHeading: View {
    var icon: String; var title: String
    var body: some View {
        Label(title, systemImage: icon).font(.system(size: 15, weight: .semibold)).labelStyle(.titleAndIcon)
    }
}
struct FieldLabel: View {
    var text: String
    var body: some View { Text(text).font(.system(size: 10, weight: .semibold)).tracking(1.2).foregroundStyle(.secondary) }
}
struct Metric: View {
    var label: String; var value: String
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            FieldLabel(text: label)
            Text(value).font(.system(size: 18, weight: .medium, design: .rounded)).monospacedDigit()
        }
    }
}
struct StatusBadge: View {
    var text: String; var active: Bool
    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(active ? BridgeStyle.accent : Color.secondary).frame(width: 6, height: 6)
            Text(text).font(.system(size: 11, weight: .medium))
        }.padding(.horizontal, 11).padding(.vertical, 7)
            .background((active ? BridgeStyle.accent : Color.secondary).opacity(0.10), in: Capsule())
    }
}
struct ActionCard: View {
    var icon: String; var title: String; var detail: String; var button: String; var action: () -> Void
    var body: some View {
        BridgeCard {
            Image(systemName: icon).font(.system(size: 24, weight: .light)).foregroundStyle(BridgeStyle.accent).frame(height: 30).padding(.bottom, 4)
            Text(title).font(.system(size: 15, weight: .semibold))
            Text(detail).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true).frame(minHeight: 38, alignment: .top)
            Button(button, action: action).buttonStyle(.bordered).padding(.top, 5)
        }.frame(maxWidth: .infinity)
    }
}
struct NoteCard: View {
    var icon: String; var title: String; var text: String
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: icon).font(.system(size: 18)).foregroundStyle(BridgeStyle.accent).frame(width: 22)
            VStack(alignment: .leading, spacing: 6) {
                Text(title).font(.callout.weight(.semibold))
                Text(text).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }.padding(18).frame(maxWidth: .infinity, alignment: .leading)
            .background(BridgeStyle.accent.opacity(0.055), in: RoundedRectangle(cornerRadius: 12))
    }
}
struct BlendValue: View {
    var title: String; var value: Int
    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            FieldLabel(text: title)
            Text("\(value)%").font(.system(size: 30, weight: .medium, design: .rounded)).monospacedDigit()
        }
    }
}
struct AppliedValue: View {
    var value: UInt32; var label: String; var busy: Bool
    var body: some View {
        HStack {
            Text(label).font(.callout).foregroundStyle(.secondary)
            Spacer()
            if busy { ProgressView().controlSize(.small) }
            Text(value > 0 ? "\(value) frames" : "Unavailable").font(.callout.weight(.semibold)).monospacedDigit()
        }.padding(12).background(BridgeStyle.accent.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
    }
}
struct DiagnosticMetric: View {
    var title: String; var value: String; var icon: String
    var body: some View {
        BridgeCard {
            Image(systemName: icon).foregroundStyle(BridgeStyle.accent)
            Text(value).font(.system(size: 17, weight: .semibold, design: .rounded))
            Text(title).font(.caption).foregroundStyle(.secondary)
        }
    }
}
