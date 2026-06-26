#pragma once

#include <webgpu/webgpu_cpp.h>
#include <napi/napi.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// Dawn exposed to JavaScript as real WebGPU, as N-API ObjectWrap classes.
//
// Each wrapper OWNS a refcounted Dawn handle (wgpu C++ objects are refcounted), so a
// JS GPU* object keeps the underlying Dawn object alive and releases its reference on
// GC. These wrap the SAME Dawn objects the native renderer submits on, so a resource
// JS creates here is the exact resource the native render loop binds and draws.
//
// This is the "expose Dawn as navigator.gpu / GPU*" surface. It now covers enough of
// the resource-creation API (shader modules, buffers, textures, samplers, bind group
// layouts, pipeline layouts, bind groups, render pipelines) for a JS engine layer to
// build all of a scene's GPU resources, leaving only the per-frame render loop native
// (Model B: zero engine JS per frame, setup in JS over the polyfill).
namespace lite::webgpu
{
    // Each wrapper follows the same shape: an ObjectWrap with a static DefineClass, a
    // constructor that copies a Dawn handle smuggled in via Napi::External, and a
    // Handle() accessor the native side (or sibling wrappers) reads.

    // Construction payload for a Buffer wrapper. Besides the Dawn handle it carries the
    // sub-allocation view (baseOffset/size/isSub) so the polyfill can pack many tiny logical
    // GPUBuffers into a few large Dawn buffers (D3D12 rounds every standalone buffer up to a
    // 64 KB placement floor, so 8000 unique meshes × ~6 small buffers ≈ 3 GB of waste —
    // suballocation collapses that to the real ~8 MB). For a sub-buffer, `buffer` is the
    // shared arena, `baseOffset` is the byte offset of this logical buffer within it, and
    // `queue` lets unmap() upload a mapped-at-creation sub-buffer via writeBuffer.
    struct BufferInit
    {
        wgpu::Buffer buffer;
        uint64_t baseOffset = 0;
        uint64_t size = 0;
        bool isSub = false;
        wgpu::Queue queue;
    };

    class Buffer : public Napi::ObjectWrap<Buffer>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Buffer(const Napi::CallbackInfo& info);
        const wgpu::Buffer& Handle() const { return m_buffer; }
        // Byte offset of this logical buffer within Handle() (0 unless sub-allocated). Every
        // buffer-consuming site (writeBuffer, setVertex/IndexBuffer, bind-group entry,
        // copyBufferToBuffer) adds this to the caller's local offset.
        uint64_t BaseOffset() const { return m_baseOffset; }
        uint64_t Size() const { return m_size; }
        bool IsSub() const { return m_isSub; }
        Napi::Value GetMappedRange(const Napi::CallbackInfo& info);
        Napi::Value Unmap(const Napi::CallbackInfo& info);
        Napi::Value Destroy(const Napi::CallbackInfo& info);
    private:
        wgpu::Buffer m_buffer;
        uint64_t m_baseOffset = 0;  // sub-allocation offset within m_buffer (arena)
        uint64_t m_size = 0;        // logical buffer size
        bool m_isSub = false;       // true if m_buffer is a shared arena
        wgpu::Queue m_queue;        // device queue (used to upload mapped sub-buffers at unmap)
        // Outstanding getMappedRange() allocations. ChakraCore's N-API does NOT alias
        // external ArrayBuffers (Napi::ArrayBuffer::New(env, ptr, size) gives JS a private
        // copy), so we hand JS an engine-owned ArrayBuffer and copy it into Dawn's mapped
        // memory at unmap(). offset/size locate the destination within the Dawn buffer.
        struct MappedRange
        {
            Napi::Reference<Napi::ArrayBuffer> ref;
            uint64_t offset;
            uint64_t size;
        };
        std::vector<MappedRange> m_mappedRanges;
    };

    class TextureView : public Napi::ObjectWrap<TextureView>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        TextureView(const Napi::CallbackInfo& info);
        const wgpu::TextureView& Handle() const { return m_view; }
    private:
        wgpu::TextureView m_view;
    };

    class Texture : public Napi::ObjectWrap<Texture>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Texture(const Napi::CallbackInfo& info);
        const wgpu::Texture& Handle() const { return m_texture; }
        Napi::Value CreateView(const Napi::CallbackInfo& info);
        Napi::Value Destroy(const Napi::CallbackInfo& info);
        Napi::Value GetWidth(const Napi::CallbackInfo& info);
        Napi::Value GetHeight(const Napi::CallbackInfo& info);
        Napi::Value GetMipLevelCount(const Napi::CallbackInfo& info);
        Napi::Value GetFormat(const Napi::CallbackInfo& info);
        Napi::Value GetDepthOrArrayLayers(const Napi::CallbackInfo& info);
    private:
        wgpu::Texture m_texture;
    };

    class Sampler : public Napi::ObjectWrap<Sampler>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Sampler(const Napi::CallbackInfo& info);
        const wgpu::Sampler& Handle() const { return m_sampler; }
    private:
        wgpu::Sampler m_sampler;
    };

    class ShaderModule : public Napi::ObjectWrap<ShaderModule>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        ShaderModule(const Napi::CallbackInfo& info);
        const wgpu::ShaderModule& Handle() const { return m_module; }
    private:
        wgpu::ShaderModule m_module;
    };

    class BindGroupLayout : public Napi::ObjectWrap<BindGroupLayout>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        BindGroupLayout(const Napi::CallbackInfo& info);
        const wgpu::BindGroupLayout& Handle() const { return m_layout; }
    private:
        wgpu::BindGroupLayout m_layout;
    };

    class PipelineLayout : public Napi::ObjectWrap<PipelineLayout>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        PipelineLayout(const Napi::CallbackInfo& info);
        const wgpu::PipelineLayout& Handle() const { return m_layout; }
    private:
        wgpu::PipelineLayout m_layout;
    };

    class BindGroup : public Napi::ObjectWrap<BindGroup>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        BindGroup(const Napi::CallbackInfo& info);
        const wgpu::BindGroup& Handle() const { return m_group; }
    private:
        wgpu::BindGroup m_group;
    };

    class RenderPipeline : public Napi::ObjectWrap<RenderPipeline>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        RenderPipeline(const Napi::CallbackInfo& info);
        const wgpu::RenderPipeline& Handle() const { return m_pipeline; }
        Napi::Value GetBindGroupLayout(const Napi::CallbackInfo& info);
    private:
        wgpu::RenderPipeline m_pipeline;
    };

    // GPUComputePipeline — a compiled compute shader + layout. Used by Lite's environment
    // prefilter (cubemap RGBD decode, specular mip prefilter, BRDF LUT) and GPU culling.
    // getBindGroupLayout(i) returns the auto-derived layout when created with layout:"auto".
    class ComputePipeline : public Napi::ObjectWrap<ComputePipeline>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        ComputePipeline(const Napi::CallbackInfo& info);
        const wgpu::ComputePipeline& Handle() const { return m_pipeline; }
        Napi::Value GetBindGroupLayout(const Napi::CallbackInfo& info);
    private:
        wgpu::ComputePipeline m_pipeline;
    };

    // GPUComputePassEncoder — records compute dispatches. setPipeline/setBindGroup/
    // dispatchWorkgroups/end. Created by GPUCommandEncoder.beginComputePass().
    class ComputePassEncoder : public Napi::ObjectWrap<ComputePassEncoder>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        ComputePassEncoder(const Napi::CallbackInfo& info);
        Napi::Value SetPipeline(const Napi::CallbackInfo& info);
        Napi::Value SetBindGroup(const Napi::CallbackInfo& info);
        Napi::Value DispatchWorkgroups(const Napi::CallbackInfo& info);
        Napi::Value End(const Napi::CallbackInfo& info);
    private:
        wgpu::ComputePassEncoder m_pass;
    };

    // GPURenderBundle — opaque handle returned by GPURenderBundleEncoder.finish().
    // Replayed via GPURenderPassEncoder.executeBundles(); this is the mechanism Lite
    // uses to record opaque draws ONCE and replay them every frame for free — exactly
    // what the native render loop reads + replays (zero JS per frame).
    class RenderBundle : public Napi::ObjectWrap<RenderBundle>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        RenderBundle(const Napi::CallbackInfo& info);
        const wgpu::RenderBundle& Handle() const { return m_bundle; }
    private:
        wgpu::RenderBundle m_bundle;
    };

    // GPURenderBundleEncoder — records draw commands into a reusable render bundle.
    // Same draw surface as GPURenderPassEncoder (setPipeline/setBindGroup/
    // setVertexBuffer/setIndexBuffer/draw/drawIndexed) plus finish() -> GPURenderBundle.
    class RenderBundleEncoder : public Napi::ObjectWrap<RenderBundleEncoder>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        RenderBundleEncoder(const Napi::CallbackInfo& info);
        Napi::Value SetPipeline(const Napi::CallbackInfo& info);
        Napi::Value SetBindGroup(const Napi::CallbackInfo& info);
        Napi::Value SetVertexBuffer(const Napi::CallbackInfo& info);
        Napi::Value SetIndexBuffer(const Napi::CallbackInfo& info);
        Napi::Value Draw(const Napi::CallbackInfo& info);
        Napi::Value DrawIndexed(const Napi::CallbackInfo& info);
        Napi::Value Finish(const Napi::CallbackInfo& info);
    private:
        wgpu::RenderBundleEncoder m_encoder;
    };

    // GPUQueue — writeBuffer + writeTexture + submit onto the device queue.
    class Queue : public Napi::ObjectWrap<Queue>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Queue(const Napi::CallbackInfo& info);
        Napi::Value WriteBuffer(const Napi::CallbackInfo& info);
        Napi::Value WriteTexture(const Napi::CallbackInfo& info);
        Napi::Value CopyExternalImageToTexture(const Napi::CallbackInfo& info);
        Napi::Value Submit(const Napi::CallbackInfo& info);
    private:
        wgpu::Queue m_queue;
    };

    // ImageBitmap — decoded RGBA8 pixels of an image (PNG/JPEG), produced by the global
    // createImageBitmap(). Not a GPU object: it holds CPU pixels until uploaded to a GPU
    // texture via GPUQueue.copyExternalImageToTexture. Real Lite's glTF/texture loaders
    // decode images this way. width/height accessors + close() match the web ImageBitmap.
    class ImageBitmap : public Napi::ObjectWrap<ImageBitmap>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        ImageBitmap(const Napi::CallbackInfo& info);
        Napi::Value GetWidth(const Napi::CallbackInfo& info);
        Napi::Value GetHeight(const Napi::CallbackInfo& info);
        Napi::Value Close(const Napi::CallbackInfo& info);
        // Returns the decoded RGBA8 pixels as a fresh Uint8ClampedArray (copy), for a JS
        // 2D-canvas shim's getImageData(). Top-left origin, row-major, 4 bytes/pixel.
        Napi::Value GetPixels(const Napi::CallbackInfo& info);
        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }
        const std::vector<uint8_t>& Pixels() const { return m_pixels; }
    private:
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        std::vector<uint8_t> m_pixels; // RGBA8, row-major, top-left origin
    };

    // GPUCommandBuffer — opaque handle returned by GPUCommandEncoder.finish().
    class CommandBuffer : public Napi::ObjectWrap<CommandBuffer>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        CommandBuffer(const Napi::CallbackInfo& info);
        const wgpu::CommandBuffer& Handle() const { return m_commandBuffer; }
    private:
        wgpu::CommandBuffer m_commandBuffer;
    };

    // GPURenderPassEncoder — records draw commands into a render pass. Methods mirror
    // the WebGPU spec (setPipeline/setBindGroup/setVertexBuffer/setIndexBuffer/draw/
    // drawIndexed/setViewport/setScissorRect/end).
    class RenderPassEncoder : public Napi::ObjectWrap<RenderPassEncoder>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        RenderPassEncoder(const Napi::CallbackInfo& info);
        Napi::Value SetPipeline(const Napi::CallbackInfo& info);
        Napi::Value SetBindGroup(const Napi::CallbackInfo& info);
        Napi::Value SetVertexBuffer(const Napi::CallbackInfo& info);
        Napi::Value SetIndexBuffer(const Napi::CallbackInfo& info);
        Napi::Value Draw(const Napi::CallbackInfo& info);
        Napi::Value DrawIndexed(const Napi::CallbackInfo& info);
        Napi::Value SetViewport(const Napi::CallbackInfo& info);
        Napi::Value SetScissorRect(const Napi::CallbackInfo& info);
        Napi::Value ExecuteBundles(const Napi::CallbackInfo& info);
        Napi::Value End(const Napi::CallbackInfo& info);
    private:
        wgpu::RenderPassEncoder m_pass;
    };

    // GPUCommandEncoder — beginRenderPass(descriptor) + finish(). The descriptor's
    // color/depth attachments reference GPUTextureView wrappers.
    class CommandEncoder : public Napi::ObjectWrap<CommandEncoder>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        CommandEncoder(const Napi::CallbackInfo& info);
        Napi::Value BeginRenderPass(const Napi::CallbackInfo& info);
        Napi::Value BeginComputePass(const Napi::CallbackInfo& info);
        Napi::Value CopyTextureToTexture(const Napi::CallbackInfo& info);
        Napi::Value CopyBufferToBuffer(const Napi::CallbackInfo& info);
        Napi::Value Finish(const Napi::CallbackInfo& info);
    private:
        wgpu::CommandEncoder m_encoder;
    };

    // GPUDevice — the resource factory. Each create* parses a WebGPU descriptor and
    // builds the corresponding Dawn object, wrapped for JS. Exposes `queue`.
    class Device : public Napi::ObjectWrap<Device>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Device(const Napi::CallbackInfo& info);

        Napi::Value GetQueue(const Napi::CallbackInfo& info);
        Napi::Value CreateBuffer(const Napi::CallbackInfo& info);
        Napi::Value CreateTexture(const Napi::CallbackInfo& info);
        Napi::Value CreateSampler(const Napi::CallbackInfo& info);
        Napi::Value CreateShaderModule(const Napi::CallbackInfo& info);
        Napi::Value CreateBindGroupLayout(const Napi::CallbackInfo& info);
        Napi::Value CreatePipelineLayout(const Napi::CallbackInfo& info);
        Napi::Value CreateBindGroup(const Napi::CallbackInfo& info);
        Napi::Value CreateRenderPipeline(const Napi::CallbackInfo& info);
        Napi::Value CreateComputePipeline(const Napi::CallbackInfo& info);
        Napi::Value CreateRenderBundleEncoder(const Napi::CallbackInfo& info);
        Napi::Value CreateCommandEncoder(const Napi::CallbackInfo& info);
        Napi::Value GetFeatures(const Napi::CallbackInfo& info);
        Napi::Value Destroy(const Napi::CallbackInfo& info);

        const wgpu::Device& Handle() const { return m_device; }

    private:
        wgpu::Device m_device;
        Napi::ObjectReference m_queue;

        // ---- Small-buffer suballocation arena ----------------------------------------
        // D3D12 rounds every standalone buffer up to a 64 KB placement floor, so scenes
        // with thousands of tiny per-mesh vertex/index/uniform buffers waste enormous VRAM.
        // We pack buffers smaller than kSubAllocMax into shared arena buffers (one bucket
        // per requested usage mask) and hand out {arena, offset} views. Buffers needing a
        // host-visible mapping (MAP_READ/MAP_WRITE) or larger than the threshold are created
        // standalone. mappedAtCreation sub-buffers are uploaded via queue.writeBuffer at
        // unmap() instead of a real GPU mapping.
        struct SubArena
        {
            uint32_t usage = 0;        // exact JS-requested usage mask this arena serves
            wgpu::Buffer buffer;       // backing buffer, created with (usage | CopyDst)
            uint64_t capacity = 0;
            uint64_t used = 0;
        };
        std::vector<SubArena> m_arenas;
        wgpu::Queue m_arenaQueue;
        // Try to carve `size` bytes for a buffer of `usage` from an arena. Returns true and
        // fills outArena/outOffset on success; false means the caller should create a
        // standalone Dawn buffer.
        bool SubAllocate(uint64_t size, uint32_t usage, wgpu::Buffer& outArena, uint64_t& outOffset);
    };

    // GPUCanvasContext — canvas.getContext("webgpu"). Wraps the Module's HWND-bound
    // swapchain; `getCurrentTexture()` acquires this frame's swapchain GPUTexture;
    // `unconfigure()` tears it down. Presentation is driven by the native loop (the
    // browser presents implicitly; on Dawn native the host calls Module::Present()).
    class CanvasContext : public Napi::ObjectWrap<CanvasContext>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        CanvasContext(const Napi::CallbackInfo& info);
        Napi::Value Configure(const Napi::CallbackInfo& info);
        Napi::Value Unconfigure(const Napi::CallbackInfo& info);
        Napi::Value GetCurrentTexture(const Napi::CallbackInfo& info);
    private:
        uint32_t m_width = 0;
        uint32_t m_height = 0;
    };

    // GPUAdapter — wraps a wgpu::Adapter. `requestDevice(descriptor)` returns a Promise
    // resolving to a GPUDevice; `features` is a setlike (.has). Real Lite's createEngine
    // does `await navigator.gpu.requestAdapter()` then `await adapter.requestDevice(...)`.
    class Adapter : public Napi::ObjectWrap<Adapter>
    {
    public:
        static Napi::Function DefineClass(Napi::Env env);
        Adapter(const Napi::CallbackInfo& info);
        Napi::Value GetFeatures(const Napi::CallbackInfo& info);
        Napi::Value RequestDevice(const Napi::CallbackInfo& info);
    private:
        wgpu::Adapter m_adapter;
    };

    // Native window handle for the swapchain surface (HWND-bound). Graphics-API-agnostic
    // shape so the polyfill doesn't depend on the Renderer.
    struct WindowHandle
    {
        void* hwnd = nullptr;
        void* hinstance = nullptr;
    };

    // Owns the class constructors for one JS environment. Constructed once after the
    // env exists. A translation-unit-static pointer to the live Module lets the
    // ObjectWrap instance methods (e.g. Device::CreateBuffer) build sibling wrappers,
    // since Chakra's N-API shim lacks napi_set_instance_data.
    class Module
    {
    public:
        // Constructs the class table and creates the Dawn instance. `window` is the
        // HWND the canvas/swapchain binds to (see InstallNavigatorGpu / GPUCanvasContext).
        Module(Napi::Env env, WindowHandle window);

        // Stops the render thread (if started) before the Dawn objects are released.
        ~Module();

        // Installs `navigator.gpu` (requestAdapter / getPreferredCanvasFormat) on the
        // global. The adapter/device are created lazily from the Dawn instance.
        void InstallNavigatorGpu(Napi::Env env) const;

        // Builds an offscreen-style canvas object (width/height/getContext("webgpu"),
        // deliberately no clientWidth so Lite treats it as an OffscreenCanvas — size is
        // pushed in, no DOM layout path). One canvas per window/surface.
        Napi::Object CreateCanvas(Napi::Env env, uint32_t width, uint32_t height) const;

        // Installs the global `createImageBitmap(blob|ArrayBuffer|TypedArray, opts?)` which
        // decodes PNG/JPEG bytes to an ImageBitmap (RGBA8) via stb_image. Returns a Promise
        // per the web API. Real Lite's glTF/texture loaders call this at setup.
        void InstallCreateImageBitmap(Napi::Env env) const;

        const wgpu::Instance& Instance() const { return m_instance; }
        const wgpu::Surface& Surface() const { return m_surface; }
        WindowHandle Window() const { return m_window; }
        bool SurfaceConfigured() const { return m_surfaceConfigured; }

        // Called by GPUCanvasContext::configure — configures the Dawn surface, and by
        // the host loop to present the rendered frame + pump Dawn events.
        void ConfigureSurface(const wgpu::Device& device, wgpu::TextureFormat format,
            uint32_t width, uint32_t height);
        void Present();

        // Blocks until the GPU finishes all submitted work. Used by the headless benchmark
        // path in place of surface.Present() to give each frame a real GPU-completion boundary.
        void WaitForGpuIdle();

        // Debug ground-truth: copies the about-to-be-presented surface texture back to the
        // CPU and logs the center pixel + a coarse histogram to stderr. Guarded by env
        // LITE_READBACK=<frameNumber> (logs once when the present counter reaches it).
        // PrintWindow can't see D3D12 swapchain content, so this is the trustworthy check.
        void ReadbackAndLog(const char* label);

        // Records the surface texture acquired this frame (by either the JS getCurrentTexture
        // path or the native render loop) so ReadbackAndLog reads the texture that was actually
        // rendered + presented — re-acquiring in Present() returns a fresh, undrawn texture.
        void NoteAcquiredTexture(const wgpu::Texture& tex) { m_lastAcquiredTexture = tex; }

        // ---- Threaded submit (LITE_THREAD_SUBMIT=1) ----
        // Mirrors the browser's GPU-process split: the JS thread records the frame and hands
        // the finished CommandBuffer(s) off; a dedicated render thread runs the expensive
        // Dawn->D3D12 translation in queue.Submit() + surface.Present() OFF the JS thread.
        // EnqueueSubmit is called by GPUQueue.submit when threading is on; Present() then
        // enqueues a present marker and applies frame-in-flight back-pressure. Deferral is
        // armed only AFTER setup (first getCurrentTexture) so setup submits that destroy
        // transient textures (env prefilter mips) still run synchronously and stay valid.
        bool ThreadedSubmit() const { return m_threadedSubmit && m_renderLoopStarted.load(); }
        // Counts swapchain acquires; arms deferral only after m_armFrame frames so all
        // async asset loading + mip generation (whose setup submits destroy transient
        // textures) has completed synchronously first.
        void MarkRenderLoopStarted()
        {
            if (m_renderLoopStarted.load()) return;
            if (m_acquireCount.fetch_add(1) + 1 >= m_armFrame) m_renderLoopStarted.store(true);
        }
        void EnqueueSubmit(const wgpu::Queue& queue, std::vector<wgpu::CommandBuffer>&& buffers);
        void StartRenderThread();
        void StopRenderThread();

        // When true, the surface uses an uncapped present mode (Mailbox→Immediate fallback)
        // instead of Fifo, so a benchmark measures real GPU/CPU cost rather than the display
        // refresh. Set before the surface is configured.
        void SetNoVsync(bool noVsync) { m_noVsync = noVsync; }

        // Headless benchmark mode: render into a persistent OFFSCREEN color texture instead of
        // the swapchain, and replace surface.Present() with a per-frame GPU-completion wait.
        // This removes the desktop compositor's present back-pressure (which otherwise dominates
        // windowed wall-clock timings and varies wildly with focus/occlusion), giving a
        // reproducible "time to render" that reflects only CPU record/submit + GPU execute —
        // matching the reference benchmark's --no-window mode. Set before the surface configures.
        void SetHeadless(bool headless) { m_headless = headless; }
        bool Headless() const { return m_headless; }
        const wgpu::Texture& OffscreenColor() const { return m_offscreenColor; }

        Napi::Object CreateDevice(Napi::Env env, const wgpu::Device& device) const;
        Napi::Object WrapAdapter(Napi::Env env, const wgpu::Adapter& adapter) const;

        // Wrapper factories used by Device's create* methods and the native bridge.
        Napi::Object WrapBuffer(Napi::Env env, const BufferInit& init) const;
        // Convenience: wrap a standalone (non-suballocated) Dawn buffer.
        Napi::Object WrapBuffer(Napi::Env env, const wgpu::Buffer& buffer) const;
        Napi::Object WrapTexture(Napi::Env env, const wgpu::Texture& texture) const;
        Napi::Object WrapTextureView(Napi::Env env, const wgpu::TextureView& view) const;
        Napi::Object WrapSampler(Napi::Env env, const wgpu::Sampler& sampler) const;
        Napi::Object WrapShaderModule(Napi::Env env, const wgpu::ShaderModule& module) const;
        Napi::Object WrapBindGroupLayout(Napi::Env env, const wgpu::BindGroupLayout& layout) const;
        Napi::Object WrapPipelineLayout(Napi::Env env, const wgpu::PipelineLayout& layout) const;
        Napi::Object WrapBindGroup(Napi::Env env, const wgpu::BindGroup& group) const;
        Napi::Object WrapRenderPipeline(Napi::Env env, const wgpu::RenderPipeline& pipeline) const;
        Napi::Object WrapComputePipeline(Napi::Env env, const wgpu::ComputePipeline& pipeline) const;
        Napi::Object WrapComputePassEncoder(Napi::Env env, const wgpu::ComputePassEncoder& pass) const;
        Napi::Object WrapRenderBundle(Napi::Env env, const wgpu::RenderBundle& bundle) const;
        Napi::Object WrapRenderBundleEncoder(Napi::Env env, const wgpu::RenderBundleEncoder& encoder) const;
        Napi::Object WrapQueue(Napi::Env env, const wgpu::Queue& queue) const;
        Napi::Object WrapCommandEncoder(Napi::Env env, const wgpu::CommandEncoder& encoder) const;
        Napi::Object WrapRenderPassEncoder(Napi::Env env, const wgpu::RenderPassEncoder& pass) const;
        Napi::Object WrapCommandBuffer(Napi::Env env, const wgpu::CommandBuffer& cb) const;

        // Type-checked unwrap helpers (null if the JS object isn't that wrapper). Used
        // when a descriptor references another GPU object (bind group entries, pipeline
        // layouts, render-pipeline shader modules) and by the native scene bridge.
        Buffer* AsBuffer(const Napi::Object& o) const;
        Texture* AsTexture(const Napi::Object& o) const;
        TextureView* AsTextureView(const Napi::Object& o) const;
        Sampler* AsSampler(const Napi::Object& o) const;
        ShaderModule* AsShaderModule(const Napi::Object& o) const;
        BindGroupLayout* AsBindGroupLayout(const Napi::Object& o) const;
        PipelineLayout* AsPipelineLayout(const Napi::Object& o) const;
        BindGroup* AsBindGroup(const Napi::Object& o) const;
        RenderPipeline* AsRenderPipeline(const Napi::Object& o) const;
        ComputePipeline* AsComputePipeline(const Napi::Object& o) const;
        RenderBundle* AsRenderBundle(const Napi::Object& o) const;
        CommandBuffer* AsCommandBuffer(const Napi::Object& o) const;
        ImageBitmap* AsImageBitmap(const Napi::Object& o) const;

        static const Module* Current() { return s_current; }

    private:
        static const Module* s_current;

        Napi::FunctionReference m_bufferCtor;
        Napi::FunctionReference m_textureCtor;
        Napi::FunctionReference m_textureViewCtor;
        Napi::FunctionReference m_samplerCtor;
        Napi::FunctionReference m_shaderModuleCtor;
        Napi::FunctionReference m_bindGroupLayoutCtor;
        Napi::FunctionReference m_pipelineLayoutCtor;
        Napi::FunctionReference m_bindGroupCtor;
        Napi::FunctionReference m_renderPipelineCtor;
        Napi::FunctionReference m_computePipelineCtor;
        Napi::FunctionReference m_computePassEncoderCtor;
        Napi::FunctionReference m_renderBundleCtor;
        Napi::FunctionReference m_renderBundleEncoderCtor;
        Napi::FunctionReference m_queueCtor;
        Napi::FunctionReference m_deviceCtor;
        Napi::FunctionReference m_commandEncoderCtor;
        Napi::FunctionReference m_renderPassEncoderCtor;
        Napi::FunctionReference m_commandBufferCtor;
        Napi::FunctionReference m_adapterCtor;
        Napi::FunctionReference m_canvasContextCtor;
        Napi::FunctionReference m_imageBitmapCtor;

        wgpu::Instance m_instance;
        wgpu::Surface m_surface;
        mutable wgpu::Adapter m_adapter; // captured in WrapAdapter; used for surface caps query
        wgpu::TextureFormat m_surfaceFormat{};
        bool m_surfaceConfigured = false;
        bool m_noVsync = false;
        bool m_headless = false;          // render offscreen, no surface present (benchmark)
        wgpu::Texture m_offscreenColor;   // persistent offscreen render target (headless mode)
        wgpu::Device m_surfaceDevice;    // device the surface was configured with (for readback)
        bool m_surfaceCanCopySrc = false; // CopySrc added to surface usage (caps permitting)
        int m_presentCount = 0;          // presents issued; drives LITE_READBACK frame trigger
        int m_readbackFrame = -1;        // env LITE_READBACK target frame (-1 = disabled)
        wgpu::Texture m_lastAcquiredTexture; // surface texture acquired this frame (for readback)
        WindowHandle m_window;

        // ---- Threaded submit state ----
        // Each work item carries the queue + finished command buffers for one submit. The
        // render thread runs queue.Submit() (the expensive Dawn->D3D12 translation) off the
        // JS thread. Surface ops (getCurrentTexture/present) stay on the JS thread; Present()
        // waits on m_pendingSubmits before presenting the backbuffer the submit rendered into.
        struct RenderWork
        {
            wgpu::Queue queue;
            std::vector<wgpu::CommandBuffer> buffers;
        };
        bool m_threadedSubmit = false;
        std::atomic<bool> m_renderLoopStarted{false};   // armed after m_armFrame acquires (post-loading)
        std::atomic<int> m_acquireCount{0};
        int m_armFrame = 30;                            // frames to run synchronously before deferring
        std::thread m_renderThread;
        std::mutex m_renderMutex;
        std::condition_variable m_renderCv;             // signals render thread of new work
        std::condition_variable m_drainCv;              // signals JS thread when a submit completes
        std::deque<RenderWork> m_renderQueue;
        std::atomic<int> m_pendingSubmits{0};           // submits enqueued but not yet completed
        bool m_renderThreadStop = false;
        void RenderThreadMain();
    };
}
