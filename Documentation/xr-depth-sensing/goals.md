# Goals: xr-depth-sensing

## Summary

Implement the WebXR Depth Sensing Module API (CPU-optimized path) in Babylon
Native, enabling AR applications to access real-world depth data from device
sensors on Android (ARCore) and iOS (ARKit/LiDAR).

Babylon.js has a `WebXRDepthSensing` feature class that calls standard WebXR
depth sensing APIs. In a browser, the browser implements these APIs. In Babylon
Native, the NativeXR plugin plays the role of the browser — we need to expose
the same WebXR surface, backed by platform abstractions in `XR.h` with
per-platform implementations using ARCore and ARKit.

## Target API Surface

Implement the W3C WebXR Depth Sensing Module
(https://www.w3.org/TR/webxr-depth-sensing-1/), specifically:

- **Feature descriptor**: `"depth-sensing"` for `requiredFeatures` /
  `optionalFeatures` in `requestSession()`
- **Session init**: `depthSensing: { usagePreference, dataFormatPreference }`
  in `XRSessionInit`
- **XRSession properties**: `depthUsage`, `depthDataFormat`
- **XRDepthInformation** base interface: `width`, `height`,
  `normDepthBufferFromNormView` (XRRigidTransform), `rawValueToMeters`
- **XRCPUDepthInformation**: `data` (ArrayBuffer), `getDepthInMeters(x, y)`
- **XRFrame.getDepthInformation(view)**: returns `XRCPUDepthInformation` or
  null

## Scope

### In scope

- CPU-optimized depth access path only (`"cpu-optimized"`)
- `"luminance-alpha"` data format (spec-required minimum; 16-bit uint packed
  in 2 bytes)
- Android (ARCore) platform backend — `ArFrame_acquireDepthImage16Bits()`
  provides uint16 depth in mm, maps directly to luminance-alpha
- iOS (ARKit) platform backend — `ARFrame.sceneDepth.depthMap` provides
  float32 depth in meters, requires conversion to uint16 luminance-alpha
- Platform abstraction structs in `Dependencies/xr/Include/XR.h`
- NAPI wrappers for `XRCPUDepthInformation` in `Plugins/NativeXr/Source/`
- Integration with `XRFrame` and `XRSession`
- Computation of `normDepthBufferFromNormView` transform from platform
  camera/depth sensor intrinsics
- Graceful null return when depth is unavailable (no LiDAR on iOS, etc.)
- Compatibility with Babylon.js `WebXRDepthSensing` feature class

### Out of scope (future work)

- GPU-optimized path (`XRWebGLDepthInformation` / `XRWebGLBinding.getDepthInformation()`)
- `"float32"` and `"unsigned-short"` data formats
- `XRDepthType` ("raw" / "smooth") — not used by Babylon.js
- `depthActive`, `pauseDepthSensing()`, `resumeDepthSensing()` — not used by
  Babylon.js
- Confidence map data (non-standard, not in WebXR spec)
- OpenXR desktop backend (no desktop XR in this project)

## Architecture Approach

Follow the established NativeXR feature pattern:

1. **XR.h** — Define `DepthSensingData` struct (width, height, buffer,
   rawValueToMeters, normDepthBufferFromNormView transform)
2. **Platform backends** — ARCore: call `ArFrame_acquireDepthImage16Bits()`,
   ARKit: read `sceneDepth.depthMap` and convert float32→uint16
3. **NAPI wrappers** — `XRCPUDepthInformation` class with `data`, `width`,
   `height`, `rawValueToMeters`, `normDepthBufferFromNormView`,
   `getDepthInMeters(x, y)`
4. **XRFrame** — Add `getDepthInformation(view)` method
5. **XRSession** — Accept `depthSensing` config, expose `depthUsage`,
   `depthDataFormat` properties

## Platform Considerations

- **ARCore**: Depth-from-motion available on most AR-capable Android devices;
  hardware ToF on some. `ArFrame_acquireDepthImage16Bits()` returns uint16 mm
  depth map. Must release `ArImage` after use.
- **ARKit**: Requires LiDAR scanner (iPhone Pro / iPad Pro). Enable via
  `.sceneDepth` in `ARWorldTrackingConfiguration.frameSemantics`.
  `depthMap` is a `CVPixelBuffer` with float32 meters per pixel (~256×192).
  Convert to uint16 mm for luminance-alpha format.
- Devices without depth capability: `getDepthInformation()` returns null.

## Relationship to Existing Code

- **Distinct from render pipeline depth**: The existing `DepthTexturePointer`,
  `DepthTextureFormat`, `SetDepthsNearFar()` in `XR.h` are for controlling
  the virtual camera's depth buffer — completely separate from real-world
  depth sensing.
- **Builds on existing patterns**: Same feature architecture as plane
  detection, hand tracking, mesh detection, etc.
- **XRWebGLBinding**: Exists as a stub. Not modified in v1 (GPU path deferred).

## Success Criteria

- Babylon.js `WebXRDepthSensing` feature works end-to-end with
  `usagePreference: ["cpu"]` and `dataFormatPreference: ["luminance-alpha"]`
- `frame.getDepthInformation(view)` returns valid `XRCPUDepthInformation`
  with correct depth data on both Android and iOS
- `getDepthInMeters(x, y)` returns correct depth in meters at normalized
  view coordinates
- Graceful degradation (null return) on devices without depth support
- No regression in existing XR features
