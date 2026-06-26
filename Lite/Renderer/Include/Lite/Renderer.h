#pragma once

#include <webgpu/webgpu_cpp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lite
{
    // Material feature flags — the "permutation" that drives WGSL composition and
    // selects/creates a cached pipeline. Mirrors how Babylon Lite composes shader
    // source per material variant and caches the compiled pipeline by a key.
    enum MaterialFeatures : uint32_t
    {
        MaterialFeature_None = 0,
        MaterialFeature_VertexColor = 1u << 0, // multiply in per-vertex color
        MaterialFeature_Lighting = 1u << 1,    // apply the scene lights via normals
        MaterialFeature_Texture = 1u << 2,     // sample a base-color texture at UV
        MaterialFeature_PBR = 1u << 3,         // metallic-roughness Cook-Torrance (needs Lighting)
    };

    // A GPU texture handle (the sampled image) + its default view.
    struct Texture
    {
        wgpu::Texture texture;
        wgpu::TextureView view;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // A material: a feature permutation + a material uniform (baseColor + PBR params),
    // optionally a texture. Materials with the same feature set share one cached
    // pipeline; each keeps its own uniform buffer + bind group.
    struct Material
    {
        uint32_t features = MaterialFeature_None;
        wgpu::Buffer colorBuffer;     // vec4 baseColor + vec4 params (metallic, roughness, _, _)
        wgpu::BindGroup bindGroup;    // material uniform (+ texture + sampler when textured)
        Texture* texture = nullptr;
    };

    // A scene-graph transform node: position/rotation/scale components, a composed
    // local matrix, an optional parent, and a computed world matrix. World =
    // parent.world * local, recomputed natively each frame in the renderer's update
    // pass (the plan's update()/record() split). Keeping the TRS components (not just
    // the composed matrix) lets a native animation override a single channel and
    // recompose. A node may have a mesh attached; nodes without a mesh are pure groups.
    struct Node
    {
        Node* parent = nullptr;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float rotation[3] = {0.0f, 0.0f, 0.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
        float local[16];   // composed local TRS (column-major)
        float world[16];   // computed world matrix
        wgpu::Buffer modelBuffer; // per-node mat4 uniform (world), written each frame
    };

    // An externally-built drawable (Model B): the GPU resources are created in JS via
    // the WebGPU polyfill and registered here; the native loop only binds + draws them
    // and writes their node's world matrix each frame. This is the inversion of the
    // native-factory path — JS owns resource creation, native owns the render loop.
    struct ExternalDrawable
    {
        wgpu::RenderPipeline pipeline;
        std::vector<wgpu::BindGroup> bindGroups;
        wgpu::Buffer vertexBuffer;
        wgpu::Buffer indexBuffer;
        wgpu::IndexFormat indexFormat = wgpu::IndexFormat::Uint16;
        uint32_t indexCount = 0;
        Node* node = nullptr;
    };

    // A native keyframe animation: drives one transform channel (position, rotation,
    // or scale) of a target node from a list of time/value keys, advanced entirely in
    // the renderer's update pass (zero JS per frame — the project's core thesis).
    enum AnimationProperty : uint32_t
    {
        AnimationProperty_Position = 0,
        AnimationProperty_Rotation = 1,
        AnimationProperty_Scale = 2,
    };
    struct AnimationKey { float time; float value[3]; };
    struct Animation
    {
        Node* node = nullptr;
        uint32_t property = AnimationProperty_Position;
        std::vector<AnimationKey> keys;
        float duration = 1.0f;
        bool loop = true;
    };

    // A GPU-resident indexed 3D mesh: interleaved position.xyz + normal.xyz + uv.xy +
    // color.rgb (11 floats), a per-mesh model-matrix uniform, a material ref, and the
    // scene-graph node that provides its world transform.
    struct Mesh
    {
        wgpu::Buffer vertexBuffer;
        wgpu::Buffer indexBuffer;
        uint32_t indexCount = 0;
        wgpu::Buffer modelBuffer;     // mat4 (mesh's node world matrix, uploaded per frame)
        wgpu::BindGroup modelBindGroup;
        Material* material = nullptr;
        Node* node = nullptr;
    };

    // The native engine renderer: owns every Dawn object, a camera, a directional
    // light, a depth buffer, a shared sampler, a material/pipeline cache, a scene
    // graph, and a list of meshes. Pure native — no JavaScript/N-API dependency.
    class Renderer
    {
    public:
        struct WindowHandle
        {
            void* hwnd = nullptr;
            void* hinstance = nullptr;
            void* metalLayer = nullptr;
        };

        Renderer() = default;
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        bool Initialize(const WindowHandle& window, uint32_t width, uint32_t height);
        void RenderFrame();
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }
        void RequestResize(int width, int height);

        // Texture factory. pixelsRGBA is width*height*4 bytes (8-bit RGBA). Returns a
        // renderer-owned pointer valid until Shutdown.
        Texture* AddTexture(const uint8_t* pixelsRGBA, uint32_t width, uint32_t height);

        // Material factory. `texture` may be null (only used when the Texture feature
        // bit is set). `metallic`/`roughness` are used when the PBR feature bit is set.
        // Warms the composed-WGSL pipeline cache for the permutation.
        Material* AddMaterial(uint32_t features, const float baseColor[4], Texture* texture,
            float metallic = 0.0f, float roughness = 0.5f);
        void SetMaterialColor(Material* material, const float baseColor[4]);

        // Scene-graph node factory. `parent` may be null (a root node). Sets the
        // local transform via SetNodeTransform; world matrices are computed per frame.
        Node* AddNode(Node* parent);
        void SetNodeTransform(Node* node, const float position[3], const float rotation[3],
            const float scale[3]);

        // Native animation factory. Drives one transform channel of `node` from the
        // given keyframes; advanced each frame in the update pass with no JS. `keys`
        // are time/value pairs (value = vec3 in the channel's units); `duration` is the
        // loop period in seconds; `loop` repeats vs. clamps at the end.
        Animation* AddAnimation(Node* node, uint32_t property, const AnimationKey* keys,
            uint32_t keyCount, float duration, bool loop);

        // Mesh factory. positions/normals/colors are vec3 * vertexCount, uvs are
        // vec2 * vertexCount, indices uint16 * indexCount. `node` provides the world
        // transform (created via AddNode).
        Mesh* AddMesh(const float* positions, const float* normals, const float* uvs,
            const float* colors, uint32_t vertexCount, const uint16_t* indices,
            uint32_t indexCount, Material* material, Node* node);

        void SetCameraView(const float eye[3], const float target[3], const float up[3]);
        void SetCameraProjection(float fovYRadians, float aspect, float nearZ, float farZ);

        // Lights. type 0 = directional (vec is the direction of travel), type 1 =
        // point (vec is the world position). Up to kMaxLights are used; extras are
        // ignored. ClearLights resets the list.
        static constexpr uint32_t kMaxLights = 4;
        void ClearLights();
        void AddLight(uint32_t type, const float vec[3], const float color[3], float intensity);

        const wgpu::Device& Device() const { return m_device; }

        // ---- Model B (JS-created GPU resources, native loop) -------------------
        // The frame uniform buffer (camera/lights/viewProj) the native loop writes each
        // frame; JS references it in a group-0 bind group. Exposed as a GPUBuffer.
        const wgpu::Buffer& FrameBuffer() const { return m_frameBuffer; }
        // The swapchain color format + depth format, as WebGPU format strings, so
        // JS-built render pipelines target the same formats the native loop uses.
        const char* ColorFormatString() const;
        const char* DepthFormatString() const;
        // Register a JS-built drawable (pipeline + bind groups + vertex/index buffers +
        // a scene-graph node). The native loop draws it and writes node->modelBuffer.
        void RegisterDrawable(const wgpu::RenderPipeline& pipeline,
            std::vector<wgpu::BindGroup> bindGroups, const wgpu::Buffer& vertexBuffer,
            const wgpu::Buffer& indexBuffer, uint32_t indexCount, wgpu::IndexFormat indexFormat,
            Node* node);
        // The model uniform buffer owned by a node (created in AddNode), exposed so JS
        // can reference it in a group-1 bind group.
        const wgpu::Buffer& NodeModelBuffer(Node* node) const;

    private:
        bool InitDawn(const WindowHandle& window);
        void Configure();
        void CreateDepthTexture();
        void UploadFrameUniform();
        void ComputeLightMatrix(); // build the directional light's view-proj for shadows
        void CreateShadowResources(); // shadow map texture, sampler, depth-only pipeline
        void AdvanceAnimations(float timeSeconds); // sample animations -> node TRS, recompose locals
        void UpdateWorldMatrices(); // the per-frame update pass (scene graph -> model buffers)
        wgpu::RenderPipeline GetOrCreatePipeline(uint32_t features);
        const wgpu::BindGroupLayout& MaterialLayoutFor(uint32_t features) const;

        bool m_initialized = false;

        wgpu::Instance m_instance;
        wgpu::Adapter m_adapter;
        wgpu::Device m_device;
        wgpu::Queue m_queue;
        wgpu::Surface m_surface;

        // Bind group layouts: 0 = frame (camera+lights+shadow map), 1 = model,
        // 2 = material. The material layout has two forms: uniform-only, or
        // uniform+texture+sampler. The frame layout carries the shadow depth texture +
        // comparison sampler in addition to the frame uniform.
        wgpu::BindGroupLayout m_frameBindGroupLayout;
        wgpu::BindGroupLayout m_modelBindGroupLayout;
        wgpu::BindGroupLayout m_materialBindGroupLayout;        // color only
        wgpu::BindGroupLayout m_texturedMaterialBindGroupLayout; // color + texture + sampler
        wgpu::PipelineLayout m_pipelineLayout;          // for untextured permutations
        wgpu::PipelineLayout m_texturedPipelineLayout;  // for textured permutations
        wgpu::Sampler m_sampler;

        wgpu::Buffer m_frameBuffer; // viewProj + cameraPos + lightCount + lights[] + lightViewProj
        wgpu::BindGroup m_frameBindGroup;

        // Shadow mapping: a depth-only pass renders the scene from the primary
        // directional light's POV into m_shadowTexture; the main pass samples it with a
        // comparison sampler to darken occluded fragments.
        static constexpr uint32_t kShadowMapSize = 2048;
        wgpu::TextureFormat m_shadowFormat = wgpu::TextureFormat::Depth32Float;
        wgpu::Texture m_shadowTexture;
        wgpu::TextureView m_shadowView;
        wgpu::Sampler m_shadowSampler;                       // comparison sampler
        wgpu::Buffer m_shadowUniformBuffer;                  // lightViewProj (shadow pass)
        wgpu::BindGroup m_shadowUniformBindGroup;
        wgpu::BindGroupLayout m_shadowFrameBindGroupLayout;  // group 0 of the shadow pass
        wgpu::PipelineLayout m_shadowPipelineLayout;
        wgpu::RenderPipeline m_shadowPipeline;
        float m_lightViewProj[16];
        bool m_hasShadowLight = false;

        wgpu::Texture m_depthTexture;
        wgpu::TextureView m_depthView;
        wgpu::TextureFormat m_format{};
        wgpu::TextureFormat m_depthFormat = wgpu::TextureFormat::Depth24Plus;
        uint32_t m_width = 0;
        uint32_t m_height = 0;

        std::unordered_map<uint32_t, wgpu::RenderPipeline> m_pipelineCache;

        float m_view[16];
        float m_proj[16];
        float m_cameraPos[3] = {0.0f, 0.0f, 0.0f};

        // Each light = 8 floats: [x,y,z,type] + [r,g,b,intensity].
        struct LightData { float posType[4]; float colorIntensity[4]; };
        std::vector<LightData> m_lights;

        std::vector<std::unique_ptr<Texture>> m_textures;
        std::vector<std::unique_ptr<Material>> m_materials;
        std::vector<std::unique_ptr<Node>> m_nodes; // creation order = topological (parents first)
        std::vector<std::unique_ptr<Mesh>> m_meshes;
        std::vector<std::unique_ptr<Animation>> m_animations;
        std::vector<ExternalDrawable> m_externalDrawables;
        bool m_clockStarted = false;
        std::chrono::steady_clock::time_point m_clockStart;

        std::atomic<bool> m_resizeDirty{false};
        std::atomic<int> m_pendingWidth{0};
        std::atomic<int> m_pendingHeight{0};
    };

    // The WGSL-composition generator: assembles vertex + fragment shader source for a
    // material feature permutation. The parity-critical analogue of Babylon Lite's
    // runtime shader-string composition.
    std::string ComposeMeshWgsl(uint32_t features);
}
