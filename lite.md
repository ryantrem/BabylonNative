# Native Babylon Lite — Architecture Plan (native render loop on Dawn)

## ⚠️ ARCHITECTURE CLARIFICATION (2026-06-25) — two render paths; the scene corpus uses the JS one

Investigating the per-draw perf gap surfaced a critical distinction that reframes every
scene-corpus benchmark in this doc. The host (`NativeLite.cpp`) has **two** render-loop
drivers, selected by how the scene bundle was built:

1. **Native zero-JS path** (`RealRenderFrame` → walks `scene._frameGraph._tasks`, dispatches
   by `task._kind`, `ExecuteForwardRenderTaskNative` issues draws in C++ by reading exposed
   `binding._draw` DrawCommands + a cached opaque render-bundle). Runs **only** for bundles
   that **externalize the render loop** so Lite's `renderFrame` is tree-shaken out
   (the hand-built validation scenes: `box`/`multi`/`pbr`/`gltf`/`gltfenv` via
   `bundler/scenes-lite/*.ts`). This is the project's core thesis — proven on a real
   glTF+PBR+IBL scene — and it genuinely runs **zero Lite JS per frame**.

2. **JS rAF path** (`requestAnimationFrame` pump, NativeLite.cpp ~line 1123). Real Lite's
   `startEngine` calls `requestAnimationFrame(_renderFn)`; `_renderFn` runs Lite's **JS
   `renderFrame`** (which records the whole frame — `setPipeline`/`setVertexBuffer`/
   `setBindGroup`/`drawIndexed` per draw) **over the WebGPU N-API polyfill**; the host then
   presents + times it. This is "config #2 / Model-B" — JS in the loop.

**The entire canonical scene corpus (`dist/sceneN.lite.js`, built by `build-lite.mjs`) uses
path #2.** Verified: every `dist/*.lite.js` (incl. `box`/`gltfenv`) still contains a
`renderFrame` def + `requestAnimationFrame` refs, so `RealRenderFrame`/
`ExecuteForwardRenderTaskNative` are **never called** for them (a drawdiag counter placed in
that function never fired across 300 frames, while the bench still completed — proof the
frame came from the rAF path).

**Consequence for the perf numbers below:** the "native ~3–5× slower than browser above ~30
draws" gap is **not** a native-render-loop cost — it is the **per-WebGPU-call N-API marshaling
cost of running Lite's JS `renderFrame` over the polyfill**, frame after frame (each draw =
~5 N-API calls × engine boundary crossings). Above ~30 draws this dominates. The native
zero-JS path (#1) avoids it entirely but currently covers only the 5 validation scenes
(it needs frame-graph generalization + `_draw` exposure for every material/task family —
the `lite-codesign` work — to cover the broad corpus).

**Two levers to close the heavy-scene gap, by path:**
- **Path #2 (broad corpus, what scenes run today):** cut per-WebGPU-call N-API overhead —
  the **V8 direct-binding / fast-call** idea (the `_mbV8Fast*` probes are already installed).
  Batch or fast-path `setVertexBuffer`/`setBindGroup`/`drawIndexed`. Highest-leverage,
  bounded-ish, benefits ALL config-#2 scenes.
- **Path #1 (native loop):** extend coverage (generalize frame-graph task kinds + expose
  `_draw` for PBR-shadow/geometry/post-process/transmission families + thin-instance/cull).
  Bigger lift; each scene that converts drops to zero JS/frame.

Heavy canonical scenes (`scene20` ~7502 draws, `scene24` 133 draws) currently stall in
**setup** under the rAF bundle on the latest build (never reach steady-state render) — the
`scene20` 12.5 ms / `scene24` 1.53 ms figures were captured earlier this session and reflect
path #2 when those bundles built/ran. Re-validate setup before re-benchmarking heavy scenes.


## Benchmark results — consolidated (V8, `cubes.glb` = 8000 unique meshes → 8000 draws/frame)

Metric: **render-loop CPU ms/frame** (present/GPU excluded). NO_VSYNC. `min` is the
contention-resistant floor (best cross-session comparator); `avg` is same-session mean.
ChakraCore rows omitted. Full detail + caveats in the session `files/benchmark-results.md`.

| # | Stack | Render loop | Render path | cpu_avg | cpu_min |
|---|---|---|---|---:|---:|
| 1  | **Browser** (Chrome, native WebGPU) | Lite JS | no bundle (per-draw) | 17.4 | 3.8 |
| 1b | **Browser** (Chrome, native WebGPU) | Lite JS | **render bundle** | **3.4** | 1.0 |
| 2  | Native host + our Dawn/WebGPU | **Lite JS** | no bundle (per-draw) | 39.4 | 33.7 |
| 3  | Native host + our Dawn/WebGPU | **native C++** | no bundle (per-draw) | 61.8 | 56.7 |
| 4  | Native host + our Dawn/WebGPU | **Lite JS** | **render bundle** | 14.8 | 13.1 |
| 5  | Native host + our Dawn/WebGPU | **native C++** | **render bundle** | **13.2** | **11.5** |
| 6  | **Regular Babylon Native** (full Babylon.js + bgfx) | Babylon.js | no bundles | 483.6 | 368.6 |
| 7  | Native host (**QuickJS**) + our Dawn/WebGPU | **Lite JS** | **render bundle** | ~27.9 | ~24.5 |

### QuickJS vs V8 — the headline comparison (`cubes.glb`, 8000 draws)

The point of swapping the JS engine: Lite pushes ~zero engine JS into the render loop (a
cached opaque `GPURenderBundle` → one `executeBundles`/frame), so even a **bytecode
interpreter** (QuickJS, no JIT) should stay far ahead of a full retained-mode engine on a
JIT. Measured (present-excluded render-loop CPU ms/frame, NO_VSYNC):

| Stack | JS engine | render-loop CPU | load time¹ | main mem² | GPU mem² | vs full BN |
|---|---|---:|---:|---:|---:|---:|
| **Lite Native + QuickJS** (bundle) | QuickJS (interpreter) | **~27.9 ms** (~36 fps) | ~9–35 s³ | (leak³) | ~0.10 GB⁴ | **~17× faster** |
| Lite Native + V8 (bundle) | V8 (JIT) | ~14.8 ms | **~1.8 s**⁴ | **~0.51 GB**⁴ | **~0.10 GB**⁴ | ~33× faster |
| Lite Native + V8 — *before suballoc fix* | V8 (JIT) | ~14.8 ms | ~3.1 s | ~2.8 GB | ~3.0 GB | — |
| **Regular Babylon Native + V8** | V8 (JIT) | **483.6 ms** (~2 fps) | ~18.5 s | ~1.1 GB | ~0.12 GB | 1× (baseline) |

¹ Load time = wall clock from process launch to scene-ready (model fetched + parsed + GPU
  resources built + render loop starting), same 8000-cube `cubes.glb`.
² Memory sampled at steady state: **main** = process peak working set; **GPU** = per-process
  GPU "Dedicated Usage" (Windows perf counter). Box-scene baseline (Lite+V8): 158 MB main /
  80 MB GPU — confirms the figures scale with the scene, not a fixed allocation.
³ QuickJS load time is noisy and its main-mem is inflated by the napi leak-hack (see caveats),
  so it's omitted; GPU mem matches V8 (GPU resources are engine-independent).
⁴ **After the small-buffer suballocation fix** (see below). Before it, Lite used ~3.0 GB GPU /
  ~2.8 GB main — *more* than full Babylon Native. After, Lite uses **~0.10 GB GPU** (104 MB
  dedicated / 159 MB total committed — now *below* bgfx's 120 MB) and **~0.51 GB main**, and
  loads in **1.8 s** (the 48 K individual D3D12 buffer allocations were also a load-time cost).

### GPU-memory bug found and fixed: D3D12's 64 KB-per-buffer floor

Instrumenting `createBuffer` revealed Lite creates **6 GPU buffers per mesh** for this model
(3 vertex + 1 index + 2 uniform) → **48 002 buffers** holding only **8.3 MB** of real data.
But D3D12 rounds *every* buffer up to a **64 KB placement floor** (Dawn's allocator even
sub-allocates at `GetResourceAllocationInfo`'s 64 KB-floored size), so 48 002 × 64 KB ≈
**3.0 GB** — matching the measured 3 086 MB almost exactly. The waste is per-buffer
granularity, not data.

**Fix (polyfill `Device::CreateBuffer`):** a small-buffer **arena suballocator**. Buffers
below 8 KB that don't need a host-visible mapping are packed (256-aligned) into shared 4 MB
Dawn buffers, one bucket per usage mask; `GPUBuffer` becomes a `{arena, offset}` view and
every consuming site (`writeBuffer`, `setVertex/IndexBuffer`, bind-group entries,
`copyBufferToBuffer`) adds the base offset. `mappedAtCreation` sub-buffers (vertex/index)
upload via `queue.writeBuffer` into the arena at `unmap()` instead of a real GPU mapping. Net:
**3 086 → 104 MB GPU (≈30×), 2.8 → 0.51 GB main, 3.1 → 1.8 s load** — and box/cubes/gltfenv/
scene2/multi all render pixel-identically with zero Dawn validation errors. The proper
upstream fix is in Lite (interleave vertex attributes, share a uniform arena with dynamic
offsets), but the polyfill suballocator recovers the memory transparently for any scene.

**Takeaway:** Lite + QuickJS (~28 ms) is **~17× faster than full Babylon Native + V8
(~484 ms)** on the same 8000-cube model — *despite* QuickJS being a far slower JS engine
than V8. Architecture (lean engine + render-bundle replay) dominates JS-engine speed. The
QuickJS-vs-V8 gap within Lite (~28 vs ~15 ms, ~1.9×) is the expected interpreter-vs-JIT tax
on the per-frame JS that the bundle doesn't eliminate (scene `_update`, frame-graph execute).

**The memory/load story INVERTS the perf story (important).** Lite renders 17–33× faster but,
on this pathological **8000-unique-mesh** model, uses **~25× more GPU memory** (~3.0 GB vs
bgfx's 0.12 GB) and ~2.5× more main memory than full Babylon Native. Conversely Lite **loads
faster on V8** (3.1 s vs 18.5 s — Babylon.js's per-mesh scene-graph construction over 8000
meshes is slow), while Lite **on QuickJS loads slowly** (~9–35 s — interpreter glTF parse +
leak-hack overhead), in the same ballpark as full BN. Net: native-Lite trades **memory for
speed**, and the JS engine mostly moves load time, not render time.

**Why Lite's GPU memory is so high (flagged for investigation):** ~3.0 GB for 8000 small
cubes is ~25× bgfx's 0.12 GB and ~375× a naive per-cube estimate (~1 KB × 8000 ≈ 8 MB). It
scales with mesh count (box = 80 MB), so it's per-mesh GPU-resource bloat — likely each unique
mesh getting its own non-suballocated vertex/index/uniform buffers + bind groups through our
WebGPU→Dawn polyfill, and/or Dawn D3D12 heap over-commit. bgfx suballocates aggressively and
reports 0.12 GB. This is an optimization opportunity in the polyfill/Lite, not a perf-path
issue. (It also explains why the 8000-unique-mesh case is called pathological — real scenes
share geometry/materials and won't hit this.)

### Why QuickJS load is ~5× slower than V8 (it's interpreter compute, not the napi layer)

The QuickJS load looked suspiciously slow (the glTF *loader* in Lite is algorithmically much
leaner than Babylon.js, so one might expect QuickJS to "even out" against full BN). Phase-timed
profiling (per-phase JS timing + native wall-time accumulators inside `createBuffer`/
`writeBuffer`) shows where it goes, on the same `cubes.glb`:

| Phase | V8 | QuickJS | ratio |
|---|---:|---:|---:|
| createEngine (Dawn device init) | 290 ms | 298 ms | 1.0× |
| **loadGltf** (fetch + parse + GPU upload) | **733 ms** | **~3600 ms** | **~5×** |
| registerScene (render-bundle build) | 187 ms | 504 ms | 2.7× |
| startEngine | 26 ms | 70 ms | 2.7× |
| **TOTAL** | **1.25 s** | **~4.5 s** | **~3.7×** |
| *native* `createBuffer` (48 002 calls) | *86.6 ms* | *99.5 ms* | *1.15×* |
| *native* `writeBuffer` (16 003 calls) | *17.9 ms* | *16.7 ms* | *0.93×* |

**Key result: the native/Dawn work is identical (~100 ms on both engines)** — so the
napi-crossing cost (and the QuickJS leak-hack's per-call `JS_DupValue`/`JS_FreeValue`) is **not**
the bottleneck; it adds only ~13 ms across 48 K calls. The slowdown is **pure-JS execution**:
parsing a 14 MB binary glTF and decoding 8000 nodes' accessors/meshes is tight compute-heavy JS,
exactly what a JIT (V8) accelerates and a bytecode interpreter (QuickJS) does not. So `loadGltf`
takes the full ~5× interpreter penalty, while the render loop only sees ~2× (it's mostly the
native render-bundle replay, with little per-frame JS).

This means Lite's faster-than-Babylon.js loader and the JS-engine choice are **independent**
axes: Lite's loader is the same lean algorithm on both engines, but running it on QuickJS still
pays a ~5× interpreter tax on the parse. A correct (non-leaking) QuickJS napi backend would
**not** materially speed up load — the cost is interpreter compute, not the leak. (It would,
however, remove the load-time flakiness and the inflated main-mem.)


JsRuntimeHost `quickjs` fork (quickjs-ng) whose N-API prototype has a handle-scope/refcount
bug (close frees borrowed values → UAF on any ObjectWrap value through a promise/callback).
A quick hack — make `FromJSValue` dup so the scope's free balances — unblocks it but (a)
leaks one ref at every owned-value site and (b) adds a `JS_DupValue`/`JS_FreeValue` on every
napi value crossing, which **inflates** both the per-frame number and main-mem. A correct napi
memory-model fix would make Lite+QuickJS *faster* and lighter than shown. Setup (8000-cube
buffer alloc) is also intermittently flaky under the leak pressure. The V8 Lite and 483.6 ms
full-BN render numbers are prior same-session/same-machine/same-model measurements; the load
and memory figures were measured this session (Lite+V8 + BN+V8 rebuilt with raised bgfx
buffer caps to render 8000 unique meshes).

**Reading it:**
- **Render path dominates, not loop language.** No-bundle (re-record 8000 draws/frame): native
  C++ (61.8) is *slower* than the Lite JS loop (39.4) because the C++ loop pays N-API to read
  each draw's data out of JS every frame. With a render bundle (replay 1 pre-recorded bundle):
  native (13.2) ≈ JS (14.8). Bundling is a ~3–4× win and collapses the native-vs-JS gap to ~1.5 ms.
- **Native loop only beats the JS loop once draws are bundled** (row 5 < row 4); on the per-draw
  path it loses (row 3 > row 2). "Zero JS per frame" only pays off when native also stops
  *reading* JS per draw — which the bundle achieves (native reads ONE bundle handle, not 8000 draws).
- **Browser is ~4× faster than our native stack even on the bundle path** (3.4 vs 13.2). The
  per-draw cost is gone for both, so this residual gap is our **N-API/WebGPU→Dawn binding-layer +
  per-frame O(1) JS-read overhead** (scene/task navigation, UBO writes, encoder/pass calls all
  cross N-API in our stack; in-browser they're native JS→native WebGPU with no marshaling).
- **Regular Babylon Native is ~33× slower (484 ms).** Same model, full Babylon.js scene graph
  per-mesh ×8000 each frame, no render bundles. GPU time was only 13 ms — pure CPU scene-graph
  overhead. (Also couldn't render 8000 unique meshes until bgfx vertex/index buffer handle caps
  were raised from the 4096 default.)

**Takeaways:** For **static/cacheable** scenes the big win is **Lite + render bundles** (~30× vs
full Babylon Native), and native-loop vs JS-loop barely differ. The native render loop's *unique*
value is the **per-frame-mutated / animated** class — many independently-moving objects — where
neither a static bundle nor per-object JS reads work; that needs native-owned or shared-arena
per-frame transform data (see TODO). The remaining native-vs-browser gap on the bundle path is the
binding layer, not draw cost.

## CURRENT DIRECTION (2026-06-24) — all Lite JS executes; focus = WebGPU polyfill

Decision after the benchmarks above: **stop investing in the native C++ render loop / native
Lite-API ports as a perf play.** The data shows the native loop buys ~1.5 ms (~10%) on static
scenes and is *slower* on uncacheable scenes; the real wins (Lite itself = ~33×, render bundles =
~3×) are free and live entirely in JS. The remaining native-vs-browser gap (bundle path: 13.2 vs
3.4 ms, ~4×) is our **N-API/WebGPU→Dawn binding layer**, which a robust WebGPU polyfill addresses
and which benefits *every* configuration.

**Config (now the default):**
- The native C++ render loop and the native Lite-API plugin code are **RETAINED but INACTIVE**.
- The bundler default (`node build-lite.mjs`, no env) now builds the **all-JS** bundle (`.lite.js`):
  real Babylon Lite JS runs the entire engine + render loop in-app, driven by the native
  `requestAnimationFrame` polyfill. **Only WebGPU (+ canvas/document/rAF/fetch/createImageBitmap
  shims) is native.** Verified: `box.lite.js` renders via the real Lite JS loop (no native loop).
- The native render loop is **opt-in**: `LITE_NATIVE_LOOP=1 node build-lite.mjs` →
  `.nativeloop.lite.js` (externalizes startEngine/stopEngine/renderFrame onto the
  `BabylonNativeLite` global). Kept for the animated/per-frame-mutated experiments only.

**Focus going forward:** make the **WebGPU polyfill** (`Polyfills/WebGPU/`, 20 GPU* classes, 54
methods) robust + complete (run the Lite scene corpus, fix correctness gaps), then attack the ~4×
binding-layer gap.

### Next experiment — bypass N-API, expose WebGPU through V8 directly (FEASIBLE)
Hypothesis: the native-vs-browser bundle-path gap (4×) is N-API per-call + per-frame marshaling
overhead (e.g. `queue.writeBuffer` typed-array, `beginRenderPass` nested descriptor,
`setBindGroup`/`executeBundles`). Test: reimplement the hot per-frame WebGPU calls using the V8
C++ API directly (v8::FunctionTemplate + **V8 fast API calls**, internal fields, direct typed-array
backing-store access) instead of `Napi::`, and measure whether it closes the gap.
- **Feasibility CONFIRMED:** JsRuntimeHost's V8 N-API exposes `Napi::GetContext(Napi::Env)`
  (`Core/Node-API/Include/Engine/V8/napi/env.h`) → `v8::Local<v8::Context>` → `context->GetIsolate()`.
  So we can reach the live isolate/context from the existing module and mix direct-V8 bindings into
  the WebGPU polyfill incrementally (no fork of JsRuntimeHost needed).

### Threaded-submit prototype RESULT (2026-06-24) — JS-thread CPU hits BROWSER PARITY
Implemented an opt-in render thread (`LITE_THREAD_SUBMIT=1`) in the WebGPU polyfill: the JS
thread records the frame and hands the finished CommandBuffer(s) to a dedicated render thread
that runs `queue.Submit()` (the expensive Dawn→D3D12 translation) OFF the JS thread. Surface ops
(`getCurrentTexture`/`present`) STAY on the JS thread (the swapchain can't be touched cross-thread
— doing so produced "GetCurrentTexture was not called prior to Present" errors); `Present()` waits
for the frame's submit to finish, then presents. Dawn device/queue thread-safety via the
`implicit_device_synchronization` toggle (added at requestDevice only when threading is on).
Deferral is armed only after 30 frames (`LITE_THREAD_ARM_FRAME`) so async asset loading + mip
generation (whose setup submits `destroy()` transient textures) completes synchronously first —
that eliminated the "Destroyed texture used in a submit" errors (350 → 0).

Same-session result (cubesbundle, 8000 draws, ms; `min` = clean floor):

| Config | cpu_avg | cpu_min | device errors |
|---|---:|---:|---:|
| non-threaded (submit on JS thread) | 18.7–19.4 | **13.0–13.8** | 0 |
| **threaded submit (LITE_THREAD_SUBMIT=1)** | 8.0–8.9 | **3.4** | **0** |
| browser (reference) | 3.4 | 1.0 | — |

Profiler (threaded) confirms `submit` (now ~24 ms across 2 calls/frame) runs on the render thread
and no longer counts against the JS-thread frame; the JS thread's measured cost is just record +
executeBundles + endPass (~3 ms).

Findings:
1. **JS-thread render-loop CPU drops from 13.0 ms → 3.4 ms — exact browser parity.** This closes
   the entire native-vs-browser gap on the metric, and confirms the root cause: it was Dawn's
   synchronous in-process submit, exactly as the profiling predicted. Nothing else mattered.
2. **0 device errors, identical command stream.** The threaded change only moves WHERE Submit
   runs (different thread), not WHAT is recorded — the CommandBuffer is byte-identical, so pixel
   output is unchanged. (Visual PrintWindow capture is blank for BOTH threaded and non-threaded —
   a DXGI flip-model swapchain limitation, not a regression.)
3. **Honest caveat — wall-clock throughput** (CORRECTED with a real measurement after this
   write-up). I initially assumed FPS was unchanged; measuring **absolute wall-clock FPS (no
   vsync)** disproved that. Added `wall_ms_per_frame`/`wall_fps` to the harness (total real time
   across the measured window ÷ frames — includes submit, present, blocking; the honest end-to-end
   metric the present-excluded CPU number misses). Same scene, 300 frames, no vsync:

   | Config | cpu_avg | cpu_min | **wall ms/frame** | **wall fps** |
   |---|---:|---:|---:|---:|
   | non-threaded | 32 | 13.7 | **32–34** | **29–31** |
   | threaded submit | 6–7 | 3.3 | **21–25** | **40–47** |

   So threading improves **both** the JS-thread CPU (13.7→3.3 ms) **and** real throughput
   (~30→~43 fps, +45%) — there's already partial pipelining (the render thread submits frame N
   while the JS event loop dispatches/records frame N+1). It is NOT full parity: wall is still
   ~23 ms/frame (submit-bound at ~13 ms + present/process-events), so more overlap is available
   (decoupled acquire/present, deeper swapchain). Browser wall-FPS isn't directly comparable —
   its harness uses `requestAnimationFrame`, which is vsync-locked to ~60 fps, so the browser
   "cpu 3.4 ms" is present-excluded only and a true uncapped browser FPS wasn't collected.

### Metric note — prefer wall-clock FPS as the headline number
`cpu_ms/frame` (present-excluded, JS-thread only) answers "how much main-thread budget does a
frame cost" — useful, but **gameable**: moving work to another thread shrinks it without
necessarily improving throughput (exactly the trap I nearly fell into above). **Absolute
wall-clock FPS (no vsync)** is easier (one number: frames ÷ real time) and harder to game (it
includes submit/present/blocking), so it's the better *headline* throughput metric. Keep both:
wall-FPS for "how fast do we render", cpu/frame for "how much main-thread headroom is left for
game logic". The harness now emits both on the BENCH line.

**Bottom line of the whole investigation:** the native render loop (~1.5 ms), V8/N-API rewrite
(~0 ms), and polyfill micro-opt (<0.1 ms) were all dead ends; the one real lever — **moving Dawn
submit off the JS thread** — takes the JS-thread render-loop CPU from 13 ms to 3.4 ms (browser
parity), with zero correctness regressions. (Threaded path retained behind `LITE_THREAD_SUBMIT=1`.)


Instrumented the hot per-frame WebGPU polyfill methods with a wall-clock accumulator
(`Polyfills/WebGPU/Source/WebGPU.cpp`, gated by `LITE_GPU_PROFILE=1`; dumps a per-frame
breakdown every 60 frames in `Module::Present`). Ran `cubesbundle.lite.js` (all-JS, bundle
path, 8000 draws). Per-frame averages (two runs, ms):

| WebGPU call | run A | run B | calls/frame |
|---|---:|---:|---:|
| **submit** | **16.59** | **13.14** | 1 |
| executeBundles | 2.65 | 1.90 | 1 |
| endPass | 0.47 | 0.35 | 1 |
| present (excluded from cpu metric) | 0.41 | 0.32 | 1 |
| beginRenderPass | 0.07 | 0.05 | 1 |
| getCurrentTexture / createView / createCommandEncoder / setBindGroup / finish | <0.02 each | <0.02 | 1–2 |
| **SUM (instrumented)** | 20.3 | 15.8 | |
| frame cpu_avg (same run) | 24.1 | 16.1 | |

Findings:
1. **`queue.submit()` is ~80% of GPU-call time and ~80% of the whole frame.** Everything else
   except `executeBundles` is sub-0.1 ms.
2. **The JS update loop is negligible:** SUM(instrumented WebGPU) ≈ frame cpu_avg (15.8 vs 16.1),
   so the 8000-binding `updateBindings`/`b.update()` loop and the rest of Lite's `renderFrame`
   cost almost nothing. The frame IS the Dawn calls.
3. **Render bundles do NOT pre-translate to D3D12 in Dawn's native backend.** `executeBundles`
   only records "replay this bundle" (1.9–2.7 ms); the actual **Dawn→D3D12 command-list
   generation for all 8000 draws happens synchronously in `queue.submit()` on the JS thread**
   (13–16 ms). That's the entire native-vs-browser gap.

**Why the browser is 3.4 ms:** Chrome runs the SAME Dawn, but in a **separate GPU process**.
`queue.submit()` there just serializes commands and returns; the Dawn→D3D12 translation runs on
the GPU-process thread, OFF the JS thread that `performance.now()` measures. Our LiteApp runs Dawn
**in-process, synchronously on the JS thread**, so the submit cost lands inside the measured frame.

**Conclusion — the gap is architectural, not the polyfill.** It is NOT N-API overhead (microbench
proved that), NOT our descriptor marshaling (sub-0.1 ms), and NOT the JS loop (negligible). It is
Dawn's synchronous in-process command-list generation at submit. To match the browser we'd
**move `finish`+`submit` off the JS thread onto a dedicated render/submit thread** (JS thread
records the encoder + hands the CommandBuffer to a render thread that finishes/submits), mirroring
the browser's GPU-process split. That is a real, well-scoped change — and the only thing that can
close the ~10 ms gap. (Caveat: this overlaps submit with the next frame's CPU work; it lowers
per-frame *CPU-on-JS-thread* time, which is the metric, but total GPU throughput is unchanged. For
a real app that frees the JS thread for game logic — the actual goal.)
(Profiler retained behind `LITE_GPU_PROFILE=1` for future measurement.)


Built `Plugins/NativeLite/Source/Microbench.cpp` (probes installed on the global) + a JS driver
(`bundler/dist/microbench.js`). Each probe does trivial work (return x+1, or read float32[0]); only
the binding mechanism varies. 5M calls/run × 5 repeats, best-of (min ns/call), V8 engine:

| Arg shape | N-API | direct-V8 | V8 fast-API | N-API ÷ V8 |
|---|---:|---:|---:|---:|
| scalar (number→number) | 35.2 ns | 13.3 ns | 13.2 ns | **2.65×** |
| typed-array (f32[]→number) | 131.1 ns | 77.5 ns | 85.0 ns | **1.69×** |

Findings:
1. **N-API adds ~22 ns/call (scalar) / ~54 ns/call (typed array) over direct V8.** Real per-call
   overhead, but small in absolute terms.
2. **V8 fast-API did NOT engage** — fast ≈ direct-V8 (13.2 vs 13.3; TA fast even slightly slower,
   noise). The V8 redist (`v8-v143` 11.9.169.4) is evidently not built with
   `v8_enable_fast_api_calls`, so fast-API is not an available lever without a custom V8 build.
3. **The overhead is too small to explain the browser gap.** On the **bundle path** the per-frame
   WebGPU call count is tiny (≈20–40: getCurrentTexture/createView/createCommandEncoder/
   beginRenderPass/setBindGroup/executeBundles/end/finish/submit/present + ~1 scene-UBO writeBuffer
   — the 8000 draws live INSIDE the bundle, issued once at build). 40 calls × 22 ns ≈ **0.001 ms**
   saved by going direct-V8 — vs the **~9.8 ms** native(13.2)−browser(3.4) gap. Even on the
   no-bundle path (~48k calls/frame: 8000 draws × ~6 calls), the *total* N-API cost is ~1.7 ms and
   the direct-V8 *savings* ~1 ms — still a fraction of that path's ~22 ms gap.

**Conclusion: bypassing N-API for WebGPU is NOT worth it.** The boundary-crossing cost is real but
~2–3 orders of magnitude too small to account for the multi-ms browser gap. The gap must be in the
actual WebGPU *work*, not the JS↔native boundary — most likely **Dawn/D3D12 command translation +
validation + driver/submit/present cost vs Chrome's in-process WebGPU**, and/or our per-call
descriptor marshaling/validation in C++ (which runs in Chrome too, but may be heavier in Dawn).
Next investigation should **profile inside the native frame** (where the 13 ms actually goes:
Dawn validation, command encoding, submit, present) rather than optimize the binding layer.
(Microbench code retained behind the `_mb*` globals + `microbench.js` for future re-measurement.)




## DIRECTION RESET (2026-06-23) — run REAL Babylon Lite, native only where needed
After exploring native factories (Model A) and a JS engine layer (Model B), the user
corrected the architecture to its clean form. **Decision rule for what gets a native
(N-API) implementation — native ONLY if at least one holds:**
1. **It runs in the render loop** (per-frame) — required for zero-JS-per-frame. This is
   **transitive**: native `renderFrame` → its per-frame callees (frame-graph execute,
   pass execute, render-task draw loop) must also be native.
2. **The pure-JS impl needs something the JS runtime lacks** (canvas, `navigator.gpu`,
   `requestAnimationFrame`, `fetch`, …) — native may supply the gap (standard APIs
   preferred; native `createEngine` acceptable if wiring the canvas/adapter is impractical).
Otherwise it **stays real, unmodified Babylon Lite JS** running on our native WebGPU.

Anything exposed to JS via N-API must be a **real existing API** (WebGPU `GPU*`/
`navigator.gpu`, or a real Lite export with its real signature) — **no invented names**.

## TODO — come back to these (tracked here so we don't lose them)
- [ ] **Tree-shake the render-loop internals out of the bundle.** Right now `renderFrame`
  is externalized + gone, but `executePass` / `executePassBody` / `buildOpaqueRenderBundle`
  (and `FrameGraph.execute`) still ship because they're reachable via the scene context's
  `_record` *method* → `ctx._frameGraph.execute()`. GOAL: once `_record` is removed and the
  only entry is the externalized standalone `recordSceneContext`, `FrameGraph.execute` (or a
  future standalone `executeFrameGraph()`) should become unreachable and drop from the
  bundle too. Needs: stop the polymorphic `RenderingContext._update/_record` method dispatch
  in `engine.ts renderFrame` (it keeps the methods alive), route through the standalone
  `updateSceneContext`/`recordSceneContext`, and externalize those. Verify with a bundle grep
  that `executePass*`/`FrameGraph.execute` are gone. (Deferred — do AFTER native renders.)
- [ ] **Native per-frame UBO writes (dynamics).** DONE for the camera/scene UBO: native
  recomputes view + reverse-Z projection + viewProj natively (C++ ports of mat4-look-at-lh /
  mat4-perspective-lh-to-ref / mat4-multiply-into) from the arc-rotate camera's scalar DATA
  (alpha/beta/radius/target/fov/near/far) and writes the SceneUniforms head (viewProj@0,
  view@16, eye@32 — first 36 floats, preserving the setup-primed tail) into task._sceneUBO each
  frame via queue.writeBuffer. Verified: native-owned alpha offset orbits the camera, two
  captures show the box from different viewpoints, 0 Dawn errors. STILL TODO: per-frame MESH
  world-matrix UBO writes (moving/animated meshes — the box mesh is static so its mesh UBO stays
  setup-primed); lights UBO updates if lights move; reading real animation/input instead of a
  native-owned orbit offset.
- [ ] **Direct + transparent draw lists in native.** `DrawBinding._draw` (DrawCommand) is now
  exposed for the static standard-material path; native opaque replay uses the JS-built bundle.
  Extend native to iterate `_directBindings`/`_transparentBindings` reading `_draw` + pipeline
  (and do the transparent back-to-front sort) so non-bundled draws work natively.
- [ ] **Expose `_draw` for the other material families** (pbr/node/shader/sprite/text/skybox/
  shadow renderables). DONE for PBR (pbr-renderable.ts — static path: no thin-instance/cull/
  refraction; includes skinning vertex buffers as stable bindings). Standard + PBR exposed; node/
  shader/skybox/sprite/text/shadow still closure-only.
- [ ] **Thin-instance / GPU-cull draws** still use the `draw` closure (dynamic per-frame
  instance sync). Decide later whether native handles these or they stay JS-driven.

### Partition (grounded in real Lite source: engine.ts / frame-graph.ts / render-pass.ts / surface.ts)
- **Real Lite JS (setup):** `createEngine`, `createSceneContext`, `addToScene`,
  `registerScene`, `createDefaultCamera`, `createHemisphericLight`, material/pipeline/
  mesh/RT creation, `frameGraph.build()`, `pass._initialize()`.
- **Native (render loop, transitive):** `startEngine`/`stopEngine` (the loop driver —
  native pump, NOT RAF), `renderFrame`, `scene._update`/`_record`, `frameGraph.execute`,
  `task.execute`/`pass._execute`, `pass._executeFunc` (render-task draw loop), `_refreshScRT`.
- **Host shims (standard APIs, env-gap):** `navigator.gpu` (+ `GPUAdapter`) and
  `canvas.getContext("webgpu")`→`GPUCanvasContext` — the WebGPU env real Lite's
  `createEngine` needs. **No RAF / performance.now / devicePixelRatio:** RAF is only the
  JS-loop driver inside `startEngine`, which is native (see below); native `startEngine`
  computes `delta` from a native clock, and the canvas is presented offscreen-style (no
  layout) with size pushed via `setSurfaceSize`, so the DPR/clientWidth path is never hit.
- **Deleted (inventions):** native `createMaterial`/`createMesh`/`createNode`/
  `createCamera`/`addLight`/`createAnimation`/`registerDrawable`; `lite-engine.mjs`;
  `lite-runtime.mjs`; invented Renderer engine code (materials/meshes/scene-graph/
  animation/shadow/ShaderComposer). Kept: WebGPU polyfill (standard API) + Dawn device/
  surface/swapchain + command-encoder scaffolding (basis for the native loop).

### Two-step execution
`startEngine`/`stopEngine` are **native from the start** (they're the loop driver —
native pump, not RAF). The only thing that moves JS→native *between* milestones is
`renderFrame` itself:
1. **Milestone 1 — boot real Lite (native pump → JS renderFrame):** host shims
   (`navigator.gpu` + `GPUCanvasContext`) let real, unmodified Lite `createEngine`/scene
   setup run; native `startEngine` drives **real Lite JS `renderFrame`** each frame.
   Validates the WebGPU polyfill + host + real setup + real JS `renderFrame`.
2. **Milestone 2 — native render loop:** `renderFrame` (+ transitive frame-graph/
   render-task per-frame execution) becomes native, walking the real Lite structures
   registered at setup. Zero JS per frame on real Lite.

The Steps 1–18 below (native factories, multi-light, PBR, animation, shadows, bundler
externalization, Model B) were the EXPLORATION that led here; their renderer/WGSL/
material/scene-graph code is now superseded by real Lite and slated for deletion. The
reusable survivors are the WebGPU N-API polyfill and the Dawn surface/loop scaffolding.

### Real-Lite progress log
- **rl-host-webgpu DONE** — `navigator.gpu` + the full `GPUDevice` command surface real
  Lite uses, in `Polyfills/WebGPU`. Added: `navigator.gpu.requestAdapter()`
  (Promise<GPUAdapter>) / `getPreferredCanvasFormat()`; `GPUAdapter` (`.features` setlike,
  `requestDevice()` Promise<GPUDevice>); `GPUDevice.features`/`destroy`/
  `createCommandEncoder`; `GPUCommandEncoder.beginRenderPass`/`finish`;
  `GPURenderPassEncoder` (`setPipeline`/`setBindGroup`/`setVertexBuffer`/`setIndexBuffer`/
  `draw`/`drawIndexed`/`setViewport`/`setScissorRect`/`end`); `GPUCommandBuffer`;
  `GPUQueue.submit`. Dawn instance/adapter/device creation moved into the polyfill
  (`Module` owns the `wgpu::Instance` + HWND; `RequestAdapterSync`; promise-resolved
  device request). Verified via a JS smoke test: `requestAdapter → features.has →
  requestDevice → features.has → getPreferredCanvasFormat → createBuffer/writeBuffer/
  createCommandEncoder/finish/queue.submit` all succeed through `navigator.gpu`.
- **rl-host-canvas DONE** — `canvas` + `GPUCanvasContext` wired to the Dawn HWND surface.
  The `Module` now creates+owns the HWND `wgpu::Surface`; `ConfigureSurface`/`Present`
  drive it. Added an offscreen-style `canvas` (width/height/`getContext("webgpu")`/
  `setAttribute`, deliberately NO `clientWidth` so Lite's `isDomCanvas` is false →
  OffscreenCanvas path, no DOM layout) and `GPUCanvasContext`
  (`configure({device,format,alphaMode})` → `surface.Configure`; `getCurrentTexture()` →
  `surface.GetCurrentTexture` wrapped as GPUTexture; `unconfigure()`). NativeLite installs
  a global `canvas` + a minimal `document.getElementById(...)` returning it. Verified via
  JS smoke test: `document.getElementById → canvas.getContext("webgpu") → configure →
  getCurrentTexture → createView → beginRenderPass(clear) → end → finish → queue.submit`
  all succeed — the exact surface real Lite's `_buildSurface`/`_refreshScRT`/`renderFrame`
  use. On-screen present deferred to the loop driver (rl-boot-js-loop).
- **OPEN DECISION (loop driver) — RESOLVED: both native, no RAF/bridge.** Per user: native
  `startEngine` + native `renderFrame`; no RAF, no JS-renderFrame bridge. The earlier
  bridge/RAF question came from an invented intermediate "run JS renderFrame" step, now dropped.

### Native renderFrame scope (from render-task.ts / render-pass.ts / frame-graph.ts)
Real Lite's per-frame draw path (`executePass`) is heavily engineered: **GPU render bundles**
(`_opaqueBundles`), bucketed `DrawBinding` lists (opaque/direct/transparent), per-binding UBO
updates (`updateBindings`), scene-UBO pack, lights UBO, transparent depth-sort. So native
`renderFrame` (transitive) ≈ a **native reimplementation of the render-task execute path +
per-frame updates** — the bulk of the per-frame engine — reading the `Renderable`/`DrawBinding`
structures Lite's JS SETUP builds (which reference WebGPU pipelines/bind groups/buffers made via
our polyfill). The setup/per-frame line within the frame graph: `build()`/`record()`/
`pass._initialize()` = JS setup; `execute()`/`task.execute()`/`pass._execute()`/`updateBindings`/
UBO writes/draws = native per-frame.

### Refined sequencing
Before native renderFrame, real Lite's SETUP must run on our host so those structures exist.
That step also surfaces exactly which WebGPU APIs real Lite needs (e.g. render-bundle encoders) —
cheap setup-time validation, no loop. So: (1) bundle real Lite + a minimal scene, run its setup
on the host (navigator.gpu + canvas already done), filling polyfill gaps; (2) inspect the built
frame-graph/renderable structures; (3) implement native startEngine + renderFrame walking them.

### rl-bundle-lite + MILESTONE 1 DONE — real Lite boots on the native host
Consume the **PUBLISHED npm package `@babylonjs/lite@1.3.0`** (prebuilt ESM, ZERO npm deps) —
a normal dependency, NOT vendored TS source. (The source package is named `babylon-lite` and is
unpublished; the published build is renamed `@babylonjs/lite` — that's why an early `npm view
babylon-lite` 404'd and I briefly vendored TS. Switched to the published package: far cleaner.)
`bundler/build-lite.mjs` (esbuild) bundles a real-Lite scene to an IIFE ChakraCore runs: bundles
`@babylonjs/lite` from node_modules (dynamic imports inlined), targets **es2017** (legacy Chakra
can't parse `?.`/`??`), prepends `runtime/host-prelude.js` (pure-JS shims: TextEncoder/Decoder,
performance.now, queueMicrotask, BigInt passthrough), and an `onLoad` rewrites BigInt literals
(`32n`→`BigInt(32)`, in unused OpenType font-parser code esbuild can't lower / Chakra can't parse).
NativeLite also defines `globalThis` (→ global) natively.
**Result:** `minimal.ts` (createEngine→createSceneContext→registerScene→startEngine) runs REAL
Lite v0.1.0 on the host — logs `Babylon Lite v0.1.0 - WebGPU engine`, engine/scene/registerScene
all succeed via real `navigator.gpu` (requestAdapter/requestDevice) + real canvas. Stops EXACTLY
at `startEngine → requestAnimationFrame` — the JS loop driver we replace with native
startEngine/renderFrame. Confirms the boundary precisely: all SETUP is real Lite on our native
WebGPU; only the loop is native. (Polyfill gaps surfaced incrementally per scene.)

### NEXT (rl-native-loop): native startEngine + renderFrame
Implement native `startEngine` (drive from the native pump, no RAF) + native `renderFrame`,
reading real Lite's live JS structures (engine.surfaces, surface._renderingContexts, scene
frame graph / render-task `_opaque/_direct/_transparentBindings`, DrawBinding pipeline/bind
groups/buffers — all GPU-wrapper objects from our polyfill) via N-API property reads (no JS
execution), and issuing the GPU work natively. Stub `requestAnimationFrame` to a no-op (or omit)
so real Lite's `startEngine` doesn't throw; the NATIVE startEngine drives frames instead. Grow
minimal.ts to a drawable scene (mesh+material+camera+light) once the loop draws.

### EXACT STRUCTURE MAP (from live introspection of the PUBLISHED build — names confirmed present)
Mechanism for native startEngine: the bundler externalizes the scene's
`import { startEngine, stopEngine } from "@babylonjs/lite"` to the native global
(`BabylonNativeLite.startEngine`), so real Lite's JS startEngine (which uses RAF) never runs.
Everything else stays real Lite. Native startEngine receives the REAL Lite engine PLAIN object.

engine (== engine.surfaces[0], the primary surface; plain Object, 26 keys):
  ._device : GPUDevice (our wrapper → Unwrap<webgpu::Device> → wgpu::Device; .GetQueue() for queue)
  .surfaces : array[1];  ._renderingContexts : array[1] (the scene)
  ._currentEncoder (undefined→native sets), ._currentDelta (number→native sets),
  ._cbs (array; native sets [0]=finish()), .drawCallCount (native sets)
  .scRT : swapchain RenderTarget;  ._context : GPUCanvasContext (our wrapper);  .canvas, .format
scene (== surface._renderingContexts[0]; plain Object, 29 keys):
  ._update : function, ._record : function  (native must NOT call — do their work natively)
  ._frameGraph : Object (direct field — no need for getFrameGraph())
  .clearColor : {r,g,b,a};  ._renderables : array;  ._renderableVersion : number;  ._built : bool
  .camera, .lights, .meshes, ._drawCallsPre
scene._frameGraph (5 keys): ._tasks : array[1];  .build/.execute/.dispose : function
task (_frameGraph._tasks[0]; 28 keys):
  ._passes : array — EMPTY until built (build/record is lazy, structural → can stay JS, NOT per-frame)
  ._opaqueBindings/_directBindings/_transparentBindings : DrawBinding[] (draws)
  ._renderPassDescriptor, ._colorAttachment, ._sceneUBO:GPUBuffer, ._sceneBG:GPUBindGroup,
  ._lightsUBO:GPUBuffer, ._config:{rt,depth,clr,clrColor}, .record/.execute : function
engine.scRT (8 keys): ._colorTexture:GPUTexture, ._colorView:GPUTextureView (refreshed/frame),
  ._depthTexture/_depthView (null for the swapchain RT), ._descriptor, ._eager
KEY: task._passes empty after registerScene ⇒ frame-graph build()/record() (pass construction) is
LAZY structural setup, not per-frame ⇒ may stay JS (call once at startEngine, or when
_renderableVersion changes). The per-frame execute()/draws path is what native owns.

Native renderFrame(engine, delta) plan (mirrors JS renderFrame, all reads, no JS calls):
  1. device = unwrap(engine._device); 2. (first frame / on _renderableVersion change) call JS
  frameGraph.build() once to construct passes [allowed: structural, not per-frame];
  3. acquire swapchain: mod->Surface().GetCurrentTexture() → view; update scRT._colorView;
  4. encoder = device.createCommandEncoder; 5. for each task/pass: read render target color view
  + clearColor + depth; beginRenderPass natively; iterate DrawBinding lists (read pipeline/bind
  groups/vertex+index buffers from the GPU-wrappers) → setPipeline/setBindGroup/setVertexBuffer/
  setIndexBuffer/drawIndexed; end; 6. queue.submit([encoder.finish()]); 7. mod->Present().
  INCREMENT 1 DONE (loop+present validated): externalize startEngine/stopEngine to native
  (build-lite.mjs overrideLoopExports → BabylonNativeLite.*); native startEngine stores the REAL
  Lite engine object (Controller.realEngine) + drives RealPump (native pump, no RAF); RealRenderFrame
  reads engine._device + scene.clearColor via N-API, clears the swapchain, presents. On-screen: real
  Lite scene clears to its default clearColor ~(0.2,0.2,0.3) — native loop, zero JS/frame, reading
  real Lite's live JS data. (Controller.webgpu set in Initialize for Present; stopEngine/shutdown
  handle the real path; old Engine-wrapper path kept behind an InstanceOf branch until rl-cleanup.)
  INCREMENT 2 (next): read frame-graph passes + draw bindings → draw meshes. Needs DrawBinding shape
  (render/renderable.ts): pipeline/bindGroups/vertex+index buffers/counts. Grow minimal.ts to a
  mesh+material+camera+light scene. INCREMENT 3: per-frame _update (scene/lights UBO, world matrices).

### KEY: data may live in JS; native READS it (no JS execution)
Reading a JS object's properties from native via N-API (napi_get_property, typed-array data
pointers, GPU-wrapper unwraps) does NOT execute JS. So the scene/mesh/renderable/draw-binding/
frame-graph DATA stays as the JS objects real Lite's setup builds; native `renderFrame` READS
them each frame and issues GPU work — no per-frame JS, and no need to mirror data into native
structs. The only rule: don't CALL JS functions per frame (native does the work `pass._executeFunc`/
`renderable.update`/etc. would have done, reading the same data). This makes native renderFrame a
reader/walker of Lite's live JS structures rather than a parallel native scene database.

## Decision & goal
Port Babylon Lite's per-frame render loop to native C++ on **Dawn (native WebGPU)**,
not bgfx. Lite is WebGPU-exclusive and emits WGSL; Dawn keeps WGSL + pixel parity and
makes the port mechanical. **Goal: zero Lite JS executes per frame.** Scene setup,
asset loading, and sparse user callbacks stay in JS and call into native.

Cedric's `webgpu-cross-platform-app` is reference-only (it runs Lite JS *in* the loop
over a webgpu.h passthrough — the opposite goal). We reuse its ideas where handy
(Dawn surface creation per-platform, JS-engine tradeoffs) but build our own loop.

## The port seam (grounded in Lite source)
`engine.ts::renderFrame(engine, delta)` is the entire hot path and has a clean split:

```
renderFrame:
  encoder = device.createCommandEncoder()
  for each surface:
    refresh swapchain RT
    for each renderingContext (a frame-graph instance):
      ctx._update()      // CPU: transforms, UBOs, animation, culling, skinning
      ctx._record()      // GPU: encode render passes + draw calls
  device.queue.submit([encoder.finish()])
```
Plus `scene-core.ts` runs a list of per-frame "updatables" (`u.update(eng)`) registered
by factories/loaders at `startEngine()`. **Native owns `_update()`, `_record()`, the
updatables list, and submit.** This `update`/`record` boundary is mirrored 1:1 in C++.

## Integrating into BN's existing structure (analyzed; DEFERRED by decision)
The exploration currently lives in a standalone `Lite/` tree (own CMake, own App host).
The end-state should fold into BN's existing `Polyfills/` + `Plugins/`, reusing AppRuntime
and the JS polyfills — but integration is **deferred until more Lite subsystems are ported**
(revisit later). Findings from auditing the coupling (so we don't re-derive them):

REUSABLE as-is (graphics-independent):
- `AppRuntime` (JsRuntimeHost) — already reused; pure JS thread + N-API.
- JsRuntimeHost polyfills — Console, Scheduling, URL, XHR/Fetch, TextDecoder/Encoder, Blob,
  File, Performance. (Fetch/XHR wanted later for glTF loading.)

NOT reusable as-is (bgfx-coupled — verified via target_link_libraries):
- `Polyfills/Window` — links `GraphicsDeviceContext` (uses it for devicePixelRatio). Provides
  `requestAnimationFrame`/`window`, which native Lite wants, so it'd need decoupling or a
  Dawn-backed devicePixelRatio path.
- `Polyfills/Canvas` — bgfx + nanovg.
- `Embedding` — `View.h` is built around `Babylon::Graphics::WindowT`; links `GraphicsDevice`.
  An app-only path can skip it (we wrote our own `App/` host).
- `Core/Graphics` — IS bgfx; our `Renderer` is the Dawn replacement (a Core-level lib, NOT a
  polyfill/plugin).

Target structure when we integrate:
- `Polyfills/WebGPU/` (gated `BABYLON_NATIVE_POLYFILL_WEBGPU`) — the WebGPU browser-API polyfill.
- `Plugins/NativeLite/` (gated `BABYLON_NATIVE_PLUGIN_NATIVELITE`) — the Lite engine plugin.
- `Renderer` → a `Core/`-level Dawn engine lib that `Plugins/NativeLite` depends on.
- Disable the Babylon.js plugins via existing `BABYLON_NATIVE_PLUGIN_*=OFF` (NativeEngine,
  ShaderCompiler/Cache/Tool, NativeCamera/Input/Xr, ...). The gating mechanism already exists
  (each plugin/polyfill is an `if()`-gated `add_subdirectory`).
- BIGGEST WORK ITEM: BN assumes bgfx is always present — root `CMakeLists.txt` does
  `add_subdirectory(Core)` unconditionally and `Dependencies` always pulls bgfx/glslang/
  SPIRV-Cross. Integration needs a graphics-backend switch (e.g. `BABYLON_NATIVE_GRAPHICS=
  Dawn|bgfx`) gating Core/Graphics + bgfx deps + bgfx plugins vs Dawn + Renderer + the new
  polyfill/plugin. That root-CMake refactor is why integration is deferred.

## Module layout (standalone exploration tree — `Lite/`)
- `Lite/Renderer/` — native engine core (Dawn). Ports Lite subsystems to C++/Dawn, mirroring
  Lite's data-oriented structs and the `update()`/`record()` split. **No JS dependency** (so
  it can be built/tested headless). The Dawn equivalent of `Core/Graphics`.
- `Lite/Polyfills/WebGPU/` — **Dawn exposed to JS as WebGPU** (`navigator.gpu` + `GPU*`).
  WebGPU is a browser API, so it belongs in `Polyfills/` (like `Polyfills/Window`), NOT
  `Plugins/`. Implemented as N-API `ObjectWrap` classes (GPUDevice/GPUQueue/GPUBuffer/...)
  that each own a refcounted Dawn handle. Required so stay-in-JS code and user-custom JS
  RenderingContexts/materials keep working. Cedric's `wgpu_bridge` is a reference (ours is
  class-based).
- `Lite/Plugins/NativeLite/` — N-API surface + engine handles. Installs global
  `BabylonNativeLite` and defines the `Engine`/`Mesh` (and future Material/Camera/...)
  `ObjectWrap` classes that mirror Lite's `index.ts` factories. Bridges JS setup → native.
- `Lite/App/` — Win32 host (our own, in lieu of bgfx-coupled `Embedding`).
- Dawn added via FetchContent, trimmed per-platform backend (D3D12/Metal/Vulkan). Surface
  from native window handle.
- REUSE from BabylonNative: `JsRuntimeHost` (JS engine, N-API shim, event loop, AppRuntime,
  Console/...), `arcana.cpp`. (NOT `Embedding`/`Polyfills/Window` — bgfx-coupled, see above.)
- DROP for native Lite: `Core/Graphics` (bgfx), `Plugins/NativeEngine`,
  `ShaderCompiler`, `ShaderCache` (all bgfx/GLSL — unneeded; Lite ships WGSL to Tint).

## JS / native boundary
- **Native (per-frame, ~45k LOC TS → C++ core):** engine(2.3k), frame-graph(3.5k),
  material(19.6k incl WGSL composition), mesh(5.7k), camera(2.3k), animation(1.9k),
  shadow(2.0k), light(0.9k), texture(1.4k), skeleton(0.9k), morph, render, resource,
  scene, math, effect, shader. Later/optional native: sprite(5.2k), text(2.4k),
  post-process(1.5k), vat, large-world (these record GPU work, so eventually native or
  register native renderables).
- **Stays JS:** loaders (gltf 5k, env, hdr, babylon, skybox, splat), picking,
  navigation, physics, gizmo, app logic, user per-frame callbacks. These build/mutate
  the scene by calling native factories; they never touch the GPU directly.

## Expose Dawn AS WebGPU via N-API (required — not optional)
Lite's public surface genuinely leaks WebGPU: `engine._device` is read all over by code
that STAYS JS (`effect-renderer`, `uniform-effect-renderer`, `device-lost-recovery`:
`createBuffer/createShaderModule/createBindGroup/queue.writeBuffer/...`), and
`RenderingContext` is a **public, user-implementable interface** (`registerRenderingContext`)
— Lite's own `EffectRenderer` implements it, and user custom contexts/materials will too.
Replacing these with opaque handles would break all of it.

Therefore the native layer **must implement WebGPU over Dawn and expose it through N-API**:
`navigator.gpu`, `GPUDevice`, `GPUBuffer`, `GPUTexture`, `GPUSampler`, `GPUCommandEncoder`,
`GPURenderPassEncoder`, etc. (this is exactly Cedric's `wgpu_bridge`, which becomes
genuinely reusable here). The unifying invariant:

> The N-API `GPU*` wrappers and the native C++ render loop wrap the **same** refcounted
> Dawn objects. `engine._device` is a real wrapped `wgpu::Device`; a JS custom context
> records into a `GPUCommandEncoder` that wraps the **same** `wgpu::CommandEncoder` the
> native loop created that frame. Dawn handles are refcounted, so dual-wrapping is safe.

Consequences:
- Ported built-in subsystems talk to Dawn in **C++ directly** → zero JS/frame.
- Stay-in-JS code and **user-custom JS RenderingContexts/materials** talk to the same Dawn
  via N-API WebGPU. A custom JS context pays JS + per-WebGPU-call N-API cost each frame —
  **opt-in, only for the custom part** (matches the original "per-frame user hooks are OK").
- Opaque native handles (`Napi::External`/ObjectWrap) are reserved for **engine concepts
  with no WebGPU equivalent** (Mesh, Material, Camera, Light, Scene), NOT for GPU primitives.

## N-API surface (`BabylonNativeLite`)
- Two handle kinds: (a) **real WebGPU wrappers** over Dawn (above) for anything that is a
  GPU object; (b) **engine handles** for ported concepts (mesh/material/camera/...).
- Setup + sparse hooks hit the surface, never the ported hot path, so ergonomics > micro-perf.
  Keep `onBeforeRender`/task callbacks a direct same-thread call so no-hook frames cost 0 JS.
- The ~675-line `index.ts` export list defines the surface; generate bindings from a
  manifest where possible to avoid hand-maintaining hundreds of factories.

## Threading
Default: run the native render loop **on the JS (dedicated, non-UI) thread** via a native
rAF pump. This mirrors the browser model — the browser runs `requestAnimationFrame` on the
main thread, records commands, calls `queue.submit` (cheap; just enqueues), and returns
while the GPU executes asynchronously. Because BN's JS thread is dedicated (not the OS UI
thread), blocking it with native C++ recording is acceptable: nothing else needs it
mid-frame, and user hooks run inline with **no marshaling** (preserves Babylon's
single-threaded scene model). Zero JS runs per frame when no hooks are registered.
- **Risk:** JS-engine GC pauses can stall a frame.
- **Alternative (deferred):** a dedicated C++ render thread owning the loop, marshaling to
  the JS thread only for user hooks. Decouples GC from frame pacing but breaks the
  single-thread scene assumption (hooks mutate scene state) and adds sync cost. Revisit only
  if GC-induced jank shows up in P5+ profiling.

## WGSL / material strategy
Lite composes WGSL strings per material permutation at runtime, then hands WGSL to the
browser. Port that string-composition generator to C++ (mechanical, large) and feed WGSL
straight to Dawn/Tint — **no shader translation, no SPIR-V, parity preserved**. Cache
compiled pipelines by permutation key (native pipeline/bind-group cache replaces Lite's).

## Bundler externalization (explicit manifest, not automatic)
There is **no automatic fallback** — routing is per-export and explicit. A **generated
barrel/shim** re-exports each name from either native (`globalThis.BabylonNativeLite`) or
the real `@babylonjs/lite`, driven by a **manifest** listing which exports are
native-backed. Exports absent from the manifest re-export from the real TS module — that
is all "fallback" means. esbuild/rollup marks `@babylonjs/lite` external and aliases it to
this shim, so app + stay-in-JS loaders keep `import { createX } from "@babylonjs/lite"`
but resolve to native-or-TS per the manifest.

**Constraint (important):** you cannot split arbitrarily. Every object that crosses between
a JS-stay function and a native function must be compatible — i.e. a native engine handle
or a real (N-API) WebGPU object. So each phase must pick a **consistent cut**: a subsystem
boundary where all objects passed across are already in the shared handle/WebGPU vocabulary.
Incremental coverage advances cut-by-cut, not function-by-function in isolation.

## Parity & bench harness
- Pixel-diff harness modeled on BN Playground validation (reference images, threshold/
  errorRatio). Validate each ported subsystem against browser Lite per-scene.
- Reuse Lite's scene bundles (125 scenes) as the regression corpus; `scene200` as the
  perf benchmark to prove the native loop beats JS-in-loop (the project's core thesis).

## Phased roadmap (each phase = a runnable vertical slice + parity gate)
- **P0 Foundation:** Dawn in CMake; cross-platform device/surface/swapchain; native rAF
  pump driven by JsRuntimeHost; **WebGPU-over-N-API layer** (`navigator.gpu` + `GPU*`)
  wrapping the same Dawn device; clear-screen + hello-triangle from the native loop. No
  Lite yet, but the JS-visible WebGPU device is real from day one.
  - **Step 1 DONE** — standalone `Lite/` builds Dawn (pinned Chrome 146) via FetchContent
    and runs a native clear-screen loop on Win32+D3D12 (verified: RTX 4070, BGRA8Unorm).
    Learnings baked into `Lite/`: (a) `GIT_SUBMODULES ""` on the Dawn FetchContent is
    required — default recursive submodule init pulls angle→VK-GL-CTS and hangs;
    `DAWN_FETCH_DEPENDENCIES` provides the real trimmed deps. (b) Ship `d3dcompiler_47.dll`
    next to the exe (post-build copy) — Dawn's FXC loader rejects a path-less fallback with
    error 87. (c) `use_dxc` is force-disabled unless `DAWN_USE_BUILT_DXC`, so FXC is the
    active path. (d) Disable Git Credential Manager for the configure (googlesource is
    anonymous; GCM stalls it).
  - **Step 2 DONE** — hello-triangle: WGSL shader compiled through Tint→HLSL→DXBC, render
    pipeline + draw verified on-screen (interpolated RGB triangle). Confirms the full WGSL
    pipeline path Lite depends on works end-to-end.
  - **Step 3 DONE** — render loop driven from the JS thread's event loop. JsRuntimeHost
    AppRuntime (Chakra) added via FetchContent (pinned to the root BN commit); Chakra is
    zero-download on Windows (system Chakra.dll + SDK Chakrart.lib). The JS thread owns the
    Chakra engine + all Dawn objects; a native callback renders one frame then
    re-dispatches itself via `AppRuntime::Dispatch` (the JS thread's event loop) → zero JS
    per frame. Learnings: ScriptLoader hard-depends on UrlLib (only fetched with the network
    polyfills) — disable it and use `napi_run_script` (BN's variant takes a `source_url`
    arg); link `AppRuntime` + `Console` only.
  - **Step 4 DONE** — JS triggers the native loop. Added a `BabylonNativeLite` N-API global
    with `createEngine()` / `startEngine()` / `stopEngine()`. Startup JS runs the real
    contract (`const e = createEngine(); startEngine(e);`) and the native render loop starts
    in response → flips the loop trigger from C++ main() to JS (mirrors Lite engine.ts).
  - **Step 5 DONE** — Dawn exposed to JS as real WebGPU (the plan's core invariant). The
    `engine` object now carries `_device` — a wrapped `wgpu::Device` → `queue` →
    `writeBuffer` over the SAME refcounted Dawn objects the native loop uses (GPUBuffer is a
    `Napi::External` pointing at the wgpu::Buffer in RenderState). Added a per-frame JS hook
    (`engine.setBeforeRender(fn)`), called as a DIRECT same-thread Napi call inside the pump
    (no marshaling, since the loop is a JS-thread task) — the allowed "user per-frame
    callback" path; zero JS/frame when no hook is set. Demo: the native pipeline samples a
    `tint` uniform; the JS hook animates it via `device.queue.writeBuffer`, and the
    natively-rendered triangle's apex red channel sweeps in lockstep with the JS `sin()`
    (verified by pixel sampling 123→97→68→40→20→6). Proves JS WebGPU calls hit the same Dawn
    objects the native loop renders with — so stay-in-JS code and user custom contexts can
    coexist with the native hot path.
  - **Step 6 DONE** — refactor to `ObjectWrap` classes + module split (BN conventions).
    Replaced the plain-`Napi::Object` scaffolding with real `Napi::ObjectWrap` classes
    (`DefineClass` + `InstanceMethod`/`InstanceAccessor`), each owning its refcounted Dawn
    handle with proper lifetime. Split the monolithic `Main.cpp` into modules: `Renderer/`
    (pure native engine core, JS-free), `Polyfills/WebGPU/` (GPUDevice/GPUQueue/GPUBuffer
    ObjectWrap classes — WebGPU is a browser API so it lives in Polyfills, NOT Plugins),
    `Plugins/NativeLite/` (the `Engine` ObjectWrap + `BabylonNativeLite` surface), `App/`
    (Win32 host). Class constructors stored as module-owned `FunctionReference`s with
    `SuppressDestruct()` — Chakra's N-API shim does NOT implement `napi_set_instance_data`
    (only V8 does), so env instance data is unavailable. Same JS-driven tint pulse verified
    through the ObjectWrap path (apex red 116→148→177→199→213→219→202→184→156).
  - **Step 7 DONE** — first engine-concept factory: `Mesh` handle + `createMesh()`. The
    Renderer gained a real mesh list (each mesh = a vertex buffer of interleaved pos.xy +
    color.rgb, drawn through a vertex-buffer pipeline, with a per-mesh transform uniform).
    `BabylonNativeLite.createMesh(engine, positionsF32, colorsF32)` is the native factory:
    JS passes Float32Arrays, native builds the GPU buffers, returns a `Mesh` ObjectWrap.
  - **Step 8 DONE** — 3D scene: `Camera` handle + indexed 3D meshes + depth. Renderer
    rewritten with a column-major mat4 math kit (perspective RH/ZO, look-at, model = T*R*S),
    a depth texture (Depth24Plus, recreated on resize), and a two-bind-group pipeline (group
    0 = shared camera viewProj, group 1 = per-mesh model). `createMesh` now takes 3D
    positions + Uint16 indices (DrawIndexed). New `createCamera()` → `Camera` ObjectWrap with
    native `setProjection(fovY,aspect,near,far)` + `setView(eye,target,up)` (matrices computed
    in C++). `Mesh.setTransform(pos,rot,scale)` computes the model matrix natively. Demo: 3
    spinning perspective-projected cubes with correct depth occlusion — verified on-screen
    (perspective foreshortening + per-face gradients; left cube silhouette pixel-count varies
    639→765→680 confirming the spin). Both handle kinds now real: engine concepts
    (Engine/Mesh/Camera) + WebGPU wrappers (GPUDevice/Queue/Buffer).
  - **Step 9 DONE** — Material subsystem + WGSL composition + pipeline cache (the parity-
    critical long pole, in miniature). New `ShaderComposer.cpp` is the native analogue of
    Lite's runtime shader-string composition: `ComposeMeshWgsl(features)` assembles vertex +
    fragment WGSL from fragments selected by a material feature permutation (bits:
    VertexColor, Lighting), handed straight to Tint (no translation). `Renderer` gained a
    `Material` (feature bits + baseColor uniform), a **pipeline cache** keyed by the
    permutation (`unordered_map<features, RenderPipeline>` — identical materials reuse the
    compiled pipeline, exactly like Lite), 3 bind groups (frame=camera+light / model /
    material), per-face normals, and a directional light. New N-API: `Material` ObjectWrap +
    `createMaterial(engine,{vertexColor,lighting,color})`, `setLight(engine,dir,color,int)`;
    `createMesh` now takes positions+normals+colors+indices+material. Demo: 3 cubes, 3
    permutations (0x1 unlit+VC, 0x2 lit+solid, 0x3 lit+VC) — console shows all 3 composed +
    cached; on-screen the unlit cube is flat while the lit cubes show directional shading,
    proving the composed fragment shaders genuinely differ. This establishes the pattern the
    full material/WGSL-composition port scales from.
  - **Step 10 DONE** — textures + samplers. Added a `Texture` feature bit + `Texture`/`Sampler`
    to the system. `ComposeMeshWgsl` now emits `texture_2d` + `sampler` bindings and
    `textureSample(...)` when the bit is set; the vertex format gained UVs (now pos+normal+uv+
    color = 11 floats, uniform across permutations). Renderer: `AddTexture(pixelsRGBA,w,h)`
    creates an RGBA8 texture and uploads via `queue.WriteTexture`; a shared linear/repeat
    sampler; a SECOND material bind-group layout + pipeline layout for the textured case
    (color uniform + texture + sampler at group 2). N-API: `Texture` ObjectWrap +
    `createTexture(engine,w,h,pixelsU8)`; `createMaterial` accepts `{texture}`; `createMesh`
    now takes uvs. Demo: 4 cubes / 4 permutations (0x1,0x2,0x3, and 0x6 = lit+texture) — a
    JS-generated checkerboard uploaded natively and sampled with lighting, verified on-screen.
    Exercises more of the WebGPU surface (textures, samplers, WriteTexture) and shows the
    composer scaling: a new feature = a fragment + a bit + a binding, not a new shader.
  - **Step 11 DONE** — scene graph + native world-matrix update pass (the world-matrix/scene
    subsystem; realizes the plan's update()/record() split). Added a `Node` (local TRS +
    optional parent + computed world matrix); meshes now attach to a node instead of owning a
    transform directly. `Renderer::AddNode(parent)`, `SetNodeTransform` (sets LOCAL), and a
    per-frame `UpdateWorldMatrices()` that walks nodes in creation order (parents first),
    computes `world = parent.world * local`, and uploads each mesh's node world to its model
    buffer — called at the top of RenderFrame BEFORE the encode pass (explicit update→record).
    N-API: `Node` ObjectWrap + `createNode(engine[, parent])` with `setTransform`; mesh
    transform moved off `Mesh` onto `Node`; `createMesh` now takes a node. Demo: a root node
    spins on Y; 4 cubes are CHILD nodes (fixed offsets + local spin), so the whole row tilts/
    orbits as one group while each cube spins — verified across two frames (group diagonal
    flips; cubes show different faces). JS sets only LOCAL transforms; native composes worlds.
    Engine handle surface now: Engine/Mesh/Material/Texture/Node/Camera.
  - **Step 12 DONE** — multiple lights + Blinn-Phong specular (the light/lights-UBO
    subsystem). Replaced the single hard-coded directional light with a light list (up to
    `kMaxLights=4`) supporting both directional and point lights. The frame UBO grew from
    96→224 bytes: viewProj(64) + cameraPos(16) + lightCount(16) + `lights[4]` each 32 bytes
    (`posType:vec4` where w=type 0/1, `colorIntensity:vec4` where w=intensity). `ComposeMeshWgsl`
    now emits a `Light` struct + per-fragment loop over `frame.lightCount.x`: directional vs
    point selected by `posType.w` (point lights apply inverse-square attenuation), Lambert
    diffuse + Blinn-Phong specular (half-vector, using `frame.cameraPos`), plus ambient; the
    vertex stage now also outputs `worldPos` when lighting is on. Renderer: `m_cameraPos`
    captured in `SetCameraView`, `m_lights` vector, `ClearLights()` / `AddLight(type,vec,color,
    intensity)`, and `UploadFrameUniform()` packs the 56-float layout. N-API: replaced `setLight`
    with `addLight(engine,{type,direction|position,color,intensity})` + `clearLights(engine)`.
    Demo: one warm directional + one blue point light (intensity 6) over the 4 parented cubes —
    verified on-screen (warm directional shading, bright specular highlights on the glossy cubes,
    blue point-light tint, group still orbiting). All 4 material permutations recompose/cache
    cleanly through Tint, proving the new multi-light WGSL is valid.
  - **Step 13 DONE** — PBR metallic-roughness material model (Cook-Torrance BRDF). Added a
    `MaterialFeature_PBR` bit (implies Lighting). The material uniform grew 16→32 bytes:
    `vec4 baseColor` + `vec4 params` (x=metallic, y=roughness). `ComposeMeshWgsl` now branches
    the per-light shading: PBR materials use a Cook-Torrance BRDF (GGX/Trowbridge-Reitz normal
    distribution, Smith-Schlick geometry, Schlick Fresnel with `F0 = mix(0.04, albedo, metallic)`)
    while non-PBR lit materials keep the Blinn-Phong path — both share the same multi-light loop
    + attenuation. Ambient adds a flat F0-tinted environment-reflectance term so metals show their
    color without a full IBL probe (a pure metal is otherwise black under direct-only lighting).
    Renderer: `AddMaterial` gained `metallic`/`roughness` args and packs the 8-float uniform;
    PBR forces the Lighting bit. N-API: `createMaterial` parses `{ pbr, metallic, roughness }`.
    Demo: a smooth gold metal (metallic 1.0, roughness 0.25) + a rough red dielectric
    (metallic 0, roughness 0.6) alongside the Blinn-Phong vertex-color cube and the textured cube —
    verified on-screen (gold shows warm metallic tint + tight highlights, red shows broad soft
    roughness response with the blue point light, all 4 distinct). Gold and red share ONE cached
    PBR pipeline (key `0xA`) but differ only in uniform params, confirming the cache keys on the
    feature permutation, not values.
  - **Step 14 DONE** — native keyframe animation system (the project's core thesis made
    concrete: animation with ZERO JS per frame). Restructured `Node` to keep its TRS
    *components* (position/rotation/scale) alongside the composed `local` matrix, so an
    animation can override a single channel and recompose. Added an `Animation` (target node,
    property = position/rotation/scale, time/value keyframes, duration, loop) and an
    `AdvanceAnimations(t)` step that runs at the top of `RenderFrame` (before the world-matrix
    update): it samples each animation at the renderer's own `std::chrono::steady_clock` time
    (lerp between bracketing keys, looped via `fmod`), writes the result into the node's channel,
    and recomposes `local`. Renderer: `m_animations`, `m_clockStart`, `AddAnimation(...)`. N-API:
    `createAnimation(engine, node, { property, keys:[{time,value}], duration?, loop? })`. Demo:
    REPLACED the per-frame `setBeforeRender` JS hook with native animations — the root Y-spins and
    each child tumbles, all advanced natively. Verified across two frames on-screen (the whole row
    rotated about the root's Y axis AND each cube turned to show different faces) with no JS
    executing per frame. The optional user `onBeforeRender` hook path remains available but is now
    unused by the demo, proving the loop is JS-free by default.
  - **Step 15 DONE** — shadow mapping (multi-pass rendering; part of P4). Added a depth-only
    shadow pass that renders the scene from the primary directional light's POV into a
    2048² `Depth32Float` shadow map, then the main pass samples it with a comparison sampler
    (3×3 PCF) to darken occluded fragments. Frame UBO grew 224→288 bytes (added `lightViewProj`
    mat4). Group 0 (frame) BGL gained two fragment bindings: `texture_depth_2d` + `sampler_comparison`.
    New `CreateShadowResources()` builds the shadow texture/sampler/uniform + a dedicated depth-only
    pipeline (`kShadowWgsl`, position-only vertex layout, reuses the model BGL at group 1).
    `ComputeLightMatrix()` builds an orthographic light view-proj (auto from the first directional
    light) each `UploadFrameUniform`. `MatOrtho` (RH, ZO depth) added. The composer's
    `shadowFactor(worldPos)` helper (uniform-control-flow PCF, masked for off-map fragments)
    multiplies the directional light's contribution in both the PBR and Blinn-Phong branches.
    Demo: added a ground plane + lifted the cube row; verified on-screen — four cubes cast distinct
    PCF-soft shadows onto the ground in the light's direction, with the blue point-light pool also
    visible. Single command encoder, two render passes (shadow → main).
  - **Step 16 DONE — BUNDLER EXTERNALIZATION** (the pivot from feature-dev to the loadable-scene
    goal). Built `Lite/bundler/`: an esbuild pipeline that rewrites a scene's
    `import { … } from "babylon-lite"` so the named bindings come from the native
    `BabylonNativeLite` global instead of bundling the TS engine — exactly the "externalize the
    Lite APIs onto a global" mechanism from the original plan. `externalize-babylon-lite.mjs`
    (esbuild plugin) resolves the `babylon-lite` specifier to a generated virtual module of
    `export const NAME = BabylonNativeLite.NAME;` bindings (tree-shaken per scene; names the host
    doesn't implement become guarded stubs that throw a precise "not implemented" only if called).
    `build.mjs` emits a self-contained ES2020 IIFE per scene in `scenes/` → `dist/<name>.bundle.js`
    (no module loader / DOM / Node builtins — the form ChakraCore runs directly). `scenes/
    spinning-cubes.js` is a real scene authored in the Lite import style using the host's current
    API. `App/Main.cpp` now loads `LITE_SCENE_JS` (a bundle path) at startup, falling back to the
    inline demo, and prints the JS exception text on failure. Verified end-to-end: bundled the
    scene, ran `LiteApp` with `LITE_SCENE_JS` pointing at it, and the scene rendered **natively**
    (cubes incl. gold/red PBR + cyan, ground plane, PCF shadows, point light, native animation) —
    pipelines composed/cached (0x3/0xA/0x2), zero engine JS per frame. `bundler/README.md` documents
    the mechanism plus the explicit mapping of upstream `scene1`'s functional API
    (`createSceneContext`/`addToScene`/`loadGltf`/`loadEnvironment`/`createDefaultCamera`/
    `attachControl`/`createHemisphericLight`/`registerScene`) — already externalized by the bundler;
    remaining items are native impls/adapters (glTF + IBL stay in JS / loaders-in-JS boundary).
  - **Step 17 DONE — JS-orchestration split + scene-context demo.** Refined the externalization to
    the correct boundary (render-loop vs not): the bundler now resolves setup-time orchestration
    (`createSceneContext`/`addToScene`/`createDefaultCamera`/`createHemisphericLight`/`registerScene`/
    `attachControl`/`onBeforeRender`/`loadGltf`/`loadEnvironment`) to a **pure-JS runtime**
    (`bundler/runtime/lite-runtime.mjs`) layered over the native primitives — these run once at setup,
    so they stay in JS. Added `scenes/scene-context-cubes.js` in the upstream `scene1` style;
    verified it renders natively. Only render-loop-critical names stay native.
  - **Step 18 DONE — MODEL B: WebGPU polyfill + JS-created GPU resources, native loop only.** The
    architectural pivot the user steered to: only the per-frame render loop must be native; ALL setup
    (engine bring-up aside) — material/mesh creation AND WGSL composition — can run in JS over a WebGPU
    N-API polyfill. Expanded `Polyfills/WebGPU` from {Device,Queue,Buffer} to the full resource surface:
    `GPUDevice.create{ShaderModule,Buffer,Texture,Sampler,BindGroupLayout,PipelineLayout,BindGroup,
    RenderPipeline}`, `GPUTexture.createView`, `GPUQueue.writeBuffer/writeTexture`, with WebGPU enum +
    descriptor parsing (a `Module::Current()` static + `As*` type-checked unwraps work around Chakra's
    missing `napi_set_instance_data`). Exposed `GPUBufferUsage`/`GPUTextureUsage`/`GPUShaderStage` global
    constants. Native Renderer became a **draw-list consumer**: `RegisterDrawable(pipeline, bindGroups,
    vb, ib, indexCount, node)` records JS-built drawables; per-node `modelBuffer` + the engine
    `frameBuffer` are native-owned (written each frame) but exposed to JS as `GPUBuffer`s so JS bind
    groups reference them; `engine.colorFormat`/`depthFormat` exposed for JS pipeline targets. The
    native loop draws external drawables alongside (animation/world-matrix/camera/lights still native).
    Built a **JS engine layer** (`bundler/runtime/lite-engine.mjs`) implementing `createMaterial`/
    `createMesh` + a WGSL composer (Blinn-Phong + metallic-roughness PBR + vertexColor) entirely in JS
    over the polyfill, registering drawables natively. The bundler now resolves `createMaterial`/
    `createMesh` to this JS layer (NOT native). Verified end-to-end: `spinning-cubes.bundle.js` renders
    natively with **no native pipeline-composition** (the `[renderer] composed + cached pipeline` logs
    are gone — JS built the pipelines via the polyfill), four correctly-shaded animating cubes + ground,
    zero engine JS per frame. This is the configuration that can eventually run unmodified upstream Lite
    code (which creates resources through WebGPU).
  - Remaining: grow the JS engine layer toward feature-parity (textures/shadows over the polyfill;
    move `createEngine`'s device/format setup into JS too, leaving only the HWND surface + loop native);
    implement scene1's `loadGltf` (JS) + `loadEnvironment`/IBL; eventual BN Polyfills/Plugins integration.
- **P1 Minimal scene:** port engine + surface + math + one unlit material + mesh (VB/IB) +
  ArcRotate camera + default render-task → swapchain. JS builds scene via `BabylonNativeLite`
  handles; native renders. Pixel-diff one static scene. Proves the whole boundary +
  handle/WebGPU vocabulary.
- **P2 Frame graph + standard material:** port frame-graph/render-task/pass, transforms/
  world-matrix, multi-mesh + thin instancing, standard material, directional light.
- **P3 PBR + textures + IBL:** PBR material + full WGSL composition, texture/RTT upload
  (JS decodes → native upload), HDR/env IBL, multiple lights, lights UBO.
- **P4 Animation & shadows:** animation eval, skeleton skinning, morph, shadow task/maps.
- **P5 Loaders & hooks integration:** glTF/GLB loader (JS) drives native factories end-to-
  end; user `onBeforeRender`/task callbacks; resize/device-lost recovery.
- **P6 Optional native:** sprite, text, post-process, gizmo, picking as native renderables.

## Key risks
1. **Scope** — porting ~45k LOC engine core is multi-quarter; phasing keeps it shippable.
2. **Upstream drift** — Lite is pre-1.0; the native port forks logic that must track
   upstream. Biggest long-term cost; pin a Lite version per phase, diff on upgrade.
3. **Dawn build/size** — FetchContent integration, per-platform surface, binary size
   (Cedric: 4.6–11 MB depending on JS engine, Dawn trimmed to one backend).
4. **Dual-wrapped Dawn objects** — the same `wgpu::Device`/encoder is wrapped both natively
   and via N-API WebGPU; lifetime/refcount and current-frame-encoder sharing between the
   native loop and JS custom contexts must be designed carefully (P0/P1).
5. **WebGPU-over-N-API completeness** — must cover enough of the WebGPU surface that
   stay-in-JS code (`effect-renderer`, `device-lost-recovery`) and user custom contexts run
   unmodified; under-coverage silently breaks those paths.
6. **WGSL composition fidelity** — the material generator is the largest, parity-critical
   port; gate it hard with pixel-diff in P3.
