// Host prelude — pure-JS shims for standard web globals that legacy ChakraCore (the
// native host's JS engine) lacks. Prepended to every real-Lite bundle. These run at
// setup only (never in the render loop) and need nothing beyond basic JS, so they live
// in JS rather than native. Grow as real Lite surfaces more missing globals.
(function (g) {
    "use strict";

    // ---- TextEncoder / TextDecoder (UTF-8) ----
    if (typeof g.TextEncoder === "undefined") {
        g.TextEncoder = function TextEncoder() {};
        g.TextEncoder.prototype.encode = function (str) {
            str = String(str == null ? "" : str);
            const bytes = [];
            for (let i = 0; i < str.length; i++) {
                let c = str.charCodeAt(i);
                if (c < 0x80) {
                    bytes.push(c);
                } else if (c < 0x800) {
                    bytes.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
                } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
                    const c2 = str.charCodeAt(i + 1);
                    const cp = 0x10000 + ((c & 0x3ff) << 10) + (c2 & 0x3ff);
                    i++;
                    bytes.push(
                        0xf0 | (cp >> 18),
                        0x80 | ((cp >> 12) & 0x3f),
                        0x80 | ((cp >> 6) & 0x3f),
                        0x80 | (cp & 0x3f)
                    );
                } else {
                    bytes.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
                }
            }
            return new Uint8Array(bytes);
        };
    }

    if (typeof g.TextDecoder === "undefined") {
        g.TextDecoder = function TextDecoder(label) { this.encoding = label || "utf-8"; };
        g.TextDecoder.prototype.decode = function (buf) {
            if (buf == null) return "";
            const bytes = buf instanceof Uint8Array
                ? buf
                : new Uint8Array(buf.buffer ? buf.buffer : buf);
            let out = "";
            let i = 0;
            while (i < bytes.length) {
                let c = bytes[i++];
                if (c < 0x80) {
                    out += String.fromCharCode(c);
                } else if (c >= 0xc0 && c < 0xe0) {
                    out += String.fromCharCode(((c & 0x1f) << 6) | (bytes[i++] & 0x3f));
                } else if (c >= 0xe0 && c < 0xf0) {
                    out += String.fromCharCode(
                        ((c & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f)
                    );
                } else {
                    let cp = ((c & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) |
                        ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
                    cp -= 0x10000;
                    out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
                }
            }
            return out;
        };
    }

    // ---- performance.now ----
    if (typeof g.performance === "undefined") {
        g.performance = { now: function () { return Date.now(); } };
    }

    // ---- queueMicrotask (Promise-based fallback) ----
    if (typeof g.queueMicrotask === "undefined") {
        g.queueMicrotask = function (cb) { Promise.resolve().then(cb); };
    }

    // ---- BigInt passthrough ----
    // Legacy ChakraCore lacks BigInt. Only referenced by unused physics module-level
    // initializers (e.g. `[BigInt(0)]`) that never run in non-physics scenes; a Number
    // passthrough keeps those declarations from throwing. NOT real BigInt semantics —
    // a physics scene would need the engine's true BigInt.
    if (typeof g.BigInt === "undefined") {
        g.BigInt = function (n) { return Number(n); };
    }

    // ---- Array.prototype.flat / flatMap ----
    // Legacy ChakraCore predates these ES2019 methods. Used by the glTF loader (loadGltf
    // flatMaps accessor/primitive lists). Setup-only (asset parse), never per-frame.
    if (typeof Array.prototype.flat !== "function") {
        Object.defineProperty(Array.prototype, "flat", {
            configurable: true, writable: true,
            value: function (depth) {
                const d = depth === undefined ? 1 : Math.floor(Number(depth)) || 0;
                const flatten = (arr, dd) => arr.reduce((acc, v) => {
                    if (Array.isArray(v) && dd > 0) {
                        acc.push.apply(acc, flatten(v, dd - 1));
                    } else {
                        acc.push(v);
                    }
                    return acc;
                }, []);
                return flatten(this, d);
            },
        });
    }
    if (typeof Array.prototype.flatMap !== "function") {
        Object.defineProperty(Array.prototype, "flatMap", {
            configurable: true, writable: true,
            value: function (cb, thisArg) {
                return this.map(function (v, i, a) { return cb.call(thisArg, v, i, a); }).flat();
            },
        });
    }
    // ---- fetch: resolve root-relative URLs ----
    // The native host has no document origin, so a scene that fetches a root-relative asset
    // (e.g. "/brdf-lut.png", "/textures/foo.env" — served from lab/public on the web) would
    // hand WinHTTP a hostless URL and stall. Rewrite leading-"/" URLs to a file:// URL under
    // a configured public root (globalThis.__LITE_PUBLIC_ROOT, injected by the bundler). This
    // lets the upstream scene corpus run unmodified. Absolute (http/https/file) URLs pass through.
    if (typeof g.fetch === "function" && !g.__fetchRootPatched) {
        const root = g.__LITE_PUBLIC_ROOT;
        const origFetch = g.fetch.bind(g);
        g.fetch = function (url, opts) {
            if (typeof url === "string" && url.charAt(0) === "/" && root) {
                url = root.replace(/\/+$/, "") + url;
            }
            return origFetch(url, opts);
        };
        g.__fetchRootPatched = true;
    }

    // ---- Image + 2D canvas (heightmaps / texture-from-canvas) ----
    // Some scenes load a PNG/JPEG via `new Image()` and read its pixels through a 2D canvas
    // (createGroundFromHeightMap: drawImage + getImageData). The native host has no DOM 2D
    // context, but it does have fetch + createImageBitmap (WIC decode). Implement Image and a
    // minimal 2D canvas over those: Image.src fetches+decodes to an ImageBitmap; the 2D
    // context's drawImage stashes that bitmap and getImageData returns its RGBA bytes (via the
    // native ImageBitmap._getPixels()). Only the pixel-readback subset is supported (no actual
    // 2D rasterization), which is all the heightmap path needs.
    if (typeof g.Image === "undefined") {
        g.Image = function Image() {
            this.width = 0;
            this.height = 0;
            this.crossOrigin = null;
            this.onload = null;
            this.onerror = null;
            this._bitmap = null;
            const self = this;
            Object.defineProperty(this, "src", {
                configurable: true,
                get() { return self._src; },
                set(url) {
                    self._src = url;
                    Promise.resolve()
                        .then(() => fetch(url))
                        .then((r) => r.blob())
                        .then((b) => createImageBitmap(b))
                        .then((bmp) => {
                            self._bitmap = bmp;
                            self.width = bmp.width;
                            self.height = bmp.height;
                            if (typeof self.onload === "function") self.onload();
                        })
                        .catch((e) => {
                            if (typeof self.onerror === "function") self.onerror(e);
                        });
                },
            });
        };
    }

    // Factory the native document.createElement("canvas") calls to get a 2D-capable canvas.
    g.__lite2DCanvasFactory = function () {
        const canvas = { width: 0, height: 0 };
        let drawn = null; // the Image (or its bitmap) most recently drawn
        canvas.getContext = function (type) {
            if (type !== "2d") return null;
            return {
                canvas: canvas,
                drawImage(img /*, dx, dy, ... */) {
                    drawn = (img && img._bitmap) ? img._bitmap : img;
                },
                getImageData(_x, _y, w, h) {
                    const bmp = drawn;
                    if (bmp && typeof bmp._getPixels === "function") {
                        return { data: bmp._getPixels(), width: bmp.width, height: bmp.height };
                    }
                    // No source drawn — return a zeroed buffer of the requested size.
                    return { data: new Uint8ClampedArray(w * h * 4), width: w, height: h };
                },
                fillRect() {},
                clearRect() {},
                putImageData() {},
            };
        };
        canvas.toDataURL = function () { return ""; };
        return canvas;
    };
})(typeof globalThis !== "undefined" ? globalThis : this);
