// Build script: bundle every scene in ./scenes into ./dist as a single self-contained
// IIFE, with the `babylon-lite` import externalized to the native BabylonNativeLite
// global. The output is plain ES2020 that ChakraCore (Babylon Native Lite's JS engine)
// can run directly via napi_run_script — no module loader, no DOM, no Node builtins.

import { build } from "esbuild";
import { externalizeBabylonLite } from "./externalize-babylon-lite.mjs";
import { readdirSync, mkdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const scenesDir = join(here, "scenes");
const outDir = join(here, "dist");
mkdirSync(outDir, { recursive: true });

const entries = readdirSync(scenesDir).filter((f) => f.endsWith(".js") || f.endsWith(".ts"));
if (entries.length === 0) {
    console.error("No scenes found in", scenesDir);
    process.exit(1);
}

for (const entry of entries) {
    const name = entry.replace(/\.(js|ts)$/, "");
    const outfile = join(outDir, `${name}.bundle.js`);
    await build({
        entryPoints: [join(scenesDir, entry)],
        bundle: true,
        format: "iife",
        target: "es2020",
        platform: "neutral",
        outfile,
        plugins: [externalizeBabylonLite()],
        legalComments: "none",
        logLevel: "info",
    });
    console.log(`bundled ${entry} -> dist/${name}.bundle.js`);
}
