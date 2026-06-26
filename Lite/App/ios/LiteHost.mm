// iOS bring-up of Babylon Native Lite. Obj-C++ (.mm) because it touches NSBundle to locate
// the bundled scene script + assets; the rest is the same C++ wiring as the Win32 Main.cpp.
//
// Threading: Babylon::AppRuntime owns the JS thread. Lite's requestAnimationFrame pump (see
// Plugins/NativeLite) self-reschedules onto that thread's event loop, so the render loop runs
// without any UIKit-side timer — the main thread just hands over the CAMetalLayer and exits
// StartLite. This mirrors how the browser drives Lite via rAF on its own thread.

#include "LiteHost.h"

#include <Lite/NativeLite.h>
#include <Lite/Renderer.h>

#include <Babylon/AppRuntime.h>
#include <Babylon/Polyfills/Console.h>
#include <Babylon/Polyfills/Fetch.h>
#include <Babylon/Polyfills/Blob.h>
#include <Babylon/Polyfills/URL.h>

#include <future>
#include <memory>
#include <string>

#import <Foundation/Foundation.h>

namespace
{
    // Process-lifetime host state. Kept in a single struct so StopLite can tear down in order
    // (controller shutdown drains the JS loop and releases Dawn before the runtime dies).
    struct HostState
    {
        std::shared_ptr<lite::Renderer> renderer;
        std::unique_ptr<Babylon::AppRuntime> runtime;
        std::shared_ptr<lite::nativelite::Controller> controller;
        bool started = false;
    };

    HostState& State()
    {
        static HostState s;
        return s;
    }

    // Read the bundled scene script from the .app's Resources. Asset URLs inside the scene use
    // the `app://` scheme, which UrlLib (UrlRequest_Apple.mm) resolves to NSBundle resources —
    // so no path rewriting is needed here, just load the script text.
    std::string LoadBundledScene(std::string& outName)
    {
        @autoreleasepool
        {
            NSBundle* bundle = [NSBundle mainBundle];

            // Accept a couple of conventional names so the bundler/CMake copy step is flexible.
            NSArray<NSString*>* candidates = @[ @"scene.lite", @"cubes.lite", @"scene", @"cubes" ];
            NSString* path = nil;
            NSString* picked = nil;
            for (NSString* name in candidates)
            {
                path = [bundle pathForResource:name ofType:@"js"];
                if (path != nil) { picked = name; break; }
            }
            if (path == nil)
            {
                NSLog(@"[lite] no bundled scene .js found (looked for scene.lite.js / cubes.lite.js)");
                outName.clear();
                return {};
            }

            NSError* err = nil;
            NSString* contents = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&err];
            if (contents == nil)
            {
                NSLog(@"[lite] failed to read scene %@: %@", path, err);
                outName.clear();
                return {};
            }

            outName = std::string([picked UTF8String]);
            return std::string([contents UTF8String]);
        }
    }
}

namespace lite::ios
{
    bool StartLite(void* metalLayer, uint32_t widthPx, uint32_t heightPx)
    {
        HostState& st = State();
        if (st.started)
        {
            NSLog(@"[lite] StartLite called twice — ignoring");
            return true;
        }
        if (metalLayer == nullptr || widthPx == 0 || heightPx == 0)
        {
            NSLog(@"[lite] StartLite: invalid layer/size");
            return false;
        }

        std::string sceneName;
        std::string sceneSource = LoadBundledScene(sceneName);
        if (sceneSource.empty())
        {
            return false;
        }

        st.renderer = std::make_shared<lite::Renderer>();
        lite::Renderer::WindowHandle windowHandle{};
        windowHandle.metalLayer = metalLayer;

        Babylon::AppRuntime::Options options{};
        options.UnhandledExceptionHandler = [](const Napi::Error& error) {
            NSLog(@"[js] uncaught: %s", error.what());
        };
        st.runtime = std::make_unique<Babylon::AppRuntime>(options);

        std::promise<std::shared_ptr<lite::nativelite::Controller>> controllerPromise;

        Babylon::AppRuntime& runtime = *st.runtime;
        auto renderer = st.renderer;
        runtime.Dispatch([&runtime, renderer, windowHandle, widthPx, heightPx,
                          &controllerPromise, &sceneSource, &sceneName](Napi::Env env) {
            Babylon::Polyfills::Console::Initialize(env,
                [](const char* message, Babylon::Polyfills::Console::LogLevel level) {
                    const char* tag = level == Babylon::Polyfills::Console::LogLevel::Error ? "error"
                        : level == Babylon::Polyfills::Console::LogLevel::Warn ? "warn"
                        : "log";
                    NSLog(@"[js:%s] %s", tag, message);
                });

            // Fetch (UrlLib — NSURLSession-backed on Apple) pulls glTF/env/texture assets.
            Babylon::Polyfills::Fetch::Initialize(env);
            // Blob — Lite's glTF loader wraps decoded image bytes before createImageBitmap.
            Babylon::Polyfills::Blob::Initialize(env);
            // URL / URLSearchParams — scene resolves multi-file glTF paths + query params.
            Babylon::Polyfills::URL::Initialize(env);

            auto ctrl = lite::nativelite::Initialize(env, runtime, renderer, windowHandle,
                widthPx, heightPx);

            ctrl->sceneLabel = sceneName;
            controllerPromise.set_value(ctrl);

            Napi::String source = Napi::String::New(env, sceneSource);
            napi_value result = nullptr;
            if (napi_run_script(env, source, sceneName.c_str(), &result) != napi_ok)
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
                        NSLog(@"[lite] startup script failed: %s", buf.c_str());
                    }
                }
                else
                {
                    NSLog(@"[lite] startup script failed");
                }
            }
        });

        st.controller = controllerPromise.get_future().get();
        st.started = true;
        return true;
    }

    void Resize(uint32_t widthPx, uint32_t heightPx)
    {
        HostState& st = State();
        if (st.renderer != nullptr && widthPx > 0 && heightPx > 0)
        {
            st.renderer->RequestResize(static_cast<int>(widthPx), static_cast<int>(heightPx));
        }
    }

    void StopLite()
    {
        HostState& st = State();
        if (!st.started)
        {
            return;
        }
        if (st.controller != nullptr)
        {
            st.controller->RequestShutdownAndWait();
        }
        st.controller.reset();
        st.runtime.reset();
        st.renderer.reset();
        st.started = false;
    }

    void GetHudStats(double& fps, double& frameMs, double& cpuMs)
    {
        HostState& st = State();
        if (st.controller != nullptr)
        {
            fps = st.controller->hudFps.load(std::memory_order_relaxed);
            frameMs = st.controller->hudFrameMs.load(std::memory_order_relaxed);
            cpuMs = st.controller->hudCpuMs.load(std::memory_order_relaxed);
        }
        else
        {
            fps = frameMs = cpuMs = 0.0;
        }
    }
}
