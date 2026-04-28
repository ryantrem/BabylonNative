# Requirements: xr-depth-sensing

## Overview

These requirements describe the changes needed to implement CPU-optimized
WebXR Depth Sensing in Babylon Native, following the established NativeXR
feature pattern. Each requirement maps to a specific layer of the
architecture.

---

## R1: Session Initialization — Accept Depth Sensing Configuration

**Layer**: `XRSession.cpp` (NAPI) + `NativeXrImpl`

The `XRSession::CreateAsync` method receives a feature object as `info[1]`.
This object must now support a `depthSensing` key.

### Input Format (from Babylon.js `getXRSessionInitExtension()`)

```js
{
  depthSensing: {
    usagePreference: ["cpu-optimized"],       // or ["cpu-optimized", "gpu-optimized"]
    dataFormatPreference: ["luminance-alpha"]  // or ["luminance-alpha", "float32"]
  }
}
```

### Behavior

1. In `CreateAsync`, check if `featureObject.Has("depthSensing")`.
2. If present, extract:
   - `usagePreference` — array of strings
   - `dataFormatPreference` — array of strings
3. Negotiate: select the first supported usage and format. For v1, only
   `"cpu-optimized"` and `"luminance-alpha"` are supported.
4. Store the negotiated usage and format on the session (as member variables
   or pass through to the platform layer).
5. Signal to the platform backend that depth sensing should be enabled.
6. If depth sensing is requested but no supported combination exists, depth
   sensing is silently disabled (returns null from `getDepthInformation`).

---

## R2: XRSession Properties — depthUsage, depthDataFormat

**Layer**: `XRSession.h/cpp` (NAPI)

Add two read-only accessors to the `XRSession` class:

| Property | Type | Value (v1) | Behavior |
|----------|------|------------|----------|
| `depthUsage` | string | `"cpu-optimized"` | Returns the negotiated depth usage |
| `depthDataFormat` | string | `"luminance-alpha"` | Returns the negotiated data format |

### Registration

Add to `XRSession::Initialize()`:
```cpp
InstanceAccessor("depthUsage", &XRSession::GetDepthUsage, nullptr),
InstanceAccessor("depthDataFormat", &XRSession::GetDepthDataFormat, nullptr),
```

### Behavior

- If depth sensing was not enabled on the session, accessing these should
  return `null` or `undefined` (matching browser behavior where Babylon.js
  checks `session.depthDataFormat == null`).

---

## R3: Platform Abstraction — Depth Sensing Data in XR.h

**Layer**: `Dependencies/xr/Include/XR.h`

Add a new struct to the `Frame` class for depth sensing data, alongside the
existing `View`, `Plane`, `Mesh`, etc.

### New Struct

```cpp
struct DepthSensingData
{
    uint32_t Width{0};
    uint32_t Height{0};

    // Raw depth buffer data. For "luminance-alpha" format, each pixel is
    // a uint16 value representing depth in platform-specific units.
    // Row-major order, no padding.
    std::vector<uint16_t> DepthBuffer{};

    // Scale factor to convert raw buffer values to meters.
    // For ARCore (mm): rawValueToMeters = 0.001f
    // For ARKit (after float→uint16 mm conversion): rawValueToMeters = 0.001f
    float RawValueToMeters{0.001f};

    // 4x4 column-major matrix that transforms normalized view coordinates
    // (origin top-left, X right, Y down, range [0,1]) to normalized depth
    // buffer coordinates (same convention, range [0,1]).
    std::array<float, 16> NormDepthBufferFromNormView{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
};
```

### Frame Integration

Add to `Frame`:

```cpp
// Per-view depth sensing data. Index corresponds to Views index.
// Empty if depth sensing is not enabled or data unavailable.
std::vector<DepthSensingData> DepthSensingViews;
```

### Session Integration

Add to `Session`:

```cpp
// Enable depth sensing for this session. Called during session creation.
void SetDepthSensingEnabled(bool enabled);

// Returns true if depth sensing is enabled and the platform supports it.
bool IsDepthSensingSupported() const;
```

---

## R4: XRCPUDepthInformation NAPI Class

**Layer**: `Plugins/NativeXr/Source/XRCPUDepthInformation.h` (new file)

Create a new NAPI wrapper class following the pattern of `XRPlane`,
`XRMesh`, etc.

### JS Interface

| Member | Type | Description |
|--------|------|-------------|
| `width` | unsigned long | Width of depth buffer (columns) |
| `height` | unsigned long | Height of depth buffer (rows) |
| `data` | ArrayBuffer | Raw depth data, row-major, no padding. For luminance-alpha: `Uint16Array` view, each entry is depth in raw units |
| `rawValueToMeters` | float | Multiply raw values by this to get meters |
| `normDepthBufferFromNormView` | XRRigidTransform | Transform from normalized view coords to normalized depth buffer coords |
| `getDepthInMeters(x, y)` | method → float | Returns depth in meters at normalized view coordinates (x, y) ∈ [0, 1] |

### Registration

```cpp
static void Initialize(Napi::Env env)
{
    DefineClass(env, JS_CLASS_NAME, {
        InstanceAccessor("width", &GetWidth, nullptr),
        InstanceAccessor("height", &GetHeight, nullptr),
        InstanceAccessor("data", &GetData, nullptr),
        InstanceAccessor("rawValueToMeters", &GetRawValueToMeters, nullptr),
        InstanceAccessor("normDepthBufferFromNormView", &GetNormDepthBufferFromNormView, nullptr),
        InstanceMethod("getDepthInMeters", &GetDepthInMeters),
    });
}
```

### Data Storage

- Store a copy of (or reference to) the `xr::DepthSensingData` from the
  current frame.
- The `data` accessor must return an `ArrayBuffer` containing the raw uint16
  depth values. For `"luminance-alpha"`, each pixel is 2 bytes
  (lo byte = value & 0xFF, hi byte = value >> 8).
- The `ArrayBuffer` should be created once and reused across frames if
  dimensions don't change, with contents updated each frame.

### getDepthInMeters(x, y) Algorithm

Per the WebXR spec:

1. Validate x ∈ [0, 1] and y ∈ [0, 1], throw `RangeError` if not.
2. Transform (x, y, 0, 1) by `normDepthBufferFromNormView` matrix.
3. Scale result by (width, height) to get buffer coordinates.
4. Truncate and clamp column to [0, width-1], row to [0, height-1].
5. Index into depth buffer: `index = row * width + column`.
6. Return `depthBuffer[index] * rawValueToMeters`.

---

## R5: XRFrame.getDepthInformation(view)

**Layer**: `Plugins/NativeXr/Source/XRFrame.h/cpp`

Add a new instance method to `XRFrame`.

### Registration

Add to `XRFrame::Initialize()`:
```cpp
InstanceMethod("getDepthInformation", &XRFrame::GetDepthInformation),
```

### Behavior

1. Accept one argument: an `XRView` object.
2. Determine the view index from the XRView (map to the corresponding
   `Frame::DepthSensingViews` entry).
3. If depth sensing is not enabled, or `DepthSensingViews` is empty, or the
   view index has no data, return `null`.
4. Otherwise, create/update an `XRCPUDepthInformation` object with the depth
   data for that view and return it.
5. Cache the `XRCPUDepthInformation` object per view index (like planes and
   meshes are cached) to avoid reallocating each frame.

### XRView Identification

The `XRView` objects are created in `XRViewerPose` with an index. We need a
way to map from an `XRView` JS object to its view index. Options:
- Store the view index on `XRView` (add a getter or internal field).
- Match by reference (iterate the views array).
The preferred approach is to add an internal `m_viewIdx` on `XRView` that
is set during `XRViewerPose::Update`.

---

## R6: ARCore Backend — Depth Data Acquisition

**Layer**: `Dependencies/xr/Source/ARCore/`

### Enabling Depth

- When `SetDepthSensingEnabled(true)` is called, configure the ARCore
  session to provide depth data. ARCore depth-from-motion is available on
  most AR-capable devices without special configuration — just call the
  acquisition function.
- Check `ArSession_isDepthModeSupported()` during session setup if needed.

### Per-Frame Depth Acquisition

In the frame update loop (where planes, meshes, etc. are already updated):

```cpp
ArImage* depth_image = nullptr;
ArStatus status = ArFrame_acquireDepthImage16Bits(ar_session, ar_frame, &depth_image);
if (status == AR_SUCCESS && depth_image != nullptr) {
    int width, height;
    ArImage_getWidth(ar_session, depth_image, &width);
    ArImage_getHeight(ar_session, depth_image, &height);

    const uint8_t* buffer;
    int row_stride;
    ArImage_getPlaneData(ar_session, depth_image, 0, &buffer, nullptr);
    ArImage_getPlaneRowStride(ar_session, depth_image, 0, &row_stride);

    // Copy to DepthSensingData
    auto& depthData = frame.DepthSensingViews[viewIndex];
    depthData.Width = width;
    depthData.Height = height;
    depthData.RawValueToMeters = 0.001f;  // mm → meters
    depthData.DepthBuffer.resize(width * height);

    const uint16_t* src = reinterpret_cast<const uint16_t*>(buffer);
    for (int r = 0; r < height; r++) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(
            buffer + r * row_stride);
        std::copy(row, row + width, depthData.DepthBuffer.data() + r * width);
    }

    ArImage_release(depth_image);
}
```

### normDepthBufferFromNormView Computation

The depth image from ARCore may have a different resolution and field of
view than the camera image. Use `ArFrame_transformCoordinates2d()` to map
from `AR_COORDINATES_2D_VIEW_NORMALIZED` to
`AR_COORDINATES_2D_TEXTURE_NORMALIZED` (the depth texture space), or compute
from camera intrinsics.

The transform is a 4×4 matrix. For many devices this is identity (depth
aligned with camera), but rotation and cropping differences can occur. The
platform implementation must compute and populate this correctly.

---

## R7: ARKit Backend — Depth Data Acquisition

**Layer**: `Dependencies/xr/Source/ARKit/`

### Enabling Depth

- When `SetDepthSensingEnabled(true)` is called, add `.sceneDepth` to
  `ARWorldTrackingConfiguration.frameSemantics`:

```objc
if ([ARWorldTrackingConfiguration supportsFrameSemantics:
        ARFrameSemanticSceneDepth]) {
    configuration.frameSemantics |= ARFrameSemanticSceneDepth;
}
```

- If the device doesn't support scene depth (no LiDAR), skip — depth data
  will simply not be available, and `getDepthInformation` returns null.

### Per-Frame Depth Acquisition

```objc
ARDepthData* sceneDepth = frame.sceneDepth;
if (sceneDepth != nil) {
    CVPixelBufferRef depthMap = sceneDepth.depthMap;
    CVPixelBufferLockBaseAddress(depthMap, kCVPixelBufferLock_ReadOnly);

    size_t width = CVPixelBufferGetWidth(depthMap);
    size_t height = CVPixelBufferGetHeight(depthMap);
    float* floatData = (float*)CVPixelBufferGetBaseAddress(depthMap);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(depthMap);

    // Convert float32 meters → uint16 millimeters
    auto& depthData = frame.DepthSensingViews[viewIndex];
    depthData.Width = width;
    depthData.Height = height;
    depthData.RawValueToMeters = 0.001f;
    depthData.DepthBuffer.resize(width * height);

    for (size_t r = 0; r < height; r++) {
        const float* row = (const float*)((uint8_t*)floatData + r * bytesPerRow);
        for (size_t c = 0; c < width; c++) {
            float meters = row[c];
            uint16_t mm = static_cast<uint16_t>(
                std::min(meters * 1000.0f, 65535.0f));
            depthData.DepthBuffer[r * width + c] = mm;
        }
    }

    CVPixelBufferUnlockBaseAddress(depthMap, kCVPixelBufferLock_ReadOnly);
}
```

### normDepthBufferFromNormView Computation

ARKit's depth map may not be aligned with the camera view. Use
`ARCamera.intrinsics` and the depth map dimensions vs. camera image
dimensions to compute the transform. The depth sensor on LiDAR devices
typically has a different resolution (e.g., 256×192) than the camera
(e.g., 1920×1440).

The transform should account for:
- Resolution difference between depth map and camera view
- Any rotation between depth sensor and camera coordinate frames
- Display orientation

---

## R8: Initialization Wiring

**Layer**: `Plugins/NativeXr/Source/NativeXr.cpp`

### New Class Registration

Add to the initialization sequence:
```cpp
XRCPUDepthInformation::Initialize(env);
```

This must be called **before** `XRSession::Initialize(env)` and
`XRFrame::Initialize(env)` so the class is available when those classes
reference it.

---

## R9: NativeXrImpl Bridge

**Layer**: `Plugins/NativeXr/Source/NativeXrImpl.h`

Add a method to enable depth sensing on the session:

```cpp
void SetDepthSensingEnabled(bool enabled)
{
    m_sessionState->Session->SetDepthSensingEnabled(enabled);
}
```

This is called from `XRSession::CreateAsync` after parsing the
`depthSensing` config.

---

## R10: Error Handling and Edge Cases

### Graceful Null Returns

`XRFrame::GetDepthInformation(view)` returns `null` (not throw) when:
- Depth sensing was not requested in session init
- Platform does not support depth (e.g., non-LiDAR iOS device)
- Depth data is temporarily unavailable for this frame
- The view index is out of range

### Range Validation

`XRCPUDepthInformation::GetDepthInMeters(x, y)` must:
- Accept (x, y) as floats in [0, 1] range
- Throw a JS `RangeError` if x or y is outside [0, 1]
- Return depth in meters (float)

### Memory Management

- Depth buffer (`std::vector<uint16_t>`) is owned by the platform frame
  and copied to the NAPI `ArrayBuffer` each frame.
- The `ArrayBuffer` should be allocated once (when dimensions first become
  known) and reused if dimensions stay constant.
- The `XRCPUDepthInformation` NAPI object should be cached per view index
  (similar to how `XRPlane` objects are cached in `m_trackedPlanes`).

### Thread Safety

- Depth data is acquired on the XR frame thread (same as other features).
- The NAPI object is only accessed on the JS thread.
- The existing frame update pattern (acquire frame → update features →
  expose to JS) applies without changes.

---

## R11: Data Flow Summary

```
┌──────────────────────────────────────────────────────────────────┐
│  Babylon.js  (WebXRDepthSensing feature)                         │
│  ─ calls frame.getDepthInformation(view)                         │
│  ─ reads .data, .width, .height, .rawValueToMeters               │
│  ─ calls .getDepthInMeters(x, y)                                 │
│  ─ uploads to RawTexture for rendering                           │
└──────────────────────┬───────────────────────────────────────────┘
                       │  NAPI boundary
┌──────────────────────▼───────────────────────────────────────────┐
│  XRFrame.getDepthInformation(view)                               │
│  → XRCPUDepthInformation (cached per view index)                 │
│  → reads from m_frame->DepthSensingViews[viewIdx]                │
└──────────────────────┬───────────────────────────────────────────┘
                       │  xr:: abstraction
┌──────────────────────▼───────────────────────────────────────────┐
│  xr::System::Session::Frame::DepthSensingViews                   │
│  → DepthSensingData { Width, Height, DepthBuffer,                │
│     RawValueToMeters, NormDepthBufferFromNormView }               │
└──────────────────────┬───────────────────────────────────────────┘
                       │  platform backend
┌──────────────────────▼───────────────────────────────────────────┐
│  ARCore: ArFrame_acquireDepthImage16Bits → uint16 mm depth       │
│  ARKit:  ARFrame.sceneDepth.depthMap → float32 m → uint16 mm    │
└──────────────────────────────────────────────────────────────────┘
```

---

## Non-Requirements (Out of Scope)

- GPU-optimized path (`XRWebGLDepthInformation`, `XRWebGLBinding` changes)
- `"float32"` and `"unsigned-short"` data formats
- `XRDepthType` (raw/smooth), `depthActive`, pause/resume
- Confidence maps
- Feature detection changes to `isSessionSupported` (depth availability
  is handled at runtime by returning null)
