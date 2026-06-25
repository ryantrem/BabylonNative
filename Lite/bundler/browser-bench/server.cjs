const http = require("http");
const fs = require("fs");
const path = require("path");
const root = __dirname;
// Root-relative assets ("/brdf-lut.png", "/textures/...") are served from lab/public on the
// web; mirror that here so the canonical scene corpus runs unmodified in the browser bench.
const publicRoot = "D:\\Repos\\Babylon-Lite-3\\lab\\public";
const types = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".png": "image/png", ".env": "application/octet-stream",
  ".dds": "image/vnd.ms-dds", ".jpg": "image/jpeg", ".glb": "model/gltf-binary",
  ".wasm": "application/wasm" };
http.createServer((req, res) => {
  const u = decodeURIComponent(req.url.split("?")[0]);
  const benchFile = path.join(root, u === "/" ? "/harness.html" : u);
  const tryFiles = [benchFile];
  // If not found under the bench dir, fall back to lab/public (root-relative assets).
  tryFiles.push(path.join(publicRoot, u));
  (function attempt(i) {
    if (i >= tryFiles.length) { res.writeHead(404); res.end("404"); return; }
    fs.readFile(tryFiles[i], (err, data) => {
      if (err) return attempt(i + 1);
      res.setHeader("Access-Control-Allow-Origin", "*");
      res.writeHead(200, { "Content-Type": types[path.extname(tryFiles[i])] || "application/octet-stream" });
      res.end(data);
    });
  })(0);
}).listen(8099, () => console.log("serving on http://localhost:8099"));
