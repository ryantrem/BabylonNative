// Browser bench builder — bundles a CANONICAL Babylon Lite scene
// (lab/lite/src/lite/<scene>.ts) into a browser ESM module that imports the REAL Lite
// engine from the local dist. Paired with harness.html, which wraps requestAnimationFrame
// to measure the same present-excluded "render-loop CPU per frame" metric the native host
// reports — giving an apples-to-apples browser-vs-native comparison.
//
// Usage: node build-browser.mjs scene2[,scene3,...]
//   (or set LITE_CANON=scene2). Output: browser-bench/dist/<scene>.js

import { build } from "esbuild";
import { mkdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const outDir = join(here, "dist");
mkdirSync(outDir, { recursive: true });

const canonDir = process.env.LITE_CANON_DIR
    ? process.env.LITE_CANON_DIR
    : "D:\\Repos\\Babylon-Lite-3\\lab\\lite\\src\\lite";
const realLiteEntry = process.env.LITE_DIST
    ? process.env.LITE_DIST
    : "D:\\Repos\\Babylon-Lite-3\\packages\\babylon-lite\\dist\\index.js";

const sel = (process.argv[2] || process.env.LITE_CANON || "").trim();
if (!sel) {
    console.error("usage: node build-browser.mjs scene2[,scene3,...]");
    process.exit(1);
}
const entries = sel.split(",").map((s) => s.trim()).filter(Boolean)
    .map((s) => (s.endsWith(".ts") ? s : `${s}.ts`));

const aliasLite = {
    name: "alias-lite",
    setup(b) {
        b.onResolve({ filter: /^(babylon-lite|@babylonjs\/lite)$/ }, () => ({ path: realLiteEntry }));
    },
};

for (const entry of entries) {
    const name = entry.replace(/\.ts$/, "");
    const outfile = join(outDir, `${name}.js`);
    await build({
        entryPoints: [join(canonDir, entry)],
        bundle: true,
        format: "esm",
        target: "esnext", // real browser; no legacy lowering needed
        platform: "browser",
        outfile,
        legalComments: "none",
        logLevel: "warning",
        plugins: [aliasLite],
    });
    console.log(`browser-bundled ${entry} -> dist/${name}.js`);
}
