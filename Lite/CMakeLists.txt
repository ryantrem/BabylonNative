cmake_minimum_required(VERSION 3.21)

# Babylon Native Lite — standalone build for the P0 Dawn milestone.
#
# This is intentionally a self-contained CMake project (NOT yet wired into the
# root BabylonNative build) so the heavyweight Dawn dependency and its long first
# build don't disturb the existing bgfx-based tree. Later phases will fold this
# into the root once the Dawn integration is proven.

project(BabylonNativeLite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "The Lite P0 milestone currently targets Win32 + D3D12 only.")
endif()

include(FetchContent)

# --------------------------------------------------------------------------
# Dawn (native WebGPU). Trim every backend/feature except D3D12 to keep the
# build and binary small. These must be set BEFORE Dawn is added so they reach
# Dawn's own options.
# --------------------------------------------------------------------------

# Fetch Dawn's third-party dependencies via its own python script instead of
# requiring a full gclient/depot_tools checkout. Requires python3 on PATH.
set(DAWN_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)

# Backends: D3D12 only.
set(DAWN_ENABLE_D3D11    OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D12    ON  CACHE BOOL "" FORCE)
set(DAWN_ENABLE_VULKAN   OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_METAL    OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL     OFF CACHE BOOL "" FORCE)

# Drop tooling/samples/tests we don't need.
set(DAWN_BUILD_SAMPLES       OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_BENCHMARKS    OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_NODE_BINDINGS OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_INSTALL      OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_SWIFTSHADER  OFF CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW            OFF CACHE BOOL "" FORCE)
set(DAWN_USE_WINDOWS_UI      OFF CACHE BOOL "" FORCE)
set(DAWN_USE_BUILT_DXC       OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_SPIRV_VALIDATION OFF CACHE BOOL "" FORCE)

# Tint: D3D consumes HLSL, so only build the WGSL reader + HLSL writer.
set(TINT_BUILD_CMD_TOOLS   OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_DOCS        OFF CACHE BOOL "" FORCE)
set(TINT_ENABLE_INSTALL    OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_READER ON  CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_SPV_READER  OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_SPV_WRITER  OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_GLSL_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_GLSL_VALIDATOR OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_MSL_WRITER  OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_HLSL_WRITER ON  CACHE BOOL "" FORCE)

# Dawn pinned to the same commit Cedric Guillemet's webgpu-cross-platform-app
# validated (Chrome 146). Bump deliberately, not casually — Dawn's API drifts.
# GIT_SUBMODULES "" is critical: Dawn's .gitmodules lists only gclient/standalone
# tooling (buildtools, depot_tools, libc++, angle, ...) that the CMake +
# DAWN_FETCH_DEPENDENCIES path does NOT need. Without this, FetchContent's default
# recursive submodule init clones angle and its giant VK-GL-CTS conformance suite,
# which hangs the configure. The Python fetch script provides the real trimmed deps.
FetchContent_Declare(dawn
    GIT_REPOSITORY https://github.com/google/dawn.git
    GIT_TAG a44d7a3d78f23c680491c0fc04f53a1df62e02ff
    GIT_SUBMODULES ""
    GIT_SUBMODULES_RECURSE OFF)
FetchContent_MakeAvailable(dawn)

# --------------------------------------------------------------------------
# JsRuntimeHost — the JS engine + N-API host that owns the JS thread. We use it
# here to drive the native render loop from the JS thread's event loop (via
# AppRuntime::Dispatch), proving the JS engine and the native loop coexist with
# zero JS executing per frame.
#
# Engine: Chakra (the Windows default) — links the system Chakra.dll via the
# Windows SDK's Chakrart.lib, so no extra runtime download is required. Most
# polyfills are disabled to keep the build lean; the Fetch polyfill is ENABLED so
# real Lite's loadGltf/loadEnvironment can fetch assets over the network (it pulls
# UrlLib, WinHTTP-backed on Windows — asio/llhttp are only for the V8 inspector and
# are not pulled here). We also need Console.
# --------------------------------------------------------------------------
# Default to Chakra (Windows default, zero-download), but allow overriding from the
# command line (e.g. -DNAPI_JAVASCRIPT_ENGINE=V8) to benchmark a different JS engine.
# V8 on Windows is pulled from NuGet by JsRuntimeHost (v8-v143-<arch> + redist DLLs,
# auto-copied next to the exe via $<TARGET_RUNTIME_DLLS:LiteApp> in App/CMakeLists.txt).
set(NAPI_JAVASCRIPT_ENGINE "Chakra" CACHE STRING "JavaScript engine for Node-API (Chakra or V8)")
set(JSRUNTIMEHOST_TESTS OFF CACHE BOOL "" FORCE)
# ScriptLoader hard-depends on UrlLib (only fetched with the network polyfills),
# so disable it; we run the startup JS snippet via the core napi_run_script API.
set(JSRUNTIMEHOST_CORE_SCRIPTLOADER OFF CACHE BOOL "" FORCE)
set(JSRUNTIMEHOST_POLYFILL_XMLHTTPREQUEST OFF CACHE BOOL "" FORCE)
# Fetch: ON — provides globalThis.fetch for real Lite's glTF/env asset loading.
set(JSRUNTIMEHOST_POLYFILL_FETCH ON CACHE BOOL "" FORCE)
set(JSRUNTIMEHOST_POLYFILL_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(JSRUNTIMEHOST_POLYFILL_URL ON CACHE BOOL "" FORCE)
set(JSRUNTIMEHOST_POLYFILL_ABORT_CONTROLLER OFF CACHE BOOL "" FORCE)
# Blob: ON — real Lite's glTF loader wraps decoded image bytes in a Blob before handing
# them to createImageBitmap. JsRuntimeHost ships a native Blob polyfill (no extra deps).
set(JSRUNTIMEHOST_POLYFILL_BLOB ON CACHE BOOL "" FORCE)
set(JSRUNTIMEHOST_POLYFILL_FILE OFF CACHE BOOL "" FORCE)
set(JSRUNTIMEHOST_POLYFILL_PERFORMANCE OFF CACHE BOOL "" FORCE)
set(JSRUNTIMEHOST_POLYFILL_TEXTDECODER OFF CACHE BOOL "" FORCE)
set(JSRUNTIMEHOST_POLYFILL_TEXTENCODER OFF CACHE BOOL "" FORCE)

# Pinned to the same commit the root BabylonNative build uses (known-good combo).
FetchContent_Declare(JsRuntimeHost
    GIT_REPOSITORY https://github.com/BabylonJS/JsRuntimeHost.git
    GIT_TAG 272f6a9f3de78f7c4cd8a838ae9655c81fc4881a)
FetchContent_MakeAvailable(JsRuntimeHost)

# --------------------------------------------------------------------------
# Modules, following BabylonNative's conventions:
#   Renderer          — pure native Dawn engine core (no N-API, JS-free)
#   Polyfills/WebGPU  — the WebGPU browser-API polyfill: navigator.gpu / GPU*
#                       ObjectWrap classes over the same Dawn objects
#   Plugins/NativeLite— the Babylon Lite native engine plugin: Engine ObjectWrap
#                       + the BabylonNativeLite surface (createEngine/startEngine)
#   App               — the Win32 host executable
# --------------------------------------------------------------------------
add_subdirectory(Renderer)
add_subdirectory(Polyfills/WebGPU)
add_subdirectory(Plugins/NativeLite)
add_subdirectory(App)
