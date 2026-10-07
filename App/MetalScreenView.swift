import MetalKit
import SwiftUI

struct MetalScreenView: NSViewRepresentable {
    @ObservedObject var engine: EmulatorEngine

    func makeCoordinator() -> Renderer {
        Renderer(engine: engine)
    }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView()
        view.device = MTLCreateSystemDefaultDevice()
        view.delegate = context.coordinator
        view.framebufferOnly = false
        view.colorPixelFormat = .bgra8Unorm
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        view.preferredFramesPerSecond = 60
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        context.coordinator.setup(view: view)
        return view
    }

    func updateNSView(_ nsView: MTKView, context: Context) {
        context.coordinator.engine = engine
    }
}

final class Renderer: NSObject, MTKViewDelegate {
    var engine: EmulatorEngine
    private var texture: MTLTexture?
    private var pipeline: MTLRenderPipelineState?
    private var sampler: MTLSamplerState?
    private var vertexBuffer: MTLBuffer?

    init(engine: EmulatorEngine) {
        self.engine = engine
    }

    func setup(view: MTKView) {
        guard let device = view.device else { return }
        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm,
            width: Int(GBA_WIDTH),
            height: Int(GBA_HEIGHT),
            mipmapped: false
        )
        desc.usage = [.shaderRead]
        desc.storageMode = .shared
        texture = device.makeTexture(descriptor: desc)

        let verts: [Float] = [
            -1, -1, 0, 1,
             1, -1, 1, 1,
            -1,  1, 0, 0,
             1,  1, 1, 0
        ]
        vertexBuffer = device.makeBuffer(bytes: verts, length: verts.count * 4)

        let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };
        struct VOut { float4 pos [[position]]; float2 uv; };
        vertex VOut vs(VIn in [[stage_in]]) {
            VOut o; o.pos = float4(in.pos, 0, 1); o.uv = in.uv; return o;
        }
        fragment float4 fs(VOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
            return tex.sample(smp, in.uv);
        }
        """
        do {
            let lib = try device.makeLibrary(source: source, options: nil)
            let pdesc = MTLRenderPipelineDescriptor()
            pdesc.vertexFunction = lib.makeFunction(name: "vs")
            pdesc.fragmentFunction = lib.makeFunction(name: "fs")
            pdesc.colorAttachments[0].pixelFormat = view.colorPixelFormat
            let vdesc = MTLVertexDescriptor()
            vdesc.attributes[0].format = .float2
            vdesc.attributes[0].offset = 0
            vdesc.attributes[0].bufferIndex = 0
            vdesc.attributes[1].format = .float2
            vdesc.attributes[1].offset = 8
            vdesc.attributes[1].bufferIndex = 0
            vdesc.layouts[0].stride = 16
            pdesc.vertexDescriptor = vdesc
            pipeline = try device.makeRenderPipelineState(descriptor: pdesc)
        } catch {
            NSLog("Metal pipeline failed: \(error)")
        }

        let sdesc = MTLSamplerDescriptor()
        sdesc.minFilter = .nearest
        sdesc.magFilter = .nearest
        sdesc.sAddressMode = .clampToEdge
        sdesc.tAddressMode = .clampToEdge
        sampler = device.makeSamplerState(descriptor: sdesc)
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let texture,
              let pipeline,
              let sampler,
              let drawable = view.currentDrawable,
              let rpd = view.currentRenderPassDescriptor,
              let queue = view.device?.makeCommandQueue(),
              let cmd = queue.makeCommandBuffer(),
              let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }

        engine.tick()
        let pixels = engine.framebuffer
        pixels.withUnsafeBytes { raw in
            texture.replace(
                region: MTLRegionMake2D(0, 0, Int(GBA_WIDTH), Int(GBA_HEIGHT)),
                mipmapLevel: 0,
                withBytes: raw.baseAddress!,
                bytesPerRow: Int(GBA_WIDTH) * 4
            )
        }

        enc.setRenderPipelineState(pipeline)
        enc.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
        enc.setFragmentTexture(texture, index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()
        cmd.present(drawable)
        cmd.commit()
    }
}
