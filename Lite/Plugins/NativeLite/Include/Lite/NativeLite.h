#pragma once

#include <Lite/Renderer.h>
#include <Lite/WebGPU.h>

#include <Babylon/AppRuntime.h>
#include <napi/napi.h>

#include <atomic>
#include <future>
#include <memory>

// The JavaScript-facing native engine surface — the counterpart to Babylon Lite's
// engine.ts factories. Installs a `BabylonNativeLite` global exposing
// createEngine() / startEngine() / stopEngine(), and an `Engine` ObjectWrap class
// that carries the wrapped WebGPU device. User JS calls startEngine() and the
// native render loop takes over, running on the JS thread's event loop with zero
// JS per frame (except an optional per-frame onBeforeRender hook).
namespace lite::nativelite
{
    // Shared state for one engine. Lives as a shared_ptr captured by the render
    // pump, the Engine wrapper, and the app's controller handle.
    struct Controller
    {
        Babylon::AppRuntime* runtime = nullptr;
        std::shared_ptr<lite::Renderer> renderer;
        lite::Renderer::WindowHandle window{};
        uint32_t width = 0;
        uint32_t height = 0;

        std::atomic<bool> running{false};

        // JS-loop pump gate (benchmark config #2). The native requestAnimationFrame pump
        // runs while this is true; RecordFrameAndMaybeFinish / RequestShutdownAndWait clear
        // it. Defaults true so the JS loop runs as soon as real Lite arms rAF; unused by the
        // native path (which never calls rAF).
        std::atomic<bool> jsLoopRunning{true};

        // Optional per-frame JS hook. Only touched on the JS thread.
        Napi::FunctionReference onBeforeRender;

        // ---- Real Babylon Lite render-loop path (Stage B) ----
        // When the externalized real-Lite `startEngine` runs, it stores the REAL Lite
        // engine plain object here; the native pump drives a native renderFrame that
        // READS this engine's JS structures (no JS execution) and issues GPU work.
        webgpu::Module* webgpu = nullptr;       // for swapchain present (set in Initialize)
        Napi::ObjectReference realEngine;       // the real Lite engine object (JS thread only)
        bool realMode = false;                  // true once real-Lite startEngine ran
        std::atomic<int> frameCounter{0};

        // ---- Live render stats for the on-screen HUD overlay ----
        // Written on the JS thread at the end of each frame (rAF pump), read on the main
        // (window) thread by the overlay timer. Atomics make the cross-thread read safe.
        // wallMs/fps are an EMA of the real frame-to-frame cadence (the cadence the user
        // observes); cpuMs is the EMA of the JS-thread render-loop CPU (present-excluded).
        std::atomic<double> hudFrameMs{0.0};
        std::atomic<double> hudFps{0.0};
        std::atomic<double> hudCpuMs{0.0};
        // Records the HUD EMAs for one frame:
        //   fullFrameMs — the full frame WORK time: Lite renderFrame (record) + Present
        //                 (submit drain + present), i.e. the real end-to-end cost of producing
        //                 the frame, EXCLUDING the inter-frame vsync idle wait.
        //   cpuMs       — the JS-thread render-loop CPU (Lite renderFrame only; present-excluded).
        //   cadenceMs   — real time since the previous frame completed (includes vsync idle);
        //                 drives the observed FPS the user actually sees.
        void RecordHudStats(double fullFrameMs, double cpuMs, double cadenceMs);

        // ---- Benchmark harness ----
        // When benchFrames > 0, the render loop measures per-frame wall time and, after
        // collecting benchFrames samples (a warm-up frame is dropped), prints a machine-
        // readable BENCH line and posts a close so the app exits. loopLabel distinguishes
        // the native C++ loop ("native") from the in-app JS loop ("js"); sceneLabel is the
        // scene file's basename. Set from env vars in main() before the startup script runs.
        uint32_t benchFrames = 0;               // 0 = run forever (no benchmarking)
        bool noVsync = false;                   // uncap present mode for a real speed measurement
        std::string loopLabel = "native";
        std::string sceneLabel = "scene";

        // Records one rendered frame's render-loop CPU time (milliseconds — the cost of the
        // per-frame render work itself, EXCLUDING vsync/present wait) and, once benchFrames
        // samples are gathered, emits the BENCH line + requests exit. This isolates the
        // variable under test (render-loop language) independent of display refresh, so it's
        // directly comparable across native loop / in-app JS loop / browser. Called at the
        // end of every frame by whichever pump is active.
        void RecordFrameAndMaybeFinish(double frameCpuMs);

        // Called from the app (main) thread to stop the loop and release Dawn on the
        // JS thread before the window/HWND is destroyed.
        void RequestShutdownAndWait();
    };

    // The GPUDevice-bearing engine handle handed to JS by createEngine().
    class Engine : public Napi::ObjectWrap<Engine>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Engine(const Napi::CallbackInfo& info);

        std::shared_ptr<Controller> GetController() const { return m_controller; }

    private:
        Napi::Value GetDevice(const Napi::CallbackInfo& info);
        Napi::Value GetFrameBuffer(const Napi::CallbackInfo& info);
        Napi::Value GetColorFormat(const Napi::CallbackInfo& info);
        Napi::Value GetDepthFormat(const Napi::CallbackInfo& info);
        Napi::Value SetBeforeRender(const Napi::CallbackInfo& info);

        std::shared_ptr<Controller> m_controller;
        Napi::ObjectReference m_device;
    };

    // An engine-concept handle for a mesh — created by createMesh(). A mesh's
    // transform comes from its scene-graph Node, so the mesh handle is currently an
    // opaque reference (geometry + material binding live natively).
    class Mesh : public Napi::ObjectWrap<Mesh>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Mesh(const Napi::CallbackInfo& info);

        lite::Mesh* Native() const { return m_mesh; }

    private:
        lite::Mesh* m_mesh = nullptr;
    };

    // An engine-concept handle for a scene-graph node — created by createNode(node?).
    // Exposes setTransform(position, rotation, scale); world matrices are computed
    // natively each frame from the node hierarchy.
    class Node : public Napi::ObjectWrap<Node>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Node(const Napi::CallbackInfo& info);

        lite::Node* Native() const { return m_node; }

    private:
        Napi::Value SetTransform(const Napi::CallbackInfo& info);
        Napi::Value GetModelBuffer(const Napi::CallbackInfo& info);

        lite::Node* m_node = nullptr;
        lite::Renderer* m_renderer = nullptr;
    };

    // An engine-concept handle for a material — created by createMaterial(). Wraps a
    // native Material (a feature permutation + base color). Exposes setColor().
    class Material : public Napi::ObjectWrap<Material>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Material(const Napi::CallbackInfo& info);

        lite::Material* Native() const { return m_material; }

    private:
        Napi::Value SetColor(const Napi::CallbackInfo& info);

        lite::Material* m_material = nullptr;
        lite::Renderer* m_renderer = nullptr;
    };

    // An engine-concept handle for a texture — created by createTexture(). Wraps a
    // native Texture (a GPU image uploaded from JS pixel data).
    class Texture : public Napi::ObjectWrap<Texture>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Texture(const Napi::CallbackInfo& info);

        lite::Texture* Native() const { return m_texture; }

    private:
        lite::Texture* m_texture = nullptr;
    };

    // An engine-concept handle for the camera — created by createCamera(). Exposes
    // setProjection(fovY, aspect, near, far) and setView(eye, target, up), which
    // compute view/projection matrices natively and upload the combined viewProj.
    class Camera : public Napi::ObjectWrap<Camera>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Camera(const Napi::CallbackInfo& info);

    private:
        Napi::Value SetProjection(const Napi::CallbackInfo& info);
        Napi::Value SetView(const Napi::CallbackInfo& info);

        lite::Renderer* m_renderer = nullptr;
    };

    // Installs the `BabylonNativeLite` global on the given env. Returns a controller
    // the app holds to coordinate shutdown. `renderer` may be uninitialized;
    // createEngine() initializes it (bringing up Dawn) on first call.
    std::shared_ptr<Controller> Initialize(
        Napi::Env env,
        Babylon::AppRuntime& runtime,
        std::shared_ptr<lite::Renderer> renderer,
        lite::Renderer::WindowHandle window,
        uint32_t width,
        uint32_t height);

    // Installs the stage-0 N-API-vs-V8 call-overhead microbenchmark globals
    // (_mbNowNs + the napi/v8-direct/v8-fast scalar & typed-array probes) on the
    // given env's global object. Cheap; always safe to call. Used by the
    // microbench scene script to measure per-call boundary-crossing cost.
    void InstallMicrobench(Napi::Env env);
}
