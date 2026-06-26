#include <Lite/Renderer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lite
{
    namespace
    {
        // --- Minimal column-major mat4 math (matches WGSL mat4x4<f32> layout). ---
        void MatIdentity(float* m)
        {
            std::memset(m, 0, sizeof(float) * 16);
            m[0] = m[5] = m[10] = m[15] = 1.0f;
        }

        void MatMul(const float* a, const float* b, float* out)
        {
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k)
                        sum += a[k * 4 + row] * b[col * 4 + k];
                    out[col * 4 + row] = sum;
                }
        }

        void MatPerspective(float fovY, float aspect, float nearZ, float farZ, float* m)
        {
            std::memset(m, 0, sizeof(float) * 16);
            const float f = 1.0f / std::tan(fovY * 0.5f);
            m[0] = f / aspect;
            m[5] = f;
            m[10] = farZ / (nearZ - farZ);
            m[11] = -1.0f;
            m[14] = -(farZ * nearZ) / (farZ - nearZ);
        }

        // Right-handed orthographic projection with a [0,1] depth range (WebGPU/D3D
        // convention) — used to build the directional light's shadow frustum.
        void MatOrtho(float halfExtent, float nearZ, float farZ, float* m)
        {
            std::memset(m, 0, sizeof(float) * 16);
            m[0] = 1.0f / halfExtent;
            m[5] = 1.0f / halfExtent;
            m[10] = 1.0f / (nearZ - farZ);
            m[14] = nearZ / (nearZ - farZ);
            m[15] = 1.0f;
        }

        void Normalize3(float* v)
        {
            const float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
        }
        void Cross3(const float* a, const float* b, float* out)
        {
            out[0] = a[1]*b[2] - a[2]*b[1];
            out[1] = a[2]*b[0] - a[0]*b[2];
            out[2] = a[0]*b[1] - a[1]*b[0];
        }
        float Dot3(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

        void MatLookAt(const float* eye, const float* target, const float* up, float* m)
        {
            float f[3] = {target[0]-eye[0], target[1]-eye[1], target[2]-eye[2]};
            Normalize3(f);
            float s[3]; Cross3(f, up, s); Normalize3(s);
            float u[3]; Cross3(s, f, u);
            m[0]=s[0];  m[4]=s[1];  m[8]=s[2];   m[12]=-Dot3(s, eye);
            m[1]=u[0];  m[5]=u[1];  m[9]=u[2];   m[13]=-Dot3(u, eye);
            m[2]=-f[0]; m[6]=-f[1]; m[10]=-f[2]; m[14]=Dot3(f, eye);
            m[3]=0.0f;  m[7]=0.0f;  m[11]=0.0f;  m[15]=1.0f;
        }

        void MatModel(const float* pos, const float* rot, const float* scale, float* out)
        {
            const float cx=std::cos(rot[0]), sx=std::sin(rot[0]);
            const float cy=std::cos(rot[1]), sy=std::sin(rot[1]);
            const float cz=std::cos(rot[2]), sz=std::sin(rot[2]);
            float rx[16]; MatIdentity(rx); rx[5]=cx; rx[6]=sx; rx[9]=-sx; rx[10]=cx;
            float ry[16]; MatIdentity(ry); ry[0]=cy; ry[2]=-sy; ry[8]=sy; ry[10]=cy;
            float rz[16]; MatIdentity(rz); rz[0]=cz; rz[1]=sz; rz[4]=-sz; rz[5]=cz;
            float ryx[16]; MatMul(ry, rx, ryx);
            float r[16]; MatMul(rz, ryx, r);
            float sm[16]; MatIdentity(sm); sm[0]=scale[0]; sm[5]=scale[1]; sm[10]=scale[2];
            float rs[16]; MatMul(r, sm, rs);
            std::memcpy(out, rs, sizeof(float) * 16);
            out[12]=rs[12]+pos[0]; out[13]=rs[13]+pos[1]; out[14]=rs[14]+pos[2];
        }

        // Depth-only shader for the shadow pass: transform vertex positions into the
        // light's clip space; no fragment stage (the pass writes depth only).
        const char* kShadowWgsl = R"(
@group(0) @binding(0) var<uniform> lightViewProj : mat4x4<f32>;
@group(1) @binding(0) var<uniform> model : mat4x4<f32>;
@vertex
fn vs_main(@location(0) inPos : vec3<f32>) -> @builtin(position) vec4<f32> {
    return lightViewProj * model * vec4<f32>(inPos, 1.0);
}
)";

        wgpu::Surface CreateSurfaceForWindow(const wgpu::Instance& instance, void* hwnd, void* hinstance, void* metalLayer)
        {
#if defined(_WIN32)
            (void)metalLayer;
            wgpu::SurfaceSourceWindowsHWND chained{};
            chained.hwnd = hwnd;
            chained.hinstance = hinstance;
            wgpu::SurfaceDescriptor desc{};
            desc.nextInChain = &chained;
            return instance.CreateSurface(&desc);
#elif defined(__APPLE__)
            (void)hwnd; (void)hinstance;
            wgpu::SurfaceSourceMetalLayer chained{};
            chained.layer = metalLayer;
            wgpu::SurfaceDescriptor desc{};
            desc.nextInChain = &chained;
            return instance.CreateSurface(&desc);
#else
            (void)instance; (void)hwnd; (void)hinstance; (void)metalLayer;
            return {};
#endif
        }

        wgpu::Adapter RequestAdapter(const wgpu::Instance& instance)
        {
            wgpu::RequestAdapterOptions options{};
#if defined(_WIN32)
            options.backendType = wgpu::BackendType::D3D12;
#elif defined(__APPLE__)
            options.backendType = wgpu::BackendType::Metal;
#endif
            options.powerPreference = wgpu::PowerPreference::HighPerformance;
            wgpu::Adapter adapter;
            wgpu::Future future = instance.RequestAdapter(
                &options, wgpu::CallbackMode::WaitAnyOnly,
                [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView message) {
                    if (status == wgpu::RequestAdapterStatus::Success) adapter = std::move(result);
                    else std::fprintf(stderr, "[renderer] requestAdapter failed: %.*s\n",
                        static_cast<int>(message.length), message.data);
                });
            instance.WaitAny(future, UINT64_MAX);
            return adapter;
        }

        wgpu::Device RequestDevice(const wgpu::Instance& instance, const wgpu::Adapter& adapter)
        {
            wgpu::DeviceDescriptor desc{};
            desc.SetUncapturedErrorCallback(
                [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
                    std::fprintf(stderr, "[renderer] device error (%d): %.*s\n",
                        static_cast<int>(type), static_cast<int>(message.length), message.data);
                });
            desc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
                [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
                    std::fprintf(stderr, "[renderer] device lost (%d): %.*s\n",
                        static_cast<int>(reason), static_cast<int>(message.length), message.data);
                });
            static const char* kEnableDxc = "use_dxc";
            wgpu::DawnTogglesDescriptor toggles{};
            toggles.enabledToggleCount = 1;
            toggles.enabledToggles = &kEnableDxc;
            desc.nextInChain = &toggles;
            wgpu::Device device;
            wgpu::Future future = adapter.RequestDevice(
                &desc, wgpu::CallbackMode::WaitAnyOnly,
                [&device](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message) {
                    if (status == wgpu::RequestDeviceStatus::Success) device = std::move(result);
                    else std::fprintf(stderr, "[renderer] requestDevice failed: %.*s\n",
                        static_cast<int>(message.length), message.data);
                });
            instance.WaitAny(future, UINT64_MAX);
            return device;
        }

        wgpu::TextureFormat PickSurfaceFormat(const wgpu::Surface& surface, const wgpu::Adapter& adapter)
        {
            wgpu::SurfaceCapabilities caps{};
            surface.GetCapabilities(adapter, &caps);
            if (caps.formatCount > 0 && caps.formats != nullptr) return caps.formats[0];
            return wgpu::TextureFormat::BGRA8Unorm;
        }

        wgpu::BindGroupLayout MakeUniformBgl(const wgpu::Device& device, wgpu::ShaderStage visibility)
        {
            wgpu::BindGroupLayoutEntry entry{};
            entry.binding = 0;
            entry.visibility = visibility;
            entry.buffer.type = wgpu::BufferBindingType::Uniform;
            wgpu::BindGroupLayoutDescriptor desc{};
            desc.entryCount = 1;
            desc.entries = &entry;
            return device.CreateBindGroupLayout(&desc);
        }

        wgpu::BindGroup MakeUniformBindGroup(const wgpu::Device& device, const wgpu::BindGroupLayout& layout,
            const wgpu::Buffer& buffer, uint64_t size)
        {
            wgpu::BindGroupEntry entry{};
            entry.binding = 0;
            entry.buffer = buffer;
            entry.size = size;
            wgpu::BindGroupDescriptor desc{};
            desc.layout = layout;
            desc.entryCount = 1;
            desc.entries = &entry;
            return device.CreateBindGroup(&desc);
        }
    }

    Renderer::~Renderer() { Shutdown(); }

    bool Renderer::Initialize(const WindowHandle& window, uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
        m_pendingWidth = static_cast<int>(width);
        m_pendingHeight = static_cast<int>(height);
        MatIdentity(m_view);
        MatIdentity(m_proj);
        if (!InitDawn(window)) return false;
        m_initialized = true;
        return true;
    }

    bool Renderer::InitDawn(const WindowHandle& window)
    {
        static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
        wgpu::InstanceDescriptor instanceDesc{};
        instanceDesc.requiredFeatureCount = 1;
        instanceDesc.requiredFeatures = &kTimedWaitAny;
        m_instance = wgpu::CreateInstance(&instanceDesc);
        if (m_instance == nullptr) { std::fprintf(stderr, "[renderer] CreateInstance failed\n"); return false; }

        m_surface = CreateSurfaceForWindow(m_instance, window.hwnd, window.hinstance, window.metalLayer);
        if (m_surface == nullptr) { std::fprintf(stderr, "[renderer] CreateSurface failed\n"); return false; }

        m_adapter = RequestAdapter(m_instance);
        if (m_adapter == nullptr) { std::fprintf(stderr, "[renderer] no adapter\n"); return false; }

        m_device = RequestDevice(m_instance, m_adapter);
        if (m_device == nullptr) { std::fprintf(stderr, "[renderer] no device\n"); return false; }
        m_queue = m_device.GetQueue();

        m_format = PickSurfaceFormat(m_surface, m_adapter);
        Configure();
        CreateDepthTexture();

        // Bind group layout 0 = frame: the frame uniform (vertex+fragment) plus the
        // shadow depth texture + comparison sampler (fragment) for shadow lookups.
        {
            wgpu::BindGroupLayoutEntry entries[3]{};
            entries[0].binding = 0;
            entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
            entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
            entries[1].binding = 1;
            entries[1].visibility = wgpu::ShaderStage::Fragment;
            entries[1].texture.sampleType = wgpu::TextureSampleType::Depth;
            entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
            entries[2].binding = 2;
            entries[2].visibility = wgpu::ShaderStage::Fragment;
            entries[2].sampler.type = wgpu::SamplerBindingType::Comparison;
            wgpu::BindGroupLayoutDescriptor desc{};
            desc.entryCount = 3;
            desc.entries = entries;
            m_frameBindGroupLayout = m_device.CreateBindGroupLayout(&desc);
        }
        m_modelBindGroupLayout = MakeUniformBgl(m_device, wgpu::ShaderStage::Vertex);

        // Material layout (untextured): just the color uniform (fragment).
        m_materialBindGroupLayout = MakeUniformBgl(m_device, wgpu::ShaderStage::Fragment);

        // Material layout (textured): color uniform + texture + sampler (fragment).
        {
            wgpu::BindGroupLayoutEntry entries[3]{};
            entries[0].binding = 0;
            entries[0].visibility = wgpu::ShaderStage::Fragment;
            entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
            entries[1].binding = 1;
            entries[1].visibility = wgpu::ShaderStage::Fragment;
            entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
            entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
            entries[2].binding = 2;
            entries[2].visibility = wgpu::ShaderStage::Fragment;
            entries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
            wgpu::BindGroupLayoutDescriptor desc{};
            desc.entryCount = 3;
            desc.entries = entries;
            m_texturedMaterialBindGroupLayout = m_device.CreateBindGroupLayout(&desc);
        }

        // Two pipeline layouts (textured vs not) — they differ only in group 2.
        {
            wgpu::BindGroupLayout bgls[3] = {m_frameBindGroupLayout, m_modelBindGroupLayout, m_materialBindGroupLayout};
            wgpu::PipelineLayoutDescriptor desc{};
            desc.bindGroupLayoutCount = 3;
            desc.bindGroupLayouts = bgls;
            m_pipelineLayout = m_device.CreatePipelineLayout(&desc);
        }
        {
            wgpu::BindGroupLayout bgls[3] = {m_frameBindGroupLayout, m_modelBindGroupLayout, m_texturedMaterialBindGroupLayout};
            wgpu::PipelineLayoutDescriptor desc{};
            desc.bindGroupLayoutCount = 3;
            desc.bindGroupLayouts = bgls;
            m_texturedPipelineLayout = m_device.CreatePipelineLayout(&desc);
        }

        // Shared linear sampler with repeat addressing.
        wgpu::SamplerDescriptor samplerDesc{};
        samplerDesc.magFilter = wgpu::FilterMode::Linear;
        samplerDesc.minFilter = wgpu::FilterMode::Linear;
        samplerDesc.addressModeU = wgpu::AddressMode::Repeat;
        samplerDesc.addressModeV = wgpu::AddressMode::Repeat;
        m_sampler = m_device.CreateSampler(&samplerDesc);

        // Frame uniform: viewProj(64) + cameraPos(16) + lightCount(16) +
        // lights[kMaxLights]*32 + lightViewProj(64) = 224 + 64 = 288 bytes.
        wgpu::BufferDescriptor frameDesc{};
        frameDesc.size = 288;
        frameDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        m_frameBuffer = m_device.CreateBuffer(&frameDesc);

        // Shadow map texture, comparison sampler, shadow uniform + depth-only pipeline.
        CreateShadowResources();

        // Frame bind group: uniform + shadow depth view + comparison sampler.
        {
            wgpu::BindGroupEntry entries[3]{};
            entries[0].binding = 0;
            entries[0].buffer = m_frameBuffer;
            entries[0].size = 288;
            entries[1].binding = 1;
            entries[1].textureView = m_shadowView;
            entries[2].binding = 2;
            entries[2].sampler = m_shadowSampler;
            wgpu::BindGroupDescriptor desc{};
            desc.layout = m_frameBindGroupLayout;
            desc.entryCount = 3;
            desc.entries = entries;
            m_frameBindGroup = m_device.CreateBindGroup(&desc);
        }
        UploadFrameUniform();

        wgpu::AdapterInfo info{};
        m_adapter.GetInfo(&info);
        std::fprintf(stderr, "[renderer] device ready — adapter: %.*s, backend %d, format %d, %ux%u (materials/textures/pipeline cache)\n",
            static_cast<int>(info.device.length), info.device.data,
            static_cast<int>(info.backendType),
            static_cast<int>(m_format), m_width, m_height);
        return true;
    }

    const wgpu::BindGroupLayout& Renderer::MaterialLayoutFor(uint32_t features) const
    {
        return (features & MaterialFeature_Texture) ? m_texturedMaterialBindGroupLayout
                                                    : m_materialBindGroupLayout;
    }

    wgpu::RenderPipeline Renderer::GetOrCreatePipeline(uint32_t features)
    {
        auto it = m_pipelineCache.find(features);
        if (it != m_pipelineCache.end())
            return it->second;

        std::string wgsl = ComposeMeshWgsl(features);
        wgpu::ShaderSourceWGSL wgslSource{};
        wgslSource.code = wgsl.c_str();
        wgpu::ShaderModuleDescriptor moduleDesc{};
        moduleDesc.nextInChain = &wgslSource;
        wgpu::ShaderModule module = m_device.CreateShaderModule(&moduleDesc);

        // Vertex layout: interleaved [pos.xyz, normal.xyz, uv.xy, color.rgb] = 11 floats.
        wgpu::VertexAttribute attributes[4];
        attributes[0].format = wgpu::VertexFormat::Float32x3;
        attributes[0].offset = 0;
        attributes[0].shaderLocation = 0;
        attributes[1].format = wgpu::VertexFormat::Float32x3;
        attributes[1].offset = 3 * sizeof(float);
        attributes[1].shaderLocation = 1;
        attributes[2].format = wgpu::VertexFormat::Float32x2;
        attributes[2].offset = 6 * sizeof(float);
        attributes[2].shaderLocation = 2;
        attributes[3].format = wgpu::VertexFormat::Float32x3;
        attributes[3].offset = 8 * sizeof(float);
        attributes[3].shaderLocation = 3;
        wgpu::VertexBufferLayout vbLayout{};
        vbLayout.arrayStride = 11 * sizeof(float);
        vbLayout.attributeCount = 4;
        vbLayout.attributes = attributes;

        wgpu::ColorTargetState colorTarget{};
        colorTarget.format = m_format;
        wgpu::FragmentState fragment{};
        fragment.module = module;
        fragment.entryPoint = "fs_main";
        fragment.targetCount = 1;
        fragment.targets = &colorTarget;

        wgpu::DepthStencilState depthStencil{};
        depthStencil.format = m_depthFormat;
        depthStencil.depthWriteEnabled = true;
        depthStencil.depthCompare = wgpu::CompareFunction::Less;

        wgpu::RenderPipelineDescriptor pipelineDesc{};
        pipelineDesc.layout = (features & MaterialFeature_Texture) ? m_texturedPipelineLayout : m_pipelineLayout;
        pipelineDesc.vertex.module = module;
        pipelineDesc.vertex.entryPoint = "vs_main";
        pipelineDesc.vertex.bufferCount = 1;
        pipelineDesc.vertex.buffers = &vbLayout;
        pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
        pipelineDesc.depthStencil = &depthStencil;
        pipelineDesc.fragment = &fragment;

        wgpu::RenderPipeline pipeline = m_device.CreateRenderPipeline(&pipelineDesc);
        m_pipelineCache.emplace(features, pipeline);
        std::fprintf(stderr, "[renderer] composed + cached pipeline for material features 0x%X (cache size %zu)\n",
            features, m_pipelineCache.size());
        return pipeline;
    }

    Texture* Renderer::AddTexture(const uint8_t* pixelsRGBA, uint32_t width, uint32_t height)
    {
        if (!m_initialized || width == 0 || height == 0)
            return nullptr;

        auto tex = std::make_unique<Texture>();
        tex->width = width;
        tex->height = height;

        wgpu::TextureDescriptor desc{};
        desc.size.width = width;
        desc.size.height = height;
        desc.size.depthOrArrayLayers = 1;
        desc.format = wgpu::TextureFormat::RGBA8Unorm;
        desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        tex->texture = m_device.CreateTexture(&desc);

        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = tex->texture;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = width * 4;
        layout.rowsPerImage = height;
        wgpu::Extent3D extent{width, height, 1};
        m_queue.WriteTexture(&dst, pixelsRGBA, static_cast<size_t>(width) * height * 4, &layout, &extent);

        tex->view = tex->texture.CreateView();

        Texture* raw = tex.get();
        m_textures.push_back(std::move(tex));
        return raw;
    }

    Material* Renderer::AddMaterial(uint32_t features, const float baseColor[4], Texture* texture,
        float metallic, float roughness)
    {
        if (!m_initialized) return nullptr;
        if ((features & MaterialFeature_Texture) && texture == nullptr)
            features &= ~static_cast<uint32_t>(MaterialFeature_Texture); // no texture -> drop the bit
        if (features & MaterialFeature_PBR)
            features |= MaterialFeature_Lighting; // PBR shading runs inside the lighting loop

        GetOrCreatePipeline(features); // warm cache + validate WGSL

        auto material = std::make_unique<Material>();
        material->features = features;
        material->texture = texture;

        // Material uniform: vec4 baseColor + vec4 params(metallic, roughness, _, _) = 32 bytes.
        float uniformData[8] = {
            baseColor[0], baseColor[1], baseColor[2], baseColor[3],
            metallic, roughness, 0.0f, 0.0f,
        };
        wgpu::BufferDescriptor desc{};
        desc.size = 32;
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        material->colorBuffer = m_device.CreateBuffer(&desc);
        m_queue.WriteBuffer(material->colorBuffer, 0, uniformData, 32);

        if (features & MaterialFeature_Texture)
        {
            wgpu::BindGroupEntry entries[3]{};
            entries[0].binding = 0;
            entries[0].buffer = material->colorBuffer;
            entries[0].size = 32;
            entries[1].binding = 1;
            entries[1].textureView = texture->view;
            entries[2].binding = 2;
            entries[2].sampler = m_sampler;
            wgpu::BindGroupDescriptor bgDesc{};
            bgDesc.layout = m_texturedMaterialBindGroupLayout;
            bgDesc.entryCount = 3;
            bgDesc.entries = entries;
            material->bindGroup = m_device.CreateBindGroup(&bgDesc);
        }
        else
        {
            material->bindGroup = MakeUniformBindGroup(m_device, m_materialBindGroupLayout, material->colorBuffer, 32);
        }

        Material* raw = material.get();
        m_materials.push_back(std::move(material));
        return raw;
    }

    void Renderer::SetMaterialColor(Material* material, const float baseColor[4])
    {
        if (material != nullptr)
            m_queue.WriteBuffer(material->colorBuffer, 0, baseColor, 16);
    }

    void Renderer::CreateDepthTexture()
    {
        wgpu::TextureDescriptor desc{};
        desc.size.width = m_width > 0 ? m_width : 1;
        desc.size.height = m_height > 0 ? m_height : 1;
        desc.size.depthOrArrayLayers = 1;
        desc.format = m_depthFormat;
        desc.usage = wgpu::TextureUsage::RenderAttachment;
        m_depthTexture = m_device.CreateTexture(&desc);
        m_depthView = m_depthTexture.CreateView();
    }

    // Build the shadow-map render target, its comparison sampler, the shadow-pass
    // uniform (lightViewProj) + bind group, and the depth-only shadow pipeline. Called
    // once during init; the shadow texture is a fixed kShadowMapSize square.
    void Renderer::CreateShadowResources()
    {
        // Depth texture sampled by the main pass as texture_depth_2d.
        wgpu::TextureDescriptor texDesc{};
        texDesc.size.width = kShadowMapSize;
        texDesc.size.height = kShadowMapSize;
        texDesc.size.depthOrArrayLayers = 1;
        texDesc.format = m_shadowFormat;
        texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        m_shadowTexture = m_device.CreateTexture(&texDesc);
        m_shadowView = m_shadowTexture.CreateView();

        // Comparison sampler (depth <= reference). Clamp so off-map lookups stay lit.
        wgpu::SamplerDescriptor sampDesc{};
        sampDesc.compare = wgpu::CompareFunction::LessEqual;
        sampDesc.magFilter = wgpu::FilterMode::Linear;
        sampDesc.minFilter = wgpu::FilterMode::Linear;
        sampDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
        sampDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
        m_shadowSampler = m_device.CreateSampler(&sampDesc);

        // Shadow-pass group 0: just the lightViewProj uniform (vertex).
        m_shadowFrameBindGroupLayout = MakeUniformBgl(m_device, wgpu::ShaderStage::Vertex);
        wgpu::BufferDescriptor ubDesc{};
        ubDesc.size = 64;
        ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        m_shadowUniformBuffer = m_device.CreateBuffer(&ubDesc);
        m_shadowUniformBindGroup = MakeUniformBindGroup(m_device, m_shadowFrameBindGroupLayout, m_shadowUniformBuffer, 64);

        // Depth-only pipeline. Reuses the model BGL (group 1) for the per-mesh world.
        {
            wgpu::BindGroupLayout bgls[2] = {m_shadowFrameBindGroupLayout, m_modelBindGroupLayout};
            wgpu::PipelineLayoutDescriptor plDesc{};
            plDesc.bindGroupLayoutCount = 2;
            plDesc.bindGroupLayouts = bgls;
            m_shadowPipelineLayout = m_device.CreatePipelineLayout(&plDesc);
        }

        wgpu::ShaderSourceWGSL wgslSource{};
        wgslSource.code = kShadowWgsl;
        wgpu::ShaderModuleDescriptor moduleDesc{};
        moduleDesc.nextInChain = &wgslSource;
        wgpu::ShaderModule module = m_device.CreateShaderModule(&moduleDesc);

        // Read only position (location 0) from the shared interleaved vertex buffer.
        wgpu::VertexAttribute posAttr{};
        posAttr.format = wgpu::VertexFormat::Float32x3;
        posAttr.offset = 0;
        posAttr.shaderLocation = 0;
        wgpu::VertexBufferLayout vbLayout{};
        vbLayout.arrayStride = 11 * sizeof(float);
        vbLayout.attributeCount = 1;
        vbLayout.attributes = &posAttr;

        wgpu::DepthStencilState depthStencil{};
        depthStencil.format = m_shadowFormat;
        depthStencil.depthWriteEnabled = true;
        depthStencil.depthCompare = wgpu::CompareFunction::Less;

        wgpu::RenderPipelineDescriptor pipelineDesc{};
        pipelineDesc.layout = m_shadowPipelineLayout;
        pipelineDesc.vertex.module = module;
        pipelineDesc.vertex.entryPoint = "vs_main";
        pipelineDesc.vertex.bufferCount = 1;
        pipelineDesc.vertex.buffers = &vbLayout;
        pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
        pipelineDesc.depthStencil = &depthStencil;
        pipelineDesc.fragment = nullptr; // depth-only
        m_shadowPipeline = m_device.CreateRenderPipeline(&pipelineDesc);
    }

    // Compute the directional light's view-projection (an orthographic frustum aimed
    // along the first directional light) and upload it to the shadow uniform buffer.
    // Sets m_lightViewProj for the main pass and m_hasShadowLight.
    void Renderer::ComputeLightMatrix()
    {
        m_hasShadowLight = false;
        for (const auto& l : m_lights)
        {
            if (l.posType[3] >= 0.5f) continue; // not directional
            float dir[3] = {l.posType[0], l.posType[1], l.posType[2]};
            float len = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
            if (len < 1e-4f) continue;
            dir[0]/=len; dir[1]/=len; dir[2]/=len;

            // Place the light eye opposite the light's travel direction, framing origin.
            const float dist = 12.0f;
            float eye[3] = {-dir[0]*dist, -dir[1]*dist, -dir[2]*dist};
            float target[3] = {0.0f, 0.0f, 0.0f};
            float up[3] = {0.0f, 1.0f, 0.0f};
            if (std::fabs(dir[1]) > 0.99f) { up[0]=0.0f; up[1]=0.0f; up[2]=1.0f; }

            float view[16]; MatLookAt(eye, target, up, view);
            float proj[16]; MatOrtho(9.0f, 0.1f, 30.0f, proj);
            MatMul(proj, view, m_lightViewProj);
            m_hasShadowLight = true;
            break;
        }
        if (!m_hasShadowLight)
            MatIdentity(m_lightViewProj);
        if (m_shadowUniformBuffer != nullptr)
            m_queue.WriteBuffer(m_shadowUniformBuffer, 0, m_lightViewProj, sizeof(float) * 16);
    }

    Node* Renderer::AddNode(Node* parent)
    {
        if (!m_initialized) return nullptr;
        auto node = std::make_unique<Node>();
        node->parent = parent;
        MatIdentity(node->local);
        MatIdentity(node->world);
        // Per-node model uniform (mat4 world), written each frame. Exposed to JS so a
        // JS-built bind group (Model B) can reference the same buffer native writes.
        wgpu::BufferDescriptor mDesc{};
        mDesc.size = 64;
        mDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        node->modelBuffer = m_device.CreateBuffer(&mDesc);
        float identity[16]; MatIdentity(identity);
        m_queue.WriteBuffer(node->modelBuffer, 0, identity, sizeof(identity));
        Node* raw = node.get();
        m_nodes.push_back(std::move(node));
        return raw;
    }

    const wgpu::Buffer& Renderer::NodeModelBuffer(Node* node) const
    {
        return node->modelBuffer;
    }

    const char* Renderer::ColorFormatString() const
    {
        switch (m_format)
        {
        case wgpu::TextureFormat::BGRA8Unorm: return "bgra8unorm";
        case wgpu::TextureFormat::RGBA8Unorm: return "rgba8unorm";
        case wgpu::TextureFormat::BGRA8UnormSrgb: return "bgra8unorm-srgb";
        case wgpu::TextureFormat::RGBA8UnormSrgb: return "rgba8unorm-srgb";
        default: return "bgra8unorm";
        }
    }

    const char* Renderer::DepthFormatString() const
    {
        return m_depthFormat == wgpu::TextureFormat::Depth32Float ? "depth32float" : "depth24plus";
    }

    void Renderer::RegisterDrawable(const wgpu::RenderPipeline& pipeline,
        std::vector<wgpu::BindGroup> bindGroups, const wgpu::Buffer& vertexBuffer,
        const wgpu::Buffer& indexBuffer, uint32_t indexCount, wgpu::IndexFormat indexFormat,
        Node* node)
    {
        ExternalDrawable d{};
        d.pipeline = pipeline;
        d.bindGroups = std::move(bindGroups);
        d.vertexBuffer = vertexBuffer;
        d.indexBuffer = indexBuffer;
        d.indexCount = indexCount;
        d.indexFormat = indexFormat;
        d.node = node;
        m_externalDrawables.push_back(std::move(d));
    }

    void Renderer::SetNodeTransform(Node* node, const float position[3], const float rotation[3],
        const float scale[3])
    {
        if (node == nullptr) return;
        std::memcpy(node->position, position, sizeof(float) * 3);
        std::memcpy(node->rotation, rotation, sizeof(float) * 3);
        std::memcpy(node->scale, scale, sizeof(float) * 3);
        MatModel(position, rotation, scale, node->local);
    }

    Animation* Renderer::AddAnimation(Node* node, uint32_t property, const AnimationKey* keys,
        uint32_t keyCount, float duration, bool loop)
    {
        if (!m_initialized || node == nullptr || keys == nullptr || keyCount == 0)
            return nullptr;
        auto anim = std::make_unique<Animation>();
        anim->node = node;
        anim->property = property;
        anim->keys.assign(keys, keys + keyCount);
        anim->duration = duration > 0.0f ? duration : anim->keys.back().time;
        if (anim->duration <= 0.0f) anim->duration = 1.0f;
        anim->loop = loop;
        Animation* raw = anim.get();
        m_animations.push_back(std::move(anim));
        return raw;
    }

    // Sample every animation at the current time and write the result into its target
    // node's TRS channel, then recompose that node's local matrix. Runs each frame in
    // the update pass, fully native — the demonstration that animation needs zero JS.
    void Renderer::AdvanceAnimations(float timeSeconds)
    {
        for (const auto& anim : m_animations)
        {
            if (anim->keys.empty()) continue;

            float t = timeSeconds;
            if (anim->loop)
                t = std::fmod(timeSeconds, anim->duration);
            else if (t > anim->duration)
                t = anim->duration;

            // Find the key span [k0, k1] bracketing t and lerp; clamp at the ends.
            const AnimationKey* k0 = &anim->keys.front();
            const AnimationKey* k1 = &anim->keys.front();
            for (size_t i = 0; i + 1 < anim->keys.size(); ++i)
            {
                if (t >= anim->keys[i].time && t <= anim->keys[i + 1].time)
                {
                    k0 = &anim->keys[i];
                    k1 = &anim->keys[i + 1];
                    break;
                }
                if (t > anim->keys[i + 1].time)
                {
                    k0 = &anim->keys[i + 1];
                    k1 = &anim->keys[i + 1];
                }
            }
            float span = k1->time - k0->time;
            float alpha = span > 1e-6f ? (t - k0->time) / span : 0.0f;

            float value[3];
            for (int c = 0; c < 3; ++c)
                value[c] = k0->value[c] + (k1->value[c] - k0->value[c]) * alpha;

            Node* node = anim->node;
            float* channel = node->position;
            if (anim->property == AnimationProperty_Rotation) channel = node->rotation;
            else if (anim->property == AnimationProperty_Scale) channel = node->scale;
            std::memcpy(channel, value, sizeof(float) * 3);

            MatModel(node->position, node->rotation, node->scale, node->local);
        }
    }

    Mesh* Renderer::AddMesh(const float* positions, const float* normals, const float* uvs,
        const float* colors, uint32_t vertexCount, const uint16_t* indices,
        uint32_t indexCount, Material* material, Node* node)
    {
        if (!m_initialized || vertexCount == 0 || indexCount == 0 || material == nullptr || node == nullptr)
            return nullptr;

        auto mesh = std::make_unique<Mesh>();
        mesh->indexCount = indexCount;
        mesh->material = material;
        mesh->node = node;

        std::vector<float> interleaved(static_cast<size_t>(vertexCount) * 11);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            interleaved[i*11+0] = positions[i*3+0];
            interleaved[i*11+1] = positions[i*3+1];
            interleaved[i*11+2] = positions[i*3+2];
            interleaved[i*11+3] = normals[i*3+0];
            interleaved[i*11+4] = normals[i*3+1];
            interleaved[i*11+5] = normals[i*3+2];
            interleaved[i*11+6] = uvs[i*2+0];
            interleaved[i*11+7] = uvs[i*2+1];
            interleaved[i*11+8] = colors[i*3+0];
            interleaved[i*11+9] = colors[i*3+1];
            interleaved[i*11+10] = colors[i*3+2];
        }

        wgpu::BufferDescriptor vbDesc{};
        vbDesc.size = interleaved.size() * sizeof(float);
        vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        mesh->vertexBuffer = m_device.CreateBuffer(&vbDesc);
        m_queue.WriteBuffer(mesh->vertexBuffer, 0, interleaved.data(), vbDesc.size);

        const size_t indexBytes = ((static_cast<size_t>(indexCount) * sizeof(uint16_t)) + 3u) & ~size_t(3u);
        std::vector<uint16_t> paddedIndices(indexBytes / sizeof(uint16_t), 0);
        std::memcpy(paddedIndices.data(), indices, static_cast<size_t>(indexCount) * sizeof(uint16_t));
        wgpu::BufferDescriptor ibDesc{};
        ibDesc.size = indexBytes;
        ibDesc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        mesh->indexBuffer = m_device.CreateBuffer(&ibDesc);
        m_queue.WriteBuffer(mesh->indexBuffer, 0, paddedIndices.data(), indexBytes);

        wgpu::BufferDescriptor mDesc{};
        mDesc.size = 64;
        mDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        mesh->modelBuffer = m_device.CreateBuffer(&mDesc);
        float identity[16]; MatIdentity(identity);
        m_queue.WriteBuffer(mesh->modelBuffer, 0, identity, sizeof(identity));
        mesh->modelBindGroup = MakeUniformBindGroup(m_device, m_modelBindGroupLayout, mesh->modelBuffer, 64);

        Mesh* raw = mesh.get();
        m_meshes.push_back(std::move(mesh));
        return raw;
    }

    // The per-frame update pass: walk the scene graph (creation order guarantees a
    // parent precedes its children), compute each node's world = parent.world * local,
    // then upload each mesh's node world matrix to its model buffer. This is the
    // native realization of Babylon Lite's world-matrix update phase.
    void Renderer::UpdateWorldMatrices()
    {
        for (const auto& node : m_nodes)
        {
            if (node->parent != nullptr)
                MatMul(node->parent->world, node->local, node->world);
            else
                std::memcpy(node->world, node->local, sizeof(float) * 16);
            // Write the node's own model buffer (used by Model B external drawables).
            if (node->modelBuffer != nullptr)
                m_queue.WriteBuffer(node->modelBuffer, 0, node->world, sizeof(float) * 16);
        }
        for (const auto& mesh : m_meshes)
        {
            m_queue.WriteBuffer(mesh->modelBuffer, 0, mesh->node->world, sizeof(float) * 16);
        }
    }

    void Renderer::SetCameraView(const float eye[3], const float target[3], const float up[3])
    {
        m_cameraPos[0] = eye[0];
        m_cameraPos[1] = eye[1];
        m_cameraPos[2] = eye[2];
        MatLookAt(eye, target, up, m_view);
        UploadFrameUniform();
    }

    void Renderer::SetCameraProjection(float fovYRadians, float aspect, float nearZ, float farZ)
    {
        MatPerspective(fovYRadians, aspect, nearZ, farZ, m_proj);
        UploadFrameUniform();
    }

    void Renderer::ClearLights()
    {
        m_lights.clear();
        UploadFrameUniform();
    }

    void Renderer::AddLight(uint32_t type, const float vec[3], const float color[3], float intensity)
    {
        if (m_lights.size() >= kMaxLights)
            return;
        LightData l{};
        l.posType[0] = vec[0]; l.posType[1] = vec[1]; l.posType[2] = vec[2];
        l.posType[3] = static_cast<float>(type);
        l.colorIntensity[0] = color[0]; l.colorIntensity[1] = color[1]; l.colorIntensity[2] = color[2];
        l.colorIntensity[3] = intensity;
        m_lights.push_back(l);
        UploadFrameUniform();
    }

    void Renderer::UploadFrameUniform()
    {
        if (m_frameBuffer == nullptr) return;
        // Recompute the directional light's shadow matrix from the current lights.
        ComputeLightMatrix();
        // 288 bytes / 72 floats: viewProj[16] + cameraPos[4] + lightCount[4] +
        // lights[4*8] + lightViewProj[16].
        float frame[72];
        std::memset(frame, 0, sizeof(frame));
        MatMul(m_proj, m_view, frame);
        frame[16] = m_cameraPos[0];
        frame[17] = m_cameraPos[1];
        frame[18] = m_cameraPos[2];
        frame[20] = static_cast<float>(m_lights.size());
        for (size_t i = 0; i < m_lights.size() && i < kMaxLights; ++i)
        {
            float* dst = frame + 24 + i * 8;
            std::memcpy(dst, m_lights[i].posType, sizeof(float) * 4);
            std::memcpy(dst + 4, m_lights[i].colorIntensity, sizeof(float) * 4);
        }
        std::memcpy(frame + 56, m_lightViewProj, sizeof(float) * 16);
        m_queue.WriteBuffer(m_frameBuffer, 0, frame, sizeof(frame));
    }

    void Renderer::Configure()
    {
        wgpu::SurfaceConfiguration config{};
        config.device = m_device;
        config.format = m_format;
        config.usage = wgpu::TextureUsage::RenderAttachment;
        config.width = m_width > 0 ? m_width : 1;
        config.height = m_height > 0 ? m_height : 1;
        config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
        config.presentMode = wgpu::PresentMode::Fifo;
        m_surface.Configure(&config);
    }

    void Renderer::RequestResize(int width, int height)
    {
        if (width > 0 && height > 0)
        {
            m_pendingWidth = width;
            m_pendingHeight = height;
            m_resizeDirty = true;
        }
    }

    void Renderer::RenderFrame()
    {
        if (!m_initialized) return;

        if (m_resizeDirty.exchange(false))
        {
            m_width = static_cast<uint32_t>(m_pendingWidth.load());
            m_height = static_cast<uint32_t>(m_pendingHeight.load());
            Configure();
            CreateDepthTexture();
        }

        // Update pass: advance native animations, recompute the scene-graph world
        // matrices and upload them, then the record pass below encodes the draws. This
        // is the update()/record() split — all of it runs with zero JavaScript.
        if (!m_clockStarted)
        {
            m_clockStart = std::chrono::steady_clock::now();
            m_clockStarted = true;
        }
        float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - m_clockStart).count();
        AdvanceAnimations(elapsed);
        UpdateWorldMatrices();

        wgpu::SurfaceTexture surfaceTexture{};
        m_surface.GetCurrentTexture(&surfaceTexture);
        if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
            surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
        {
            Configure();
            CreateDepthTexture();
            return;
        }

        wgpu::RenderPassColorAttachment colorAttachment{};
        colorAttachment.view = surfaceTexture.texture.CreateView();
        colorAttachment.loadOp = wgpu::LoadOp::Clear;
        colorAttachment.storeOp = wgpu::StoreOp::Store;
        colorAttachment.clearValue = wgpu::Color{0.07, 0.08, 0.10, 1.0};

        wgpu::RenderPassDepthStencilAttachment depthAttachment{};
        depthAttachment.view = m_depthView;
        depthAttachment.depthLoadOp = wgpu::LoadOp::Clear;
        depthAttachment.depthStoreOp = wgpu::StoreOp::Store;
        depthAttachment.depthClearValue = 1.0f;

        wgpu::RenderPassDescriptor passDesc{};
        passDesc.colorAttachmentCount = 1;
        passDesc.colorAttachments = &colorAttachment;
        passDesc.depthStencilAttachment = &depthAttachment;

        wgpu::CommandEncoder encoder = m_device.CreateCommandEncoder();

        // ---- Shadow pass: render scene depth from the light's POV into the shadow map.
        if (m_hasShadowLight)
        {
            wgpu::RenderPassDepthStencilAttachment shadowDepth{};
            shadowDepth.view = m_shadowView;
            shadowDepth.depthLoadOp = wgpu::LoadOp::Clear;
            shadowDepth.depthStoreOp = wgpu::StoreOp::Store;
            shadowDepth.depthClearValue = 1.0f;
            wgpu::RenderPassDescriptor shadowPassDesc{};
            shadowPassDesc.colorAttachmentCount = 0;
            shadowPassDesc.depthStencilAttachment = &shadowDepth;

            wgpu::RenderPassEncoder shadowPass = encoder.BeginRenderPass(&shadowPassDesc);
            shadowPass.SetPipeline(m_shadowPipeline);
            shadowPass.SetBindGroup(0, m_shadowUniformBindGroup);
            for (const auto& mesh : m_meshes)
            {
                shadowPass.SetBindGroup(1, mesh->modelBindGroup);
                shadowPass.SetVertexBuffer(0, mesh->vertexBuffer);
                shadowPass.SetIndexBuffer(mesh->indexBuffer, wgpu::IndexFormat::Uint16);
                shadowPass.DrawIndexed(mesh->indexCount);
            }
            shadowPass.End();
        }

        // ---- Main pass: shade the scene, sampling the shadow map for occlusion.
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
        pass.SetBindGroup(0, m_frameBindGroup);

        for (const auto& mesh : m_meshes)
        {
            pass.SetPipeline(m_pipelineCache[mesh->material->features]);
            pass.SetBindGroup(1, mesh->modelBindGroup);
            pass.SetBindGroup(2, mesh->material->bindGroup);
            pass.SetVertexBuffer(0, mesh->vertexBuffer);
            pass.SetIndexBuffer(mesh->indexBuffer, wgpu::IndexFormat::Uint16);
            pass.DrawIndexed(mesh->indexCount);
        }

        // ---- Model B drawables: JS created the pipeline + bind groups + buffers; the
        // native loop just binds and draws them. Each carries its own full set of bind
        // groups (group 0 = frame, group 1 = model, group 2 = material), so we don't
        // assume the native frame bind group / layout here.
        for (const auto& d : m_externalDrawables)
        {
            pass.SetPipeline(d.pipeline);
            for (uint32_t i = 0; i < d.bindGroups.size(); ++i)
                pass.SetBindGroup(i, d.bindGroups[i]);
            pass.SetVertexBuffer(0, d.vertexBuffer);
            pass.SetIndexBuffer(d.indexBuffer, d.indexFormat);
            pass.DrawIndexed(d.indexCount);
        }
        pass.End();
        wgpu::CommandBuffer commands = encoder.Finish();
        m_queue.Submit(1, &commands);

        m_surface.Present();
        m_instance.ProcessEvents();
    }

    void Renderer::Shutdown()
    {
        if (!m_initialized && m_instance == nullptr) return;
        m_initialized = false;
        m_externalDrawables.clear();
        m_meshes.clear();
        m_nodes.clear();
        m_animations.clear();
        m_materials.clear();
        m_textures.clear();
        m_pipelineCache.clear();
        m_shadowPipeline = nullptr;
        m_shadowPipelineLayout = nullptr;
        m_shadowFrameBindGroupLayout = nullptr;
        m_shadowUniformBindGroup = nullptr;
        m_shadowUniformBuffer = nullptr;
        m_shadowSampler = nullptr;
        m_shadowView = nullptr;
        m_shadowTexture = nullptr;
        m_depthView = nullptr;
        m_depthTexture = nullptr;
        m_frameBindGroup = nullptr;
        m_frameBuffer = nullptr;
        m_sampler = nullptr;
        m_texturedPipelineLayout = nullptr;
        m_pipelineLayout = nullptr;
        m_texturedMaterialBindGroupLayout = nullptr;
        m_materialBindGroupLayout = nullptr;
        m_modelBindGroupLayout = nullptr;
        m_frameBindGroupLayout = nullptr;
        m_surface = nullptr;
        m_queue = nullptr;
        m_device = nullptr;
        m_adapter = nullptr;
        m_instance = nullptr;
    }
}
