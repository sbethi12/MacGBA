import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject private var engine: EmulatorEngine
    @State private var isDropTargeted = false

    var body: some View {
        VStack(spacing: 0) {
            ZStack {
                Color.black
                MetalScreenView(engine: engine)
                    .aspectRatio(240.0 / 160.0, contentMode: .fit)
                    .padding(12)
                    .opacity(isDropTargeted ? 0.7 : 1)
            }
            .onDrop(of: [UTType.fileURL], isTargeted: $isDropTargeted) { providers in
                engine.handleDrop(providers)
            }

            HStack(spacing: 16) {
                Button("Open ROM") { engine.openROM() }
                Button("Demo") { engine.loadDemo() }
                Button(engine.paused ? "Resume" : "Pause") { engine.paused.toggle() }
                Button("Reset") { engine.reset() }
                Spacer()
                Text(engine.status)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .background(.bar)
        }
        .frame(minWidth: 480, minHeight: 360)
        .background(KeyboardCatcher(engine: engine))
        .onAppear { engine.start() }
        .onDisappear { engine.stop() }
    }
}
