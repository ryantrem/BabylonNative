# Architecture: xr-depth-sensing

## Overview

This document describes the architectural changes needed to implement
CPU-optimized WebXR Depth Sensing in Babylon Native. The design follows
the established NativeXR feature pattern:

```
XR.h (platform abstraction)
  → ARCore/XR.cpp + ARKit/XR.mm (platform backends)
    → NativeXrImpl.h (bridge)
      → XRSession / XRFrame / XRCPUDepthInformation (NAPI wrappers)
        → Babylon.js WebXRDepthSensing (JS consumer)
```

---

## 1. Platform Abstraction Layer

### File: `Dependencies/xr/Include/XR.h`

#### New struct inside `System::Session::Frame` (after `ImageTrackingResult`, ~line 335)

```cpp
struct DepthSensingData
{
    uint32_t Width{0};
    uint32_t Height{0};

    // Raw depth buffer: uint16 values in row-major order, no padding.
    // For "luminance-alpha" format, each entry is depth in
    // platform-specific units (typically millimeters).
    std::vector<uint16_t> DepthBuffer{};

    // Multiply raw values by this to get depth in meters.
    float RawValueToMeters{0.001f};

    // 4x4 column-major matrix: transforms normalized view coordinates
    // (origin top-left, X→right, Y→down, [0,1]) to normalized depth
    // buffer coordinates (same convention).
    std::array<float, 16> NormDepthBufferFromNormView{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    bool HasData{false};
};
```

#### New member on `Frame` (after `UpdatedImageTrackingResults`, ~line 349)

```cpp
// Per-view depth sensing data. Indexed same as Views.
// Empty if depth sensing not enabled or unavailable this frame.
std::vector<DepthSensingData> DepthSensingViews;
```

#### New methods on `Session` (after `CreateAugmentedImageDatabase`, ~line 389)

```cpp
void SetDepthSensingEnabled(bool enabled);
bool IsDepthSensingEnabled() const;
```

**Rationale**: The `DepthSensingData` struct follows the same pattern as
`Plane`, `Mesh`, etc. — a plain data struct populated by the platform
backend. The `HasData` flag avoids needing a separate "updated" vector
since depth data is simply present-or-absent each frame (not tracked by
ID like planes/meshes). Per-view indexing matches the existing `Views`
vector.

---

## 2. Platform Backends

### 2a. ARCore: `Dependencies/xr/Source/ARCore/XR.cpp`

#### Session::Impl additions (~line 240, alongside existing members)

```cpp
bool DepthSensingEnabled{false};
std::vector<Frame::DepthSensingData> DepthSensingFrameData;
```

#### New method on Session::Impl

```cpp
void UpdateDepthSensing()
{
    if (!DepthSensingEnabled) {
        DepthSensingFrameData.clear();
        return;
    }

    // Ensure we have at least one view's worth of depth data
    DepthSensingFrameData.resize(ActiveFrameViews.size());

    ArImage* depth_image = nullptr;
    ArStatus status = ArFrame_acquireDepthImage16Bits(
        ArSession, ArFrame, &depth_image);

    if (status != AR_SUCCESS || depth_image == nullptr) {
        for (auto& d : DepthSensingFrameData) d.HasData = false;
        return;
    }

    int32_t width, height;
    ArImage_getWidth(ArSession, depth_image, &width);
    ArImage_getHeight(ArSession, depth_image, &height);

    const uint8_t* buffer = nullptr;
    int32_t buffer_length = 0;
    ArImage_getPlaneData(ArSession, depth_image, 0, &buffer, &buffer_length);

    int32_t row_stride;
    ArImage_getPlaneRowStride(ArSession, depth_image, 0, &row_stride);

    // Populate first view's depth data (ARCore is mono)
    auto& depthData = DepthSensingFrameData[0];
    depthData.Width = static_cast<uint32_t>(width);
    depthData.Height = static_cast<uint32_t>(height);
    depthData.RawValueToMeters = 0.001f;  // mm → meters
    depthData.HasData = true;
    depthData.DepthBuffer.resize(width * height);

    // Copy with row stride handling
    for (int r = 0; r < height; r++) {
        const uint16_t* row_ptr = reinterpret_cast<const uint16_t*>(
            buffer + r * row_stride);
        std::copy(row_ptr, row_ptr + width,
                  depthData.DepthBuffer.data() + r * width);
    }

    // Compute normDepthBufferFromNormView
    // For ARCore, use ArFrame_transformCoordinates2d to map
    // from VIEW_NORMALIZED to TEXTURE_NORMALIZED coordinates.
    // Build the matrix from the corner transforms.
    ComputeNormDepthBufferFromNormView(depthData);

    ArImage_release(depth_image);

    // Additional views (if any) don't have depth
    for (size_t i = 1; i < DepthSensingFrameData.size(); i++) {
        DepthSensingFrameData[i].HasData = false;
    }
}
```

#### ComputeNormDepthBufferFromNormView (ARCore)

ARCore provides `ArFrame_transformCoordinates2d()` which can map
normalized view coordinates to normalized depth texture coordinates.
Transform 3 reference points (e.g., (0,0), (1,0), (0,1)) to derive an
affine matrix, then pack into the 4×4 format.

```cpp
void ComputeNormDepthBufferFromNormView(Frame::DepthSensingData& depthData)
{
    // Map 3 corners: (0,0), (1,0), (0,1) from VIEW to TEXTURE space
    float src[] = {0, 0,  1, 0,  0, 1};
    float dst[6];
    ArFrame_transformCoordinates2d(
        ArSession, ArFrame,
        AR_COORDINATES_2D_VIEW_NORMALIZED, 3, src,
        AR_COORDINATES_2D_TEXTURE_NORMALIZED, dst);

    // Derive affine transform:  dst = M * src
    // [a b tx]   [x]   [dst_x]
    // [c d ty] * [y] = [dst_y]
    // [0 0  1]   [1]   [  1  ]
    float ax = dst[0], ay = dst[1];  // (0,0) → (ax, ay) → tx=ax, ty=ay
    float bx = dst[2], by = dst[3];  // (1,0) → (bx, by) → a=bx-ax, c=by-ay
    float cx = dst[4], cy = dst[5];  // (0,1) → (cx, cy) → b=cx-ax, d=cy-ay

    float a = bx - ax, b = cx - ax, tx = ax;
    float c = by - ay, d = cy - ay, ty = ay;

    // Pack into column-major 4×4 matrix
    depthData.NormDepthBufferFromNormView = {
        a,  c,  0, 0,
        b,  d,  0, 0,
        0,  0,  1, 0,
        tx, ty, 0, 1
    };
}
```

#### Frame constructor change (~line 1405)

Add `DepthSensingViews` initialization:
```cpp
System::Session::Frame::Frame(Session::Impl& sessionImpl)
    : Views{ sessionImpl.ActiveFrameViews }
    , ...existing...
    , DepthSensingViews{}  // NEW
    , m_impl{ ... }
{
    if (IsTracking)
    {
        ...existing update calls...
        m_impl->sessionImpl.UpdateDepthSensing();  // NEW
        DepthSensingViews = m_impl->sessionImpl.DepthSensingFrameData; // NEW
    }
}
```

#### Session methods (~line 1588)

```cpp
void System::Session::SetDepthSensingEnabled(bool enabled)
{
    m_impl->DepthSensingEnabled = enabled;
}

bool System::Session::IsDepthSensingEnabled() const
{
    return m_impl->DepthSensingEnabled;
}
```

---

### 2b. ARKit: `Dependencies/xr/Source/ARKit/XR.mm`

#### Session::Impl additions (~line 690, alongside existing members)

```cpp
bool DepthSensingEnabled{false};
std::vector<Frame::DepthSensingData> DepthSensingFrameData;
```

#### ARWorldTrackingConfiguration change

When `DepthSensingEnabled` is set to true and the session is being
configured, add `.sceneDepth` to `frameSemantics`:

```objc
if (DepthSensingEnabled) {
    if ([ARWorldTrackingConfiguration supportsFrameSemantics:
            ARFrameSemanticSceneDepth]) {
        configuration.frameSemantics |= ARFrameSemanticSceneDepth;
    }
}
```

This requires `#if (__IPHONE_OS_VERSION_MAX_ALLOWED >= 140000)` guard
since `ARFrameSemanticSceneDepth` was introduced in iOS 14.0 / ARKit 4.

#### UpdateDepthSensing method

```objc
void UpdateDepthSensing()
{
    if (!DepthSensingEnabled) {
        DepthSensingFrameData.clear();
        return;
    }

    DepthSensingFrameData.resize(ActiveFrameViews.size());

#if (__IPHONE_OS_VERSION_MAX_ALLOWED >= 140000)
    ARFrame* currentFrame = [sessionDelegate session].currentFrame;
    ARDepthData* sceneDepth = currentFrame.sceneDepth;

    if (sceneDepth == nil) {
        for (auto& d : DepthSensingFrameData) d.HasData = false;
        return;
    }

    CVPixelBufferRef depthMap = sceneDepth.depthMap;
    CVPixelBufferLockBaseAddress(depthMap, kCVPixelBufferLock_ReadOnly);

    size_t width = CVPixelBufferGetWidth(depthMap);
    size_t height = CVPixelBufferGetHeight(depthMap);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(depthMap);
    float* floatData = (float*)CVPixelBufferGetBaseAddress(depthMap);

    auto& depthData = DepthSensingFrameData[0];
    depthData.Width = static_cast<uint32_t>(width);
    depthData.Height = static_cast<uint32_t>(height);
    depthData.RawValueToMeters = 0.001f;
    depthData.HasData = true;
    depthData.DepthBuffer.resize(width * height);

    // Convert float32 meters → uint16 millimeters
    for (size_t r = 0; r < height; r++) {
        const float* row = (const float*)
            ((uint8_t*)floatData + r * bytesPerRow);
        for (size_t c = 0; c < width; c++) {
            float meters = row[c];
            depthData.DepthBuffer[r * width + c] =
                static_cast<uint16_t>(std::min(meters * 1000.0f, 65535.0f));
        }
    }

    CVPixelBufferUnlockBaseAddress(depthMap, kCVPixelBufferLock_ReadOnly);

    // Compute normDepthBufferFromNormView using ARCamera intrinsics
    ComputeNormDepthBufferFromNormView(currentFrame, depthData);
#else
    for (auto& d : DepthSensingFrameData) d.HasData = false;
#endif

    for (size_t i = 1; i < DepthSensingFrameData.size(); i++) {
        DepthSensingFrameData[i].HasData = false;
    }
}
```

#### ComputeNormDepthBufferFromNormView (ARKit)

ARKit's depth sensor resolution differs from the camera. Use the camera
intrinsics and display transform to compute the mapping:

```objc
void ComputeNormDepthBufferFromNormView(ARFrame* frame,
                                         Frame::DepthSensingData& depthData)
{
    // The depth map is aligned to the camera but at a different resolution.
    // For LiDAR devices, the depth sensor covers the same field of view
    // as the camera, so the transform is typically identity after
    // accounting for display orientation.
    //
    // Use displayTransform to account for screen rotation:
    CGAffineTransform displayTransform = [frame
        displayTransformForOrientation:UIInterfaceOrientationPortrait
        viewportSize:CGSizeMake(depthData.Width, depthData.Height)];

    // Convert CGAffineTransform to 4x4 column-major matrix
    depthData.NormDepthBufferFromNormView = {
        static_cast<float>(displayTransform.a),
        static_cast<float>(displayTransform.c),
        0, 0,
        static_cast<float>(displayTransform.b),
        static_cast<float>(displayTransform.d),
        0, 0,
        0, 0, 1, 0,
        static_cast<float>(displayTransform.tx),
        static_cast<float>(displayTransform.ty),
        0, 1
    };
}
```

#### Frame constructor change (~line 1717)

Same pattern as ARCore:
```objc
System::Session::Frame::Frame(Session::Impl& sessionImpl)
    : Views{ sessionImpl.ActiveFrameViews }
    , ...existing...
    , DepthSensingViews{}  // NEW
    , m_impl{ ... }
{
    ...existing code...
    m_impl->sessionImpl.UpdateDepthSensing();   // NEW
    DepthSensingViews = m_impl->sessionImpl.DepthSensingFrameData; // NEW
}
```

---

## 3. NAPI Layer

### 3a. New File: `Plugins/NativeXr/Source/XRCPUDepthInformation.h`

Header-only NAPI wrapper (following the pattern of XRPlane.h, XRMesh.h):

```cpp
#pragma once

#include "XRRigidTransform.h"

namespace Babylon
{
    class XRCPUDepthInformation : public Napi::ObjectWrap<XRCPUDepthInformation>
    {
        static constexpr auto JS_CLASS_NAME = "XRCPUDepthInformation";

    public:
        static void Initialize(Napi::Env env)
        {
            Napi::HandleScope scope{env};
            Napi::Function func = DefineClass(env, JS_CLASS_NAME, {
                InstanceAccessor("width", &GetWidth, nullptr),
                InstanceAccessor("height", &GetHeight, nullptr),
                InstanceAccessor("data", &GetData, nullptr),
                InstanceAccessor("rawValueToMeters", &GetRawValueToMeters, nullptr),
                InstanceAccessor("normDepthBufferFromNormView", &GetNormDepthBufferFromNormView, nullptr),
                InstanceMethod("getDepthInMeters", &GetDepthInMeters),
            });
            env.Global().Set(JS_CLASS_NAME, func);
        }

        static Napi::Object New(const Napi::Env& env)
        {
            return env.Global().Get(JS_CLASS_NAME)
                .As<Napi::Function>().New({});
        }

        XRCPUDepthInformation(const Napi::CallbackInfo& info)
            : Napi::ObjectWrap<XRCPUDepthInformation>{info}
            , m_normTransform{Napi::Persistent(XRRigidTransform::New(info.Env()))}
        {}

        void Update(const xr::System::Session::Frame::DepthSensingData& data)
        {
            m_width = data.Width;
            m_height = data.Height;
            m_rawValueToMeters = data.RawValueToMeters;

            // Copy depth buffer to ArrayBuffer (reuse if same size)
            size_t byteLength = data.DepthBuffer.size() * sizeof(uint16_t);
            if (!m_dataBuffer ||
                m_dataBuffer.Value().ByteLength() != byteLength)
            {
                m_dataBuffer = Napi::Persistent(
                    Napi::ArrayBuffer::New(
                        m_normTransform.Env(), byteLength));
            }
            std::memcpy(m_dataBuffer.Value().Data(),
                        data.DepthBuffer.data(), byteLength);

            // Store raw depth data pointer for getDepthInMeters
            m_depthBufferPtr = static_cast<uint16_t*>(
                m_dataBuffer.Value().Data());
            m_depthBufferSize = data.DepthBuffer.size();

            // Update the normDepthBufferFromNormView matrix
            // We only need the .matrix property; set it directly.
            auto* transform = XRRigidTransform::Unwrap(
                m_normTransform.Value());
            auto matrixArray = m_normTransform.Value()
                .Get("matrix").As<Napi::Float32Array>();
            std::memcpy(matrixArray.Data(),
                        data.NormDepthBufferFromNormView.data(),
                        16 * sizeof(float));
        }

    private:
        uint32_t m_width{0};
        uint32_t m_height{0};
        float m_rawValueToMeters{0.001f};
        Napi::Reference<Napi::ArrayBuffer> m_dataBuffer{};
        Napi::ObjectReference m_normTransform{};
        uint16_t* m_depthBufferPtr{nullptr};
        size_t m_depthBufferSize{0};

        Napi::Value GetWidth(const Napi::CallbackInfo& info)
        {
            return Napi::Number::New(info.Env(), m_width);
        }

        Napi::Value GetHeight(const Napi::CallbackInfo& info)
        {
            return Napi::Number::New(info.Env(), m_height);
        }

        Napi::Value GetData(const Napi::CallbackInfo& info)
        {
            if (!m_dataBuffer)
                return info.Env().Null();
            return m_dataBuffer.Value();
        }

        Napi::Value GetRawValueToMeters(const Napi::CallbackInfo& info)
        {
            return Napi::Number::New(info.Env(), m_rawValueToMeters);
        }

        Napi::Value GetNormDepthBufferFromNormView(const Napi::CallbackInfo&)
        {
            return m_normTransform.Value();
        }

        Napi::Value GetDepthInMeters(const Napi::CallbackInfo& info)
        {
            // Validate arguments
            if (info.Length() < 2) {
                throw Napi::RangeError::New(info.Env(),
                    "getDepthInMeters requires (x, y) arguments");
            }

            float x = info[0].As<Napi::Number>().FloatValue();
            float y = info[1].As<Napi::Number>().FloatValue();

            if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) {
                throw Napi::RangeError::New(info.Env(),
                    "x and y must be in [0, 1] range");
            }

            if (m_depthBufferPtr == nullptr || m_width == 0 || m_height == 0) {
                return Napi::Number::New(info.Env(), 0.0f);
            }

            // Apply normDepthBufferFromNormView transform
            const auto& m = /* read from stored matrix */
                *reinterpret_cast<const std::array<float,16>*>(
                    m_normTransform.Value()
                        .Get("matrix").As<Napi::Float32Array>().Data());

            // Column-major: result = M * [x, y, 0, 1]
            float nx = m[0]*x + m[4]*y + m[12];
            float ny = m[1]*x + m[5]*y + m[13];

            // Scale to buffer coordinates
            int col = static_cast<int>(nx * m_width);
            int row = static_cast<int>(ny * m_height);

            // Clamp
            col = std::max(0, std::min(col, static_cast<int>(m_width) - 1));
            row = std::max(0, std::min(row, static_cast<int>(m_height) - 1));

            size_t index = static_cast<size_t>(row) * m_width + col;
            if (index >= m_depthBufferSize) {
                return Napi::Number::New(info.Env(), 0.0f);
            }

            float depthInMeters = m_depthBufferPtr[index] * m_rawValueToMeters;
            return Napi::Number::New(info.Env(), depthInMeters);
        }
    };
} // Babylon
```

**Key design decisions**:

- **Header-only**: Follows pattern of XRPlane.h, XRMesh.h, XRView.h.
- **ArrayBuffer reuse**: The data ArrayBuffer is allocated once and reused
  if dimensions stay constant (avoiding per-frame allocation).
- **normDepthBufferFromNormView as XRRigidTransform**: We reuse the existing
  `XRRigidTransform` class, setting its `.matrix` property directly via
  memcpy. Babylon.js only reads `.matrix`, not position/orientation.
- **getDepthInMeters**: Implements the WebXR spec algorithm —
  transform → scale → clamp → index → multiply by rawValueToMeters.

---

### 3b. Modify: `Plugins/NativeXr/Source/XRFrame.h`

Add new members and method:

```cpp
// After m_imageTrackingResultsArray (~line 40):
#include "XRCPUDepthInformation.h"

// New private members:
std::vector<Napi::ObjectReference> m_depthInfoObjects{};
bool m_depthSensingEnabled{false};

// New method declaration:
Napi::Value GetDepthInformation(const Napi::CallbackInfo& info);
```

### 3c. Modify: `Plugins/NativeXr/Source/XRFrame.cpp`

#### Initialize — register new method (~line 47)

```cpp
InstanceMethod("getDepthInformation", &XRFrame::GetDepthInformation),
```

#### GetDepthInformation implementation

```cpp
Napi::Value XRFrame::GetDepthInformation(const Napi::CallbackInfo& info)
{
    if (!m_depthSensingEnabled || !m_frame ||
        m_frame->DepthSensingViews.empty())
    {
        return info.Env().Null();
    }

    // Determine view index from the XRView argument.
    // XRView stores m_eyeIdx; we need to access it.
    auto* xrView = XRView::Unwrap(info[0].As<Napi::Object>());
    size_t viewIdx = xrView->EyeIndex();

    if (viewIdx >= m_frame->DepthSensingViews.size())
    {
        return info.Env().Null();
    }

    const auto& depthData = m_frame->DepthSensingViews[viewIdx];
    if (!depthData.HasData)
    {
        return info.Env().Null();
    }

    // Ensure we have a cached depth info object for this view index.
    while (m_depthInfoObjects.size() <= viewIdx)
    {
        m_depthInfoObjects.push_back(Napi::Persistent(
            XRCPUDepthInformation::New(info.Env())));
    }

    auto* depthInfo = XRCPUDepthInformation::Unwrap(
        m_depthInfoObjects[viewIdx].Value());
    depthInfo->Update(depthData);

    return m_depthInfoObjects[viewIdx].Value();
}
```

#### SetDepthSensingEnabled

```cpp
void XRFrame::SetDepthSensingEnabled(bool enabled)
{
    m_depthSensingEnabled = enabled;
}
```

---

### 3d. Modify: `Plugins/NativeXr/Source/XRView.h`

Add a public accessor for the eye/view index (~line 43):

```cpp
public:
    size_t EyeIndex() const { return m_eyeIdx; }
```

This is needed so `XRFrame::GetDepthInformation(view)` can determine
which view's depth data to return.

---

### 3e. Modify: `Plugins/NativeXr/Source/XRSession.h` / `XRSession.cpp`

#### New private members

```cpp
bool m_depthSensingEnabled{false};
std::string m_depthUsage{};
std::string m_depthDataFormat{};
```

#### Initialize — add accessors (~line 217)

```cpp
InstanceAccessor("depthUsage", &XRSession::GetDepthUsage, nullptr),
InstanceAccessor("depthDataFormat", &XRSession::GetDepthDataFormat, nullptr),
```

#### CreateAsync — parse depthSensing config (~after trackedImages, line 278)

```cpp
if (featureObject.Has("depthSensing"))
{
    auto depthConfig = featureObject.Get("depthSensing").As<Napi::Object>();

    // Parse usagePreference
    if (depthConfig.Has("usagePreference"))
    {
        auto usages = depthConfig.Get("usagePreference").As<Napi::Array>();
        for (uint32_t i = 0; i < usages.Length(); i++)
        {
            auto usage = usages.Get(i).As<Napi::String>().Utf8Value();
            if (usage == "cpu-optimized")
            {
                session.m_depthUsage = "cpu-optimized";
                break;
            }
        }
    }

    // Parse dataFormatPreference
    if (depthConfig.Has("dataFormatPreference"))
    {
        auto formats = depthConfig.Get("dataFormatPreference").As<Napi::Array>();
        for (uint32_t i = 0; i < formats.Length(); i++)
        {
            auto fmt = formats.Get(i).As<Napi::String>().Utf8Value();
            if (fmt == "luminance-alpha")
            {
                session.m_depthDataFormat = "luminance-alpha";
                break;
            }
        }
    }

    // Enable depth sensing if we found a supported combination
    session.m_depthSensingEnabled =
        !session.m_depthUsage.empty() &&
        !session.m_depthDataFormat.empty();
}
```

#### After session creation resolves — enable depth on the platform

In the `BeginSessionAsync` success path, call:
```cpp
if (session.m_depthSensingEnabled)
{
    session.m_xr->SetDepthSensingEnabled(true);
}
```

#### Also propagate to XRFrame

When the frame callback fires (in the requestAnimationFrame path),
set the depth sensing flag on the XRFrame:
```cpp
m_frame.SetDepthSensingEnabled(m_depthSensingEnabled);
```

#### Accessor implementations

```cpp
Napi::Value XRSession::GetDepthUsage(const Napi::CallbackInfo& info)
{
    if (!m_depthSensingEnabled)
        return info.Env().Null();
    return Napi::String::New(info.Env(), m_depthUsage);
}

Napi::Value XRSession::GetDepthDataFormat(const Napi::CallbackInfo& info)
{
    if (!m_depthSensingEnabled)
        return info.Env().Null();
    return Napi::String::New(info.Env(), m_depthDataFormat);
}
```

---

### 3f. Modify: `Plugins/NativeXr/Source/NativeXrImpl.h`

Add bridge method (~after `CreateAugmentedImageDatabase`, line 361):

```cpp
void SetDepthSensingEnabled(bool enabled)
{
    m_sessionState->Session->SetDepthSensingEnabled(enabled);
}
```

---

### 3g. Modify: `Plugins/NativeXr/Source/NativeXr.cpp`

Add initialization (~line 55, after XRWebGLBinding::Initialize):

```cpp
XRCPUDepthInformation::Initialize(env);
```

Add include (~line 14):

```cpp
#include "XRCPUDepthInformation.h"
```

---

## 4. Data Flow (Detailed)

```
┌─────────────────────────────────────────────────────────────────┐
│ Babylon.js WebXRDepthSensing                                     │
│                                                                   │
│ getXRSessionInitExtension() →                                     │
│   { depthSensing: {usagePreference:["cpu-optimized"],             │
│                     dataFormatPreference:["luminance-alpha"]} }    │
│                                                                   │
│ _onXRFrame(frame) →                                               │
│   depthInfo = frame.getDepthInformation(view)                     │
│   depthInfo.data / .width / .height / .rawValueToMeters           │
│   depthInfo.getDepthInMeters(x, y)                                │
│   depthInfo.normDepthBufferFromNormView.matrix                    │
└──────────────┬──────────────────────────────────────────────────┘
               │ NAPI
┌──────────────▼──────────────────────────────────────────────────┐
│ XRSession::CreateAsync                                           │
│   Parses depthSensing config → m_depthUsage, m_depthDataFormat   │
│   Calls m_xr->SetDepthSensingEnabled(true)                       │
│                                                                   │
│ XRSession properties: .depthUsage → "cpu-optimized"               │
│                       .depthDataFormat → "luminance-alpha"        │
│                                                                   │
│ XRFrame::GetDepthInformation(view)                               │
│   viewIdx = XRView::EyeIndex()                                   │
│   depth = m_frame->DepthSensingViews[viewIdx]                    │
│   → XRCPUDepthInformation.Update(depth)                          │
│   → returns cached JS object                                     │
└──────────────┬──────────────────────────────────────────────────┘
               │ xr:: abstraction
┌──────────────▼──────────────────────────────────────────────────┐
│ XR.h: Frame::DepthSensingViews                                   │
│   vector<DepthSensingData> — one per view                        │
│   DepthSensingData: Width, Height, DepthBuffer (uint16),         │
│     RawValueToMeters, NormDepthBufferFromNormView, HasData       │
└──────────────┬──────────────────────────────────────────────────┘
               │ platform
┌──────────────▼──────────────┬───────────────────────────────────┐
│ ARCore (XR.cpp)              │ ARKit (XR.mm)                     │
│                              │                                   │
│ ArFrame_acquireDepthImage    │ ARFrame.sceneDepth.depthMap        │
│   16Bits()                   │   CVPixelBuffer (float32 m)       │
│ → uint16 mm depth map       │ → convert to uint16 mm            │
│ → copy to DepthBuffer       │ → copy to DepthBuffer             │
│                              │                                   │
│ ArFrame_transform            │ ARFrame.displayTransform           │
│   Coordinates2d()            │   ForOrientation()                │
│ → build affine matrix        │ → build affine matrix             │
│ → NormDepthBufferFromNormView│ → NormDepthBufferFromNormView     │
└──────────────────────────────┴───────────────────────────────────┘
```

---

## 5. Files Changed Summary

| File | Action | Description |
|------|--------|-------------|
| `Dependencies/xr/Include/XR.h` | Modify | Add `DepthSensingData` struct, `DepthSensingViews` member, session methods |
| `Dependencies/xr/Source/ARCore/XR.cpp` | Modify | Add depth acquisition, transform computation, Frame integration |
| `Dependencies/xr/Source/ARKit/XR.mm` | Modify | Add depth acquisition, float→uint16 conversion, Frame integration |
| `Plugins/NativeXr/Source/XRCPUDepthInformation.h` | **Create** | New NAPI wrapper for depth information |
| `Plugins/NativeXr/Source/XRFrame.h` | Modify | Add depth info members and method declaration |
| `Plugins/NativeXr/Source/XRFrame.cpp` | Modify | Add `getDepthInformation` registration and implementation |
| `Plugins/NativeXr/Source/XRView.h` | Modify | Add public `EyeIndex()` accessor |
| `Plugins/NativeXr/Source/XRSession.h` | Modify | Add depth config members |
| `Plugins/NativeXr/Source/XRSession.cpp` | Modify | Parse `depthSensing` config, add accessors, propagate to platform |
| `Plugins/NativeXr/Source/NativeXrImpl.h` | Modify | Add `SetDepthSensingEnabled` bridge |
| `Plugins/NativeXr/Source/NativeXr.cpp` | Modify | Add `XRCPUDepthInformation::Initialize(env)` |

---

## 6. Key Design Decisions

### D1: DepthSensingData as value type (copied per frame)
Rather than using references like `Views` and `InputSources`, depth data
is copied into the Frame as a `std::vector<DepthSensingData>`. This is
acceptable because:
- Depth buffers are small (~160×120 to 640×480 × 2 bytes = 38KB–614KB)
- Data ownership is clear (frame owns its depth data)
- Avoids lifetime issues between frames

### D2: Cached XRCPUDepthInformation objects
The NAPI wrapper objects are created once per view index and reused across
frames (via `m_depthInfoObjects` vector on XRFrame). This avoids JS object
allocation overhead every frame. The `Update()` method refreshes data in
place. This follows the same caching pattern used for XRPlane/XRMesh.

### D3: normDepthBufferFromNormView via XRRigidTransform
We reuse the existing `XRRigidTransform` class, writing the matrix data
directly. Babylon.js only accesses `.matrix`, so position/orientation
values don't need to be meaningful. This avoids creating a new class just
for this transform.

### D4: View index via EyeIndex()
Adding a public `EyeIndex()` accessor to `XRView` is minimally invasive.
The index is already stored (`m_eyeIdx`) and set during
`XRViewerPose::Update()`. This lets `getDepthInformation(view)` map a JS
view object back to the correct depth data index.

### D5: Feature enablement flow
Depth sensing follows the session-init pattern: configuration is parsed
during `CreateAsync`, a boolean is set on `Session::Impl`, and the
platform backend checks it each frame. This matches how
`FeaturePointCloudEnabled` and `PlaneDetectionEnabled` work — simple
boolean gating with no complex feature negotiation.

### D6: Platform-specific iOS version guards
ARKit depth sensing requires iOS 14+ (LiDAR hardware + API). The ARKit
backend uses `#if (__IPHONE_OS_VERSION_MAX_ALLOWED >= 140000)` guards,
matching the existing pattern used for mesh detection
(`__IPHONE_OS_VERSION_MAX_ALLOWED >= 130400`).
