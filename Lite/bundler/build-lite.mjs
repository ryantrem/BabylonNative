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
const hostPreludeBody = readFileSync(join(here, "runtime", "host-prelude.js"), "utf8");

// Public asset root for resolving root-relative URLs ("/brdf-lut.png", "/textures/...").
// On the web these are served from lab/public; in the native host (no origin) the prelude's
// fetch wrapper rewrites them to file:// URLs under this root. Overridable via LITE_PUBLIC_ROOT.
const publicRootDir = process.env.LITE_PUBLIC_ROOT
    ? process.env.LITE_PUBLIC_ROOT
    : "D:\\Repos\\Babylon-Lite-3\\lab\\public";
const publicRootUrl = "file:///" + publicRootDir.replace(/\\/g, "/").replace(/^\/+/, "");
const hostPrelude =
    `globalThis.__LITE_PUBLIC_ROOT = ${JSON.stringify(publicRootUrl)};\n` + hostPreludeBody;

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

// Scene source selection:
//  - Default: build the hand-authored scenes in ./scenes-lite (box, multi, pbr, ...).
//  - LITE_CANON=scene2[,scene3,...]: build the CANONICAL Babylon Lite scene corpus from
//    lab/lite/src/lite/<name>.ts in the Babylon-Lite-3 repo (overridable via LITE_CANON_DIR).
//    These are the upstream reference scenes (scene1..sceneN) run unmodified over our host.
const canonDir = process.env.LITE_CANON_DIR
    ? process.env.LITE_CANON_DIR
    : "D:\\Repos\\Babylon-Lite-3\\lab\\lite\\src\\lite";
const canonSel = (process.env.LITE_CANON || "").trim();

let srcDir;
let entries;
if (canonSel) {
    srcDir = canonDir;
    if (canonSel.toLowerCase() === "all") {
        entries = readdirSync(canonDir).filter((f) => /^scene\d+\.ts$/.test(f));
    } else {
        entries = canonSel.split(",").map((s) => s.trim()).filter(Boolean)
            .map((s) => (s.endsWith(".ts") ? s : `${s}.ts`));
    }
} else {
    srcDir = scenesDir;
    entries = existsSync(scenesDir)
        ? readdirSync(scenesDir).filter((f) => f.endsWith(".ts") || f.endsWith(".js"))
        : [];
}
if (entries.length === 0) {
    console.error("No scenes selected in", srcDir);
    process.exit(1);
}

// WebGPU-only baseline: the build runs the REAL Babylon Lite JS render loop in-app
// (startEngine/renderFrame are real Lite, driven by the native requestAnimationFrame
// pump in NativeLite). Only the WebGPU layer is native — all Lite JS executes; we just
// implement WebGPU over Dawn. The scene imports from the bare "babylon-lite" /
// "@babylonjs/lite" specifier; alias it to the local Lite dist with NO export rewriting
// (no render-loop externalization).
const outSuffix = ".lite.js";
const aliasLiteOnly = {
    name: "alias-lite-only",
    setup(b) {
        b.onResolve({ filter: /^(babylon-lite|@babylonjs\/lite)$/ }, () => ({ path: realLiteEntry }));
    },
};
const activePlugins = [aliasLiteOnly, lowerBigIntLiterals];

for (const entry of entries) {
    const name = entry.replace(/\.(ts|js)$/, "");
    const outfile = join(outDir, `${name}${outSuffix}`);
    await build({
        entryPoints: [join(srcDir, entry)],
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
