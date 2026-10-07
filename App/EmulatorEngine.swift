import AppKit
import AVFoundation
import Combine
import MetalKit
import UniformTypeIdentifiers

final class EmulatorEngine: ObservableObject {
    @Published var paused = false
    @Published var status = "No ROM loaded"
    @Published var frameID: UInt64 = 0

    private var gba: OpaquePointer?
    private var audioEngine: AVAudioEngine?
    private var keyMask: UInt16 = 0
    private let lock = NSLock()
    private var pixels = [UInt32](repeating: 0xFF101018, count: Int(GBA_FRAMEBUFFER_PIXELS))
    private var lastTitle = ""

    var framebuffer: [UInt32] {
        lock.lock()
        let copy = pixels
        lock.unlock()
        return copy
    }

    init() {
        gba = gba_create()
        if let gba {
            gba_load_demo(gba)
            lastTitle = String(cString: gba_rom_title(gba))
            status = "Demo • \(lastTitle)"
        }
    }

    deinit {
        if let gba { gba_destroy(gba) }
    }

    func start() {
        startAudio()
    }

    func stop() {
        audioEngine?.stop()
    }

    func tick() {
        guard !paused, let gba else { return }
        lock.lock()
        gba_set_keys(gba, keyMask)
        gba_run_frame(gba)
        if let fb = gba_framebuffer(gba) {
            pixels.withUnsafeMutableBufferPointer { dest in
                dest.baseAddress!.update(from: fb, count: Int(GBA_FRAMEBUFFER_PIXELS))
            }
        }
        lock.unlock()
        frameID &+= 1
        if frameID.isMultiple(of: 30) {
            let title = String(cString: gba_rom_title(gba))
            status = "\(title.isEmpty ? "ROM" : title)  •  \(gba_frame_count(gba)) frames"
        }
    }

    func setKey(_ bit: UInt16, down: Bool) {
        if down { keyMask |= bit } else { keyMask &= ~bit }
    }

    func reset() {
        guard let gba else { return }
        lock.lock()
        gba_reset(gba)
        lock.unlock()
    }

    func loadDemo() {
        guard let gba else { return }
        lock.lock()
        gba_load_demo(gba)
        lock.unlock()
        status = "Demo loaded"
    }

    func openROM() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [
            UTType(filenameExtension: "gba") ?? .data,
            UTType(filenameExtension: "agb") ?? .data
        ]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        if panel.runModal() == .OK, let url = panel.url {
            loadROM(url: url)
        }
    }

    func handleDrop(_ providers: [NSItemProvider]) -> Bool {
        guard let provider = providers.first else { return false }
        _ = provider.loadObject(ofClass: URL.self) { [weak self] url, _ in
            guard let url else { return }
            Task { @MainActor in
                self?.loadROM(url: url)
            }
        }
        return true
    }

    func loadROM(url: URL) {
        guard url.startAccessingSecurityScopedResource() else {
            status = "Couldn't access file"
            return
        }
        defer { url.stopAccessingSecurityScopedResource() }
        do {
            let data = try Data(contentsOf: url)
            try data.withUnsafeBytes { raw in
                guard let ptr = raw.bindMemory(to: UInt8.self).baseAddress, let gba else {
                    throw POSIXError(.ENOMEM)
                }
                lock.lock()
                let ok = gba_load_rom(gba, ptr, data.count)
                lock.unlock()
                if !ok { throw POSIXError(.EINVAL) }
            }
            status = url.lastPathComponent
        } catch {
            status = "Failed to load ROM"
        }
    }

    private func startAudio() {
        let engine = AVAudioEngine()
        let format = AVAudioFormat(standardFormatWithSampleRate: 32768, channels: 1)!
        let node = AVAudioSourceNode(format: format) { [weak self] _, _, frameCount, audioBufferList -> OSStatus in
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard let buf = abl.first?.mData?.assumingMemoryBound(to: Float.self) else {
                return noErr
            }
            var produced = 0
            if let self {
                self.lock.lock()
                if let gba = self.gba {
                    produced = Int(gba_audio_samples(gba, buf, Int32(frameCount)))
                }
                self.lock.unlock()
            }
            if produced < Int(frameCount) {
                for i in produced..<Int(frameCount) { buf[i] = 0 }
            }
            return noErr
        }
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        engine.mainMixerNode.outputVolume = 0.4
        do {
            try engine.start()
            audioEngine = engine
        } catch {
            status = "Audio failed to start"
        }
    }
}

struct KeyboardCatcher: NSViewRepresentable {
    var engine: EmulatorEngine

    func makeNSView(context: Context) -> KeyView {
        let view = KeyView()
        view.engine = engine
        DispatchQueue.main.async { view.window?.makeFirstResponder(view) }
        return view
    }

    func updateNSView(_ nsView: KeyView, context: Context) {
        nsView.engine = engine
    }

    final class KeyView: NSView {
        weak var engine: EmulatorEngine?

        override var acceptsFirstResponder: Bool { true }

        override func keyDown(with event: NSEvent) {
            handle(event, down: true)
        }

        override func keyUp(with event: NSEvent) {
            handle(event, down: false)
        }

        private func handle(_ event: NSEvent, down: Bool) {
            guard !event.isARepeat else { return }
            let bit: UInt16?
            switch event.keyCode {
            case 6: bit = UInt16(GBA_KEY_A)          // Z
            case 7: bit = UInt16(GBA_KEY_B)          // X
            case 0: bit = UInt16(GBA_KEY_L)          // A
            case 1: bit = UInt16(GBA_KEY_R)          // S
            case 36: bit = UInt16(GBA_KEY_START)     // Return
            case 56, 60: bit = UInt16(GBA_KEY_SELECT)
            case 123: bit = UInt16(GBA_KEY_LEFT)
            case 124: bit = UInt16(GBA_KEY_RIGHT)
            case 125: bit = UInt16(GBA_KEY_DOWN)
            case 126: bit = UInt16(GBA_KEY_UP)
            default: bit = nil
            }
            if let bit {
                engine?.setKey(bit, down: down)
            }
        }
    }
}
