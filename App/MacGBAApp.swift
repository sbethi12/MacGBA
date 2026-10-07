import SwiftUI

@main
struct MacGBAApp: App {
    @StateObject private var engine = EmulatorEngine()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(engine)
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 720, height: 560)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Open ROM…") {
                    engine.openROM()
                }
                .keyboardShortcut("o", modifiers: .command)
                Button("Load Built-in Demo") {
                    engine.loadDemo()
                }
                Divider()
                Button(engine.paused ? "Resume" : "Pause") {
                    engine.paused.toggle()
                }
                .keyboardShortcut("p", modifiers: .command)
                Button("Reset") {
                    engine.reset()
                }
                .keyboardShortcut("r", modifiers: .command)
            }
        }

        Settings {
            VStack(alignment: .leading, spacing: 12) {
                Text("Keyboard")
                    .font(.headline)
                Text("Z / X = A / B    A / S = L / R")
                Text("Enter = Start    Shift = Select    Arrows = D-Pad")
                Text("Drop a .gba file onto the window, or use File → Open ROM.")
                    .foregroundStyle(.secondary)
            }
            .padding(24)
            .frame(width: 420)
        }
    }
}
