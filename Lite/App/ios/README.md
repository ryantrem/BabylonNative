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
./stage-resources.sh
```

This populates `Resources/` with `scene.lite.js` + `cubes.glb` + `brdf-lut.png` +
`environmentSpecular.env`. `cubes.glb` is taken from the in-repo `Lite/assets/`,
`brdf-lut.png` already ships in `Resources/` (committed), and the `.env` is downloaded if
missing. (Override sources via `LITE_IOS_*` env vars — see the script header.)

### 3. Patch JsRuntimeHost (REQUIRED) and configure the Xcode project

The glTF loader calls `new URL(".", urlObject)`; the upstream URL polyfill at our pinned
commit throws "A string was expected" on that. The fix lives in
`patches/0001-url-polyfill-coerce-args-to-string.patch`. Clone JsRuntimeHost at the pinned
commit, apply the patch, and point the build at it via `FETCHCONTENT_SOURCE_DIR_JSRUNTIMEHOST`:

```bash
# one-time: prepare a patched JsRuntimeHost source tree
git clone https://github.com/BabylonJS/JsRuntimeHost.git ~/JsRuntimeHost
cd ~/JsRuntimeHost
git checkout 272f6a9f3de78f7c4cd8a838ae9655c81fc4881a
git apply /path/to/BabylonNative/Lite/App/ios/patches/0001-url-polyfill-coerce-args-to-string.patch

# configure (note the FETCHCONTENT override so Dawn/JRH aren't re-pinned)
cd /path/to/BabylonNative/Lite
cmake -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DFETCHCONTENT_SOURCE_DIR_JSRUNTIMEHOST=$HOME/JsRuntimeHost \
  -DLITE_IOS_BUNDLE_ID=com.yourorg.nativelite \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YOUR_TEAMID
```

For the **Simulator** instead of a device, add
`-DCMAKE_OSX_SYSROOT=iphonesimulator` (and you can drop the signing/team flags).

> The first configure downloads + builds Dawn/Tint (Metal + MSL writer). This is slow.
> The Node-API target auto-selects **JavaScriptCore** on Apple (the system framework).

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

## Session handoff (Windows → MacBook)

Everything needed to build on the Mac is committed to this repo on the **`lite`** branch — no
external session state required. Start a fresh agent session on the Mac and give it this repo +
the prompt below.

**Pins / facts the new session needs:**
- Repo branch: `lite`; the iOS port is the commit titled
  *"feat(lite): port cubes.glb demo to iOS (Dawn/Metal + JavaScriptCore)"*.
- Dawn pinned tag: `a44d7a3d78f23c680491c0fc04f53a1df62e02ff` (Metal backend, MSL writer).
- JsRuntimeHost pinned: `272f6a9f3de78f7c4cd8a838ae9655c81fc4881a` **+ the URL patch** in
  `patches/` (apply it; point `-DFETCHCONTENT_SOURCE_DIR_JSRUNTIMEHOST` at the patched clone).
- JS engine on Apple auto-selects **JavaScriptCore** (system framework; no download, no JIT
  entitlement). Do **not** try V8 on iOS.
- Assets: `cubes.glb` (in `Lite/assets/`) + `brdf-lut.png` (committed in `Resources/`) travel
  with the repo; `environmentSpecular.env` is downloaded by `stage-resources.sh`.

**What's verified vs not:**
- ✅ Windows Release build is green after the cross-platform refactor (no D3D12 regression).
- ❌ Nothing has been compiled for iOS yet. The Obj-C++/CMake is written from Dawn/UrlLib
  source inspection, not a real Apple toolchain run.

**Most likely first issues to debug on the Mac (in priority order):**
1. CMake/Xcode generator wiring: framework `find_library` calls, the `MACOSX_BUNDLE` resource
   copy, code-signing attributes. Iterate at the configure/link stage first.
2. Dawn Metal `SurfaceSourceMetalLayer` field/usage drift vs. the pinned tag (the struct/member
   names — `.layer` — are from the pinned Dawn; verify against the actual headers).
3. Runtime: `app://` asset loads via `NSBundle` (confirm `cubes.glb`/`.env`/`brdf-lut.png`
   actually land in `NativeLite.app/` and resolve). Watch the `[js:*]` NSLog output.
4. First-frame surface configure / drawable size on rotation.

**Suggested prompt for the new Mac session:**

> I'm continuing a Babylon Native Lite port to iOS on macOS. The work is committed on the
> `lite` branch of this BabylonNative repo — see `Lite/App/ios/README.md` for the full design
> and build steps. Please: (1) prepare the patched JsRuntimeHost per the README §3, (2)
> configure + build the `LiteApp` iOS target for the Simulator first, (3) fix compile/link
> errors iteratively (Dawn Metal + JavaScriptCore + UIKit host), then (4) get the cubes.glb
> demo rendering with the orbiting camera. The Windows build is the reference and must stay
> working — keep all platform-specific code behind `_WIN32` / `__APPLE__` guards.
