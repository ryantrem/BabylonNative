# Babylon Native Lite (native render loop on Dawn)

An exploration of a **native** Babylon Lite: porting Lite's per-frame render loop
to C++ on **Dawn (native WebGPU)** so that **zero Lite JavaScript executes per
frame**. Scene setup, asset loading, and sparse user callbacks stay in JS and
call into native. See [`../lite.md`](../lite.md) for the full architecture plan.

This directory currently contains **milestone P0, steps 1-5**: the Dawn build
integration, a native render loop driven from the JS thread, a
`BabylonNativeLite.startEngine()` entry point, **and Dawn exposed to JS as real
WebGPU** so a per-frame JS hook can drive the GPU directly.

## What the milestone app does

`LiteApp` creates a Win32 window, spins up the JsRuntimeHost **AppRuntime** (the
Chakra JS engine on its own thread), and installs a `BabylonNativeLite` global on
that thread. A startup script then runs the user-facing contract:

```js
const engine = BabylonNativeLite.createEngine();   // native: bring up Dawn (device/surface/depth/cache)

// Materials = feature permutations. WGSL is composed per permutation and the
// resulting pipeline cached by key (identical permutations share a pipeline).
const litVC = BabylonNativeLite.createMaterial(engine, { lighting:true, vertexColor:true });
const gold  = BabylonNativeLite.createMaterial(engine, { pbr:true, metallic:1.0, roughness:0.25, color:[1,0.78,0.34,1] }); // metallic-roughness PBR
const red   = BabylonNativeLite.createMaterial(engine, { pbr:true, metallic:0.0, roughness:0.6,  color:[0.85,0.1,0.12,1] });

// Indexed 3D meshes (positions/normals/colors + indices), each bound to a material.
const cube = BabylonNativeLite.createMesh(engine, posF32, normF32, colF32, idxU16, litVC);

// Native camera + scene lights (matrix/lighting math run in C++).
const camera = BabylonNativeLite.createCamera(engine);
camera.setProjection(0.8, 1280/720, 0.1, 100);
camera.setView([0, 1.6, 5], [0, 0, 0], [0, 1, 0]);
// Up to 4 directional/point lights; Lambert diffuse + Blinn-Phong specular.
BabylonNativeLite.addLight(engine, { type:'directional', direction:[-0.4,-1,-0.6], color:[1,0.97,0.9], intensity:0.9 });
BabylonNativeLite.addLight(engine, { type:'point', position:[2.5,1.5,2.5], color:[0.3,0.5,1.0], intensity:6.0 });

// Native keyframe animation — advanced entirely in the render loop, ZERO JS per frame.
BabylonNativeLite.createAnimation(engine, node, {
    property: 'rotation',
    keys: [ {time:0, value:[0,0,0]}, {time:1, value:[0, Math.PI*2, 0]} ],
    duration: 4, loop: true,
});

// A per-frame JS hook remains available for user code (the allowed exception),
// but is optional — the scene above animates without one:
// engine.setBeforeRender(() => { /* user code */ });

BabylonNativeLite.startEngine(engine);               // native: start the render loop
```

`createEngine()` brings up a Dawn WebGPU device on **D3D12**, configures the
window surface as a swapchain + depth buffer, and prepares a material/pipeline
cache. `createMaterial(engine, options)` is a **native factory**: each material is
a feature permutation (`vertexColor`, `lighting`, `texture`, `pbr`) + base color
(+ `metallic`/`roughness` for PBR), and the renderer
**composes the WGSL for that permutation** (`ComposeMeshWgsl` — the native analogue
of Lite's runtime shader-string composition), compiles it through Tint, and
**caches the pipeline by permutation key** so identical materials reuse it.
`createMesh(...)` builds an indexed GPU-resident 3D mesh (positions/normals/colors)
bound to a material. `createCamera(engine)` returns a `Camera` whose
`setProjection`/`setView` compute matrices **in C++**; `addLight(...)`/`clearLights()`
configure up to 4 directional/point lights (Lambert diffuse + Blinn-Phong specular,
or Cook-Torrance metallic-roughness for PBR materials)
consumed by lighting-enabled materials. `mesh.setTransform(...)`
computes the model matrix natively. `createAnimation(engine, node, options)` registers a
**native keyframe animation** that drives one transform channel of a node, advanced each
frame in the render loop with **zero JavaScript per frame**. `startEngine()` runs the native
render loop, which advances animations + scene-graph world matrices, (optionally) calls the
JS `onBeforeRender` hook, then draws each mesh with its
material's cached pipeline + depth testing, and re-dispatches itself onto the JS
thread's event loop.

Two complementary handle kinds are demonstrated, both backed by the same Dawn
objects the native loop renders with:
- **Engine-concept handles** — `Engine`, `Mesh`, `Material`, `Camera` (opaque native
  handles; geometry, matrices, lighting, and WGSL composition live in C++).
- **WebGPU wrappers** — `engine._device` is a **real wrapped `wgpu::Device`**
  (`GPUDevice`/`GPUQueue`/`GPUBuffer`), so stay-in-JS code can also drive the GPU
  directly via `device.queue.writeBuffer(...)` on the shared Dawn objects.

The scene (three spinning cubes, each a different composed-WGSL material variant) is
assembled in JS via native factories and rendered entirely natively. With no
per-frame hook, **zero JS runs per frame**; with one, it is a direct same-thread call
(the loop runs on the JS thread) — no marshaling. This is the plan's core model: JS
builds and steers, native owns the hot path.

## Requirements

- Windows + a D3D12-capable GPU
- Visual Studio 2022 (MSVC) and CMake ≥ 3.21
- **Python 3 on `PATH`** — Dawn fetches its third-party dependencies with its own
  python script (`DAWN_FETCH_DEPENDENCIES`)
- Network access (the first configure clones Dawn and its dependencies)

> The first build is **slow** (Dawn + Tint are large). Subsequent builds are
> incremental.

### Git credential note (first configure)

Dawn's dependency fetch clones from `*.googlesource.com`, which allows anonymous
access. If you have Git Credential Manager configured, it may pop an auth prompt
and stall the clone. Disable it for the configure shell so git falls through to
anonymous:

```powershell
$env:GIT_TERMINAL_PROMPT=0
$env:GCM_INTERACTIVE="never"
$env:GIT_CONFIG_COUNT=1; $env:GIT_CONFIG_KEY_0="credential.helper"; $env:GIT_CONFIG_VALUE_0=""
```

### Runtime DLL: d3dcompiler_47.dll

Dawn's D3D backend loads the FXC compiler (`d3dcompiler_47.dll`) during device
creation, and it must be **next to the executable** — Dawn's loader rejects a
path-less fallback load with `ERROR_INVALID_PARAMETER (87)`. The CMake build
copies it automatically as a post-build step (from `System32`). DXC is *not*
used here: Dawn force-disables the `use_dxc` toggle unless it is built with
`DAWN_USE_BUILT_DXC`, so only FXC is required.

## Build & run

```powershell
cd Lite
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target LiteApp
.\build\App\Release\LiteApp.exe
```

A window should open showing **four 3D cubes parented to a spinning root node** —
the whole row tilts and orbits as one group (inherited parent rotation) while each
cube also tumbles locally, all driven by **native keyframe animations** (no JS per
frame). Each uses a different **composed-WGSL material**: lit vertex-colored, a
**metallic-roughness PBR** gold metal, a rough red PBR dielectric, and a lit
**checkerboard-textured** cube. The console shows the material permutations being
composed and cached and the native animations being registered, then `startEngine()`.
Press **Esc** or close the window to exit.

### Running a bundled `babylon-lite` scene

`LiteApp` can also run a scene authored as ordinary Lite code (`import { … } from
"babylon-lite"`), bundled with the import externalized onto the native global:

```powershell
cd Lite/bundler
npm install
node build.mjs                                  # -> dist/spinning-cubes.bundle.js
$env:LITE_SCENE_JS = "$PWD/dist/spinning-cubes.bundle.js"
..\build\App\Release\LiteApp.exe                # renders the bundle natively
```

See [`bundler/README.md`](bundler/README.md) for how the externalization works and
how upstream Lite's `scene1` maps onto the native API.

## Status / next steps

**P0 (steps 1-6) + engine factories (Mesh, Camera, Material, Texture, Node) + 3D
scene + WGSL composition + textures + scene graph + multi-light shading + PBR +
native animation + shadow mapping — DONE.**
Verified on Windows + D3D12 (NVIDIA RTX 4070): `LiteApp` spins up the Chakra JS engine
(JsRuntimeHost AppRuntime), installs a `BabylonNativeLite` global, and runs JS that calls
`createEngine()` (Dawn + depth + material/pipeline cache), generates a procedural
texture (`createTexture` uploads RGBA8 pixels to the GPU), creates 4 **materials**
(a Blinn-Phong vertex-color one, two **metallic-roughness PBR** ones — gold metal +
rough red dielectric — and a textured one, each a feature permutation whose WGSL is
composed and whose pipeline is cached by key), builds a **scene graph** (`createNode` —
a spinning root with 4 child nodes) of indexed cubes with `createMesh()`, configures a
native `Camera` and **multiple lights** (`addLight` — a warm directional + a blue point
light; Cook-Torrance PBR or Blinn-Phong depending on material), and registers **native
keyframe animations** (`createAnimation` — a root Y-spin + per-child tumble) instead of a
per-frame JS hook, then `startEngine()`. The
native loop runs an explicit **update pass** (advance animations + recompute scene-graph
world matrices) then a
**record pass** (encode draws). Result: four cubes orbiting as a parented group while
tumbling locally, each a different material, with PBR metallic/roughness response,
specular highlights, and point-light tint — verified across frames on-screen, with
**zero JavaScript executing per frame**. Both handle
kinds are real: engine concepts
(`Engine`/`Mesh`/`Material`/`Texture`/`Node`/`Camera`) and WebGPU wrappers
(`engine._device` = `GPUDevice`/`GPUQueue`/`GPUBuffer`) over shared Dawn objects.
With no hook, zero JS runs per frame; with one, it's a direct same-thread call.

This is a **standalone** CMake project, intentionally not yet wired into the root
BabylonNative build, so Dawn's heavy first build doesn't disturb the existing
bgfx tree. Upcoming work (see `../lite.md`):

1. Grow the WGSL composer toward more material features (normal maps, emissive,
   real IBL). Image decoding for textures from real files stays
   in JS (per the plan's loaders-in-JS boundary).
2. Broaden the WebGPU-over-N-API surface (`navigator.gpu`, full `GPU*` objects:
   command encoders, render passes, pipelines) so a user-implemented JS
   `RenderingContext` can record real passes, not just `writeBuffer`.

## Project structure

Modules follow BabylonNative's conventions — WebGPU is a browser API so it lives
in `Polyfills/`, and the native Lite engine is a Babylon-facing plugin in
`Plugins/`:

| Module | Kind | Responsibility |
|---|---|---|
| `Renderer/` | static lib, **no N-API** | Pure native Dawn engine core: owns all Dawn objects, a mat4 math kit, camera + a **multi-light list** (directional/point, with **Blinn-Phong or Cook-Torrance metallic-roughness PBR** shading) + depth buffer, a shared sampler, a **scene graph** (nodes with parent-child world matrices, recomputed each frame), the texture/material/mesh lists, and a **pipeline cache keyed by material permutation**. `ShaderComposer.cpp` composes WGSL per permutation. JS-free so it can be built/tested headless. |
| `Polyfills/WebGPU/` | static lib (N-API) | Dawn exposed to JS as WebGPU `GPU*` `Napi::ObjectWrap` classes over the same Dawn objects the loop uses. Covers the resource-creation surface — `GPUDevice.create{ShaderModule,Buffer,Texture,Sampler,BindGroupLayout,PipelineLayout,BindGroup,RenderPipeline}`, `GPUTexture.createView`, `GPUQueue.writeBuffer/writeTexture` — so a JS engine layer can build all of a scene's GPU resources (Model B). |
| `Plugins/NativeLite/` | static lib (N-API) | The native Lite engine plugin: `Engine`, `Mesh`, `Material`, `Texture`, `Node`, `Camera` `ObjectWrap` handles + the `BabylonNativeLite` global (createEngine/createTexture/createMaterial/createNode/createMesh/createCamera/createAnimation/addLight/clearLights/startEngine/stopEngine). Mirrors Lite's `engine.ts` factories. |
| `App/` | executable | Win32 host: window + message pump, spins up AppRuntime, wires everything, runs the startup script (built-in inline demo, or a bundle from `LITE_SCENE_JS`). |
| `bundler/` | Node/esbuild | **Externalization layer**: bundles ordinary `import { … } from "babylon-lite"` scene code, rewriting the import onto the native `BabylonNativeLite` global so the same source runs natively. See `bundler/README.md`. |

The N-API classes use real `Napi::ObjectWrap` (the same pattern BabylonNative's
`NativeEngine` uses): `DefineClass` + `InstanceMethod`/`InstanceAccessor`, each
instance owning its native Dawn handle with a proper lifetime. Class constructors
are kept as module-owned `Napi::FunctionReference`s with `SuppressDestruct()` —
note the **Chakra N-API shim does not implement `napi_set_instance_data`** (only V8
does), so env instance data can't be used to store them.



## Threading model

- **Main thread:** owns the window and a blocking message pump only.
- **JS thread (AppRuntime):** owns the Chakra engine, the `BabylonNativeLite`
  N-API surface, all Dawn objects, and the render loop. `createEngine()` /
  `startEngine()` run here (called from JS). The loop is a native callback that
  renders one frame then re-dispatches itself onto the JS thread's event loop
  (`AppRuntime::Dispatch`). Window events cross to this thread via atomics. On
  shutdown the main thread clears the run flag and dispatches a teardown onto the
  JS thread to release Dawn on the thread that created it (waited on via a
  `std::promise`), before the window/HWND is destroyed. The pump checks the run
  flag first and never touches Dawn once cleared, so teardown can't race a frame.

## JS engine

Uses **Chakra** (`NAPI_JAVASCRIPT_ENGINE=Chakra`), the Windows default — it links
the system `Chakra.dll` via the SDK's `Chakrart.lib`, so no extra runtime is
shipped. JsRuntimeHost is pinned to the same commit the root BabylonNative build
uses. ScriptLoader and the network/file polyfills are disabled to keep the build
lean (only the Console polyfill is linked); the startup snippet runs via the core
`napi_run_script`.

## Dawn version

Pinned to Dawn commit `a44d7a3d78f23c680491c0fc04f53a1df62e02ff` (Chrome 146),
the revision validated by Cedric Guillemet's `webgpu-cross-platform-app`. Bump
deliberately — Dawn's WebGPU C++ API drifts between revisions.
