// Babylon Native Lite — milestone harness app.
//
// Wires the pieces together: a Win32 window (main thread), the JsRuntimeHost
// AppRuntime (Chakra JS engine on its own thread), and the BabylonNativeLite
// surface. A startup script calls createEngine()/startEngine() — exactly how user
// JS will drive the engine — and the native render loop takes over with zero JS
// per frame (plus an optional onBeforeRender hook that drives the GPU via the
// wrapped WebGPU device).
//
// The substance lives in the modules:
//   Renderer/   — pure native Dawn engine (no N-API)
//   WebGPU/     — GPU* N-API ObjectWrap classes over the same Dawn objects
//   NativeLite/ — the Engine ObjectWrap + BabylonNativeLite surface

#define _CRT_SECURE_NO_WARNINGS // std::getenv is fine for reading an optional scene path

#include "Window.h"
#include "Overlay.h"

#include <Lite/NativeLite.h>
#include <Lite/Renderer.h>

#include <Babylon/AppRuntime.h>
#include <Babylon/Polyfills/Console.h>
#include <Babylon/Polyfills/Fetch.h>
#include <Babylon/Polyfills/Blob.h>
#include <Babylon/Polyfills/URL.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    constexpr int kInitialWidth = 1280;
    constexpr int kInitialHeight = 720;

    // Resolve the startup script from LITE_SCENE_JS — a real Babylon Lite scene bundled
    // by ./bundler. All Lite JS runs in-app over the native WebGPU polyfill; the JS render
    // loop is driven by the native requestAnimationFrame pump (see NativeLite). Returns
    // {source, debugName}; debugName is empty if no readable scene was found (the caller
    // then errors out — there is no inline fallback in the WebGPU-only baseline).
    std::pair<std::string, std::string> LoadStartupSource()
    {
        const char* path = std::getenv("LITE_SCENE_JS");
        if (path != nullptr && path[0] != '\0')
        {
            std::ifstream file(path, std::ios::binary);
            if (file)
            {
                std::ostringstream ss;
                ss << file.rdbuf();
                std::fprintf(stderr, "[lite] loading bundled scene from %s\n", path);
                return {ss.str(), std::string(path)};
            }
            std::fprintf(stderr, "[lite] LITE_SCENE_JS set but unreadable: %s\n", path);
        }
        else
        {
            std::fprintf(stderr, "[lite] LITE_SCENE_JS not set; point it at a bundled real-Lite scene (see Lite/bundler).\n");
        }
        return {std::string(), std::string()};
    }
}

int main()
{
    // 0. Resolve the real-Lite scene up front; fail fast if none was provided.
    auto startup = LoadStartupSource();
    if (startup.second.empty())
    {
        return 1; // LoadStartupSource already explained why on stderr.
    }
    std::string startupSource = startup.first;
    std::string startupName = startup.second;

    // 1. Window (main thread owns it and its message pump).
    lite::Window window;
    if (!window.Create(kInitialWidth, kInitialHeight, "Babylon Native Lite"))
    {
        std::fprintf(stderr, "[lite] window creation failed\n");
        return 1;
    }

    int fbWidth = 0;
    int fbHeight = 0;
    window.GetClientSize(fbWidth, fbHeight);
    if (fbWidth <= 0 || fbHeight <= 0)
    {
        fbWidth = kInitialWidth;
        fbHeight = kInitialHeight;
    }

    auto renderer = std::make_shared<lite::Renderer>();
    lite::Renderer::WindowHandle windowHandle{window.Hwnd(), window.Hinstance()};

    window.SetResizeCallback([renderer](int w, int h) {
        renderer->RequestResize(w, h);
    });

    // 2. Spin up the JS engine + host (owns its own JS thread).
    Babylon::AppRuntime::Options options{};
    options.UnhandledExceptionHandler = [](const Napi::Error& error) {
        std::fprintf(stderr, "[js] uncaught: %s\n", error.what());
    };
    Babylon::AppRuntime runtime{options};

    // 3. On the JS thread: install console + the BabylonNativeLite surface, then run
    //    the startup script (createEngine -> setBeforeRender -> startEngine). The
    //    controller is captured here so the main thread can coordinate shutdown.
    std::shared_ptr<lite::nativelite::Controller> controller;
    std::promise<std::shared_ptr<lite::nativelite::Controller>> controllerPromise;

    runtime.Dispatch([&runtime, renderer, windowHandle, fbWidth, fbHeight, &controllerPromise,
                      &startupSource, &startupName](Napi::Env env) {
        Babylon::Polyfills::Console::Initialize(env,
            [](const char* message, Babylon::Polyfills::Console::LogLevel level) {
                const char* tag = level == Babylon::Polyfills::Console::LogLevel::Error ? "error"
                    : level == Babylon::Polyfills::Console::LogLevel::Warn ? "warn"
                    : "log";
                std::fprintf(stderr, "[js:%s] %s\n", tag, message);
            });

        // Fetch polyfill: globalThis.fetch, backed by UrlLib (WinHTTP on Windows). Real
        // Lite's loadGltf / loadEnvironment use fetch to pull .glb/.env/.dds/.png assets
        // over the network. This is SETUP (asset loading), not per-frame work.
        Babylon::Polyfills::Fetch::Initialize(env);

        // Blob polyfill: globalThis.Blob. Real Lite's glTF loader wraps decoded image
        // bytes in a Blob then passes it to createImageBitmap (installed by the WebGPU
        // polyfill). Setup-only.
        Babylon::Polyfills::Blob::Initialize(env);

        // URL / URLSearchParams — scene code resolves multi-file glTF asset paths
        // (new URL(relative, base)) and reads query params (new URLSearchParams(
        // window.location.search)). JsRuntimeHost ships a native polyfill (deps: JsRuntime).
        Babylon::Polyfills::URL::Initialize(env);
        auto ctrl = lite::nativelite::Initialize(env, runtime, renderer, windowHandle,
            static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight));

        // Benchmark config (env vars, set before the startup script configures the surface):
        //   LITE_BENCH_FRAMES=N  → measure N frames then print a BENCH line + exit.
        //   LITE_NO_VSYNC=1      → uncap present mode (required for a real speed measurement).
        //   LITE_LOOP_LABEL      → "native" | "js" (tagged into the BENCH line).
        if (const char* bf = std::getenv("LITE_BENCH_FRAMES"))
        {
            ctrl->benchFrames = static_cast<uint32_t>(std::strtoul(bf, nullptr, 10));
        }
        if (const char* nv = std::getenv("LITE_NO_VSYNC"))
        {
            ctrl->noVsync = (nv[0] == '1' || nv[0] == 't' || nv[0] == 'T');
        }
        // Push the vsync choice to the WebGPU module before the startup script configures
        // the surface (GPUCanvasContext.configure reads it).
        if (ctrl->webgpu != nullptr)
        {
            ctrl->webgpu->SetNoVsync(ctrl->noVsync);
        }
        if (const char* ll = std::getenv("LITE_LOOP_LABEL"))
        {
            ctrl->loopLabel = ll;
        }
        ctrl->sceneLabel = startupName;
        {
            // Reduce sceneLabel to a basename without extension for a clean BENCH line.
            size_t slash = ctrl->sceneLabel.find_last_of("/\\");
            if (slash != std::string::npos) ctrl->sceneLabel = ctrl->sceneLabel.substr(slash + 1);
            size_t dot = ctrl->sceneLabel.find_first_of('.');
            if (dot != std::string::npos) ctrl->sceneLabel = ctrl->sceneLabel.substr(0, dot);
        }

        controllerPromise.set_value(ctrl);

        Napi::String source = Napi::String::New(env, startupSource);
        napi_value result = nullptr;
        if (napi_run_script(env, source, startupName.c_str(), &result) != napi_ok)
        {
            napi_value exception = nullptr;
            if (napi_get_and_clear_last_exception(env, &exception) == napi_ok && exception != nullptr)
            {
                napi_value message = nullptr;
                napi_coerce_to_string(env, exception, &message);
                if (message != nullptr)
                {
                    size_t len = 0;
                    napi_get_value_string_utf8(env, message, nullptr, 0, &len);
                    std::string buf(len + 1, '\0');
                    napi_get_value_string_utf8(env, message, buf.data(), buf.size(), &len);
                    std::fprintf(stderr, "[lite] startup script failed: %s\n", buf.c_str());
                }
            }
            else
            {
                std::fprintf(stderr, "[lite] startup script failed\n");
            }
        }
    });

    controller = controllerPromise.get_future().get();

    // 3b. On-screen HUD overlay showing live FPS + frame time. Default on; opt out with
    //     LITE_HUD=0, and auto-off in benchmark mode (the window closes immediately).
    lite::Overlay overlay;
    bool hudEnabled = controller->benchFrames == 0;
    if (const char* h = std::getenv("LITE_HUD"))
    {
        hudEnabled = !(h[0] == '0' || h[0] == 'f' || h[0] == 'F');
    }
    if (hudEnabled && overlay.Create(window.Hwnd(), window.Hinstance()))
    {
        window.SetTickCallback([&overlay, &controller]() {
            double fps = controller->hudFps.load(std::memory_order_relaxed);
            double frameMs = controller->hudFrameMs.load(std::memory_order_relaxed);
            double cpuMs = controller->hudCpuMs.load(std::memory_order_relaxed);
            wchar_t buf[160];
            std::swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                L"FPS:  %6.1f\nFrame: %6.2f ms\nCPU:   %6.2f ms",
                fps, frameMs, cpuMs);
            overlay.Update(buf);
        });
        window.StartTick(250); // refresh 4x/second
    }

    // 4. Main-thread message pump. Rendering happens on the JS thread, so the main
    //    thread blocks on window messages instead of spinning.
    while (window.PumpEventsBlocking())
    {
    }

    // 5. Stop the loop and release Dawn on the JS thread before the window/HWND dies.
    controller->RequestShutdownAndWait();

    return 0;
}
