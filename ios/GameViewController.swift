import UIKit
import Metal
import MetalKit
import Darwin // for pthread_jit_write_protect_np / pthread

/// Main view controller. Hosts the emulator on a real-time thread and presents
/// the guest's output surface via Metal.
final class GameViewController: UIViewController, MTKViewDelegate {

    private var metalView: MTKView!
    private var device: MTLDevice!
    private var commandQueue: MTLCommandQueue!
    private var emulatorThread: pthread_t?

    override func viewDidLoad() {
        super.viewDidLoad()
        setupMetal()
        acquireJIT()
        startEmulator()
    }

    // MARK: - Metal surface (placeholder; see README "Rendering" section)

    private func setupMetal() {
        device = MTLCreateSystemDefaultDevice()
        commandQueue = device.makeCommandQueue()
        metalView = MTKView(frame: view.bounds, device: device)
        metalView.delegate = self
        metalView.framebufferOnly = true
        metalView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(metalView)
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
    func draw(in view: MTKView) {
        // TODO: blit the guest's composed framebuffer / latest delivered
        // libVLC pixel buffer to this drawable. See README "Rendering".
        guard let drawable = view.currentDrawable else { return }
        let desc = MTLRenderPassDescriptor()
        desc.colorAttachments[0].texture = drawable.texture
        desc.colorAttachments[0].loadAction = .clear
        desc.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        desc.colorAttachments[0].storeAction = .store
        let enc = commandQueue.makeCommandBuffer()?.makeRenderCommandEncoder(descriptor: desc)
        enc?.endEncoding()
        commandQueue.makeCommandBuffer()?.presentDrawable(drawable)
    }

    // MARK: - JIT acquisition (non-jailbroken iOS)

    /// On stock iOS you cannot flip W^X without the JIT entitlement AND the
    /// process being debugged / JIT-enabled. Two practical routes:
    ///   1. AltStore / SideStore "Enable JIT" (JIT stream) -- tap it before launch.
    ///   2. Xcode: debug this app once; the debugger attaches and grants JIT.
    /// Either way, after JIT is available, call pthread_jit_write_protect_np(1)
    /// so the JIT allocator can mark translated blocks executable.
    private func acquireJIT() {
        // This call only *succeeds* if the entitlement + debugger/JIT-stream is
        // active. If it crashes here, the app was launched without JIT -- see
        /// README "JIT on non-jailbroken iOS".
        pthread_jit_write_protect_np(1)
        print("[jit] W^X set to executable (JIT available)")
    }

    // MARK: - Emulator thread

    private func startEmulator() {
        // Bundle layout: the exe + its asset tree live in the app bundle.
        let bundle = Bundle.main.bundlePath
        let exe = (bundle as NSString).appendingPathComponent("Corruption.exe")
        let assets = bundle // path_resolve indexes the whole bundle tree

        exe.withCString { ec in
            assets.withCString { ac in
                app_set_paths(ec, ac)
            }
        }

        var attr = pthread_attr_t()
        pthread_attr_init(&attr)
        let runner: @convention(c) (UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? = { _ in
            // Best-effort real-time scheduling for low jitter (iOS keeps us at
            // SCHED_OTHER without a special entitlement; still worth setting).
            var policy = Int32(SCHED_OTHER)
            var param = sched_param()
            param.sched_priority = 0
            _ = pthread_setschedparam(pthread_self(), policy, &param)
            run_emulator()
            return nil
        }
        pthread_create(&emulatorThread, &attr, runner, nil)
        pthread_attr_destroy(&attr)
    }
}
