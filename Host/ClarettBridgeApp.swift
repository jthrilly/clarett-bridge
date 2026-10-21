// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI

@main
struct ClarettBridgeApp: App {
    @StateObject private var controller = BridgeController()
    var body: some Scene {
        WindowGroup("Clarett Bridge") {
            BridgeView(controller: controller)
        }
        .defaultSize(width: 1000, height: 740)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}
