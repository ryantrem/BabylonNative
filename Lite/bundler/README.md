# Babylon Native Lite — Scene Bundler

This is the **bundler externalization** layer. It lets ordinary Babylon Lite
scene code — code that imports from the `babylon-lite` package — run on top of
the **native** Babylon Native Lite engine, with **zero engine JavaScript in the
render loop**.

## The idea

An upstream Lite scene looks like this (`lab/lite/src/lite/scene1.ts` in the
[Babylon-Lite](https://github.com/BabylonJS/Babylon-Lite) repo):

```ts
import { createEngine, createSceneContext, addToScene, loadGltf,
         createDefaultCamera, createHemisphericLight, startEngine } from "babylon-lite";

const engine = await createEngine(canvas);
const scene = createSceneContext(engine);
addToScene(scene, await loadGltf(engine, ".../BoomBox.glb"));
// ...
await startEngine(engine);
```

Babylon Native Lite implements that API **natively** (C++/Dawn behind N-API) and
installs it on a global called `BabylonNativeLite`. The bundler's job is to
**rewrite every `import { ... } from "babylon-lite"`** so the named bindings come
from that global instead of from the bundled TypeScript engine:

```js
// after bundling:
var __bnl = globalThis.BabylonNativeLite;
const createEngine = __bnl.createEngine;
const startEngine  = __bnl.startEngine;
// ...scene body unchanged...
```

The engine code never ships in the bundle — only the scene's orchestration
(setup + optional per-frame hooks) stays in JS. Everything that runs in the
render loop (animation, world matrices, camera/lights, draw submission) executes
in native code.

## The boundary: render-loop vs. not

The split is **not** "native vs. JS per function" — it's **render-loop vs.
not-render-loop**. The only things that *must* be native are the per-frame loop
and the state it touches each frame. Everything else runs once at setup and stays
in JavaScript. So the bundler resolves the `babylon-lite` import into three layers:

1. **Native render-loop primitives** → the `BabylonNativeLite` global: `createEngine`
   (brings up Dawn + the HWND surface), `createNode`/`createCamera`/`addLight`/
   `createAnimation` (per-frame state native owns), `registerDrawable`, `startEngine`.
2. **JS engine layer** (`runtime/lite-engine.mjs`) → `createMaterial`/`createMesh`,
   **including WGSL composition**, implemented in JS over the **WebGPU N-API polyfill**
   (`engine._device`). JS creates the real Dawn resources (shader modules, pipelines,
   bind groups, vertex/index buffers) and registers a drawable with the native loop.
   This is "Model B": only the loop is native; resource creation is JS.
3. **JS orchestration runtime** (`runtime/lite-runtime.mjs`) → `createSceneContext`,
   `addToScene`, `createDefaultCamera`, `createHemisphericLight`, `registerScene`,
   `attachControl`, `loadGltf`, … — pure setup-time bookkeeping over layers 1–2.

## How it works

- `externalize-babylon-lite.mjs` — an esbuild plugin that resolves the
  `babylon-lite` specifier to a generated virtual module. Render-loop primitive
  names bind from `BabylonNativeLite.*`; the rest are re-exported from the JS
  runtime + engine modules. esbuild tree-shakes the exports a scene doesn't import.
  A native name the host doesn't implement resolves to a **guarded stub** that
  throws a clear "not implemented" error only if actually called.
- `build.mjs` — bundles every file in `scenes/` to `dist/<name>.bundle.js` as a
  self-contained ES2020 IIFE (no module loader, no DOM, no Node builtins) — the
  form ChakraCore (the native host's JS engine) runs directly.
- `runtime/lite-engine.mjs` — the JS engine layer (material/mesh + WGSL composition
  over the WebGPU polyfill).
- `runtime/lite-runtime.mjs` — the JS orchestration layer (scene/camera/light setup).
- `scenes/spinning-cubes.js`, `scenes/scene-context-cubes.js` — sample scenes
  authored in the `import … from "babylon-lite"` style.

## Build & run

```powershell
# 1. Bundle the scenes (externalizing the babylon-lite import).
cd Lite/bundler
npm install
node build.mjs                    # -> dist/spinning-cubes.bundle.js

# 2. Run the native host against the bundle.
$env:LITE_SCENE_JS = "$PWD/dist/spinning-cubes.bundle.js"
../build/App/Release/LiteApp.exe
```

`LiteApp` checks the `LITE_SCENE_JS` env var: if it points to a readable file it
runs that bundle; otherwise it falls back to its built-in inline demo. The
bundled scene renders natively, identically to the inline demo.

## API surface (today)

These names can be imported from `babylon-lite` in a scene. The **Layer** column
shows where each resolves: **native** (`BabylonNativeLite` global, render-loop
primitive), **JS engine** (`lite-engine.mjs`, over the WebGPU polyfill), or **JS
orchestration** (`lite-runtime.mjs`).

| Import | Layer | Behavior |
|---|---|---|
| `createEngine()` | native | Bring up Dawn (device/surface/depth). Exposes `_device` (WebGPU), `frameBuffer`, `colorFormat`, `depthFormat`. |
| `createNode(engine[, parent])` | native | Scene-graph transform node; owns a per-frame `modelBuffer` (GPUBuffer). |
| `createCamera(engine)` | native | Camera with native `setProjection`/`setView`. |
| `addLight(engine, opts)` / `clearLights(engine)` | native | Directional/point lights. |
| `createAnimation(engine, node, opts)` | native | Native keyframe animation (runs in the loop, no JS/frame). |
| `registerDrawable(engine, desc)` | native | Records a JS-built drawable (used internally by the JS engine layer). |
| `startEngine(engine)` / `stopEngine(engine)` | native | Start/stop the native render loop. |
| `createTexture(engine, w, h, rgba)` | native | Upload an RGBA8 texture. |
| `createMaterial(engine, opts)` | **JS engine** | Composes WGSL + creates the pipeline + material bind group **via the WebGPU polyfill** (`vertexColor`/`lighting`/`pbr` + `metallic`/`roughness`). |
| `createMesh(engine, pos, norm, uv, col, idx, mat, node)` | **JS engine** | Builds vertex/index buffers + model bind group via the polyfill, then `registerDrawable`. |
| `createSceneContext` / `addToScene` / `createDefaultCamera` / `createHemisphericLight` / `registerScene` / `attachControl` / `onBeforeRender` | **JS orchestration** | Setup-time scene/camera/light bookkeeping over the layers above. |
| `loadGltf` / `loadEnvironment` | JS orchestration | Async asset loaders (stay in JS; not implemented yet). |

The WebGPU polyfill itself (`engine._device`) exposes `createShaderModule`,
`createBuffer`, `createTexture`, `createSampler`, `createBindGroupLayout`,
`createPipelineLayout`, `createBindGroup`, `createRenderPipeline`, plus
`queue.writeBuffer`/`writeTexture` and `GPUBufferUsage`/`GPUTextureUsage`/
`GPUShaderStage` global constants — enough for the JS engine layer to build every
GPU resource a scene needs.

## Mapping upstream `scene1` (the "theoretical" target)

`scene1` uses a higher-level functional API. The bundler already externalizes
those names; the remaining work is native implementation (or thin adapters):

| `scene1` import | Status / native mapping |
|---|---|
| `createEngine(canvas)` | Implemented (native ignores the canvas arg / takes the host window). |
| `startEngine(engine)` | Implemented. |
| `createSceneContext(engine)` | Not yet — would wrap the renderer's scene list. |
| `addToScene(scene, x)` | Not yet — would register a loaded mesh/light with the scene. |
| `createDefaultCamera(scene)` | Partial — maps to `createCamera`; needs ArcRotate + `.alpha`. |
| `attachControl(cam, canvas, scene)` | Not yet — needs input plumbing (a user-hook concern). |
| `createHemisphericLight(dir, intensity)` | Maps to `addLight` (hemispheric term TBD). |
| `loadGltf(engine, url)` | Not yet — **stays in JS** per the loaders-in-JS boundary; needs a glTF parser + native mesh upload. |
| `loadEnvironment(scene, url, opts)` | Not yet — needs IBL/skybox (a material/RT feature). |
| `registerScene(scene)` | Not yet — would finalize/validate the scene before the first frame. |

Any of these that a scene imports but the host doesn't implement throws a clear
`BabylonNativeLite.<name> is not implemented by the native host yet` at the call
site — so a partially-supported scene fails loudly and precisely rather than
silently misrendering.
