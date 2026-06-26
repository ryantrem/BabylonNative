#pragma once

#include <cstdint>

// C++ bring-up of Babylon Native Lite for the iOS UIKit host. Mirrors the Win32 Main.cpp
// wiring (AppRuntime + Console/Fetch/Blob/URL polyfills + the BabylonNativeLite surface)
// but is driven by the iOS app lifecycle instead of a Win32 message pump. Rendering runs
// entirely on the JS thread via Lite's requestAnimationFrame pump, so the UIKit main thread
// only needs to own the CAMetalLayer and forward resize.
namespace lite::ios
{
    // Starts the JS engine and Lite over the given CAMetalLayer (passed as a void* so this
    // header stays Obj-C-free), loads the bundled scene script, and begins the JS-driven
    // render loop. widthPx/heightPx are the drawable size in physical pixels. Returns false
    // on a fatal setup error (logged to stderr / NSLog). Call once.
    bool StartLite(void* metalLayer, uint32_t widthPx, uint32_t heightPx);

    // Notify the engine the drawable resized (physical pixels). Safe before/after StartLite.
    void Resize(uint32_t widthPx, uint32_t heightPx);

    // Stops the render loop and releases Dawn + the JS engine. Safe to call multiple times.
    void StopLite();

    // Latest HUD figures for an optional on-screen overlay (FPS, full-frame ms, CPU ms).
    void GetHudStats(double& fps, double& frameMs, double& cpuMs);
}
