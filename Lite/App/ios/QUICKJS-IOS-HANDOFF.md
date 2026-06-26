# QuickJS-on-iOS handoff

Goal: build/run the iOS `LiteApp` (cubes.glb demo) with **QuickJS** instead of the default
JavaScriptCore. This doc is the delta from the JavaScriptCore path documented in `README.md` —
read that first for the overall iOS port (surface, image decode, host app, asset bundling).

## TL;DR — what changes vs the JSC default

1. Point the build at the **QuickJS fork** of JsRuntimeHost (the same fork used for the
   desktop QuickJS demo), at the POC branch **plus two required patches** (below).
2. Add `-DNAPI_JAVASCRIPT_ENGINE=QuickJS` to the CMake configure line.
3. Nothing else in the Lite tree changes: the iOS host (`App/ios`), the WebGPU/Metal polyfill,
   the image decoder, and the JS bundle are all **engine-agnostic**. The Apple engine default
   in `Lite/CMakeLists.txt` is a plain `set(... CACHE STRING ...)` (no FORCE), so the `-D`
   override wins; the iOS app links the JS engine **transitively** through the Node-API target
   (it does not hardcode JavaScriptCore).

## The two REQUIRED fork patches

The QuickJS POC branch (`origin/quickjs`, base commit `3ae53f8`) is **not** sufficient on its
own. These two fixes were made on top of it and have **not been pushed** — they live only as
patches in `Lite/App/ios/patches/`:

| Patch | Commit | Why it's required |
|---|---|---|
| `0002-fix-quickjs-napi-napi_escape_handle-double-free-UAF-.patch` | `5f09ad5` | **QuickJS-specific, crash-level.** `napi_escape_handle` inserted the dup'd escaped value *inside* the scope being closed, so `napi_close_escapable_handle_scope` double-freed it and corrupted the heap (`STATUS_STACK_BUFFER_OVERRUN`). It fires under the **IBL/`.env` prefilter** setup, which has heavy escapable-scope churn. The iOS demo scene (`cubesios.ts`) **loads an environment** (`loadEnvironment` + `app:///environmentSpecular.env`), so it hits this path. Without the patch, the demo crashes ~2/3 of launches during environment setup. |
| `0001-url-polyfill-coerce-args-to-string.patch` | `9621e1e` | **All engines** (also required for the JSC build). The glTF loader does `new URL(".", urlObject)`; the URL polyfill threw "A string was expected" on a non-string arg. Uses `ToString()` coercion per WHATWG. |

Both touch disjoint files, so apply order doesn't matter:
- `0002` → `Core/Node-API/Source/{env_quickjs.cc, js_native_api_quickjs.cc, js_native_api_quickjs.h}`
- `0001` → `Polyfills/URL/Source/URL.cpp`

If the recipient already has these in their fork branch (they said they have "the QuickJS POC"
— verify whether it's just `3ae53f8` or includes `5f09ad5`/`9621e1e`), they can skip
re-applying. Check with `git log --oneline | grep -E 'escape_handle|url polyfill'`.

## Build steps (macOS)

```bash
# 1) Prepare the patched QuickJS fork (the POC fork, NOT upstream JsRuntimeHost)
git clone https://github.com/CedricGuillemet/JsRuntimeHost.git ~/JsRuntimeHost-qjs
cd ~/JsRuntimeHost-qjs
git checkout quickjs            # the POC branch (base 3ae53f8)
# apply the two fixes if not already present:
git apply /path/to/BabylonNative/Lite/App/ios/patches/0002-fix-quickjs-napi-napi_escape_handle-double-free-UAF-.patch
git apply /path/to/BabylonNative/Lite/App/ios/patches/0001-url-polyfill-coerce-args-to-string.patch

# 2) Build the JS bundle (engine-agnostic; same bundle works for QuickJS or JSC)
cd /path/to/BabylonNative/Lite/bundler
node build-lite.mjs             # emits dist/cubesios.lite.js

# 3) Stage resources (see README §2)
cd /path/to/BabylonNative/Lite/App/ios
./stage-resources.sh

# 4) Configure for iOS with QuickJS
cd /path/to/BabylonNative/Lite
cmake -B build-ios-qjs -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DNAPI_JAVASCRIPT_ENGINE=QuickJS \
  -DFETCHCONTENT_SOURCE_DIR_JSRUNTIMEHOST=$HOME/JsRuntimeHost-qjs \
  -DLITE_IOS_BUNDLE_ID=com.yourorg.nativelite \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YOUR_TEAMID

# 5) Build & run
cmake --build build-ios-qjs --config Release --target LiteApp
```

For the **Simulator**, add `-DCMAKE_OSX_SYSROOT=iphonesimulator` (and you can drop signing/team).

## Things that are already handled (don't redo them)

- **quickjs-ng builds for iOS.** The QuickJS fork's own root `CMakeLists.txt` already has an
  `if(IOS)` block (ios-cmake toolchain), and the POC branch's base commit is literally
  *"suppress -Wshorten-64-to-32 on Apple Clang"* — i.e. quickjs-ng was already being compiled
  under Apple Clang. quickjs-ng is portable C; it cross-compiles to iOS/arm64 as a normal
  static lib (`qjs` target) under the standard `CMAKE_SYSTEM_NAME=iOS` toolchain.
- **Engine linkage.** `App/ios/CMakeLists.txt` links only UIKit/Foundation/QuartzCore/Metal.
  The JS engine (`qjs` or JavaScriptCore.framework) is linked by JsRuntimeHost's Node-API
  target based on `NAPI_JAVASCRIPT_ENGINE`. No JSC-specific code in the host.
- **Bundle is engine-agnostic.** The host-prelude shims (TextEncoder/Decoder, etc.) and the
  `BigInt(n)` lowering are harmless on QuickJS (they exist for legacy ChakraCore; QuickJS-ng
  supports modern JS natively). Same `cubesios.lite.js` runs on both engines.

## Potential issues to watch (QuickJS-specific, NOT yet verified on iOS)

> Honesty: QuickJS has been verified on the **Windows desktop** Lite build (including the full
> IBL/`.env` path after the escape-handle fix — 8/8 clean runs). It has **never been compiled
> or run on iOS**. The list below is what to expect/debug.

1. **Two toolchain paths must not collide.** The QuickJS fork's root CMake has its own
   `if(IOS)` ios-cmake block (sets `CMAKE_TOOLCHAIN_FILE`, `PLATFORM=OS64COMBINED`). We drive
   iOS via the **standard** `-DCMAKE_SYSTEM_NAME=iOS` instead. The fork's `if(IOS)` keys off
   ios-cmake's `IOS` variable (not set in the standard path), so it should be skipped — but if
   the `qjs` target misconfigures, check that the fork isn't trying to apply its own toolchain
   mid-configure. Prefer the standard `CMAKE_SYSTEM_NAME=iOS` path for consistency with the
   rest of the Lite app; don't mix in `-DPLATFORM=OS64COMBINED`.
2. **No JIT — perf floor is higher than V8/JSC-fast.** QuickJS is a pure interpreter. Lite's
   per-frame JS is tiny (cached render bundle → ~1 `executeBundles`/frame), so it should still
   be smooth, but setup (glTF parse of cubes.glb's 8000 nodes, `.env` prefilter) will be slower
   than V8. This is fine for a functional demo; just don't compare its setup time to the V8
   numbers.
3. **`QJS_BUILD_LIBC`.** The fork defaults `QJS_BUILD_LIBC=ON`. That should be fine on iOS, but
   if linking complains about libc/std file APIs unavailable in the iOS sandbox, try
   `-DQJS_BUILD_LIBC=OFF` (Lite doesn't use QuickJS's std/os modules — assets go through
   UrlLib/fetch, not `std.open`).
4. **Atomics/threads.** The threaded-submit path (`LITE_THREAD_SUBMIT=1`) spins a render
   thread that touches Dawn, not QuickJS, so it's engine-independent — but keep the JS engine
   single-threaded (it already is; only the GPU submit moves off-thread).

## Fallback scene if IBL is the problem

If the IBL/`.env` path still misbehaves on QuickJS/iOS after the escape-handle patch, there's a
lighter scene that **skips `loadEnvironment`** entirely (directional light instead of IBL):
`Lite/bundler/scenes-lite/cubesqjs.ts`. It exercises the same cached-render-bundle path with
8000 cubes but avoids the `.env` prefilter. To use it for the iOS bundle, copy its body into a
new `*ios` scene with `app://` asset URLs (it currently uses desktop `file://` paths), rebuild,
and stage as `scene.lite.js`. Use this only to isolate an IBL-specific failure; the real target
is `cubesios.ts` (full IBL), which is the apples-to-apples match to the JSC/desktop demos.

## What "working" looks like

The app shows the orbiting 20×20×20 cube grid with IBL lighting, HUD in the corner. Console
(`NSLog`, tagged `[js:*]`) should print:
```
cubesios: loading cubes.glb (8000 nodes) ...
cubesios: cubes.glb added
cubesios: loading environment (.env) ...
cubesios: environment loaded        <- if this line never prints, suspect the escape-handle fix isn't applied
cubesios: scene registered
cubesios: startEngine returned
```
The "environment loaded" line is the tell: reaching it means the escapable-scope churn in the
IBL prefilter survived (i.e. patch `0002` is in effect).
