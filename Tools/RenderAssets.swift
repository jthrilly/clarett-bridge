// SPDX-License-Identifier: GPL-2.0-only
// Generate project artwork and labelled UI previews. Never reads hardware or starts I/O.
import AppKit
import SwiftUI

@main
struct RenderAssets {
    @MainActor static func main() throws {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.prohibited)
        let args = CommandLine.arguments
        guard args.count == 3 else { fatalError("Usage: render-assets --icon|--previews OUTPUT_DIRECTORY") }
        let directory = URL(fileURLWithPath: args[2], isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        if args[1] == "--icon" {
            for size in [16, 32, 128, 256, 512] {
                for scale in [1, 2] {
                    let pixels = size * scale
                    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
                        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
                    NSGraphicsContext.saveGraphicsState()
                    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
                    let context = NSGraphicsContext.current!.cgContext
                    context.scaleBy(x: CGFloat(pixels) / 1024, y: CGFloat(pixels) / 1024)
                    let background = NSBezierPath(roundedRect: NSRect(x: 40, y: 40, width: 944, height: 944), xRadius: 218, yRadius: 218)
                    NSGradient(starting: NSColor(BridgeStyle.iconShadow),
                               ending: NSColor(BridgeStyle.accent))!.draw(in: background, angle: 65)
                    let path = NSBezierPath()
                    let points: [(CGFloat, CGFloat)] = [(206,512),(302,512),(365,650),(435,342),(515,714),(590,330),(660,568),(718,512),(818,512)]
                    path.move(to: NSPoint(x: points[0].0, y: points[0].1))
                    for p in points.dropFirst() { path.line(to: NSPoint(x:p.0,y:p.1)) }
                    path.lineWidth = 48; path.lineCapStyle = .round; path.lineJoinStyle = .round
                    NSColor.white.setStroke(); path.stroke()
                    NSGraphicsContext.restoreGraphicsState()
                    let name = "icon_\(size)x\(size)\(scale == 2 ? "@2x" : "").png"
                    try rep.representation(using: .png, properties: [:])!.write(to: directory.appendingPathComponent(name))
                }
            }
        } else if args[1] == "--previews" {
            let dark = ProcessInfo.processInfo.environment["CLARETT_PREVIEW_DARK"] == "1"
            let disconnected = ProcessInfo.processInfo.environment["CLARETT_PREVIEW_DISCONNECTED"] == "1"
            NSApp.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
            for page in BridgePage.allCases {
                let model = BridgeController(preview: true)
                if disconnected {
                    model.attached = false; model.streaming = false; model.audioDeviceAvailable = false
                    model.monitorAvailable = false; model.timingAvailable = false; model.rateAvailable = false; model.volumeAvailable = false; model.muteAvailable = false
                    model.rateOptions = []; model.rateMessage = "Connect the interface to change its sample rate."
                    model.sampleRate = 0; model.actualBuffer = 0; model.actualSafety = 0
                    model.summary = "Connect and power on your Clarett."
                    model.deviceStatus = "No Clarett driver service is attached."
                }
                let root = BridgeView(controller: model, initialPage: page).environment(\.colorScheme, dark ? .dark : .light)
                let host = NSHostingView(rootView: root)
                host.frame = NSRect(x: 0, y: 0, width: 1000, height: page == .outputs ? 800 : 740)
                let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
                window.isReleasedWhenClosed = false; window.contentView = host
                host.layoutSubtreeIfNeeded(); window.displayIfNeeded()
                RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.15))
                host.layoutSubtreeIfNeeded()
                guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { fatalError("Cannot render view") }
                host.cacheDisplay(in: host.bounds, to: rep)
                let name = page.rawValue.lowercased().replacingOccurrences(of: " ", with: "-") + ".png"
                try rep.representation(using: .png, properties: [:])!.write(to: directory.appendingPathComponent(name))
                window.close()
            }
        } else { fatalError("Unknown rendering mode") }
    }
}
