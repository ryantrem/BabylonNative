# Babylon Native Lite — iOS host

This is the UIKit host for the cubes.glb Lite demo running natively on iOS via **Dawn (Metal)**
+ **JavaScriptCore**. The unmodified Babylon Lite JS engine runs over our WebGPU N-API polyfill,
exactly like the Windows demo — only the platform shell differs:

| Layer | Windows | iOS (this host) |
|---|---|---|
| JS engine | Chakra / V8 | **JavaScriptCore** (system framework, no JIT entitlement needed) |
| GPU backend | Dawn **D3D12** | Dawn **Metal** |
| Surface | `HWND` | `CAMetalLayer` (`LiteMetalView`) |
| Shader writer | Tint HLSL + FXC | Tint **MSL** |
| Image decode | WIC | **ImageIO** (`WebGPU_AppleImage.mm`) |
| Window/loop | Win32 message pump | `UIViewController` (render loop stays on the JS thread via Lite's rAF pump) |
| Assets | `file:///D:/...` | bundled into the `.app`, resolved via `app://` → `NSBundle` |

The render loop runs **entirely on the JS thread** (Lite's `requestAnimationFrame` pump
re-schedules onto `AppRuntime`'s thread). UIKit's main thread only owns the `CAMetalLayer` and
forwards resize — there is no CADisplayLink-driven draw loop.

## Files

- `main.mm` / `LiteAppDelegate.*` / `LiteViewController.*` — minimal UIKit shell.
- `LiteMetalView.*` — `UIView` whose `+layerClass` is `CAMetalLayer`.
- `LiteHost.{h,mm}` — the C++/Obj-C++ bring-up (mirrors the Win32 `Main.cpp`): creates
  `AppRuntime`, installs Console/Fetch/Blob/URL, calls `nativelite::Initialize` with the Metal
  layer, loads the bundled scene, and runs it.
- `Info.plist.in` — bundle template (Metal capability, no storyboard).
- `Resources/` — staged at build time: `scene.lite.js`, `cubes.glb`, `brdf-lut.png`,
  `environmentSpecular.env`. Everything here is copied into `NativeLite.app/`.
- `stage-resources.sh` — copies/downloads those four files into `Resources/`.

## Build (on macOS with Xcode)

> iOS binaries can only be built on macOS. This host is written on Windows but compiled there.

### 1. Build the JS bundle

```bash
cd Lite/bundler
node build-lite.mjs            # emits dist/cubesios.lite.js (among others)
```

`cubesios.ts` references assets via the `app://` URL scheme (e.g. `app:///cubes.glb`);
JsRuntimeHost's UrlLib resolves those to `NSBundle` resources, so the scene loads bundled
files offline with no host-side path rewriting.

### 2. Stage bundle resources

```bash
cd Lite/App/ios
LITE_IOS_BRDF_PNG=/path/to/babylon-lite/assets/brdf-lut.png ./stage-resources.sh
```

This populates `Resources/` with `scene.lite.js` + `cubes.glb` + `brdf-lut.png` +
`environmentSpecular.env` (the `.env` is downloaded if missing).

### 3. Configure the Xcode project

```bash
cd Lite
cmake -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DLITE_IOS_BUNDLE_ID=com.yourorg.nativelite \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YOUR_TEAMID
```

For the **Simulator** instead of a device, add
`-DCMAKE_OSX_SYSROOT=iphonesimulator` (and you can drop the signing/team flags).

> The first configure downloads + builds Dawn/Tint (Metal + MSL writer). This is slow.

### 4. Build & run

```bash
cmake --build build-ios --config Release --target LiteApp
# or open build-ios/BabylonNativeLite.xcodeproj in Xcode, select the LiteApp scheme + your
# device/simulator, and Run.
```

## Notes / known rough edges

- **Resize**: `LiteHost::Resize` forwards to `Renderer::RequestResize`; the polyfill path
  reconfigures the surface on the next `GPUCanvasContext.configure`. Rotation works but a
  full reconfigure-on-rotate may need a frame to settle.
- **JIT**: third-party apps linking `JavaScriptCore.framework` run the interpreter/baseline
  (no fast-JIT entitlement). Lite's per-frame JS is tiny (cached render bundle → ~1
  `executeBundles`/frame), so this is not the bottleneck.
- **Benchmark exit**: setting `LITE_BENCH_FRAMES` makes the process `std::exit(0)` after the
  run — fine for a tethered profiling run, not for an interactive build.
- This host has not yet been compiled on a Mac in this workspace (Windows-only here); expect
  to iterate on the first round of Xcode/SDK build errors.
