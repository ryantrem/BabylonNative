#include <Lite/NativeLite.h>

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep std::min/std::max usable (windows.h otherwise defines min/max macros)
#include <windows.h> // PostMessageW (wake the main-thread pump to exit after a benchmark)

namespace lite::nativelite
{
    namespace
    {
        // Env-lifetime context shared by the BabylonNativeLite closures. Holds the
        // WebGPU class module, the engine-concept class constructors, and the single
        // engine's controller. Captured as a shared_ptr by each installed function,
        // so it lives as long as the global does.
        struct Context
        {
            std::unique_ptr<webgpu::Module> webgpu;
            Napi::FunctionReference engineCtor;   // SuppressDestruct'd
            Napi::FunctionReference meshCtor;     // SuppressDestruct'd
            Napi::FunctionReference materialCtor; // SuppressDestruct'd
            Napi::FunctionReference textureCtor;  // SuppressDestruct'd
            Napi::FunctionReference nodeCtor;     // SuppressDestruct'd
            Napi::FunctionReference cameraCtor;   // SuppressDestruct'd
            std::shared_ptr<Controller> controller;
        };

        // Payload smuggled into the Engine constructor via an External.
        struct EngineInit
        {
            std::shared_ptr<Controller> controller;
            webgpu::Module* webgpu = nullptr;
        };

        // Payload smuggled into the Mesh constructor via an External.
        struct MeshInit
        {
            lite::Mesh* mesh = nullptr;
        };

        // Payload smuggled into the Material constructor via an External.
        struct MaterialInit
        {
            lite::Material* material = nullptr;
            lite::Renderer* renderer = nullptr;
        };

        // Payload smuggled into the Texture constructor via an External.
        struct TextureInit
        {
            lite::Texture* texture = nullptr;
        };

        // Payload smuggled into the Node constructor via an External.
        struct NodeInit
        {
            lite::Node* node = nullptr;
            lite::Renderer* renderer = nullptr;
        };

        // Payload smuggled into the Camera constructor via an External.
        struct CameraInit
        {
            lite::Renderer* renderer = nullptr;
        };

        // Reads N floats from a JS array-like (Array or Float32Array) into out.
        bool ReadFloats(const Napi::Value& value, float* out, uint32_t count)
        {
            if (value.IsTypedArray())
            {
                Napi::Float32Array ta = value.As<Napi::Float32Array>();
                if (ta.ElementLength() < count) return false;
                for (uint32_t i = 0; i < count; ++i) out[i] = ta[i];
                return true;
            }
            if (value.IsArray())
            {
                Napi::Array arr = value.As<Napi::Array>();
                if (arr.Length() < count) return false;
                for (uint32_t i = 0; i < count; ++i)
                    out[i] = arr.Get(i).As<Napi::Number>().FloatValue();
                return true;
            }
            return false;
        }

        // Native math: column-major mat4 helpers replicating Lite's camera math
        // (mat4-look-at-lh.ts / mat4-perspective-lh-to-ref.ts / mat4-multiply-into.ts), so
        // the per-frame view/projection LOGIC runs in C++ — native reads only the camera's
        // scalar DATA (alpha/beta/radius/target/fov/near/far) and computes the matrices here.
        void LookAtLH(float* m, const float eye[3], const float target[3], const float up[3])
        {
            float z[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
            float zl = std::sqrt(z[0]*z[0] + z[1]*z[1] + z[2]*z[2]);
            if (zl < 1e-10f) { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; return; }
            float iz = 1.0f / zl; z[0] *= iz; z[1] *= iz; z[2] *= iz;
            float x[3] = {up[1]*z[2] - up[2]*z[1], up[2]*z[0] - up[0]*z[2], up[0]*z[1] - up[1]*z[0]};
            float xl = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
            if (xl < 1e-10f) { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; return; }
            float ix = 1.0f / xl; x[0] *= ix; x[1] *= ix; x[2] *= ix;
            float y[3] = {z[1]*x[2] - z[2]*x[1], z[2]*x[0] - z[0]*x[2], z[0]*x[1] - z[1]*x[0]};
            m[0] = x[0]; m[1] = y[0]; m[2] = z[0]; m[3] = 0;
            m[4] = x[1]; m[5] = y[1]; m[6] = z[1]; m[7] = 0;
            m[8] = x[2]; m[9] = y[2]; m[10] = z[2]; m[11] = 0;
            m[12] = -(x[0]*eye[0] + x[1]*eye[1] + x[2]*eye[2]);
            m[13] = -(y[0]*eye[0] + y[1]*eye[1] + y[2]*eye[2]);
            m[14] = -(z[0]*eye[0] + z[1]*eye[1] + z[2]*eye[2]);
            m[15] = 1;
        }
        void PerspectiveReverseZLH(float* m, float fov, float aspect, float near_, float far_)
        {
            for (int i = 0; i < 16; ++i) m[i] = 0.0f;
            float tan = 1.0f / std::tan(fov * 0.5f);
            float range = far_ - near_;
            m[0] = tan / aspect; m[5] = tan; m[10] = -near_ / range; m[11] = 1.0f; m[14] = (far_ * near_) / range;
        }
        // out = a * b (column-major), matching mat4MultiplyInto(out, a, b).
        void Mat4Multiply(float* out, const float* a, const float* b)
        {
            for (int c = 0; c < 4; ++c)
            {
                float b0 = b[c*4+0], b1 = b[c*4+1], b2 = b[c*4+2], b3 = b[c*4+3];
                out[c*4+0] = a[0]*b0 + a[4]*b1 + a[8]*b2 + a[12]*b3;
                out[c*4+1] = a[1]*b0 + a[5]*b1 + a[9]*b2 + a[13]*b3;
                out[c*4+2] = a[2]*b0 + a[6]*b1 + a[10]*b2 + a[14]*b3;
                out[c*4+3] = a[3]*b0 + a[7]*b1 + a[11]*b2 + a[15]*b3;
            }
        }

        // Per-frame scene-UBO write (native dynamics). Reads the arc-rotate camera's scalar
        // DATA off the JS object and computes view + reverse-Z projection + viewProj natively,
        // then writes the SceneUniforms head (viewProj @0, view @16, eye @32) into the
        // task-owned scene UBO. Preserves the tail (envRot/canvas/SH/exposure) written at
        // setup by writing only the first 36 floats. `extraAlpha` lets the native loop orbit
        // the camera (proving per-frame writes drive visible motion) without mutating JS.
        // Returns false if the camera/UBO data isn't an arc-rotate-shaped object.
        bool WriteSceneUBOFromCamera(const webgpu::Module* mod, wgpu::Queue& queue,
            const Napi::Object& scene, const Napi::Object& task, float aspect, float extraAlpha)
        {
            Napi::Value camVal = scene.Get("camera");
            if (!camVal.IsObject()) return false;
            Napi::Object cam = camVal.As<Napi::Object>();
            // Read the orbit pose from the camera's plain-data `_scalars` mirror (alpha/beta/
            // radius/targetX/Y/Z) rather than the alpha/beta/radius/target getters — reading
            // a JS accessor would execute JS, which the render loop must not do. `_scalars` is
            // kept current by the camera's setters/target-sync. fov/nearPlane/farPlane are
            // plain data props (no getters), so they're read directly.
            Napi::Value scalarsV = cam.Get("_scalars");
            if (!scalarsV.IsObject()) return false; // not an arc-rotate camera — leave UBO as-is
            Napi::Object s = scalarsV.As<Napi::Object>();
            Napi::Value alphaV = s.Get("alpha");
            Napi::Value betaV = s.Get("beta");
            Napi::Value radiusV = s.Get("radius");
            if (!alphaV.IsNumber() || !betaV.IsNumber() || !radiusV.IsNumber())
                return false;
            float alpha = alphaV.As<Napi::Number>().FloatValue() + extraAlpha;
            float beta = betaV.As<Napi::Number>().FloatValue();
            float radius = radiusV.As<Napi::Number>().FloatValue();
            float target[3] = {
                s.Get("targetX").IsNumber() ? s.Get("targetX").As<Napi::Number>().FloatValue() : 0.0f,
                s.Get("targetY").IsNumber() ? s.Get("targetY").As<Napi::Number>().FloatValue() : 0.0f,
                s.Get("targetZ").IsNumber() ? s.Get("targetZ").As<Napi::Number>().FloatValue() : 0.0f,
            };
            float fov = cam.Get("fov").IsNumber() ? cam.Get("fov").As<Napi::Number>().FloatValue() : 0.8f;
            float near_ = cam.Get("nearPlane").IsNumber() ? cam.Get("nearPlane").As<Napi::Number>().FloatValue() : 0.1f;
            float far_ = cam.Get("farPlane").IsNumber() ? cam.Get("farPlane").As<Napi::Number>().FloatValue() : 1000.0f;

            // eye = target + radius * (cosA*sinB, cosB, sinA*sinB)  (arc-rotate localEyePosition)
            float cosA = std::cos(alpha), sinA = std::sin(alpha);
            float cosB = std::cos(beta), sinB = std::sin(beta);
            if (sinB == 0.0f) sinB = 0.0001f;
            float eye[3] = {
                target[0] + radius * cosA * sinB,
                target[1] + radius * cosB,
                target[2] + radius * sinA * sinB,
            };
            float up[3] = {0.0f, 1.0f, 0.0f};
            float view[16], proj[16], viewProj[16];
            LookAtLH(view, eye, target, up);
            PerspectiveReverseZLH(proj, fov, aspect, near_, far_);
            Mat4Multiply(viewProj, proj, view);

            // Full SceneUniforms (floats 0-79), packed natively from JS DATA (no JS exec):
            //   viewProj(0-15), view(16-31), eye(32-34), pad(35), envRotationY(36),
            //   canvasHeight(37), pad(38-39), irradiance SH(40-75), exposure(76),
            //   contrast(77), lodGenerationScale(78), toneMappingEnabled(79).
            // Mirrors _packSceneUniforms + the writeEnvShUbo contributor, so the diffuse IBL
            // (SH) + specular LOD + image processing reach the shader every frame. Fog/clip
            // (80+) are left to the setup-primed tail (unused by this scene).
            float ubo[80] = {0};
            for (int i = 0; i < 16; ++i) ubo[i] = viewProj[i];
            for (int i = 0; i < 16; ++i) ubo[16 + i] = view[i];
            ubo[32] = eye[0]; ubo[33] = eye[1]; ubo[34] = eye[2];

            // envRotationY + canvas height (engine.canvas.{width,height} via scene.surface.engine).
            Napi::Value envRotV = scene.Get("envRotationY");
            ubo[36] = envRotV.IsNumber() ? envRotV.As<Napi::Number>().FloatValue() : 0.0f;

            // Image processing (scene.imageProcessing.{exposure,contrast,toneMappingEnabled}).
            float exposure = 1.0f, contrast = 1.0f, lodScale = 0.8f, toneMap = 0.0f;
            Napi::Value ipVal = scene.Get("imageProcessing");
            if (ipVal.IsObject())
            {
                Napi::Object ip = ipVal.As<Napi::Object>();
                if (ip.Get("exposure").IsNumber()) exposure = ip.Get("exposure").As<Napi::Number>().FloatValue();
                if (ip.Get("contrast").IsNumber()) contrast = ip.Get("contrast").As<Napi::Number>().FloatValue();
                Napi::Value tm = ip.Get("toneMappingEnabled");
                if (tm.IsBoolean()) toneMap = tm.As<Napi::Boolean>().Value() ? 1.0f : 0.0f;
            }

            // Irradiance SH (scene._envTextures.sphericalHarmonics — 36 pre-scaled floats) +
            // lodGenerationScale. Absent for non-IBL scenes (SH stays 0).
            Napi::Value envTexVal = scene.Get("_envTextures");
            if (envTexVal.IsObject())
            {
                Napi::Object envTex = envTexVal.As<Napi::Object>();
                Napi::Value shVal = envTex.Get("sphericalHarmonics");
                if (shVal.IsTypedArray())
                {
                    Napi::Float32Array sh = shVal.As<Napi::Float32Array>();
                    uint32_t n = sh.ElementLength() < 36 ? sh.ElementLength() : 36;
                    for (uint32_t i = 0; i < n; ++i) ubo[40 + i] = sh[i];
                }
                if (envTex.Get("lodGenerationScale").IsNumber())
                    lodScale = envTex.Get("lodGenerationScale").As<Napi::Number>().FloatValue();
            }

            ubo[76] = exposure;
            ubo[77] = contrast;
            ubo[78] = lodScale;
            ubo[79] = toneMap;

            Napi::Value uboVal = task.Get("_sceneUBO");
            if (!uboVal.IsObject()) return false;
            auto* uboBuf = mod->AsBuffer(uboVal.As<Napi::Object>());
            if (uboBuf == nullptr) return false;
            queue.WriteBuffer(uboBuf->Handle(), 0, ubo, sizeof(ubo));
            return true;
        }

        // Issue one DrawBinding's draw commands natively by READING its exposed DrawCommand
        // data (binding._draw) — the C++ equivalent of Lite's standalone recordDrawBinding.
        // Returns true if the draw was issued from data. The scene bind group (group 0) is
        // set once by the caller; group 0 is never listed in cmd.bindGroups. Reads only
        // readable fields (vertex/index buffers, bind groups, counts) — no JS execution.
        bool IssueDrawBindingNative(const webgpu::Module* mod, wgpu::RenderPassEncoder& pass,
            const Napi::Object& binding)
        {
            Napi::Value drawVal = binding.Get("_draw");
            if (!drawVal.IsObject()) return false; // dynamic binding (thin-instance/cull): no data path
            Napi::Object cmd = drawVal.As<Napi::Object>();

            // pipeline (binding.pipeline)
            if (binding.Get("pipeline").IsObject())
            {
                if (auto* p = mod->AsRenderPipeline(binding.Get("pipeline").As<Napi::Object>()))
                    pass.SetPipeline(p->Handle());
            }

            // vertex buffers: cmd.vertexBuffers[] = { buffer, byteOffset }
            Napi::Value vbsVal = cmd.Get("vertexBuffers");
            if (vbsVal.IsArray())
            {
                Napi::Array vbs = vbsVal.As<Napi::Array>();
                for (uint32_t i = 0; i < vbs.Length(); ++i)
                {
                    if (!vbs.Get(i).IsObject()) continue;
                    Napi::Object vb = vbs.Get(i).As<Napi::Object>();
                    auto* buf = vb.Get("buffer").IsObject() ? mod->AsBuffer(vb.Get("buffer").As<Napi::Object>()) : nullptr;
                    uint64_t off = vb.Get("byteOffset").IsNumber() ? static_cast<uint64_t>(vb.Get("byteOffset").As<Napi::Number>().Int64Value()) : 0;
                    if (buf != nullptr) pass.SetVertexBuffer(i, buf->Handle(), off);
                }
            }

            // index buffer: cmd.indexBuffer + cmd.indexFormat
            auto* ib = cmd.Get("indexBuffer").IsObject() ? mod->AsBuffer(cmd.Get("indexBuffer").As<Napi::Object>()) : nullptr;
            std::string ifmt = cmd.Get("indexFormat").IsString() ? cmd.Get("indexFormat").As<Napi::String>().Utf8Value() : "uint32";
            wgpu::IndexFormat indexFormat = ifmt == "uint16" ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32;
            if (ib != nullptr) pass.SetIndexBuffer(ib->Handle(), indexFormat);

            // bind groups: cmd.bindGroups[] = { group, bindGroup } (group 0 excluded)
            Napi::Value bgsVal = cmd.Get("bindGroups");
            if (bgsVal.IsArray())
            {
                Napi::Array bgs = bgsVal.As<Napi::Array>();
                for (uint32_t i = 0; i < bgs.Length(); ++i)
                {
                    if (!bgs.Get(i).IsObject()) continue;
                    Napi::Object e = bgs.Get(i).As<Napi::Object>();
                    uint32_t group = e.Get("group").IsNumber() ? e.Get("group").As<Napi::Number>().Uint32Value() : 0;
                    auto* bg = e.Get("bindGroup").IsObject() ? mod->AsBindGroup(e.Get("bindGroup").As<Napi::Object>()) : nullptr;
                    if (bg != nullptr) pass.SetBindGroup(group, bg->Handle());
                }
            }

            uint32_t indexCount = cmd.Get("indexCount").IsNumber() ? cmd.Get("indexCount").As<Napi::Number>().Uint32Value() : 0;
            uint32_t instanceCount = cmd.Get("instanceCount").IsNumber() ? cmd.Get("instanceCount").As<Napi::Number>().Uint32Value() : 1;
            pass.DrawIndexed(indexCount, instanceCount, 0, 0, 0);
            return true;
        }

        // Native executor for the "forward-render" frame-graph task kind — the C++ mirror of
        // Lite's executeForwardRenderTask / executePassBody. Reads the task DATA (sceneBG,
        // opaque/direct/transparent binding lists, offscreen MSAA color + depth views, clear
        // state) and the scene camera/env data, writes the task's scene UBO natively, then
        // records ONE render pass into the shared frame command `encoder`: begin (MSAA color
        // resolving into `swapchainView`, + depth), setBindGroup(0), draw opaque, draw direct,
        // draw transparent, end. Zero JS. Returns false if the task data isn't drawable.
        //
        // Scope notes (sufficient for the current scenes; extend as needed): the transparent
        // back-to-front re-sort and per-binding dynamic UBO updates that Lite's
        // prepareRenderTaskPass does in JS are not yet mirrored here — opaque/direct/static
        // content is correct; animated transparent ordering would need those added.
        bool ExecuteForwardRenderTaskNative(const webgpu::Module* mod, wgpu::Device& wgpuDevice,
            wgpu::Queue& queue, wgpu::CommandEncoder& encoder, const Napi::Object& scene,
            const Napi::Object& task, const wgpu::TextureView& swapchainView)
        {
            // sceneBG (group 0) + binding lists.
            wgpu::BindGroup sceneBG;
            if (task.Get("_sceneBG").IsObject())
            {
                if (auto* bg = mod->AsBindGroup(task.Get("_sceneBG").As<Napi::Object>())) sceneBG = bg->Handle();
            }
            Napi::Array opaqueBindings, directBindings, transparentBindings;
            if (task.Get("_opaqueBindings").IsArray()) opaqueBindings = task.Get("_opaqueBindings").As<Napi::Array>();
            if (task.Get("_directBindings").IsArray()) directBindings = task.Get("_directBindings").As<Napi::Array>();
            if (task.Get("_transparentBindings").IsArray()) transparentBindings = task.Get("_transparentBindings").As<Napi::Array>();

            // Opaque render bundle (production path). Lite's buildOpaqueRenderBundle records
            // group(0) + every opaque draw into task._opaqueBundles[0] at setup, and rebuilds
            // it ONLY when the scene mutates (_renderableVersion) or visibility (_vis) changes.
            // Native replays that one GPURenderBundle with a SINGLE executeBundles call — zero
            // per-draw JS — and uses Lite's own dirty signal to stay correct: if the bundle is
            // stale (renderableVersion advanced past the version it was built at, i.e.
            // _lastVersion) or absent (forceNoOpaqueBundle benchmark mode), native falls back to
            // the per-draw drawList from the CURRENT bindings (always correct, just slower).
            // Reading _lastVersion/_renderableVersion is the native equivalent of executePassBody's
            // JS staleness check — the answer to "how do we know when to re-read cached data".
            wgpu::RenderBundle opaqueBundle;
            {
                Napi::Value obVal = task.Get("_opaqueBundles");
                if (obVal.IsArray())
                {
                    Napi::Array ob = obVal.As<Napi::Array>();
                    if (ob.Length() > 0 && ob.Get(0u).IsObject())
                        if (auto* rb = mod->AsRenderBundle(ob.Get(0u).As<Napi::Object>())) opaqueBundle = rb->Handle();
                }
            }
            bool opaqueBundleFresh = false;
            if (opaqueBundle != nullptr)
            {
                // Compare the version the bundle was built at (task._lastVersion) against the
                // scene's current renderable version. Equal → the cached bundle still matches
                // the renderable set, so replay is safe. (Plain-data int reads; no JS exec.)
                Napi::Value lastVerV = task.Get("_lastVersion");
                Napi::Value curVerV = scene.Get("_renderableVersion");
                if (lastVerV.IsNumber() && curVerV.IsNumber())
                    opaqueBundleFresh = lastVerV.As<Napi::Number>().Int64Value() == curVerV.As<Napi::Number>().Int64Value();
                else
                    opaqueBundleFresh = true; // versions absent → trust the setup-built bundle
            }

            // Offscreen MSAA color + depth views (stable across frames) + rt dims from _config.rt.
            wgpu::TextureView msaaColorView, depthView;
            float rtWidth = 1.0f, rtHeight = 1.0f;
            Napi::Value cfgVal = task.Get("_config");
            if (cfgVal.IsObject() && cfgVal.As<Napi::Object>().Get("rt").IsObject())
            {
                Napi::Object rt = cfgVal.As<Napi::Object>().Get("rt").As<Napi::Object>();
                if (rt.Get("_colorView").IsObject())
                    if (auto* v = mod->AsTextureView(rt.Get("_colorView").As<Napi::Object>())) msaaColorView = v->Handle();
                if (rt.Get("_depthView").IsObject())
                    if (auto* v = mod->AsTextureView(rt.Get("_depthView").As<Napi::Object>())) depthView = v->Handle();
                if (rt.Get("_width").IsNumber()) rtWidth = rt.Get("_width").As<Napi::Number>().FloatValue();
                if (rt.Get("_height").IsNumber()) rtHeight = rt.Get("_height").As<Napi::Number>().FloatValue();
            }

            // clearColor (scene.clearColor {r,g,b,a}).
            float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (scene.Get("clearColor").IsObject())
            {
                Napi::Object cc = scene.Get("clearColor").As<Napi::Object>();
                auto rd = [&](const char* k, float dflt) {
                    Napi::Value v = cc.Get(k);
                    return v.IsNumber() ? v.As<Napi::Number>().FloatValue() : dflt;
                };
                clear[0] = rd("r", 0.0f); clear[1] = rd("g", 0.0f);
                clear[2] = rd("b", 0.0f); clear[3] = rd("a", 1.0f);
            }

            // depthClearValue + stencil presence from the recorded render-pass descriptor.
            float depthClear = 1.0f;
            bool hasStencil = false;
            Napi::Value rpdVal = task.Get("_renderPassDescriptor");
            if (rpdVal.IsObject() && rpdVal.As<Napi::Object>().Get("depthStencilAttachment").IsObject())
            {
                Napi::Object dsa = rpdVal.As<Napi::Object>().Get("depthStencilAttachment").As<Napi::Object>();
                if (dsa.Get("depthClearValue").IsNumber()) depthClear = dsa.Get("depthClearValue").As<Napi::Number>().FloatValue();
                hasStencil = dsa.Get("stencilLoadOp").IsString();
            }

            if (sceneBG == nullptr || msaaColorView == nullptr) return false;

            // Per-frame scene UBO (camera viewProj/view/eye + env SH + image processing).
            float aspect = rtWidth / rtHeight;
            WriteSceneUBOFromCamera(mod, queue, scene, task, aspect, 0.0f);

            wgpu::RenderPassColorAttachment color{};
            color.view = msaaColorView;            // offscreen MSAA color
            color.resolveTarget = swapchainView;   // resolve into the swapchain
            color.loadOp = wgpu::LoadOp::Clear;
            color.storeOp = wgpu::StoreOp::Store;
            color.clearValue = wgpu::Color{clear[0], clear[1], clear[2], clear[3]};

            wgpu::RenderPassDepthStencilAttachment depth{};
            bool hasDepth = depthView != nullptr;
            if (hasDepth)
            {
                depth.view = depthView;
                depth.depthLoadOp = wgpu::LoadOp::Clear;
                depth.depthStoreOp = wgpu::StoreOp::Store;
                depth.depthClearValue = depthClear;
                if (hasStencil)
                {
                    depth.stencilLoadOp = wgpu::LoadOp::Clear;
                    depth.stencilStoreOp = wgpu::StoreOp::Store;
                    depth.stencilClearValue = 0;
                }
            }

            wgpu::RenderPassDescriptor passDesc{};
            passDesc.colorAttachmentCount = 1;
            passDesc.colorAttachments = &color;
            passDesc.depthStencilAttachment = hasDepth ? &depth : nullptr;

            wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
            pass.SetBindGroup(0, sceneBG); // group 0 set once for the whole pass
            auto drawList = [&](const Napi::Array& list) {
                if (list.IsEmpty()) return;
                for (uint32_t i = 0; i < list.Length(); ++i)
                {
                    if (!list.Get(i).IsObject()) continue;
                    IssueDrawBindingNative(mod, pass, list.Get(i).As<Napi::Object>());
                }
            };
            // Opaque: replay Lite's cached bundle (1 executeBundles, zero per-draw JS) when it's
            // present and fresh; otherwise per-draw fallback from current bindings. executeBundles
            // resets the pass's bind-group state, so rebind group 0 before the direct/transparent
            // draws — exactly as Lite's executePassBody does after its own executeBundles.
            if (opaqueBundle != nullptr && opaqueBundleFresh)
            {
                wgpu::RenderBundle bundles[1] = {opaqueBundle};
                pass.ExecuteBundles(1, bundles);
                pass.SetBindGroup(0, sceneBG);
            }
            else
            {
                if (opaqueBundle != nullptr && !opaqueBundleFresh)
                {
                    static bool warnedStale = false;
                    if (!warnedStale)
                    {
                        warnedStale = true;
                        std::fprintf(stderr, "[nativelite] opaque bundle stale (scene mutated); "
                            "using per-draw fallback until Lite rebuilds it\n");
                    }
                }
                drawList(opaqueBindings);
            }
            drawList(directBindings);
            drawList(transparentBindings);
            pass.End();
            return true;
        }

        // ---- Real Babylon Lite native render loop (Stage B) ----------------------
        // Pure read-only native rendering: native NEVER calls JS — it READS the real Lite
        // engine's live JS structures and ISSUES the GPU draws itself in C++.
        //
        // Generic frame-graph walk: native iterates scene._frameGraph._tasks and dispatches
        // each by its data `_kind` tag to a native executor (the C++ mirror of Lite's
        // frameGraph.execute → task._execute). Implemented kinds run entirely in C++ (zero
        // JS); a task whose kind native doesn't implement is skipped with a one-time warning
        // (a JS-fallback delegate to task._execute can be added when such scenes appear).
        // Setup (real Lite JS, at registerScene) built every task's pipelines/UBOs/bind
        // groups/binding lists and exposed each binding's draw-state as readable DrawCommand
        // data — native only reads it.
        void RealRenderFrame(Napi::Env env, std::shared_ptr<Controller> controller)
        {
            const webgpu::Module* mod = controller->webgpu;
            if (mod == nullptr || controller->realEngine.IsEmpty()) return;

            Napi::Object engine = controller->realEngine.Value().As<Napi::Object>();

            // Time the native render-loop work (everything up to Submit, excluding Present)
            // for the benchmark — the apples-to-apples "render-loop CPU per frame" metric.
            auto cpuStart = std::chrono::steady_clock::now();

            Napi::Value devVal = engine.Get("_device");
            if (!devVal.IsObject()) return;
            webgpu::Device* device = Napi::ObjectWrap<webgpu::Device>::Unwrap(devVal.As<Napi::Object>());
            if (device == nullptr) return;
            wgpu::Device wgpuDevice = device->Handle();
            wgpu::Queue queue = wgpuDevice.GetQueue();

            // scene = engine.surfaces[0]._renderingContexts[0]
            Napi::Value surfacesVal = engine.Get("surfaces");
            if (!surfacesVal.IsArray()) return;
            Napi::Array surfaces = surfacesVal.As<Napi::Array>();
            if (surfaces.Length() == 0 || !surfaces.Get(0u).IsObject()) return;
            Napi::Object surface = surfaces.Get(0u).As<Napi::Object>();
            Napi::Value rcsVal = surface.Get("_renderingContexts");
            if (!rcsVal.IsArray()) return;
            Napi::Array rcs = rcsVal.As<Napi::Array>();
            if (rcs.Length() == 0 || !rcs.Get(0u).IsObject()) return;
            Napi::Object scene = rcs.Get(0u).As<Napi::Object>();

            // tasks = scene._frameGraph._tasks
            Napi::Value fgVal = scene.Get("_frameGraph");
            if (!fgVal.IsObject()) return;
            Napi::Value tasksVal = fgVal.As<Napi::Object>().Get("_tasks");
            if (!tasksVal.IsArray()) return;
            Napi::Array tasks = tasksVal.As<Napi::Array>();
            if (tasks.Length() == 0) return;

            // Acquire this frame's swapchain texture once (the MSAA resolve target shared by
            // any task that resolves into the swapchain).
            wgpu::SurfaceTexture surfaceTexture{};
            mod->Surface().GetCurrentTexture(&surfaceTexture);
            if (surfaceTexture.texture == nullptr) return;
            const_cast<webgpu::Module*>(mod)->NoteAcquiredTexture(surfaceTexture.texture);
            wgpu::TextureView swapchainView = surfaceTexture.texture.CreateView();

            // One command encoder per frame; every task records into it; submit once.
            wgpu::CommandEncoder encoder = wgpuDevice.CreateCommandEncoder();

            for (uint32_t i = 0; i < tasks.Length(); ++i)
            {
                if (!tasks.Get(i).IsObject()) continue;
                Napi::Object task = tasks.Get(i).As<Napi::Object>();
                std::string kind = task.Get("_kind").IsString() ? task.Get("_kind").As<Napi::String>().Utf8Value() : "";
                if (kind == "forward-render")
                {
                    ExecuteForwardRenderTaskNative(mod, wgpuDevice, queue, encoder, scene, task, swapchainView);
                }
                else
                {
                    static bool warned = false;
                    if (!warned)
                    {
                        warned = true;
                        std::fprintf(stderr, "[nativelite] frame-graph task kind '%s' not implemented natively; skipped\n", kind.c_str());
                    }
                }
            }
            wgpu::CommandBuffer cb = encoder.Finish();
            queue.Submit(1, &cb);
            double cpuMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cpuStart).count();

            const_cast<webgpu::Module*>(mod)->Present();
            int n = controller->frameCounter.fetch_add(1);
            if (n % 120 == 0)
                std::fprintf(stderr, "[nativelite] native draw frame %d (zero JS, data-driven)\n", n);
            controller->RecordFrameAndMaybeFinish(cpuMs);
        }


        // The native pump for the real-Lite path: render one native frame, then
        // re-dispatch onto the JS thread's event loop. Zero JS executes per frame
        // (native only READS the JS engine object). Replaces real Lite's JS
        // startEngine + requestAnimationFrame.
        void RealPump(std::shared_ptr<Controller> controller)
        {
            if (!controller->running.load()) return;
            controller->runtime->Dispatch([controller](Napi::Env env) {
                RealRenderFrame(env, controller);
                RealPump(controller);
            });
        }

        // The native "rAF pump": optionally calls the per-frame JS hook, renders one
        // frame, then re-dispatches itself onto the JS thread's event loop. With no
        // hook registered, zero JS runs per frame; with a hook, it's a direct
        // same-thread call (we ARE on the JS thread). Stops re-dispatching once
        // `running` is cleared; Dawn teardown is handled separately so this never
        // races with it.
        void Pump(std::shared_ptr<Controller> controller)
        {
            if (!controller->running.load())
            {
                return;
            }

            if (!controller->onBeforeRender.IsEmpty())
            {
                try
                {
                    controller->onBeforeRender.Call({});
                }
                catch (const Napi::Error& error)
                {
                    std::fprintf(stderr, "[js] onBeforeRender threw: %s\n", error.what());
                }
            }

            controller->renderer->RenderFrame();

            controller->runtime->Dispatch([controller](Napi::Env) {
                Pump(controller);
            });
        }
    }

    // Benchmark timing — JS-thread-only state held in function-local statics (one benchmark
    // at a time). Receives each frame's render-loop CPU time (ms, present excluded); after
    // benchFrames samples (the first is dropped as warm-up) prints the BENCH line + posts
    // WM_CLOSE to exit.
    void Controller::RecordFrameAndMaybeFinish(double frameCpuMs)
    {
        if (benchFrames == 0) return; // benchmarking disabled

        static bool s_done = false;
        static bool s_droppedWarmup = false;
        static std::vector<double> s_samples;
        static std::chrono::steady_clock::time_point s_wallStart; // wall clock at first sample
        if (s_done) return;

        if (!s_droppedWarmup)
        {
            s_droppedWarmup = true; // drop frame 0 (shader compile / first uploads)
        }
        else if (s_samples.size() < benchFrames)
        {
            if (s_samples.capacity() == 0) s_samples.reserve(benchFrames);
            if (s_samples.empty()) s_wallStart = std::chrono::steady_clock::now(); // start wall timer
            s_samples.push_back(frameCpuMs);
        }

        if (s_samples.size() >= benchFrames)
        {
            s_done = true;
            // Wall-clock throughput across the whole measured window: total elapsed real time
            // from the first to the last sample, divided by frame count. This is the HONEST
            // end-to-end metric (includes submit, present, and any blocking) — i.e. "absolute
            // FPS with no vsync", which the per-frame CPU number (present-excluded, JS-thread
            // only) does not capture. wall_ms = real ms/frame; wall_fps = 1000 / wall_ms.
            double wallMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - s_wallStart).count();
            std::vector<double> sorted = s_samples;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0.0;
            for (double v : s_samples) sum += v;
            const size_t n = s_samples.size();
            double avg = n ? sum / n : 0.0;
            double mn = sorted.front();
            double mx = sorted.back();
            double p95 = sorted[std::min(n - 1, static_cast<size_t>(n * 0.95))];
            double wallPerFrame = n ? wallMs / n : 0.0;
            double wallFps = wallPerFrame > 0.0 ? 1000.0 / wallPerFrame : 0.0;
            std::fprintf(stdout,
                "BENCH scene=%s loop=%s frames=%zu cpu_avg_ms=%.4f cpu_min_ms=%.4f cpu_max_ms=%.4f cpu_p95_ms=%.4f wall_ms_per_frame=%.4f wall_fps=%.1f\n",
                sceneLabel.c_str(), loopLabel.c_str(), n, avg, mn, mx, p95, wallPerFrame, wallFps);
            std::fflush(stdout);
            std::fprintf(stderr, "[nativelite] benchmark complete (%zu frames, render-loop CPU avg %.4f ms, wall %.4f ms/frame = %.1f fps)\n",
                n, avg, wallPerFrame, wallFps);
            running = false;
            jsLoopRunning = false;
            if (window.hwnd != nullptr)
                ::PostMessageW(static_cast<HWND>(window.hwnd), WM_CLOSE, 0, 0);
        }
    }

    void Controller::RequestShutdownAndWait()
    {
        running = false;
        jsLoopRunning = false;
        if (runtime == nullptr)
        {
            return;
        }
        std::promise<void> done;
        runtime->Dispatch([this, &done](Napi::Env) {
            if (!onBeforeRender.IsEmpty())
            {
                onBeforeRender.Reset();
            }
            if (!realEngine.IsEmpty())
            {
                realEngine.Reset();
            }
            if (renderer)
            {
                renderer->Shutdown();
            }
            done.set_value();
        });
        done.get_future().wait();
    }

    // --------------------------------------------------------------------- Engine
    Napi::Function Engine::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Engine>::DefineClass(env, "Engine", {
            Napi::ObjectWrap<Engine>::InstanceAccessor<&Engine::GetDevice>("_device"),
            Napi::ObjectWrap<Engine>::InstanceAccessor<&Engine::GetFrameBuffer>("frameBuffer"),
            Napi::ObjectWrap<Engine>::InstanceAccessor<&Engine::GetColorFormat>("colorFormat"),
            Napi::ObjectWrap<Engine>::InstanceAccessor<&Engine::GetDepthFormat>("depthFormat"),
            Napi::ObjectWrap<Engine>::InstanceMethod<&Engine::SetBeforeRender>("setBeforeRender"),
        });
    }

    Engine::Engine(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<Engine>{info}
    {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsExternal())
        {
            Napi::Error::New(env, "Engine is not constructible from JavaScript")
                .ThrowAsJavaScriptException();
            return;
        }

        auto* init = info[0].As<Napi::External<EngineInit>>().Data();
        m_controller = init->controller;

        // Build the wrapped WebGPU device over the SAME Dawn device the renderer
        // uses. engine._device is therefore a real GPUDevice.
        m_device = Napi::Persistent(init->webgpu->CreateDevice(env, m_controller->renderer->Device()));
    }

    Napi::Value Engine::GetDevice(const Napi::CallbackInfo& info)
    {
        if (m_device.IsEmpty())
        {
            return info.Env().Undefined();
        }
        return m_device.Value();
    }

    // frameBuffer — the camera/lights/viewProj uniform the native loop writes each
    // frame, as a GPUBuffer, so a JS group-0 bind group can reference it (Model B).
    Napi::Value Engine::GetFrameBuffer(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        const webgpu::Module* mod = webgpu::Module::Current();
        if (mod == nullptr || m_controller == nullptr || m_controller->renderer == nullptr)
            return env.Undefined();
        return mod->WrapBuffer(env, m_controller->renderer->FrameBuffer());
    }

    Napi::Value Engine::GetColorFormat(const Napi::CallbackInfo& info)
    {
        return Napi::String::New(info.Env(), m_controller->renderer->ColorFormatString());
    }

    Napi::Value Engine::GetDepthFormat(const Napi::CallbackInfo& info)
    {
        return Napi::String::New(info.Env(), m_controller->renderer->DepthFormatString());
    }

    Napi::Value Engine::SetBeforeRender(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        if (info.Length() >= 1 && info[0].IsFunction())
        {
            m_controller->onBeforeRender = Napi::Persistent(info[0].As<Napi::Function>());
        }
        return env.Undefined();
    }

    // ----------------------------------------------------------------------- Mesh
    Napi::Function Mesh::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Mesh>::DefineClass(env, "Mesh", {});
    }

    Mesh::Mesh(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<Mesh>{info}
    {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsExternal())
        {
            Napi::Error::New(env, "Mesh is not constructible from JavaScript")
                .ThrowAsJavaScriptException();
            return;
        }
        m_mesh = info[0].As<Napi::External<MeshInit>>().Data()->mesh;
    }

    // ----------------------------------------------------------------------- Node
    Napi::Function Node::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Node>::DefineClass(env, "Node", {
            Napi::ObjectWrap<Node>::InstanceMethod<&Node::SetTransform>("setTransform"),
            Napi::ObjectWrap<Node>::InstanceAccessor<&Node::GetModelBuffer>("modelBuffer"),
        });
    }

    Node::Node(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<Node>{info}
    {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsExternal())
        {
            Napi::Error::New(env, "Node is not constructible from JavaScript")
                .ThrowAsJavaScriptException();
            return;
        }
        auto* init = info[0].As<Napi::External<NodeInit>>().Data();
        m_node = init->node;
        m_renderer = init->renderer;
    }

    // setTransform(position[3], rotation[3], scale[3]) — each an Array or Float32Array.
    // Sets the node's LOCAL transform; the world matrix is composed natively per frame.
    Napi::Value Node::SetTransform(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        float position[3] = {0, 0, 0};
        float rotation[3] = {0, 0, 0};
        float scale[3] = {1, 1, 1};
        if (info.Length() >= 1) ReadFloats(info[0], position, 3);
        if (info.Length() >= 2) ReadFloats(info[1], rotation, 3);
        if (info.Length() >= 3) ReadFloats(info[2], scale, 3);
        m_renderer->SetNodeTransform(m_node, position, rotation, scale);
        return env.Undefined();
    }

    // modelBuffer — the node's per-frame mat4 world-matrix uniform, as a GPUBuffer, so
    // a JS-built bind group (Model B) can reference the same buffer native writes.
    Napi::Value Node::GetModelBuffer(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        const webgpu::Module* mod = webgpu::Module::Current();
        if (mod == nullptr || m_renderer == nullptr) return env.Undefined();
        return mod->WrapBuffer(env, m_renderer->NodeModelBuffer(m_node));
    }

    // ------------------------------------------------------------------- Material
    Napi::Function Material::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Material>::DefineClass(env, "Material", {
            Napi::ObjectWrap<Material>::InstanceMethod<&Material::SetColor>("setColor"),
        });
    }

    Material::Material(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<Material>{info}
    {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsExternal())
        {
            Napi::Error::New(env, "Material is not constructible from JavaScript")
                .ThrowAsJavaScriptException();
            return;
        }
        auto* init = info[0].As<Napi::External<MaterialInit>>().Data();
        m_material = init->material;
        m_renderer = init->renderer;
    }

    // setColor(r, g, b[, a]) or setColor([r,g,b,a])
    Napi::Value Material::SetColor(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        float color[4] = {1, 1, 1, 1};
        if (info.Length() == 1 && (info[0].IsArray() || info[0].IsTypedArray()))
        {
            ReadFloats(info[0], color, 4);
        }
        else
        {
            if (info.Length() >= 1) color[0] = info[0].As<Napi::Number>().FloatValue();
            if (info.Length() >= 2) color[1] = info[1].As<Napi::Number>().FloatValue();
            if (info.Length() >= 3) color[2] = info[2].As<Napi::Number>().FloatValue();
            if (info.Length() >= 4) color[3] = info[3].As<Napi::Number>().FloatValue();
        }
        m_renderer->SetMaterialColor(m_material, color);
        return env.Undefined();
    }

    // -------------------------------------------------------------------- Texture
    Napi::Function Texture::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Texture>::DefineClass(env, "Texture", {});
    }

    Texture::Texture(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<Texture>{info}
    {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsExternal())
        {
            Napi::Error::New(env, "Texture is not constructible from JavaScript")
                .ThrowAsJavaScriptException();
            return;
        }
        m_texture = info[0].As<Napi::External<TextureInit>>().Data()->texture;
    }

    // --------------------------------------------------------------------- Camera
    Napi::Function Camera::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Camera>::DefineClass(env, "Camera", {
            Napi::ObjectWrap<Camera>::InstanceMethod<&Camera::SetProjection>("setProjection"),
            Napi::ObjectWrap<Camera>::InstanceMethod<&Camera::SetView>("setView"),
        });
    }

    Camera::Camera(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<Camera>{info}
    {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsExternal())
        {
            Napi::Error::New(env, "Camera is not constructible from JavaScript")
                .ThrowAsJavaScriptException();
            return;
        }
        m_renderer = info[0].As<Napi::External<CameraInit>>().Data()->renderer;
    }

    // setProjection(fovYRadians, aspect, near, far)
    Napi::Value Camera::SetProjection(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        if (info.Length() < 4)
        {
            Napi::TypeError::New(env, "setProjection(fovY, aspect, near, far) expected")
                .ThrowAsJavaScriptException();
            return env.Undefined();
        }
        m_renderer->SetCameraProjection(
            info[0].As<Napi::Number>().FloatValue(),
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            info[3].As<Napi::Number>().FloatValue());
        return env.Undefined();
    }

    // setView(eye[3], target[3], up[3])
    Napi::Value Camera::SetView(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        float eye[3] = {0, 0, 1};
        float target[3] = {0, 0, 0};
        float up[3] = {0, 1, 0};
        if (info.Length() >= 1) ReadFloats(info[0], eye, 3);
        if (info.Length() >= 2) ReadFloats(info[1], target, 3);
        if (info.Length() >= 3) ReadFloats(info[2], up, 3);
        m_renderer->SetCameraView(eye, target, up);
        return env.Undefined();
    }

    // ----------------------------------------------------------------- Initialize
    std::shared_ptr<Controller> Initialize(
        Napi::Env env,
        Babylon::AppRuntime& runtime,
        std::shared_ptr<lite::Renderer> renderer,
        lite::Renderer::WindowHandle window,
        uint32_t width,
        uint32_t height)
    {
        auto controller = std::make_shared<Controller>();
        controller->runtime = &runtime;
        controller->renderer = std::move(renderer);
        controller->window = window;
        controller->width = width;
        controller->height = height;

        auto ctx = std::make_shared<Context>();
        ctx->webgpu = std::make_unique<webgpu::Module>(env,
            webgpu::WindowHandle{controller->window.hwnd, controller->window.hinstance});
        controller->webgpu = ctx->webgpu.get(); // native real-Lite loop presents via the surface

        // Legacy ChakraCore lacks `globalThis`; real Lite (and the bundle preamble)
        // reference it. Point it at the global object.
        if (!env.Global().Has("globalThis"))
            env.Global().Set("globalThis", env.Global());

        ctx->webgpu->InstallNavigatorGpu(env);
        // globalThis.createImageBitmap — decodes glTF/texture image bytes (PNG/JPEG) to an
        // ImageBitmap via stb_image, for real Lite's texture loaders (paired with the Blob
        // polyfill + GPUQueue.copyExternalImageToTexture). Setup-only.
        ctx->webgpu->InstallCreateImageBitmap(env);

        // Provide the host canvas: a global `canvas` plus a minimal
        // document.getElementById(...) that returns it, so real-Lite-style scene code
        // (`document.getElementById("renderCanvas")`) obtains a renderable surface.
        {
            Napi::Object canvas = ctx->webgpu->CreateCanvas(env, controller->width, controller->height);
            env.Global().Set("canvas", canvas);

            Napi::Value docVal = env.Global().Get("document");
            Napi::Object document = docVal.IsObject() ? docVal.As<Napi::Object>() : Napi::Object::New(env);
            document.Set("getElementById", Napi::Function::New(env,
                [](const Napi::CallbackInfo& info) -> Napi::Value {
                    // Single host canvas regardless of id — read it back from the global
                    // (avoids capturing a scope-bound handle in this long-lived closure).
                    return info.Env().Global().Get("canvas");
                },
                "getElementById"));
            env.Global().Set("document", document);
        }

        // requestAnimationFrame — native pump for the IN-APP JS render loop (benchmark
        // config #2: a bundle built WITHOUT render-loop externalization, so real Lite's
        // startEngine/renderFrame run here in JS). Real Lite's startEngine sets _renderFn
        // and calls rAF(_renderFn); _renderFn runs renderFrame (which submits the frame) and
        // re-arms rAF. We store the callback, schedule it on the JS thread, and AFTER it
        // returns we present + record frame timing (real renderFrame doesn't present — the
        // browser does it implicitly). This is a JS-runtime gap the host legitimately fills;
        // it's NOT part of the native render path (that path externalizes startEngine and
        // never calls rAF).
        {
            auto rafId = std::make_shared<std::atomic<uint32_t>>(0);
            env.Global().Set("requestAnimationFrame", Napi::Function::New(env,
                [controller, rafId](const Napi::CallbackInfo& info) -> Napi::Value {
                    Napi::Env env = info.Env();
                    if (info.Length() < 1 || !info[0].IsFunction())
                        return Napi::Number::New(env, 0);
                    auto cb = std::make_shared<Napi::FunctionReference>(Napi::Persistent(info[0].As<Napi::Function>()));
                    uint32_t id = rafId->fetch_add(1) + 1;
                    controller->runtime->Dispatch([controller, cb](Napi::Env env) {
                        if (!controller->jsLoopRunning.load()) return;
                        double nowMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        // Time the JS render-loop work (Lite renderFrame) — present excluded —
                        // for the apples-to-apples "render-loop CPU per frame" benchmark metric.
                        auto cpuStart = std::chrono::steady_clock::now();
                        cb->Call({Napi::Number::New(env, nowMs)}); // runs Lite renderFrame + re-arms rAF
                        double cpuMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - cpuStart).count();
                        if (controller->webgpu != nullptr)
                            const_cast<webgpu::Module*>(controller->webgpu)->Present();
                        controller->frameCounter.fetch_add(1);
                        controller->RecordFrameAndMaybeFinish(cpuMs);
                    });
                    return Napi::Number::New(env, id);
                },
                "requestAnimationFrame"));
            env.Global().Set("cancelAnimationFrame", Napi::Function::New(env,
                [controller](const Napi::CallbackInfo& info) -> Napi::Value {
                    controller->running = false; // stop the JS-loop pump
                    return info.Env().Undefined();
                },
                "cancelAnimationFrame"));
        }
        ctx->engineCtor = Napi::Persistent(Engine::DefineClass(env));
        ctx->engineCtor.SuppressDestruct();
        ctx->meshCtor = Napi::Persistent(Mesh::DefineClass(env));
        ctx->meshCtor.SuppressDestruct();
        ctx->cameraCtor = Napi::Persistent(Camera::DefineClass(env));
        ctx->cameraCtor.SuppressDestruct();
        ctx->materialCtor = Napi::Persistent(Material::DefineClass(env));
        ctx->materialCtor.SuppressDestruct();
        ctx->textureCtor = Napi::Persistent(Texture::DefineClass(env));
        ctx->textureCtor.SuppressDestruct();
        ctx->nodeCtor = Napi::Persistent(Node::DefineClass(env));
        ctx->nodeCtor.SuppressDestruct();
        ctx->controller = controller;

        Napi::Object lite = Napi::Object::New(env);

        // createEngine(): bring up Dawn on first call, then return an Engine handle
        // whose _device is a real wrapped WebGPU device. Mirrors Lite's createEngine.
        lite.Set("createEngine", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                Controller& controller = *ctx->controller;
                if (!controller.renderer->IsInitialized())
                {
                    if (!controller.renderer->Initialize(controller.window, controller.width, controller.height))
                    {
                        Napi::Error::New(env, "BabylonNativeLite.createEngine: Dawn initialization failed")
                            .ThrowAsJavaScriptException();
                        return env.Null();
                    }
                }
                EngineInit init{ctx->controller, ctx->webgpu.get()};
                return ctx->engineCtor.New({Napi::External<EngineInit>::New(env, &init)});
            },
            "createEngine"));

        // createTexture(engine, width, height, pixelsRGBA): native factory that
        // uploads an RGBA8 image (Uint8Array, width*height*4 bytes) to a GPU texture
        // and returns a Texture handle. Exercises the WebGPU texture-upload path.
        lite.Set("createTexture", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                if (info.Length() < 4 || !info[3].IsTypedArray())
                {
                    Napi::TypeError::New(env, "createTexture(engine, width, height, pixelsU8) expected")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                uint32_t width = info[1].As<Napi::Number>().Uint32Value();
                uint32_t height = info[2].As<Napi::Number>().Uint32Value();
                Napi::Uint8Array pixels = info[3].As<Napi::Uint8Array>();
                if (width == 0 || height == 0 || pixels.ElementLength() < static_cast<size_t>(width) * height * 4)
                {
                    Napi::TypeError::New(env, "createTexture: pixel buffer too small for width*height*4")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                lite::Texture* texture = ctx->controller->renderer->AddTexture(pixels.Data(), width, height);
                if (texture == nullptr)
                {
                    Napi::Error::New(env, "createTexture: call createEngine first")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                TextureInit init{texture};
                return ctx->textureCtor.New({Napi::External<TextureInit>::New(env, &init)});
            },
            "createTexture"));

        // createMaterial(engine, options): native factory that builds a material with
        // a feature permutation (vertexColor / lighting / texture / pbr) + base color,
        // and warms the composed-WGSL pipeline cache. options = { vertexColor?:bool,
        // lighting?:bool, pbr?:bool, metallic?:number, roughness?:number,
        // color?:[r,g,b,a], texture?:Texture }.
        lite.Set("createMaterial", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                uint32_t features = lite::MaterialFeature_None;
                float color[4] = {1, 1, 1, 1};
                float metallic = 0.0f;
                float roughness = 0.5f;
                lite::Texture* texture = nullptr;
                if (info.Length() >= 2 && info[1].IsObject())
                {
                    Napi::Object opts = info[1].As<Napi::Object>();
                    if (opts.Get("vertexColor").ToBoolean()) features |= lite::MaterialFeature_VertexColor;
                    if (opts.Get("lighting").ToBoolean()) features |= lite::MaterialFeature_Lighting;
                    if (opts.Get("pbr").ToBoolean())
                        features |= lite::MaterialFeature_PBR | lite::MaterialFeature_Lighting;
                    if (opts.Has("metallic")) metallic = opts.Get("metallic").As<Napi::Number>().FloatValue();
                    if (opts.Has("roughness")) roughness = opts.Get("roughness").As<Napi::Number>().FloatValue();
                    Napi::Value c = opts.Get("color");
                    if (c.IsArray() || c.IsTypedArray()) ReadFloats(c, color, 4);
                    Napi::Value t = opts.Get("texture");
                    if (t.IsObject())
                    {
                        Texture* tex = Napi::ObjectWrap<Texture>::Unwrap(t.As<Napi::Object>());
                        if (tex != nullptr)
                        {
                            texture = tex->Native();
                            features |= lite::MaterialFeature_Texture;
                        }
                    }
                }
                lite::Material* material = ctx->controller->renderer->AddMaterial(features, color, texture, metallic, roughness);
                if (material == nullptr)
                {
                    Napi::Error::New(env, "createMaterial: call createEngine first")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                MaterialInit init{material, ctx->controller->renderer.get()};
                return ctx->materialCtor.New({Napi::External<MaterialInit>::New(env, &init)});
            },
            "createMaterial"));

        // createNode(engine[, parentNode]): native factory for a scene-graph transform
        // node. Pass another Node as the optional parent to build a hierarchy; world
        // matrices are computed natively each frame (world = parent.world * local).
        lite.Set("createNode", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                lite::Node* parent = nullptr;
                if (info.Length() >= 2 && info[1].IsObject())
                {
                    Node* p = Napi::ObjectWrap<Node>::Unwrap(info[1].As<Napi::Object>());
                    if (p != nullptr) parent = p->Native();
                }
                lite::Node* node = ctx->controller->renderer->AddNode(parent);
                if (node == nullptr)
                {
                    Napi::Error::New(env, "createNode: call createEngine first")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                NodeInit init{node, ctx->controller->renderer.get()};
                return ctx->nodeCtor.New({Napi::External<NodeInit>::New(env, &init)});
            },
            "createNode"));

        // createAnimation(engine, node, options): native keyframe animation that drives
        // one transform channel of `node` entirely in the render loop (no JS per frame).
        // options = { property:'position'|'rotation'|'scale', keys:[{time,value:[x,y,z]},...],
        // duration?:number (seconds, defaults to last key time), loop?:bool (default true) }.
        lite.Set("createAnimation", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                if (info.Length() < 3 || !info[1].IsObject() || !info[2].IsObject())
                {
                    Napi::TypeError::New(env, "createAnimation(engine, node, options) expected")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                Node* nodeWrap = Napi::ObjectWrap<Node>::Unwrap(info[1].As<Napi::Object>());
                if (nodeWrap == nullptr)
                {
                    Napi::TypeError::New(env, "createAnimation: second arg must be a Node")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                Napi::Object opts = info[2].As<Napi::Object>();

                uint32_t property = lite::AnimationProperty_Position;
                if (opts.Get("property").IsString())
                {
                    std::string p = opts.Get("property").As<Napi::String>().Utf8Value();
                    if (p == "rotation") property = lite::AnimationProperty_Rotation;
                    else if (p == "scale") property = lite::AnimationProperty_Scale;
                }

                std::vector<lite::AnimationKey> keys;
                if (opts.Get("keys").IsArray())
                {
                    Napi::Array arr = opts.Get("keys").As<Napi::Array>();
                    for (uint32_t i = 0; i < arr.Length(); ++i)
                    {
                        if (!arr.Get(i).IsObject()) continue;
                        Napi::Object k = arr.Get(i).As<Napi::Object>();
                        lite::AnimationKey key{};
                        key.time = k.Get("time").As<Napi::Number>().FloatValue();
                        ReadFloats(k.Get("value"), key.value, 3);
                        keys.push_back(key);
                    }
                }
                if (keys.empty())
                {
                    Napi::TypeError::New(env, "createAnimation: options.keys must be a non-empty array")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }

                float duration = 0.0f;
                if (opts.Has("duration")) duration = opts.Get("duration").As<Napi::Number>().FloatValue();
                bool loop = true;
                if (opts.Has("loop")) loop = opts.Get("loop").ToBoolean();

                ctx->controller->renderer->AddAnimation(nodeWrap->Native(), property,
                    keys.data(), static_cast<uint32_t>(keys.size()), duration, loop);
                return env.Undefined();
            },
            "createAnimation"));

        // createMesh(engine, positions, normals, uvs, colors, indices, material, node):
        // native factory that builds an indexed GPU-resident 3D mesh (positions/
        // normals/colors are vec3*N, uvs vec2*N Float32Arrays, indices a Uint16Array)
        // bound to a material, transformed by the given scene-graph node.
        lite.Set("createMesh", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                if (info.Length() < 8 || !info[1].IsTypedArray() || !info[2].IsTypedArray() ||
                    !info[3].IsTypedArray() || !info[4].IsTypedArray() || !info[5].IsTypedArray() ||
                    !info[6].IsObject() || !info[7].IsObject())
                {
                    Napi::TypeError::New(env, "createMesh(engine, posF32, normF32, uvF32, colF32, idxU16, material, node) expected")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                Napi::Float32Array positions = info[1].As<Napi::Float32Array>();
                Napi::Float32Array normals = info[2].As<Napi::Float32Array>();
                Napi::Float32Array uvs = info[3].As<Napi::Float32Array>();
                Napi::Float32Array colors = info[4].As<Napi::Float32Array>();
                Napi::Uint16Array indices = info[5].As<Napi::Uint16Array>();
                Material* material = Napi::ObjectWrap<Material>::Unwrap(info[6].As<Napi::Object>());
                Node* node = Napi::ObjectWrap<Node>::Unwrap(info[7].As<Napi::Object>());
                uint32_t vertexCount = static_cast<uint32_t>(positions.ElementLength() / 3);
                uint32_t indexCount = static_cast<uint32_t>(indices.ElementLength());
                if (vertexCount == 0 || indexCount == 0 || material == nullptr || node == nullptr ||
                    normals.ElementLength() < static_cast<size_t>(vertexCount) * 3 ||
                    uvs.ElementLength() < static_cast<size_t>(vertexCount) * 2 ||
                    colors.ElementLength() < static_cast<size_t>(vertexCount) * 3)
                {
                    Napi::TypeError::New(env, "createMesh: array length mismatch or missing material/node")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }

                lite::Mesh* mesh = ctx->controller->renderer->AddMesh(
                    positions.Data(), normals.Data(), uvs.Data(), colors.Data(), vertexCount,
                    indices.Data(), indexCount, material->Native(), node->Native());
                if (mesh == nullptr)
                {
                    Napi::Error::New(env, "createMesh: renderer not initialized (call createEngine first)")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }

                MeshInit init{mesh};
                return ctx->meshCtor.New({Napi::External<MeshInit>::New(env, &init)});
            },
            "createMesh"));

        // clearLights(engine): remove all configured scene lights.
        lite.Set("clearLights", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                ctx->controller->renderer->ClearLights();
                return info.Env().Undefined();
            },
            "clearLights"));

        // addLight(engine, { type, direction|position, color, intensity }): add a
        // scene light (directional or point) consumed by lighting-enabled materials.
        // type: "point" => point light (uses position), anything else => directional
        // (uses direction). Up to Renderer::kMaxLights are honored.
        lite.Set("addLight", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                float vec[3] = {0, -1, 0};
                float color[3] = {1, 1, 1};
                float intensity = 1.0f;
                uint32_t type = 0; // 0 = directional, 1 = point
                if (info.Length() >= 2 && info[1].IsObject())
                {
                    Napi::Object opts = info[1].As<Napi::Object>();
                    if (opts.Has("type") && opts.Get("type").IsString())
                    {
                        std::string t = opts.Get("type").As<Napi::String>().Utf8Value();
                        if (t == "point") type = 1;
                    }
                    if (type == 1 && opts.Has("position")) ReadFloats(opts.Get("position"), vec, 3);
                    else if (opts.Has("direction")) ReadFloats(opts.Get("direction"), vec, 3);
                    if (opts.Has("color")) ReadFloats(opts.Get("color"), color, 3);
                    if (opts.Has("intensity")) intensity = opts.Get("intensity").As<Napi::Number>().FloatValue();
                }
                ctx->controller->renderer->AddLight(type, vec, color, intensity);
                return env.Undefined();
            },
            "addLight"));

        // createCamera(engine): returns a Camera handle with native setProjection /
        // setView. The camera owns the shared viewProj uniform in the renderer.
        lite.Set("createCamera", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                if (!ctx->controller->renderer->IsInitialized())
                {
                    Napi::Error::New(env, "createCamera: call createEngine first")
                        .ThrowAsJavaScriptException();
                    return env.Null();
                }
                CameraInit init{ctx->controller->renderer.get()};
                return ctx->cameraCtor.New({Napi::External<CameraInit>::New(env, &init)});
            },
            "createCamera"));

        // registerDrawable(engine, { pipeline, bindGroups, vertexBuffer, indexBuffer,
        // indexCount, node, indexFormat? }): the Model B entry point. JS built every
        // GPU resource via the WebGPU polyfill; native records the draw and animates +
        // draws it with zero JS per frame. The GPU objects are unwrapped to their Dawn
        // handles here.
        lite.Set("registerDrawable", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                const webgpu::Module* mod = webgpu::Module::Current();
                if (mod == nullptr || info.Length() < 2 || !info[1].IsObject())
                {
                    Napi::TypeError::New(env, "registerDrawable(engine, descriptor) expected")
                        .ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                Napi::Object d = info[1].As<Napi::Object>();

                webgpu::RenderPipeline* pipeline = mod->AsRenderPipeline(d.Get("pipeline").As<Napi::Object>());
                webgpu::Buffer* vb = mod->AsBuffer(d.Get("vertexBuffer").As<Napi::Object>());
                webgpu::Buffer* ib = mod->AsBuffer(d.Get("indexBuffer").As<Napi::Object>());
                if (pipeline == nullptr || vb == nullptr || ib == nullptr)
                {
                    Napi::TypeError::New(env, "registerDrawable: pipeline/vertexBuffer/indexBuffer must be GPU objects")
                        .ThrowAsJavaScriptException();
                    return env.Undefined();
                }

                std::vector<wgpu::BindGroup> bindGroups;
                Napi::Array bgs = d.Get("bindGroups").As<Napi::Array>();
                for (uint32_t i = 0; i < bgs.Length(); ++i)
                {
                    webgpu::BindGroup* bg = mod->AsBindGroup(bgs.Get(i).As<Napi::Object>());
                    if (bg != nullptr) bindGroups.push_back(bg->Handle());
                }

                uint32_t indexCount = static_cast<uint32_t>(d.Get("indexCount").As<Napi::Number>().Uint32Value());
                wgpu::IndexFormat fmt = wgpu::IndexFormat::Uint16;
                if (d.Has("indexFormat") && d.Get("indexFormat").IsString() &&
                    d.Get("indexFormat").As<Napi::String>().Utf8Value() == "uint32")
                    fmt = wgpu::IndexFormat::Uint32;

                Node* nodeWrap = Napi::ObjectWrap<Node>::Unwrap(d.Get("node").As<Napi::Object>());
                lite::Node* node = nodeWrap != nullptr ? nodeWrap->Native() : nullptr;

                ctx->controller->renderer->RegisterDrawable(pipeline->Handle(), std::move(bindGroups),
                    vb->Handle(), ib->Handle(), indexCount, fmt, node);
                return env.Undefined();
            },
            "registerDrawable"));

        // startEngine(engine): start the native render loop. For the REAL Babylon Lite
        // path (engine is a plain Lite engine object, not our Engine wrapper), this is
        // the externalized native startEngine: it stores the engine and drives a native
        // renderFrame that READS the engine's JS structures each frame — replacing real
        // Lite's JS startEngine + requestAnimationFrame. Returns a resolved Promise
        // (real Lite's startEngine is awaited by scenes).
        lite.Set("startEngine", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                if (info.Length() < 1 || !info[0].IsObject())
                {
                    Napi::TypeError::New(env, "startEngine(engine) expected")
                        .ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                Napi::Object arg = info[0].As<Napi::Object>();

                // Legacy path: our own Engine ObjectWrap (the invented demo path).
                if (arg.InstanceOf(ctx->engineCtor.Value()))
                {
                    Engine* engine = Napi::ObjectWrap<Engine>::Unwrap(arg);
                    if (engine != nullptr)
                    {
                        auto controller = engine->GetController();
                        if (!controller->running.exchange(true))
                        {
                            std::fprintf(stderr, "[nativelite] startEngine() (legacy) — native render loop starting\n");
                            Pump(controller);
                        }
                    }
                    return env.Undefined();
                }

                // Real Babylon Lite path: `arg` is the real Lite engine plain object.
                // The bundler's externalization shim runs a one-time JS warm-up
                // (renderFrame(engine, 0)) BEFORE calling this, so the opaque render
                // bundle + pipelines + UBOs already exist. Native only READS them.
                auto controller = ctx->controller;
                controller->realEngine = Napi::Persistent(arg);
                controller->realMode = true;
                if (!controller->running.exchange(true))
                {
                    std::fprintf(stderr, "[nativelite] startEngine() (real Lite) — native render loop starting\n");
                    RealPump(controller);
                }
                // Real Lite's startEngine returns Promise<void>; resolve immediately.
                auto deferred = Napi::Promise::Deferred::New(env);
                deferred.Resolve(env.Undefined());
                return deferred.Promise();
            },
            "startEngine"));

        // stopEngine(engine): stop the native render loop (real-Lite or legacy path).
        lite.Set("stopEngine", Napi::Function::New(env,
            [ctx](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                if (info.Length() >= 1 && info[0].IsObject())
                {
                    Napi::Object arg = info[0].As<Napi::Object>();
                    if (arg.InstanceOf(ctx->engineCtor.Value()))
                    {
                        Engine* engine = Napi::ObjectWrap<Engine>::Unwrap(arg);
                        if (engine != nullptr) engine->GetController()->running = false;
                    }
                    else
                    {
                        ctx->controller->running = false; // real-Lite path
                    }
                }
                return env.Undefined();
            },
            "stopEngine"));

        // Expose the WebGPU flag-constant namespaces as globals (spec numeric values,
        // which equal Dawn's flag values), so JS engine code builds buffer/texture
        // descriptors the standard way: `usage: GPUBufferUsage.VERTEX | ...`.
        {
            Napi::Object bufUsage = Napi::Object::New(env);
            bufUsage.Set("MAP_READ", Napi::Number::New(env, 0x0001));
            bufUsage.Set("MAP_WRITE", Napi::Number::New(env, 0x0002));
            bufUsage.Set("COPY_SRC", Napi::Number::New(env, 0x0004));
            bufUsage.Set("COPY_DST", Napi::Number::New(env, 0x0008));
            bufUsage.Set("INDEX", Napi::Number::New(env, 0x0010));
            bufUsage.Set("VERTEX", Napi::Number::New(env, 0x0020));
            bufUsage.Set("UNIFORM", Napi::Number::New(env, 0x0040));
            bufUsage.Set("STORAGE", Napi::Number::New(env, 0x0080));
            bufUsage.Set("INDIRECT", Napi::Number::New(env, 0x0100));
            bufUsage.Set("QUERY_RESOLVE", Napi::Number::New(env, 0x0200));
            env.Global().Set("GPUBufferUsage", bufUsage);

            Napi::Object texUsage = Napi::Object::New(env);
            texUsage.Set("COPY_SRC", Napi::Number::New(env, 0x01));
            texUsage.Set("COPY_DST", Napi::Number::New(env, 0x02));
            texUsage.Set("TEXTURE_BINDING", Napi::Number::New(env, 0x04));
            texUsage.Set("STORAGE_BINDING", Napi::Number::New(env, 0x08));
            texUsage.Set("RENDER_ATTACHMENT", Napi::Number::New(env, 0x10));
            env.Global().Set("GPUTextureUsage", texUsage);

            Napi::Object shaderStage = Napi::Object::New(env);
            shaderStage.Set("VERTEX", Napi::Number::New(env, 0x1));
            shaderStage.Set("FRAGMENT", Napi::Number::New(env, 0x2));
            shaderStage.Set("COMPUTE", Napi::Number::New(env, 0x4));
            env.Global().Set("GPUShaderStage", shaderStage);

            // GPUColorWrite — color write-mask flags. Read by the PBR/standard fragment
            // target setup (`writeMask: GPUColorWrite.ALL`).
            Napi::Object colorWrite = Napi::Object::New(env);
            colorWrite.Set("RED", Napi::Number::New(env, 0x1));
            colorWrite.Set("GREEN", Napi::Number::New(env, 0x2));
            colorWrite.Set("BLUE", Napi::Number::New(env, 0x4));
            colorWrite.Set("ALPHA", Napi::Number::New(env, 0x8));
            colorWrite.Set("ALL", Napi::Number::New(env, 0xF));
            env.Global().Set("GPUColorWrite", colorWrite);

            // GPUMapMode — buffer map flags (read by mapAsync-style paths if used).
            Napi::Object mapMode = Napi::Object::New(env);
            mapMode.Set("READ", Napi::Number::New(env, 0x1));
            mapMode.Set("WRITE", Napi::Number::New(env, 0x2));
            env.Global().Set("GPUMapMode", mapMode);
        }

        env.Global().Set("BabylonNativeLite", lite);

        // Install the stage-0 call-overhead microbench probes (cheap; used only by the
        // microbench scene). Harmless for normal scenes.
        InstallMicrobench(env);

        return controller;
    }
}
