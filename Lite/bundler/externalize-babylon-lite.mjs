// esbuild plugin: externalize-babylon-lite
//
// Rewrites every `import { ... } from "babylon-lite"` so the bindings come from the
// right layer of Babylon Native Lite. The key architectural point: the boundary is
// RENDER-LOOP vs. NOT-RENDER-LOOP, not native-vs-JS per function.
//
//   • Render-loop primitives (GPU-resource creation + the loop itself) are NATIVE —
//     resolved to the host-provided global `BabylonNativeLite`.
//   • Everything else (scene bookkeeping, camera/light construction, scene
//     registration, asset loaders) runs ONCE at setup, so it stays in JavaScript —
//     resolved to the pure-JS orchestration runtime (./runtime/lite-runtime.mjs),
//     which is itself layered over the native primitives.
//
// esbuild tree-shakes whichever names a scene doesn't import, so the bundle only
// pulls in the API surface the scene actually uses.

import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));

// Render-loop primitives implemented in native C++/Dawn behind N-API. These bring
// up Dawn (HWND-bound surface), own per-frame state (nodes/animations/camera/
// lights), or drive the loop, so they must be native. NOTE: createMaterial and
// createMesh are deliberately NOT here — in Model B they run in JS (lite-engine.mjs)
// over the WebGPU polyfill, creating the real Dawn GPU resources themselves.
export const NATIVE_NAMES = [
    "createEngine",
    "startEngine",
    "stopEngine",
    "createTexture",
    "createNode",
    "createCamera",
    "addLight",
    "clearLights",
    "createAnimation",
];

// Setup-time orchestration implemented in pure JS (lite-runtime.mjs), layered over
// the native primitives. None of these run in the render loop.
export const JS_RUNTIME_NAMES = [
    "createSceneContext",
    "addToScene",
    "removeFromScene",
    "onBeforeRender",
    "registerScene",
    "disposeScene",
    "createHemisphericLight",
    "createDefaultCamera",
    "attachControl",
    "loadGltf",
    "loadEnvironment",
];

export function externalizeBabylonLite(options = {}) {
    const globalName = options.globalName ?? "BabylonNativeLite";
    const nativeNames = options.nativeNames ?? NATIVE_NAMES;
    // JS layers re-exported from the virtual module: the orchestration runtime
    // (scene/camera/light construction) and the engine layer (material/mesh creation
    // over the WebGPU polyfill). Both run at setup, never in the render loop.
    const runtimeModules = options.runtimeModules ?? [
        join(here, "runtime", "lite-runtime.mjs"),
        join(here, "runtime", "lite-engine.mjs"),
    ];
    const runtimeImports = runtimeModules.map((p) => p.replace(/\\/g, "/"));

    return {
        name: "externalize-babylon-lite",
        setup(build) {
            build.onResolve({ filter: /^babylon-lite$/ }, () => ({
                path: "babylon-lite",
                namespace: "bnl-external",
            }));

            build.onLoad({ filter: /.*/, namespace: "bnl-external" }, () => {
                const lines = [
                    // Native render-loop primitives bind from the host global.
                    `var __bnl = (typeof globalThis !== "undefined" && globalThis.${globalName})`,
                    `    ? globalThis.${globalName}`,
                    `    : (typeof ${globalName} !== "undefined" ? ${globalName} : undefined);`,
                    `if (!__bnl) { throw new Error("${globalName} global is not installed — run inside Babylon Native Lite."); }`,
                    `function __missing(name) { return function () { throw new Error("${globalName}." + name + " is not implemented by the native host yet."); }; }`,
                ];
                for (const name of nativeNames) {
                    lines.push(
                        `export const ${name} = (typeof __bnl.${name} === "function") ? __bnl.${name} : __missing(${JSON.stringify(name)});`,
                    );
                }
                // Setup-time JS layers (orchestration + engine) re-exported. esbuild
                // resolves + bundles them like any module (and tree-shakes unused names).
                for (const imp of runtimeImports) {
                    lines.push(`export * from ${JSON.stringify(imp)};`);
                }
                return { contents: lines.join("\n"), loader: "js", resolveDir: here };
            });
        },
    };
}
