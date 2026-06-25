// Build script for REAL Babylon Lite scenes, consuming the LOCAL build of Lite at
// D:\Repos\Babylon-Lite-3\packages\babylon-lite\dist (so we can co-design Lite to be
// native-friendly). Bundles a scene that imports from "babylon-lite" / "@babylonjs/lite"
// into a single IIFE that ChakraCore runs directly.
//
// Edit→build loop: edit Lite src → (in Babylon-Lite-3) pnpm --filter babylon-lite build
// → here: node build-lite.mjs.

import { build } from "esbuild";
import { readdirSync, mkdirSync, existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const scenesDir = join(here, "scenes-lite");
const outDir = join(here, "dist");
mkdirSync(outDir, { recursive: true });

// The LOCAL Lite build entry (overridable via LITE_DIST env var). This is the package
// we co-design; switch back to node_modules/@babylonjs/lite to use the published one.
const realLiteEntry = process.env.LITE_DIST
    ? process.env.LITE_DIST
    : "D:\\Repos\\Babylon-Lite-3\\packages\\babylon-lite\\dist\\index.js";

// Pure-JS shims for web globals legacy ChakraCore lacks (TextEncoder/Decoder, etc.),
// prepended to every bundle as a banner.
const hostPrelude = readFileSync(join(here, "runtime", "host-prelude.js"), "utf8");

// Legacy ChakraCore can't parse BigInt literals (`32n`), and esbuild can't lower them.
// Lite uses two in OpenType font-parser code (unused by non-text scenes, but still
// parsed). Rewrite integer BigInt literals to `BigInt(n)` calls (handled by the
// host-prelude BigInt passthrough shim) so the bundle parses. Scoped to Lite .js files.
const liteFileFilter = /[\\/](babylon-lite|@babylonjs[\\/]lite)[\\/].*\.js$/;
const lowerBigIntLiterals = {
    name: "lower-bigint-literals",
    setup(b) {
        b.onLoad({ filter: liteFileFilter }, (args) => {
            let src = readFileSync(args.path, "utf8");
            if (src.indexOf("n") !== -1) {
                src = src.replace(/(?<![\w.$])(\d+)n(?![\w$])/g, "BigInt($1)");
            }
            return { contents: src, loader: "js" };
        });
    },
};

// Selective render-loop externalization: redirect the scene's render-loop imports
// (`startEngine`, `stopEngine`, `renderFrame`) from `babylon-lite` to the NATIVE global
// (BabylonNativeLite.*), while everything else comes from real Lite. The native render
// loop replaces these; because the scene imports them from the virtual module (which does
// NOT re-import them from real Lite), real Lite's renderFrame/executePass/executePassBody/
// drawList and their exclusive callees are tree-shaken OUT of the bundle entirely. All
// other Lite APIs (createEngine/scene/material/mesh/registerScene/...) stay real JS.
const NATIVE_LOOP_EXPORTS = ["startEngine", "stopEngine", "renderFrame"];
const overrideLoopExports = {
    name: "override-loop-exports",
    setup(b) {
        // Match both the unscoped source name and the published scoped name.
        b.onResolve({ filter: /^(babylon-lite|@babylonjs\/lite)$/ }, (args) => {
            if (args.namespace === "loop-override") return undefined; // avoid recursion
            return { path: "virtual-lite", namespace: "loop-override" };
        });
        b.onLoad({ filter: /^virtual-lite$/, namespace: "loop-override" }, () => {
            // Re-export everything from real Lite EXCEPT the render-loop functions, which
            // come from the native global. The opaque render bundle + pipelines + UBOs are
            // now built by real Lite's record() at registerScene (setup) — so no JS
            // warm-up frame is needed and renderFrame never has to run (or ship).
            let s = `export * from ${JSON.stringify(realLiteEntry)};\n`;
            s += `const __bnl = (typeof globalThis !== "undefined" && globalThis.BabylonNativeLite)`;
            s += ` ? globalThis.BabylonNativeLite : BabylonNativeLite;\n`;
            for (const name of NATIVE_LOOP_EXPORTS) {
                s += `export const ${name} = __bnl.${name};\n`;
            }
            return { contents: s, loader: "js", resolveDir: here };
        });
    },
};

const entries = existsSync(scenesDir)
    ? readdirSync(scenesDir).filter((f) => f.endsWith(".ts") || f.endsWith(".js"))
    : [];
if (entries.length === 0) {
    console.error("No scenes in", scenesDir);
    process.exit(1);
}

// Loop mode (current project direction): the DEFAULT build runs the REAL Babylon Lite JS
// render loop in-app (startEngine/renderFrame stay real Lite, driven by the native
// requestAnimationFrame polyfill). Only the WebGPU layer is native. This is the standing
// configuration — all Lite JS executes; we just implement WebGPU.
//
// The native C++ render loop (and the native Lite-API plugin) is RETAINED but OPT-IN: set
// LITE_NATIVE_LOOP=1 to build the variant that externalizes the render-loop functions onto
// the native BabylonNativeLite global. Its output is suffixed `.nativeloop.lite.js` so it can
// sit alongside the default all-JS bundle. (LITE_JS_LOOP is accepted as a deprecated no-op
// alias for the default since the JS loop is now the default.)
const nativeLoop = process.env.LITE_NATIVE_LOOP === "1";
const jsLoop = !nativeLoop; // default: all Lite JS executes; native loop is opt-in
const outSuffix = nativeLoop ? ".nativeloop.lite.js" : ".lite.js";

// In JS-loop mode we don't externalize the render-loop functions (real Lite's
// startEngine/renderFrame run in-app). But the scene still imports from the bare
// "babylon-lite" specifier, which the externalization plugin normally resolves — so
// provide a plain alias plugin that points "babylon-lite"/"@babylonjs/lite" at the local
// dist with NO export rewriting.
const aliasLiteOnly = {
    name: "alias-lite-only",
    setup(b) {
        b.onResolve({ filter: /^(babylon-lite|@babylonjs\/lite)$/ }, () => ({ path: realLiteEntry }));
    },
};
const activePlugins = jsLoop ? [aliasLiteOnly, lowerBigIntLiterals] : [overrideLoopExports, lowerBigIntLiterals];

for (const entry of entries) {
    const name = entry.replace(/\.(ts|js)$/, "");
    const outfile = join(outDir, `${name}${outSuffix}`);
    await build({
        entryPoints: [join(scenesDir, entry)],
        bundle: true,
        format: "iife",
        target: "es2017", // legacy ChakraCore: lower optional-chaining/nullish-coalescing/etc.
        platform: "neutral",
        outfile,
        legalComments: "none",
        logLevel: "info",
        banner: { js: hostPrelude },
        plugins: activePlugins,
        // The published package resolves "@babylonjs/lite" from node_modules. Dynamic
        // imports inside it are inlined into the single IIFE by esbuild.
    });
    console.log(`bundled ${entry} -> dist/${name}${outSuffix}`);
}
