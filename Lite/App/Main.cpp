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

#include <Lite/NativeLite.h>
#include <Lite/Renderer.h>

#include <Babylon/AppRuntime.h>
#include <Babylon/Polyfills/Console.h>
#include <Babylon/Polyfills/Fetch.h>
#include <Babylon/Polyfills/Blob.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    constexpr int kInitialWidth = 1280;
    constexpr int kInitialHeight = 720;

    // Resolve the startup script: if the LITE_SCENE_JS env var points to a readable
    // file (e.g. a scene bundled by ./bundler that externalizes "babylon-lite" onto
    // the BabylonNativeLite global), run that; otherwise fall back to the built-in
    // inline demo. Returns {source, debugName}.
    std::pair<std::string, std::string> LoadStartupSource(const char* fallback)
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
            std::fprintf(stderr, "[lite] LITE_SCENE_JS set but unreadable (%s); using inline demo\n", path);
        }
        return {std::string(fallback), std::string("lite-startup")};
    }

    const char* kStartupJs =
        "console.log('Babylon Native Lite: JS building a 3D scene with materials + a texture...');\n"
        "const engine = BabylonNativeLite.createEngine();\n"
        "\n"
        "// Cube geometry: 24 vertices (4 per face) with position, normal, uv, color.\n"
        "function cubeData() {\n"
        "    const faces = [\n"
        "        {n:[0,0,1],  v:[[-0.5,-0.5,0.5],[0.5,-0.5,0.5],[0.5,0.5,0.5],[-0.5,0.5,0.5]]},\n"
        "        {n:[0,0,-1], v:[[0.5,-0.5,-0.5],[-0.5,-0.5,-0.5],[-0.5,0.5,-0.5],[0.5,0.5,-0.5]]},\n"
        "        {n:[1,0,0],  v:[[0.5,-0.5,0.5],[0.5,-0.5,-0.5],[0.5,0.5,-0.5],[0.5,0.5,0.5]]},\n"
        "        {n:[-1,0,0], v:[[-0.5,-0.5,-0.5],[-0.5,-0.5,0.5],[-0.5,0.5,0.5],[-0.5,0.5,-0.5]]},\n"
        "        {n:[0,1,0],  v:[[-0.5,0.5,0.5],[0.5,0.5,0.5],[0.5,0.5,-0.5],[-0.5,0.5,-0.5]]},\n"
        "        {n:[0,-1,0], v:[[-0.5,-0.5,-0.5],[0.5,-0.5,-0.5],[0.5,-0.5,0.5],[-0.5,-0.5,0.5]]},\n"
        "    ];\n"
        "    const uvFace = [[0,1],[1,1],[1,0],[0,0]];\n"
        "    const palette = [[0.9,0.3,0.3],[0.3,0.9,0.3],[0.3,0.3,0.9],[0.9,0.9,0.3],[0.9,0.3,0.9],[0.3,0.9,0.9]];\n"
        "    const pos=[], nrm=[], uv=[], col=[], idx=[];\n"
        "    for (let f=0; f<6; f++) {\n"
        "        const base = f*4;\n"
        "        for (let i=0; i<4; i++) {\n"
        "            pos.push(...faces[f].v[i]); nrm.push(...faces[f].n); uv.push(...uvFace[i]); col.push(...palette[f]);\n"
        "        }\n"
        "        idx.push(base,base+1,base+2, base,base+2,base+3);\n"
        "    }\n"
        "    return { positions:new Float32Array(pos), normals:new Float32Array(nrm),\n"
        "             uvs:new Float32Array(uv), colors:new Float32Array(col), indices:new Uint16Array(idx) };\n"
        "}\n"
        "const cube = cubeData();\n"
        "\n"
        "// A large ground quad (XZ plane, facing up) to catch the cubes' shadows.\n"
        "function planeData(s) {\n"
        "    const positions = new Float32Array([-s,0,-s,  s,0,-s,  s,0,s,  -s,0,s]);\n"
        "    const normals   = new Float32Array([0,1,0, 0,1,0, 0,1,0, 0,1,0]);\n"
        "    const uvs       = new Float32Array([0,0, 1,0, 1,1, 0,1]);\n"
        "    const colors    = new Float32Array([1,1,1, 1,1,1, 1,1,1, 1,1,1]);\n"
        "    const indices   = new Uint16Array([0,1,2, 0,2,3]);\n"
        "    return { positions, normals, uvs, colors, indices };\n"
        "}\n"
        "const plane = planeData(8);\n"
        "\n"
        "// Procedural 8x8 checkerboard texture (RGBA8), uploaded to the GPU natively.\n"
        "function checkerTexture(size) {\n"
        "    const px = new Uint8Array(size*size*4);\n"
        "    for (let y=0; y<size; y++) for (let x=0; x<size; x++) {\n"
        "        const on = ((x ^ y) & 1) === 0;\n"
        "        const i = (y*size+x)*4;\n"
        "        px[i]   = on ? 240 : 40;\n"
        "        px[i+1] = on ? 200 : 60;\n"
        "        px[i+2] = on ? 70  : 120;\n"
        "        px[i+3] = 255;\n"
        "    }\n"
        "    return BabylonNativeLite.createTexture(engine, size, size, px);\n"
        "}\n"
        "const checker = checkerTexture(8);\n"
        "\n"
        "// Four material permutations: a Blinn-Phong lit one, two metallic-roughness PBR\n"
        "// materials (a smooth gold metal + a rough red dielectric), and a textured one.\n"
        "// Each composes its own WGSL and gets its own cached pipeline.\n"
        "const matLitVC    = BabylonNativeLite.createMaterial(engine, { lighting:true, vertexColor:true });\n"
        "const matGold     = BabylonNativeLite.createMaterial(engine, { pbr:true, metallic:1.0, roughness:0.25, color:[1.0,0.78,0.34,1] });\n"
        "const matRed      = BabylonNativeLite.createMaterial(engine, { pbr:true, metallic:0.0, roughness:0.6,  color:[0.85,0.1,0.12,1] });\n"
        "const matLitTex   = BabylonNativeLite.createMaterial(engine, { lighting:true, texture:checker, color:[1,1,1,1] });\n"
        "const mats = [matLitVC, matGold, matRed, matLitTex];\n"
        "\n"
        "const meshes = [];\n"
        "const childNodes = [];\n"
        "// A root node that spins; each cube is a CHILD node, so it inherits the root's\n"
        "// rotation (orbiting the center) while also spinning locally. World matrices are\n"
        "// composed natively from this hierarchy each frame.\n"
        "const root = BabylonNativeLite.createNode(engine);\n"
        "for (let i=0; i<mats.length; i++) {\n"
        "    const child = BabylonNativeLite.createNode(engine, root);\n"
        "    childNodes.push(child);\n"
        "    meshes.push(BabylonNativeLite.createMesh(engine, cube.positions, cube.normals, cube.uvs, cube.colors, cube.indices, mats[i], child));\n"
        "}\n"
        "console.log('built ' + meshes.length + ' cubes parented to a spinning root node');\n"
        "\n"
        "// Ground plane: a lit material catching shadows, placed below the cube row.\n"
        "const groundMat = BabylonNativeLite.createMaterial(engine, { lighting:true, color:[0.6,0.6,0.62,1] });\n"
        "const groundNode = BabylonNativeLite.createNode(engine);\n"
        "groundNode.setTransform([0,-1.5,0], [0,0,0], [1,1,1]);\n"
        "BabylonNativeLite.createMesh(engine, plane.positions, plane.normals, plane.uvs, plane.colors, plane.indices, groundMat, groundNode);\n"
        "\n"
        "const camera = BabylonNativeLite.createCamera(engine);\n"
        "camera.setProjection(0.8, 1280.0/720.0, 0.1, 100.0);\n"
        "camera.setView([0, 3.2, 8.0], [0, 0.3, 0], [0, 1, 0]);\n"
        "BabylonNativeLite.addLight(engine, { type: 'directional', direction: [-0.4, -1.0, -0.6], color: [1.0, 0.97, 0.9], intensity: 0.9 });\n"
        "BabylonNativeLite.addLight(engine, { type: 'point', position: [2.5, 1.5, 2.5], color: [0.3, 0.5, 1.0], intensity: 6.0 });\n"
        "\n"
        "// Native keyframe animations: the root spins on Y and each child tumbles, all\n"
        "// advanced in the native update pass. NO JavaScript runs per frame — the core\n"
        "// thesis. (A per-frame JS hook via engine.setBeforeRender remains available for\n"
        "// user code, but the scene below animates without one.)\n"
        "const TAU = Math.PI * 2;\n"
        "const span = childNodes.length - 1;\n"
        "for (let i=0; i<childNodes.length; i++) {\n"
        "    const x = (i - span/2) * 1.8;\n"
        "    childNodes[i].setTransform([x, 0, 0], [0,0,0], [0.7,0.7,0.7]);\n"
        "    BabylonNativeLite.createAnimation(engine, childNodes[i], {\n"
        "        property: 'rotation',\n"
        "        keys: [ {time:0, value:[0,0,0]}, {time:1, value:[TAU*2, TAU*2, 0]} ],\n"
        "        duration: 3 + i*0.6, loop: true\n"
        "    });\n"
        "}\n"
        "// Root Y-spin (children inherit it through the native world-matrix compose).\n"
        "// Lift the row above the ground so the cubes cast visible shadows.\n"
        "root.setTransform([0, 0.4, 0], [0,0,0], [1,1,1]);\n"
        "BabylonNativeLite.createAnimation(engine, root, {\n"
        "    property: 'rotation',\n"
        "    keys: [ {time:0, value:[0,0,0]}, {time:6, value:[0, TAU, 0]} ],\n"
        "    duration: 6, loop: true\n"
        "});\n"
        "console.log('registered native animations: 0 JS executes per frame');\n"
        "\n"
        "BabylonNativeLite.startEngine(engine);\n"
        "console.log('Babylon Native Lite: startEngine() returned; native loop renders a parented scene graph.');\n";
}

int main()
{
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

    auto startup = LoadStartupSource(kStartupJs);
    std::string startupSource = startup.first;
    std::string startupName = startup.second;

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

    // 4. Main-thread message pump. Rendering happens on the JS thread, so the main
    //    thread blocks on window messages instead of spinning.
    while (window.PumpEventsBlocking())
    {
    }

    // 5. Stop the loop and release Dawn on the JS thread before the window/HWND dies.
    controller->RequestShutdownAndWait();

    return 0;
}
