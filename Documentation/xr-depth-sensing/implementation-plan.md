# Implementation Plan: xr-depth-sensing

## Execution Order

The implementation is ordered to build from the bottom of the stack upward,
allowing incremental compilation and testing at each step.

---

## Step 1: Platform Abstraction — XR.h

**File**: `Dependencies/xr/Include/XR.h`

### Changes

1. Add `DepthSensingData` struct inside `System::Session::Frame` (after
   `ImageTrackingResult`, before the public members section at ~line 337):

```cpp
struct DepthSensingData
{
    uint32_t Width{0};
    uint32_t Height{0};
    std::vector<uint16_t> DepthBuffer{};
    float RawValueToMeters{0.001f};
    std::array<float, 16> NormDepthBufferFromNormView{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    bool HasData{false};
};
```

2. Add member to `Frame` (after `UpdatedImageTrackingResults`, ~line 349):

```cpp
std::vector<DepthSensingData> DepthSensingViews;
```

3. Add methods to `Session` (after `CreateAugmentedImageDatabase`, ~line 389):

```cpp
void SetDepthSensingEnabled(bool enabled);
bool IsDepthSensingEnabled() const;
```

### Verification

Compile the xr dependency target to confirm the new types are valid.

---

## Step 2: NAPI Wrapper — XRCPUDepthInformation

**File**: `Plugins/NativeXr/Source/XRCPUDepthInformation.h` (NEW)

### Changes

Create the header-only NAPI wrapper class as described in the architecture
document (Section 3a). Key API surface:

- `width` (accessor) → uint32
- `height` (accessor) → uint32
- `data` (accessor) → ArrayBuffer
- `rawValueToMeters` (accessor) → float
- `normDepthBufferFromNormView` (accessor) → XRRigidTransform
- `getDepthInMeters(x, y)` (method) → float

Include `Update(const DepthSensingData&)` for the XRFrame to call.

### Verification

Include the header in NativeXr.cpp and compile to verify syntax.

---

## Step 3: XRView — Expose EyeIndex

**File**: `Plugins/NativeXr/Source/XRView.h`

### Changes

Add a public accessor method (~line 43, in the public section):

```cpp
size_t EyeIndex() const { return m_eyeIdx; }
```

### Verification

This is a trivial change. Compile to confirm.

---

## Step 4: XRFrame — Add getDepthInformation

**Files**: `Plugins/NativeXr/Source/XRFrame.h`, `XRFrame.cpp`

### XRFrame.h Changes

1. Add include at top: `#include "XRCPUDepthInformation.h"`
2. Add private members:
   - `std::vector<Napi::ObjectReference> m_depthInfoObjects{};`
   - `bool m_depthSensingEnabled{false};`
3. Add public method: `void SetDepthSensingEnabled(bool enabled);`
4. Add private method declaration: `Napi::Value GetDepthInformation(const Napi::CallbackInfo& info);`

### XRFrame.cpp Changes

1. In `Initialize()`, add to the DefineClass list:
   ```cpp
   InstanceMethod("getDepthInformation", &XRFrame::GetDepthInformation),
   ```

2. Implement `SetDepthSensingEnabled`:
   ```cpp
   void XRFrame::SetDepthSensingEnabled(bool enabled)
   {
       m_depthSensingEnabled = enabled;
   }
   ```

3. Implement `GetDepthInformation` (see architecture Section 3c for full
   implementation). Key logic:
   - Return null if depth sensing disabled, frame unavailable, or no depth data
   - Get view index via `XRView::EyeIndex()`
   - Return null if view index out of range or no data
   - Create/reuse cached `XRCPUDepthInformation` per view index
   - Call `Update(depthData)` and return the JS object

### Verification

Compile the NativeXr plugin. The method exists but won't be called yet
since the session doesn't enable it.

---

## Step 5: XRSession — Depth Sensing Config and Properties

**Files**: `Plugins/NativeXr/Source/XRSession.h`, `XRSession.cpp`

### XRSession.h Changes

Add private members:
```cpp
bool m_depthSensingEnabled{false};
std::string m_depthUsage{};
std::string m_depthDataFormat{};
```

Add private method declarations:
```cpp
Napi::Value GetDepthUsage(const Napi::CallbackInfo& info);
Napi::Value GetDepthDataFormat(const Napi::CallbackInfo& info);
```

### XRSession.cpp Changes

1. In `Initialize()`, add to DefineClass:
   ```cpp
   InstanceAccessor("depthUsage", &XRSession::GetDepthUsage, nullptr),
   InstanceAccessor("depthDataFormat", &XRSession::GetDepthDataFormat, nullptr),
   ```

2. In `CreateAsync()`, after the `trackedImages` parsing block (~line 278),
   add the `depthSensing` config parsing (see architecture Section 3e).

3. After `BeginSessionAsync` resolves successfully, enable depth sensing on
   the platform:
   ```cpp
   if (session.m_depthSensingEnabled)
   {
       session.m_xr->SetDepthSensingEnabled(true);
   }
   ```

4. In the requestAnimationFrame callback path, propagate to XRFrame:
   ```cpp
   m_frame.SetDepthSensingEnabled(m_depthSensingEnabled);
   ```

5. Implement `GetDepthUsage` and `GetDepthDataFormat` (return null if
   depth sensing disabled, otherwise return the negotiated string).

### Verification

Compile and check that the session correctly parses depth config and
exposes the properties. The platform won't return depth data yet.

---

## Step 6: NativeXrImpl — Bridge Method

**File**: `Plugins/NativeXr/Source/NativeXrImpl.h`

### Changes

Add method after `CreateAugmentedImageDatabase` (~line 361):

```cpp
void SetDepthSensingEnabled(bool enabled)
{
    m_sessionState->Session->SetDepthSensingEnabled(enabled);
}
```

### Verification

Compile to confirm the bridge method links correctly.

---

## Step 7: NativeXr.cpp — Registration

**File**: `Plugins/NativeXr/Source/NativeXr.cpp`

### Changes

1. Add include (~line 14):
   ```cpp
   #include "XRCPUDepthInformation.h"
   ```

2. Add initialization call (~line 55, after `XRWebGLBinding::Initialize`):
   ```cpp
   XRCPUDepthInformation::Initialize(env);
   ```

### Verification

Full NAPI layer should compile. The JS API surface is now complete.
End-to-end test with Babylon.js (without platform data) should show:
- `session.depthUsage` returns `"cpu-optimized"`
- `session.depthDataFormat` returns `"luminance-alpha"`
- `frame.getDepthInformation(view)` returns `null` (no platform data yet)

---

## Step 8: ARCore Backend — Depth Acquisition

**File**: `Dependencies/xr/Source/ARCore/XR.cpp`

### Changes

1. Add to `Session::Impl` members (~line 240):
   ```cpp
   bool DepthSensingEnabled{false};
   std::vector<Frame::DepthSensingData> DepthSensingFrameData;
   ```

2. Add `UpdateDepthSensing()` method on `Session::Impl` (see architecture
   Section 2a). Key steps:
   - Call `ArFrame_acquireDepthImage16Bits()`
   - Copy uint16 depth data with row-stride handling
   - Release ArImage
   - Set `HasData = true`

3. Add `ComputeNormDepthBufferFromNormView()` method (see architecture
   Section 2a). Key steps:
   - Use `ArFrame_transformCoordinates2d()` to map 3 reference points
   - Derive affine matrix from the transformed coordinates
   - Pack into column-major 4×4 matrix

4. In `Frame` constructor (~line 1420), add:
   ```cpp
   m_impl->sessionImpl.UpdateDepthSensing();
   DepthSensingViews = m_impl->sessionImpl.DepthSensingFrameData;
   ```

5. Add `Session` method implementations (~line 1588):
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

### Verification

Build for Android. Test on an ARCore-capable device:
- Depth data should flow from ARCore → DepthSensingViews → NAPI
- `frame.getDepthInformation(view)` should return valid data
- `getDepthInMeters(0.5, 0.5)` should return reasonable depth values

---

## Step 9: ARKit Backend — Depth Acquisition

**File**: `Dependencies/xr/Source/ARKit/XR.mm`

### Changes

1. Add to `Session::Impl` members (~line 690):
   ```cpp
   bool DepthSensingEnabled{false};
   std::vector<Frame::DepthSensingData> DepthSensingFrameData;
   ```

2. In session configuration (where `ARWorldTrackingConfiguration` is set
   up), add scene depth frame semantics conditionally:
   ```objc
   #if (__IPHONE_OS_VERSION_MAX_ALLOWED >= 140000)
   if (DepthSensingEnabled) {
       if ([ARWorldTrackingConfiguration supportsFrameSemantics:
               ARFrameSemanticSceneDepth]) {
           configuration.frameSemantics |= ARFrameSemanticSceneDepth;
       }
   }
   #endif
   ```

3. Add `UpdateDepthSensing()` method (see architecture Section 2b). Key
   steps:
   - Read `ARFrame.sceneDepth.depthMap`
   - Lock CVPixelBuffer
   - Convert float32 meters → uint16 millimeters (clamp to 65535)
   - Unlock CVPixelBuffer
   - Set `HasData = true`

4. Add `ComputeNormDepthBufferFromNormView()` for ARKit (see architecture
   Section 2b). Use `displayTransformForOrientation:viewportSize:`.

5. In `Frame` constructor (~line 1728), add:
   ```objc
   m_impl->sessionImpl.UpdateDepthSensing();
   DepthSensingViews = m_impl->sessionImpl.DepthSensingFrameData;
   ```

6. Add `Session` method implementations (~line 1832):
   ```objc
   void System::Session::SetDepthSensingEnabled(bool enabled)
   {
       m_impl->DepthSensingEnabled = enabled;
   }

   bool System::Session::IsDepthSensingEnabled() const
   {
       return m_impl->DepthSensingEnabled;
   }
   ```

### Verification

Build for iOS. Test on a LiDAR-equipped device:
- Depth data should flow through the full pipeline
- On non-LiDAR devices, `getDepthInformation(view)` should return null
- Verify float32→uint16 conversion produces correct values

---

## Step 10: CMakeLists Update (if needed)

**File**: `Plugins/NativeXr/CMakeLists.txt`

### Changes

If the NativeXr CMakeLists uses explicit source file lists rather than
GLOBs, add `XRCPUDepthInformation.h` to the headers list.

### Verification

Confirm the new file is included in the build.

---

## Step 11: End-to-End Validation

### Test Scenarios

1. **Android (ARCore)**:
   - Request session with `depthSensing: { usagePreference: ["cpu-optimized"], dataFormatPreference: ["luminance-alpha"] }`
   - Verify `session.depthUsage === "cpu-optimized"`
   - Verify `session.depthDataFormat === "luminance-alpha"`
   - In rAF, call `frame.getDepthInformation(view)` — should return object
   - Check `.width`, `.height` are reasonable (e.g., 160×120 to 640×480)
   - Check `.data` is an ArrayBuffer of size `width * height * 2`
   - Check `.rawValueToMeters === 0.001`
   - Check `.getDepthInMeters(0.5, 0.5)` returns reasonable distance
   - Check `.normDepthBufferFromNormView.matrix` is a Float32Array(16)

2. **iOS (ARKit + LiDAR)**:
   - Same tests as Android
   - Verify depth dimensions (~256×192 typical for LiDAR)
   - Verify getDepthInMeters returns reasonable values (0.1 – 5.0 m range)

3. **iOS (ARKit, no LiDAR)**:
   - `frame.getDepthInformation(view)` should return `null`

4. **No depth sensing requested**:
   - Don't include `depthSensing` in session init
   - `session.depthUsage` should return `null`
   - `frame.getDepthInformation(view)` should return `null`

5. **Babylon.js integration**:
   - Run with Babylon.js `WebXRDepthSensing` feature enabled
   - Verify the feature attaches successfully
   - Verify depth data flows to the `RawTexture`
   - Verify occlusion rendering works if material plugin is enabled

---

## Dependency Graph

```
Step 1 (XR.h)
  ↓
Step 2 (XRCPUDepthInformation.h) ←── depends on XR.h types
  ↓
Step 3 (XRView.h) ←── no dependencies
  ↓
Step 4 (XRFrame) ←── depends on Step 2, Step 3
  ↓
Step 5 (XRSession) ←── depends on Step 4 (SetDepthSensingEnabled)
  ↓
Step 6 (NativeXrImpl) ←── depends on Step 1 (Session methods)
  ↓
Step 7 (NativeXr.cpp) ←── depends on Step 2
  ↓                        (ties everything together, NAPI complete)
Step 8 (ARCore) ←── depends on Step 1
  ↓
Step 9 (ARKit) ←── depends on Step 1
  ↓
Step 10 (CMake) ←── if needed
  ↓
Step 11 (Validation) ←── depends on everything
```

Steps 8 and 9 can be done in parallel since they're independent platform
implementations. Steps 2, 3, and 6 are simple and can potentially be
batched together.
