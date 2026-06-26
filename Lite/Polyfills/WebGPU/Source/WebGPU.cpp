#include <Lite/WebGPU.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

// Image decode uses the Windows Imaging Component (WIC) — a built-in OS codec (PNG/JPEG/
// BMP/GIF/TIFF/...), so no third-party image library is vendored. LiteApp is Windows-only
// (Win32 host + Dawn D3D), so WIC is always available.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>


// WebGPU-over-N-API: parses WebGPU descriptors from JS into Dawn objects. The enum
// strings and descriptor shapes follow the WebGPU spec, so ordinary WebGPU/Lite setup
// code creates real Dawn resources here. Numeric bitfields (usage, visibility) match
// the spec values, which equal Dawn's flag values, so they pass straight through.
namespace lite::webgpu
{
    namespace
    {
        // ---- Per-frame GPU-call profiler (LITE_GPU_PROFILE=1) ----------------------
        // Wall-clock accumulator for the hot per-frame WebGPU methods, to localize the
        // native-vs-browser bundle-path gap. Each instrumented method adds its own
        // duration to a named slot; Module::Present() dumps + resets every 60 frames.
        // Off unless the env var is set (one getenv at first construction). The timer
        // measures the TOTAL method cost (our C++ marshaling + the Dawn call), so a fat
        // slot points at either our descriptor handling or Dawn's work for that op.
        enum ProfSlot {
            P_writeBuffer, P_getCurrentTexture, P_createView, P_createCommandEncoder,
            P_beginRenderPass, P_setBindGroup, P_executeBundles, P_endPass,
            P_finish, P_submit, P_present, P_COUNT
        };
        const char* kProfNames[P_COUNT] = {
            "writeBuffer", "getCurrentTexture", "createView", "createCommandEncoder",
            "beginRenderPass", "setBindGroup", "executeBundles", "endPass",
            "finish", "submit", "present"
        };
        struct FrameProfiler
        {
            bool enabled = false;
            uint64_t frame = 0;
            double acc[P_COUNT] = {0};
            uint64_t calls[P_COUNT] = {0};
            double accAll[P_COUNT] = {0};   // running sum across the dump window
            uint64_t callsAll[P_COUNT] = {0};
            uint64_t windowFrames = 0;

            FrameProfiler()
            {
                const char* v = std::getenv("LITE_GPU_PROFILE");
                enabled = v != nullptr && (v[0] == '1' || v[0] == 't' || v[0] == 'T');
            }
            void add(ProfSlot s, double ns) { if (enabled) { acc[s] += ns; calls[s]++; } }
            void endFrame()
            {
                if (!enabled) return;
                for (int i = 0; i < P_COUNT; ++i) { accAll[i] += acc[i]; callsAll[i] += calls[i]; }
                windowFrames++;
                frame++;
                if (windowFrames >= 60)
                {
                    double totalMs = 0;
                    for (int i = 0; i < P_COUNT; ++i) totalMs += accAll[i];
                    totalMs /= 1e6 * windowFrames; // ns total -> ms/frame
                    std::fprintf(stderr, "[gpuprof] frame %llu — per-frame avg over %llu frames (ms):\n",
                        (unsigned long long)frame, (unsigned long long)windowFrames);
                    for (int i = 0; i < P_COUNT; ++i)
                    {
                        double msPerFrame = accAll[i] / (1e6 * windowFrames);
                        double callsPerFrame = (double)callsAll[i] / windowFrames;
                        std::fprintf(stderr, "[gpuprof]   %-22s %8.4f ms  (%6.1f calls/frame)\n",
                            kProfNames[i], msPerFrame, callsPerFrame);
                    }
                    std::fprintf(stderr, "[gpuprof]   %-22s %8.4f ms  (instrumented WebGPU total)\n", "SUM", totalMs);
                    for (int i = 0; i < P_COUNT; ++i) { accAll[i] = 0; callsAll[i] = 0; }
                    windowFrames = 0;
                }
                for (int i = 0; i < P_COUNT; ++i) { acc[i] = 0; calls[i] = 0; }
            }
        };
        FrameProfiler g_prof;

        struct ScopedProf
        {
            ProfSlot slot;
            std::chrono::steady_clock::time_point t0;
            explicit ScopedProf(ProfSlot s) : slot(s), t0(std::chrono::steady_clock::now()) {}
            ~ScopedProf()
            {
                if (!g_prof.enabled) return;
                double ns = std::chrono::duration<double, std::nano>(
                    std::chrono::steady_clock::now() - t0).count();
                g_prof.add(slot, ns);
            }
        };

        // ---- GPU resource-allocation profiler (LITE_MEM_PROFILE=1) -----------------
        // Tracks how many GPU buffers/textures the polyfill creates and their requested
        // byte totals, broken down by WebGPU buffer-usage class. Surfaces per-mesh GPU
        // memory bloat (e.g. 8000 unique meshes → thousands of tiny vertex/index/uniform
        // buffers, each possibly rounded up to a Dawn/D3D12 heap-alignment minimum). Dumps
        // a running summary every 4000 buffer creations and on demand. "requested" is the
        // logical byte total; the driver's actual VRAM footprint is higher due to per-buffer
        // alignment/min-size — comparing the two quantifies the suballocation opportunity.
        struct MemProfiler
        {
            bool enabled = false;
            uint64_t bufCount = 0;
            uint64_t bufBytes = 0;
            uint64_t bufBytesAligned256 = 0;   // each buffer rounded up to 256 B
            uint64_t bufBytesAligned64K = 0;    // each buffer rounded up to 64 KB (D3D12 placed-resource floor)
            uint64_t texCount = 0;
            uint64_t texBytes = 0;
            uint64_t mappedAtCreationCount = 0;
            uint64_t mappedAtCreationBytes = 0;
            // Cumulative wall time spent inside the two hot resource calls, to separate the
            // engine-independent native work + napi-marshaling cost from pure-JS glTF parse.
            double createBufferNs = 0;
            double writeBufferNs = 0;
            uint64_t writeBufferCalls = 0;
            // Per-usage-bit buffer counts/bytes (VERTEX, INDEX, UNIFORM, STORAGE, COPY_DST, MAP_*).
            uint64_t usageCount[12] = {0};
            uint64_t usageBytes[12] = {0};
            const char* usageName[12] = {
                "MAP_READ","MAP_WRITE","COPY_SRC","COPY_DST","INDEX","VERTEX",
                "UNIFORM","STORAGE","INDIRECT","QUERY_RESOLVE","u10","u11"
            };

            MemProfiler()
            {
                const char* v = std::getenv("LITE_MEM_PROFILE");
                enabled = v != nullptr && (v[0] == '1' || v[0] == 't' || v[0] == 'T');
            }
            static uint64_t alignUp(uint64_t n, uint64_t a) { return (n + a - 1) / a * a; }
            void addBuffer(uint64_t size, uint32_t usage, bool mappedAtCreation = false)
            {
                if (!enabled) return;
                bufCount++;
                bufBytes += size;
                bufBytesAligned256 += alignUp(size, 256);
                bufBytesAligned64K += alignUp(size, 64 * 1024);
                if (mappedAtCreation) { mappedAtCreationCount++; mappedAtCreationBytes += size; }
                for (int b = 0; b < 12; ++b)
                    if (usage & (1u << b)) { usageCount[b]++; usageBytes[b] += size; }
                if (bufCount % 4000 == 0) dump("interval");
            }
            void addTexture(uint64_t size)
            {
                if (!enabled) return;
                texCount++;
                texBytes += size;
            }
            void dump(const char* tag)
            {
                if (!enabled) return;
                std::fprintf(stderr, "[memprof:%s] buffers=%llu requested=%.1fMB align256=%.1fMB align64K=%.1fMB | mappedAtCreation=%llu (%.1fMB) | textures=%llu ~%.1fMB\n",
                    tag, (unsigned long long)bufCount, bufBytes / 1048576.0,
                    bufBytesAligned256 / 1048576.0, bufBytesAligned64K / 1048576.0,
                    (unsigned long long)mappedAtCreationCount, mappedAtCreationBytes / 1048576.0,
                    (unsigned long long)texCount, texBytes / 1048576.0);
                std::fprintf(stderr, "[memprof:%s]   TIME createBuffer=%.1fms (%llu calls) writeBuffer=%.1fms (%llu calls)\n",
                    tag, createBufferNs / 1e6, (unsigned long long)bufCount,
                    writeBufferNs / 1e6, (unsigned long long)writeBufferCalls);
                for (int b = 0; b < 12; ++b)
                    if (usageCount[b])
                        std::fprintf(stderr, "[memprof:%s]   %-13s count=%llu bytes=%.2fMB avg=%lluB\n",
                            tag, usageName[b], (unsigned long long)usageCount[b],
                            usageBytes[b] / 1048576.0,
                            (unsigned long long)(usageBytes[b] / usageCount[b]));
            }
        };
        MemProfiler g_mem;

        // Threaded-submit default: ON unless LITE_THREAD_SUBMIT is explicitly set to a
        // false-y value (0/f/F/n/N). Reads the env once. Keeping it a single helper means
        // the device-toggle path and the Module flag agree.
        bool ThreadedSubmitDefault()
        {
            const char* v = std::getenv("LITE_THREAD_SUBMIT");
            if (v == nullptr || v[0] == '\0') return true; // default ON
            return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
        }

        // Decoded RGBA8 image moved from createImageBitmap into an ImageBitmap wrapper via
        // an N-API External (avoids copying the pixel buffer across the boundary).
        struct DecodedImage
        {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> pixels; // RGBA8, top-left origin
        };

        // Decode encoded image bytes (PNG/JPEG/...) to RGBA8 via the Windows Imaging
        // Component. WIC auto-detects the codec from the byte stream. Returns false on any
        // failure (logged). The WIC factory + per-thread COM init are created lazily on the
        // first call (this runs on the JS thread, which createImageBitmap is invoked from).
        bool DecodeImageWIC(const uint8_t* data, size_t size,
            std::vector<uint8_t>& outRGBA, uint32_t& outWidth, uint32_t& outHeight)
        {
            using Microsoft::WRL::ComPtr;
            static ComPtr<IWICImagingFactory> s_factory;
            static bool s_init = false;
            outRGBA.clear();
            outWidth = outHeight = 0;
            if (data == nullptr || size == 0) return false;

            if (!s_init)
            {
                HRESULT hrInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
                {
                    std::fprintf(stderr, "[wic] CoInitializeEx failed: 0x%08lX\n", hrInit);
                    return false;
                }
                HRESULT hrFac = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(s_factory.GetAddressOf()));
                if (FAILED(hrFac))
                {
                    std::fprintf(stderr, "[wic] CoCreateInstance(WICImagingFactory) failed: 0x%08lX\n", hrFac);
                    return false;
                }
                s_init = true;
            }

            ComPtr<IWICStream> stream;
            if (FAILED(s_factory->CreateStream(stream.GetAddressOf()))) return false;
            if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))))
            {
                std::fprintf(stderr, "[wic] InitializeFromMemory failed (size=%zu)\n", size);
                return false;
            }

            ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(s_factory->CreateDecoderFromStream(stream.Get(), nullptr,
                WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())))
            {
                std::fprintf(stderr, "[wic] CreateDecoderFromStream failed (size=%zu)\n", size);
                return false;
            }

            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;

            UINT w = 0, h = 0;
            if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

            // Convert to 32bpp RGBA so the result is directly GPU-uploadable.
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(s_factory->CreateFormatConverter(converter.GetAddressOf()))) return false;
            if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
            {
                std::fprintf(stderr, "[wic] FormatConverter::Initialize failed\n");
                return false;
            }

            const UINT stride = w * 4u;
            const size_t total = static_cast<size_t>(stride) * h;
            outRGBA.resize(total);
            if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(total), outRGBA.data())))
            {
                std::fprintf(stderr, "[wic] CopyPixels failed\n");
                outRGBA.clear();
                return false;
            }
            outWidth = w;
            outHeight = h;
            return true;
        }


        // --- Dawn instance / adapter / device / surface creation ---------------
        // Moved out of the Renderer so real Lite's createEngine owns the device via
        // navigator.gpu. The instance is created once (Module ctor); the adapter and
        // device are requested when JS calls requestAdapter / requestDevice.

        wgpu::Surface CreateSurfaceForWindow(const wgpu::Instance& instance, void* hwnd, void* hinstance)
        {            wgpu::RequestAdapterOptions options{};
            wgpu::SurfaceSourceWindowsHWND chained{};
            chained.hwnd = hwnd;
            chained.hinstance = hinstance;
            wgpu::SurfaceDescriptor desc{};
            desc.nextInChain = &chained;
            return instance.CreateSurface(&desc);
        }

        wgpu::Adapter RequestAdapterSync(const wgpu::Instance& instance)
        {            wgpu::RequestAdapterOptions options{};
            options.backendType = wgpu::BackendType::D3D12;
            options.powerPreference = wgpu::PowerPreference::HighPerformance;
            wgpu::Adapter adapter;
            wgpu::Future future = instance.RequestAdapter(
                &options, wgpu::CallbackMode::WaitAnyOnly,
                [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView message) {
                    if (status == wgpu::RequestAdapterStatus::Success) adapter = std::move(result);
                    else std::fprintf(stderr, "[webgpu] requestAdapter failed: %.*s\n",
                        static_cast<int>(message.length), message.data);
                });
            instance.WaitAny(future, UINT64_MAX);
            return adapter;
        }

        bool ReadBufferSource(const Napi::Value& value, const uint8_t*& outData, size_t& outSize)
        {
            if (value.IsTypedArray())
            {
                Napi::TypedArray ta = value.As<Napi::TypedArray>();
                Napi::ArrayBuffer ab = ta.ArrayBuffer();
                outData = static_cast<const uint8_t*>(ab.Data()) + ta.ByteOffset();
                outSize = ta.ByteLength();
                return true;
            }
            if (value.IsArrayBuffer())
            {
                Napi::ArrayBuffer ab = value.As<Napi::ArrayBuffer>();
                outData = static_cast<const uint8_t*>(ab.Data());
                outSize = ab.ByteLength();
                return true;
            }
            return false;
        }

        const char* FeatureNameToString(wgpu::FeatureName f)
        {
            switch (f)
            {
            case wgpu::FeatureName::Float32Filterable: return "float32-filterable";
            case wgpu::FeatureName::TimestampQuery: return "timestamp-query";
            case wgpu::FeatureName::TextureCompressionBC: return "texture-compression-bc";
            case wgpu::FeatureName::TextureCompressionETC2: return "texture-compression-etc2";
            case wgpu::FeatureName::TextureCompressionASTC: return "texture-compression-astc";
            case wgpu::FeatureName::Depth32FloatStencil8: return "depth32float-stencil8";
            case wgpu::FeatureName::IndirectFirstInstance: return "indirect-first-instance";
            default: return nullptr;
            }
        }

        wgpu::FeatureName StringToFeatureName(const std::string& s)
        {
            if (s == "float32-filterable") return wgpu::FeatureName::Float32Filterable;
            if (s == "timestamp-query") return wgpu::FeatureName::TimestampQuery;
            if (s == "texture-compression-bc") return wgpu::FeatureName::TextureCompressionBC;
            if (s == "texture-compression-etc2") return wgpu::FeatureName::TextureCompressionETC2;
            if (s == "texture-compression-astc") return wgpu::FeatureName::TextureCompressionASTC;
            if (s == "depth32float-stencil8") return wgpu::FeatureName::Depth32FloatStencil8;
            if (s == "indirect-first-instance") return wgpu::FeatureName::IndirectFirstInstance;
            return static_cast<wgpu::FeatureName>(0);
        }

        // Build a JS Set of WebGPU feature strings from a SupportedFeatures list.
        Napi::Object MakeFeatureSet(Napi::Env env, const wgpu::SupportedFeatures& supported)
        {
            Napi::Function setCtor = env.Global().Get("Set").As<Napi::Function>();
            Napi::Object set = setCtor.New({});
            Napi::Function add = set.Get("add").As<Napi::Function>();
            for (size_t i = 0; i < supported.featureCount; ++i)
            {
                const char* name = FeatureNameToString(supported.features[i]);
                if (name != nullptr) add.Call(set, {Napi::String::New(env, name)});
            }
            return set;
        }

        std::string GetString(const Napi::Object& obj, const char* key, const char* dflt)
        {
            if (obj.Has(key) && obj.Get(key).IsString())
                return obj.Get(key).As<Napi::String>().Utf8Value();
            return dflt;
        }

        double GetNumber(const Napi::Object& obj, const char* key, double dflt)
        {
            if (obj.Has(key) && obj.Get(key).IsNumber())
                return obj.Get(key).As<Napi::Number>().DoubleValue();
            return dflt;
        }

        bool GetBool(const Napi::Object& obj, const char* key, bool dflt)
        {
            if (obj.Has(key) && obj.Get(key).IsBoolean())
                return obj.Get(key).As<Napi::Boolean>().Value();
            return dflt;
        }

        wgpu::TextureFormat ParseFormat(const std::string& s)
        {
            if (s == "bgra8unorm") return wgpu::TextureFormat::BGRA8Unorm;
            if (s == "rgba8unorm") return wgpu::TextureFormat::RGBA8Unorm;
            if (s == "rgba8unorm-srgb") return wgpu::TextureFormat::RGBA8UnormSrgb;
            if (s == "bgra8unorm-srgb") return wgpu::TextureFormat::BGRA8UnormSrgb;
            if (s == "depth24plus") return wgpu::TextureFormat::Depth24Plus;
            if (s == "depth32float") return wgpu::TextureFormat::Depth32Float;
            if (s == "depth24plus-stencil8") return wgpu::TextureFormat::Depth24PlusStencil8;
            if (s == "r8unorm") return wgpu::TextureFormat::R8Unorm;
            if (s == "rg8unorm") return wgpu::TextureFormat::RG8Unorm;
            if (s == "rgba16float") return wgpu::TextureFormat::RGBA16Float;
            if (s == "rgba32float") return wgpu::TextureFormat::RGBA32Float;
            // Block-compressed (BC/DXT) — KTX2/.dds textures upload in these. Without them a
            // compressed texture would default to RGBA8Unorm and Dawn's required-size check
            // would reject the (much smaller) compressed data. Requires the device's
            // texture-compression-bc feature (negotiated at requestDevice).
            if (s == "bc1-rgba-unorm") return wgpu::TextureFormat::BC1RGBAUnorm;
            if (s == "bc1-rgba-unorm-srgb") return wgpu::TextureFormat::BC1RGBAUnormSrgb;
            if (s == "bc2-rgba-unorm") return wgpu::TextureFormat::BC2RGBAUnorm;
            if (s == "bc2-rgba-unorm-srgb") return wgpu::TextureFormat::BC2RGBAUnormSrgb;
            if (s == "bc3-rgba-unorm") return wgpu::TextureFormat::BC3RGBAUnorm;
            if (s == "bc3-rgba-unorm-srgb") return wgpu::TextureFormat::BC3RGBAUnormSrgb;
            if (s == "bc4-r-unorm") return wgpu::TextureFormat::BC4RUnorm;
            if (s == "bc4-r-snorm") return wgpu::TextureFormat::BC4RSnorm;
            if (s == "bc5-rg-unorm") return wgpu::TextureFormat::BC5RGUnorm;
            if (s == "bc5-rg-snorm") return wgpu::TextureFormat::BC5RGSnorm;
            if (s == "bc6h-rgb-ufloat") return wgpu::TextureFormat::BC6HRGBUfloat;
            if (s == "bc6h-rgb-float") return wgpu::TextureFormat::BC6HRGBFloat;
            if (s == "bc7-rgba-unorm") return wgpu::TextureFormat::BC7RGBAUnorm;
            if (s == "bc7-rgba-unorm-srgb") return wgpu::TextureFormat::BC7RGBAUnormSrgb;
            return wgpu::TextureFormat::RGBA8Unorm;
        }

        wgpu::TextureViewDimension ParseViewDimension(const std::string& s)
        {
            if (s == "cube") return wgpu::TextureViewDimension::Cube;
            if (s == "cube-array") return wgpu::TextureViewDimension::CubeArray;
            if (s == "2d-array") return wgpu::TextureViewDimension::e2DArray;
            if (s == "3d") return wgpu::TextureViewDimension::e3D;
            if (s == "1d") return wgpu::TextureViewDimension::e1D;
            return wgpu::TextureViewDimension::e2D;
        }

        // Reverse of ParseFormat — Dawn TextureFormat → WebGPU string. Used by the GPUTexture
        // `format` accessor (read by mip generation + LOD code).
        std::string FormatToString(wgpu::TextureFormat f)
        {
            switch (f)
            {
            case wgpu::TextureFormat::BGRA8Unorm: return "bgra8unorm";
            case wgpu::TextureFormat::RGBA8Unorm: return "rgba8unorm";
            case wgpu::TextureFormat::RGBA8UnormSrgb: return "rgba8unorm-srgb";
            case wgpu::TextureFormat::BGRA8UnormSrgb: return "bgra8unorm-srgb";
            case wgpu::TextureFormat::Depth24Plus: return "depth24plus";
            case wgpu::TextureFormat::Depth32Float: return "depth32float";
            case wgpu::TextureFormat::Depth24PlusStencil8: return "depth24plus-stencil8";
            case wgpu::TextureFormat::R8Unorm: return "r8unorm";
            case wgpu::TextureFormat::RG8Unorm: return "rg8unorm";
            case wgpu::TextureFormat::RGBA16Float: return "rgba16float";
            case wgpu::TextureFormat::RGBA32Float: return "rgba32float";
            case wgpu::TextureFormat::BC1RGBAUnorm: return "bc1-rgba-unorm";
            case wgpu::TextureFormat::BC1RGBAUnormSrgb: return "bc1-rgba-unorm-srgb";
            case wgpu::TextureFormat::BC2RGBAUnorm: return "bc2-rgba-unorm";
            case wgpu::TextureFormat::BC2RGBAUnormSrgb: return "bc2-rgba-unorm-srgb";
            case wgpu::TextureFormat::BC3RGBAUnorm: return "bc3-rgba-unorm";
            case wgpu::TextureFormat::BC3RGBAUnormSrgb: return "bc3-rgba-unorm-srgb";
            case wgpu::TextureFormat::BC4RUnorm: return "bc4-r-unorm";
            case wgpu::TextureFormat::BC4RSnorm: return "bc4-r-snorm";
            case wgpu::TextureFormat::BC5RGUnorm: return "bc5-rg-unorm";
            case wgpu::TextureFormat::BC5RGSnorm: return "bc5-rg-snorm";
            case wgpu::TextureFormat::BC6HRGBUfloat: return "bc6h-rgb-ufloat";
            case wgpu::TextureFormat::BC6HRGBFloat: return "bc6h-rgb-float";
            case wgpu::TextureFormat::BC7RGBAUnorm: return "bc7-rgba-unorm";
            case wgpu::TextureFormat::BC7RGBAUnormSrgb: return "bc7-rgba-unorm-srgb";
            default: return "rgba8unorm";
            }
        }

        wgpu::VertexFormat ParseVertexFormat(const std::string& s)
        {
            if (s == "float32") return wgpu::VertexFormat::Float32;
            if (s == "float32x2") return wgpu::VertexFormat::Float32x2;
            if (s == "float32x3") return wgpu::VertexFormat::Float32x3;
            if (s == "float32x4") return wgpu::VertexFormat::Float32x4;
            if (s == "uint32") return wgpu::VertexFormat::Uint32;
            if (s == "uint32x2") return wgpu::VertexFormat::Uint32x2;
            if (s == "uint32x4") return wgpu::VertexFormat::Uint32x4;
            if (s == "uint8x4") return wgpu::VertexFormat::Uint8x4;
            return wgpu::VertexFormat::Float32x3;
        }

        wgpu::PrimitiveTopology ParseTopology(const std::string& s)
        {
            if (s == "triangle-list") return wgpu::PrimitiveTopology::TriangleList;
            if (s == "triangle-strip") return wgpu::PrimitiveTopology::TriangleStrip;
            if (s == "line-list") return wgpu::PrimitiveTopology::LineList;
            if (s == "line-strip") return wgpu::PrimitiveTopology::LineStrip;
            if (s == "point-list") return wgpu::PrimitiveTopology::PointList;
            return wgpu::PrimitiveTopology::TriangleList;
        }

        wgpu::CullMode ParseCullMode(const std::string& s)
        {
            if (s == "front") return wgpu::CullMode::Front;
            if (s == "back") return wgpu::CullMode::Back;
            return wgpu::CullMode::None;
        }

        wgpu::FrontFace ParseFrontFace(const std::string& s)
        {
            if (s == "cw") return wgpu::FrontFace::CW;
            return wgpu::FrontFace::CCW;
        }

        wgpu::CompareFunction ParseCompare(const std::string& s)
        {
            if (s == "never") return wgpu::CompareFunction::Never;
            if (s == "less") return wgpu::CompareFunction::Less;
            if (s == "equal") return wgpu::CompareFunction::Equal;
            if (s == "less-equal") return wgpu::CompareFunction::LessEqual;
            if (s == "greater") return wgpu::CompareFunction::Greater;
            if (s == "not-equal") return wgpu::CompareFunction::NotEqual;
            if (s == "greater-equal") return wgpu::CompareFunction::GreaterEqual;
            if (s == "always") return wgpu::CompareFunction::Always;
            return wgpu::CompareFunction::Less;
        }

        wgpu::AddressMode ParseAddressMode(const std::string& s)
        {
            if (s == "clamp-to-edge") return wgpu::AddressMode::ClampToEdge;
            if (s == "mirror-repeat") return wgpu::AddressMode::MirrorRepeat;
            return wgpu::AddressMode::Repeat;
        }

        wgpu::FilterMode ParseFilter(const std::string& s)
        {
            if (s == "nearest") return wgpu::FilterMode::Nearest;
            return wgpu::FilterMode::Linear;
        }

        wgpu::MipmapFilterMode ParseMipmapFilter(const std::string& s)
        {
            if (s == "nearest") return wgpu::MipmapFilterMode::Nearest;
            return wgpu::MipmapFilterMode::Linear;
        }

        wgpu::BufferBindingType ParseBufferBindingType(const std::string& s)
        {
            if (s == "storage") return wgpu::BufferBindingType::Storage;
            if (s == "read-only-storage") return wgpu::BufferBindingType::ReadOnlyStorage;
            return wgpu::BufferBindingType::Uniform;
        }

        wgpu::SamplerBindingType ParseSamplerBindingType(const std::string& s)
        {
            if (s == "non-filtering") return wgpu::SamplerBindingType::NonFiltering;
            if (s == "comparison") return wgpu::SamplerBindingType::Comparison;
            return wgpu::SamplerBindingType::Filtering;
        }

        wgpu::TextureSampleType ParseTextureSampleType(const std::string& s)
        {
            if (s == "unfilterable-float") return wgpu::TextureSampleType::UnfilterableFloat;
            if (s == "depth") return wgpu::TextureSampleType::Depth;
            if (s == "sint") return wgpu::TextureSampleType::Sint;
            if (s == "uint") return wgpu::TextureSampleType::Uint;
            return wgpu::TextureSampleType::Float;
        }

        wgpu::LoadOp ParseLoadOp(const std::string& s)
        {
            if (s == "load") return wgpu::LoadOp::Load;
            return wgpu::LoadOp::Clear;
        }

        wgpu::StoreOp ParseStoreOp(const std::string& s)
        {
            if (s == "discard") return wgpu::StoreOp::Discard;
            return wgpu::StoreOp::Store;
        }

        // Reads a GPUExtent3D given as {width,height,depthOrArrayLayers} or [w,h,d].
        wgpu::Extent3D ParseExtent(const Napi::Value& v)
        {
            wgpu::Extent3D e{1, 1, 1};
            if (v.IsArray())
            {
                Napi::Array a = v.As<Napi::Array>();
                if (a.Length() > 0) e.width = a.Get(0u).As<Napi::Number>().Uint32Value();
                if (a.Length() > 1) e.height = a.Get(1u).As<Napi::Number>().Uint32Value();
                if (a.Length() > 2) e.depthOrArrayLayers = a.Get(2u).As<Napi::Number>().Uint32Value();
            }
            else if (v.IsObject())
            {
                Napi::Object o = v.As<Napi::Object>();
                e.width = static_cast<uint32_t>(GetNumber(o, "width", 1));
                e.height = static_cast<uint32_t>(GetNumber(o, "height", 1));
                e.depthOrArrayLayers = static_cast<uint32_t>(GetNumber(o, "depthOrArrayLayers", 1));
            }
            return e;
        }
    }

    // ===================================================================== wrappers
    // Each simple wrapper copies a Dawn handle smuggled in via an External (the copy
    // bumps the refcount, keeping the object alive for the wrapper's lifetime).

    Napi::Function Buffer::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Buffer>::DefineClass(env, "GPUBuffer", {
            Napi::ObjectWrap<Buffer>::InstanceMethod<&Buffer::GetMappedRange>("getMappedRange"),
            Napi::ObjectWrap<Buffer>::InstanceMethod<&Buffer::Unmap>("unmap"),
            Napi::ObjectWrap<Buffer>::InstanceMethod<&Buffer::Destroy>("destroy"),
        });
    }
    Buffer::Buffer(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Buffer>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
        {
            const BufferInit& init = *info[0].As<Napi::External<BufferInit>>().Data();
            m_buffer = init.buffer;
            m_baseOffset = init.baseOffset;
            m_size = init.size;
            m_isSub = init.isSub;
            m_queue = init.queue;
        }
    }
    // getMappedRange(offset?, size?) -> ArrayBuffer for writing into a mappedAtCreation
    // buffer. ChakraCore's N-API does NOT alias external memory, so we hand JS a normal
    // engine-owned ArrayBuffer and remember it; unmap() copies it into Dawn's mapped range.
    Napi::Value Buffer::GetMappedRange(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        uint64_t offset = info.Length() >= 1 && info[0].IsNumber()
            ? static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value()) : 0;
        uint64_t size = info.Length() >= 2 && info[1].IsNumber()
            ? static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value())
            : (m_size - offset);
        // For a real (standalone) buffer, validate it's actually mapped. A sub-buffer has no
        // GPU mapping — its data is staged in the returned ArrayBuffer and uploaded to the
        // arena via queue.writeBuffer at unmap().
        if (!m_isSub && m_buffer.GetMappedRange(offset, static_cast<size_t>(size)) == nullptr)
        {
            Napi::Error::New(env, "getMappedRange: buffer is not mapped").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        Napi::ArrayBuffer ab = Napi::ArrayBuffer::New(env, static_cast<size_t>(size));
        m_mappedRanges.push_back({ Napi::Persistent(ab), offset, size });
        return ab;
    }
    Napi::Value Buffer::Unmap(const Napi::CallbackInfo& info)
    {
        // Flush each staging ArrayBuffer into the GPU before unmapping. Standalone buffers
        // memcpy into Dawn's mapped memory; sub-buffers upload into the shared arena at their
        // base offset via the queue (the arena is never CPU-mapped).
        for (auto& r : m_mappedRanges)
        {
            Napi::ArrayBuffer ab = r.ref.Value();
            if (m_isSub)
            {
                if (m_queue != nullptr)
                    m_queue.WriteBuffer(m_buffer, m_baseOffset + r.offset, ab.Data(),
                        static_cast<size_t>(r.size));
            }
            else
            {
                void* dst = m_buffer.GetMappedRange(r.offset, static_cast<size_t>(r.size));
                if (dst != nullptr)
                    std::memcpy(dst, ab.Data(), static_cast<size_t>(r.size));
            }
            r.ref.Reset();
        }
        m_mappedRanges.clear();
        if (!m_isSub) m_buffer.Unmap();
        return info.Env().Undefined();
    }
    Napi::Value Buffer::Destroy(const Napi::CallbackInfo& info)
    {
        // Never destroy a shared arena from one sub-buffer; the slice simply leaks within the
        // arena until the arena (and Device) is torn down.
        if (!m_isSub) m_buffer.Destroy();
        return info.Env().Undefined();
    }

    Napi::Function TextureView::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<TextureView>::DefineClass(env, "GPUTextureView", {});
    }
    TextureView::TextureView(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TextureView>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_view = *info[0].As<Napi::External<wgpu::TextureView>>().Data();
    }

    Napi::Function Texture::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Texture>::DefineClass(env, "GPUTexture", {
            Napi::ObjectWrap<Texture>::InstanceMethod<&Texture::CreateView>("createView"),
            Napi::ObjectWrap<Texture>::InstanceMethod<&Texture::Destroy>("destroy"),
            Napi::ObjectWrap<Texture>::InstanceAccessor<&Texture::GetWidth>("width"),
            Napi::ObjectWrap<Texture>::InstanceAccessor<&Texture::GetHeight>("height"),
            Napi::ObjectWrap<Texture>::InstanceAccessor<&Texture::GetMipLevelCount>("mipLevelCount"),
            Napi::ObjectWrap<Texture>::InstanceAccessor<&Texture::GetFormat>("format"),
            Napi::ObjectWrap<Texture>::InstanceAccessor<&Texture::GetDepthOrArrayLayers>("depthOrArrayLayers"),
        });
    }
    Texture::Texture(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Texture>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_texture = *info[0].As<Napi::External<wgpu::Texture>>().Data();
    }
    Napi::Value Texture::CreateView(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_createView);
        Napi::Env env = info.Env();
        // Parse the optional descriptor: dimension (2d/cube/2d-array/3d), format override,
        // and mip/array sub-ranges. Lite uses cube views for the env specular cubemap and
        // per-mip 2d-array views for the prefilter compute passes.
        if (info.Length() >= 1 && info[0].IsObject())
        {
            Napi::Object d = info[0].As<Napi::Object>();
            wgpu::TextureViewDescriptor desc{};
            std::string dim = GetString(d, "dimension", "");
            if (dim == "cube") desc.dimension = wgpu::TextureViewDimension::Cube;
            else if (dim == "2d-array") desc.dimension = wgpu::TextureViewDimension::e2DArray;
            else if (dim == "3d") desc.dimension = wgpu::TextureViewDimension::e3D;
            else if (dim == "2d") desc.dimension = wgpu::TextureViewDimension::e2D;
            if (d.Has("format") && d.Get("format").IsString())
                desc.format = ParseFormat(GetString(d, "format", "rgba8unorm"));
            if (d.Has("baseMipLevel")) desc.baseMipLevel = static_cast<uint32_t>(GetNumber(d, "baseMipLevel", 0));
            if (d.Has("mipLevelCount")) desc.mipLevelCount = static_cast<uint32_t>(GetNumber(d, "mipLevelCount", 0));
            if (d.Has("baseArrayLayer")) desc.baseArrayLayer = static_cast<uint32_t>(GetNumber(d, "baseArrayLayer", 0));
            if (d.Has("arrayLayerCount")) desc.arrayLayerCount = static_cast<uint32_t>(GetNumber(d, "arrayLayerCount", 0));
            wgpu::TextureView view = m_texture.CreateView(&desc);
            return Module::Current()->WrapTextureView(env, view);
        }
        wgpu::TextureView view = m_texture.CreateView();
        return Module::Current()->WrapTextureView(env, view);
    }
    Napi::Value Texture::Destroy(const Napi::CallbackInfo& info)
    {
        m_texture.Destroy();
        return info.Env().Undefined();
    }
    Napi::Value Texture::GetWidth(const Napi::CallbackInfo& info)
    {
        return Napi::Number::New(info.Env(), m_texture.GetWidth());
    }
    Napi::Value Texture::GetHeight(const Napi::CallbackInfo& info)
    {
        return Napi::Number::New(info.Env(), m_texture.GetHeight());
    }
    Napi::Value Texture::GetMipLevelCount(const Napi::CallbackInfo& info)
    {
        return Napi::Number::New(info.Env(), m_texture.GetMipLevelCount());
    }
    Napi::Value Texture::GetFormat(const Napi::CallbackInfo& info)
    {
        return Napi::String::New(info.Env(), FormatToString(m_texture.GetFormat()));
    }
    Napi::Value Texture::GetDepthOrArrayLayers(const Napi::CallbackInfo& info)
    {
        return Napi::Number::New(info.Env(), m_texture.GetDepthOrArrayLayers());
    }

    Napi::Function Sampler::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Sampler>::DefineClass(env, "GPUSampler", {});
    }
    Sampler::Sampler(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Sampler>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_sampler = *info[0].As<Napi::External<wgpu::Sampler>>().Data();
    }

    Napi::Function ShaderModule::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<ShaderModule>::DefineClass(env, "GPUShaderModule", {});
    }
    ShaderModule::ShaderModule(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ShaderModule>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_module = *info[0].As<Napi::External<wgpu::ShaderModule>>().Data();
    }

    Napi::Function BindGroupLayout::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<BindGroupLayout>::DefineClass(env, "GPUBindGroupLayout", {});
    }
    BindGroupLayout::BindGroupLayout(const Napi::CallbackInfo& info) : Napi::ObjectWrap<BindGroupLayout>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_layout = *info[0].As<Napi::External<wgpu::BindGroupLayout>>().Data();
    }

    Napi::Function PipelineLayout::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<PipelineLayout>::DefineClass(env, "GPUPipelineLayout", {});
    }
    PipelineLayout::PipelineLayout(const Napi::CallbackInfo& info) : Napi::ObjectWrap<PipelineLayout>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_layout = *info[0].As<Napi::External<wgpu::PipelineLayout>>().Data();
    }

    Napi::Function BindGroup::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<BindGroup>::DefineClass(env, "GPUBindGroup", {});
    }
    BindGroup::BindGroup(const Napi::CallbackInfo& info) : Napi::ObjectWrap<BindGroup>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_group = *info[0].As<Napi::External<wgpu::BindGroup>>().Data();
    }

    Napi::Function RenderPipeline::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<RenderPipeline>::DefineClass(env, "GPURenderPipeline", {
            Napi::ObjectWrap<RenderPipeline>::InstanceMethod<&RenderPipeline::GetBindGroupLayout>("getBindGroupLayout"),
        });
    }
    RenderPipeline::RenderPipeline(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RenderPipeline>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_pipeline = *info[0].As<Napi::External<wgpu::RenderPipeline>>().Data();
    }
    Napi::Value RenderPipeline::GetBindGroupLayout(const Napi::CallbackInfo& info)
    {
        uint32_t index = info.Length() >= 1 && info[0].IsNumber() ? info[0].As<Napi::Number>().Uint32Value() : 0;
        wgpu::BindGroupLayout layout = m_pipeline.GetBindGroupLayout(index);
        return Module::Current()->WrapBindGroupLayout(info.Env(), layout);
    }

    // ================================================================ ComputePipeline
    Napi::Function ComputePipeline::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<ComputePipeline>::DefineClass(env, "GPUComputePipeline", {
            Napi::ObjectWrap<ComputePipeline>::InstanceMethod<&ComputePipeline::GetBindGroupLayout>("getBindGroupLayout"),
        });
    }
    ComputePipeline::ComputePipeline(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ComputePipeline>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_pipeline = *info[0].As<Napi::External<wgpu::ComputePipeline>>().Data();
    }
    Napi::Value ComputePipeline::GetBindGroupLayout(const Napi::CallbackInfo& info)
    {
        uint32_t index = info.Length() >= 1 && info[0].IsNumber() ? info[0].As<Napi::Number>().Uint32Value() : 0;
        wgpu::BindGroupLayout layout = m_pipeline.GetBindGroupLayout(index);
        return Module::Current()->WrapBindGroupLayout(info.Env(), layout);
    }

    // ============================================================ ComputePassEncoder
    Napi::Function ComputePassEncoder::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<ComputePassEncoder>::DefineClass(env, "GPUComputePassEncoder", {
            Napi::ObjectWrap<ComputePassEncoder>::InstanceMethod<&ComputePassEncoder::SetPipeline>("setPipeline"),
            Napi::ObjectWrap<ComputePassEncoder>::InstanceMethod<&ComputePassEncoder::SetBindGroup>("setBindGroup"),
            Napi::ObjectWrap<ComputePassEncoder>::InstanceMethod<&ComputePassEncoder::DispatchWorkgroups>("dispatchWorkgroups"),
            Napi::ObjectWrap<ComputePassEncoder>::InstanceMethod<&ComputePassEncoder::End>("end"),
        });
    }
    ComputePassEncoder::ComputePassEncoder(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ComputePassEncoder>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_pass = *info[0].As<Napi::External<wgpu::ComputePassEncoder>>().Data();
    }
    Napi::Value ComputePassEncoder::SetPipeline(const Napi::CallbackInfo& info)
    {
        ComputePipeline* p = info[0].IsObject() ? Module::Current()->AsComputePipeline(info[0].As<Napi::Object>()) : nullptr;
        if (p != nullptr) m_pass.SetPipeline(p->Handle());
        return info.Env().Undefined();
    }
    Napi::Value ComputePassEncoder::SetBindGroup(const Napi::CallbackInfo& info)
    {
        uint32_t index = info[0].As<Napi::Number>().Uint32Value();
        BindGroup* bg = info[1].IsObject() ? Module::Current()->AsBindGroup(info[1].As<Napi::Object>()) : nullptr;
        if (bg != nullptr) m_pass.SetBindGroup(index, bg->Handle());
        return info.Env().Undefined();
    }
    Napi::Value ComputePassEncoder::DispatchWorkgroups(const Napi::CallbackInfo& info)
    {
        uint32_t x = info.Length() >= 1 && info[0].IsNumber() ? info[0].As<Napi::Number>().Uint32Value() : 1;
        uint32_t y = info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Uint32Value() : 1;
        uint32_t z = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Uint32Value() : 1;
        m_pass.DispatchWorkgroups(x, y, z);
        return info.Env().Undefined();
    }
    Napi::Value ComputePassEncoder::End(const Napi::CallbackInfo& info)
    {
        m_pass.End();
        return info.Env().Undefined();
    }

    // ======================================================================== Queue
    Napi::Function Queue::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Queue>::DefineClass(env, "GPUQueue", {
            Napi::ObjectWrap<Queue>::InstanceMethod<&Queue::WriteBuffer>("writeBuffer"),
            Napi::ObjectWrap<Queue>::InstanceMethod<&Queue::WriteTexture>("writeTexture"),
            Napi::ObjectWrap<Queue>::InstanceMethod<&Queue::CopyExternalImageToTexture>("copyExternalImageToTexture"),
            Napi::ObjectWrap<Queue>::InstanceMethod<&Queue::Submit>("submit"),
        });
    }
    Queue::Queue(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Queue>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_queue = *info[0].As<Napi::External<wgpu::Queue>>().Data();
    }

    Napi::Value Queue::WriteBuffer(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_writeBuffer);
        auto _t0 = std::chrono::steady_clock::now();
        Napi::Env env = info.Env();
        if (info.Length() < 3 || !info[0].IsObject())
        {
            Napi::TypeError::New(env, "writeBuffer(buffer, bufferOffset, data) expected").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        Buffer* buffer = Module::Current()->AsBuffer(info[0].As<Napi::Object>());
        if (buffer == nullptr)
        {
            Napi::TypeError::New(env, "writeBuffer: first argument must be a GPUBuffer").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        uint64_t bufferOffset = static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value());
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!ReadBufferSource(info[2], data, size))
        {
            Napi::TypeError::New(env, "writeBuffer: data must be a TypedArray or ArrayBuffer").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        // Optional dataOffset (elements) + size (elements) are accepted but the common
        // whole-buffer form is used here.
        m_queue.WriteBuffer(buffer->Handle(), buffer->BaseOffset() + bufferOffset, data, size);
        if (g_mem.enabled) { g_mem.writeBufferNs += std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - _t0).count(); g_mem.writeBufferCalls++; }
        return env.Undefined();
    }

    Napi::Value Queue::WriteTexture(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        if (info.Length() < 4 || !info[0].IsObject() || !info[2].IsObject() )
        {
            Napi::TypeError::New(env, "writeTexture(destination, data, dataLayout, size) expected").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        Napi::Object dest = info[0].As<Napi::Object>();
        Texture* tex = dest.Get("texture").IsObject() ? Module::Current()->AsTexture(dest.Get("texture").As<Napi::Object>()) : nullptr;
        if (tex == nullptr)
        {
            Napi::TypeError::New(env, "writeTexture: destination.texture must be a GPUTexture").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        const uint8_t* data = nullptr;
        size_t dataSize = 0;
        if (!ReadBufferSource(info[1], data, dataSize))
        {
            Napi::TypeError::New(env, "writeTexture: data must be a TypedArray or ArrayBuffer").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        Napi::Object layout = info[2].As<Napi::Object>();
        wgpu::Extent3D extent = ParseExtent(info[3]);

        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = tex->Handle();
        dst.mipLevel = static_cast<uint32_t>(GetNumber(dest, "mipLevel", 0));

        wgpu::TexelCopyBufferLayout dl{};
        dl.offset = static_cast<uint64_t>(GetNumber(layout, "offset", 0));
        dl.bytesPerRow = static_cast<uint32_t>(GetNumber(layout, "bytesPerRow", 0));
        dl.rowsPerImage = static_cast<uint32_t>(GetNumber(layout, "rowsPerImage", extent.height));

        m_queue.WriteTexture(&dst, data, dataSize, &dl, &extent);
        return env.Undefined();
    }

    // copyExternalImageToTexture(source, destination, copySize) — uploads a decoded
    // ImageBitmap's RGBA8 pixels into a GPU texture's mip 0. source = { source: ImageBitmap,
    // flipY? }; destination = { texture, mipLevel?, premultipliedAlpha? } (premultiply not
    // applied — glTF/Lite decode with premultiplyAlpha "none"); copySize = {width,height} or
    // [w,h]. flipY reverses rows. Real Lite's texture loaders call this after createImageBitmap.
    Napi::Value Queue::CopyExternalImageToTexture(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        const Module* mod = Module::Current();
        if (info.Length() < 3 || !info[0].IsObject() || !info[1].IsObject())
        {
            Napi::TypeError::New(env, "copyExternalImageToTexture(source, destination, copySize) expected").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        Napi::Object src = info[0].As<Napi::Object>();
        Napi::Object dest = info[1].As<Napi::Object>();

        ImageBitmap* bitmap = src.Get("source").IsObject() ? mod->AsImageBitmap(src.Get("source").As<Napi::Object>()) : nullptr;
        if (bitmap == nullptr)
        {
            Napi::TypeError::New(env, "copyExternalImageToTexture: source.source must be an ImageBitmap").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        Texture* tex = dest.Get("texture").IsObject() ? mod->AsTexture(dest.Get("texture").As<Napi::Object>()) : nullptr;
        if (tex == nullptr)
        {
            Napi::TypeError::New(env, "copyExternalImageToTexture: destination.texture must be a GPUTexture").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        const bool flipY = GetBool(src, "flipY", false);

        wgpu::Extent3D extent = ParseExtent(info[2]);
        const uint32_t w = extent.width != 0 ? extent.width : bitmap->Width();
        const uint32_t h = extent.height != 0 ? extent.height : bitmap->Height();
        const uint32_t copyW = w < bitmap->Width() ? w : bitmap->Width();
        const uint32_t copyH = h < bitmap->Height() ? h : bitmap->Height();
        const uint32_t srcRowBytes = bitmap->Width() * 4;
        const uint32_t dstRowBytes = copyW * 4;

        const std::vector<uint8_t>& pixels = bitmap->Pixels();
        std::vector<uint8_t> packed(static_cast<size_t>(dstRowBytes) * copyH);
        for (uint32_t y = 0; y < copyH; ++y)
        {
            const uint32_t srcY = flipY ? (bitmap->Height() - 1 - y) : y;
            const uint8_t* srcRow = pixels.data() + static_cast<size_t>(srcY) * srcRowBytes;
            std::memcpy(packed.data() + static_cast<size_t>(y) * dstRowBytes, srcRow, dstRowBytes);
        }

        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = tex->Handle();
        dst.mipLevel = static_cast<uint32_t>(GetNumber(dest, "mipLevel", 0));

        wgpu::TexelCopyBufferLayout dl{};
        dl.offset = 0;
        dl.bytesPerRow = dstRowBytes;
        dl.rowsPerImage = copyH;

        wgpu::Extent3D writeExtent{};
        writeExtent.width = copyW;
        writeExtent.height = copyH;
        writeExtent.depthOrArrayLayers = 1;

        m_queue.WriteTexture(&dst, packed.data(), packed.size(), &dl, &writeExtent);
        return env.Undefined();
    }

    Napi::Value Queue::Submit(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_submit);
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsArray())
            return env.Undefined();
        const Module* mod = Module::Current();
        Napi::Array arr = info[0].As<Napi::Array>();
        std::vector<wgpu::CommandBuffer> buffers;
        buffers.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
            if (!arr.Get(i).IsObject()) continue;
            CommandBuffer* cb = mod->AsCommandBuffer(arr.Get(i).As<Napi::Object>());
            if (cb != nullptr) buffers.push_back(cb->Handle());
        }
        if (mod->ThreadedSubmit())
        {
            // Hand the finished command buffers to the render thread and return immediately —
            // the heavy Dawn->D3D12 translation runs there, off the JS thread.
            if (!buffers.empty())
                const_cast<Module*>(mod)->EnqueueSubmit(m_queue, std::move(buffers));
            return env.Undefined();
        }
        if (!buffers.empty())
            m_queue.Submit(buffers.size(), buffers.data());
        return env.Undefined();
    }

    // ================================================================ CommandBuffer
    Napi::Function CommandBuffer::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<CommandBuffer>::DefineClass(env, "GPUCommandBuffer", {});
    }
    CommandBuffer::CommandBuffer(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CommandBuffer>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_commandBuffer = *info[0].As<Napi::External<wgpu::CommandBuffer>>().Data();
    }

    // ============================================================ RenderPassEncoder
    Napi::Function RenderPassEncoder::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<RenderPassEncoder>::DefineClass(env, "GPURenderPassEncoder", {
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::SetPipeline>("setPipeline"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::SetBindGroup>("setBindGroup"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::SetVertexBuffer>("setVertexBuffer"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::SetIndexBuffer>("setIndexBuffer"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::Draw>("draw"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::DrawIndexed>("drawIndexed"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::SetViewport>("setViewport"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::SetScissorRect>("setScissorRect"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::ExecuteBundles>("executeBundles"),
            Napi::ObjectWrap<RenderPassEncoder>::InstanceMethod<&RenderPassEncoder::End>("end"),
        });
    }
    RenderPassEncoder::RenderPassEncoder(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RenderPassEncoder>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_pass = *info[0].As<Napi::External<wgpu::RenderPassEncoder>>().Data();
    }
    Napi::Value RenderPassEncoder::SetPipeline(const Napi::CallbackInfo& info)
    {
        RenderPipeline* p = Module::Current()->AsRenderPipeline(info[0].As<Napi::Object>());
        if (p != nullptr) m_pass.SetPipeline(p->Handle());
        return info.Env().Undefined();
    }
    Napi::Value RenderPassEncoder::SetBindGroup(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_setBindGroup);
        uint32_t index = info[0].As<Napi::Number>().Uint32Value();
        BindGroup* bg = info[1].IsObject() ? Module::Current()->AsBindGroup(info[1].As<Napi::Object>()) : nullptr;
        // Dynamic offsets (info[2]) not yet supported.
        if (bg != nullptr) m_pass.SetBindGroup(index, bg->Handle());
        return info.Env().Undefined();
    }
    Napi::Value RenderPassEncoder::SetVertexBuffer(const Napi::CallbackInfo& info)
    {
        uint32_t slot = info[0].As<Napi::Number>().Uint32Value();
        Buffer* b = info[1].IsObject() ? Module::Current()->AsBuffer(info[1].As<Napi::Object>()) : nullptr;
        uint64_t offset = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int64Value() : 0;
        if (b != nullptr) m_pass.SetVertexBuffer(slot, b->Handle(), b->BaseOffset() + offset);
        return info.Env().Undefined();
    }
    Napi::Value RenderPassEncoder::SetIndexBuffer(const Napi::CallbackInfo& info)
    {
        Buffer* b = info[0].IsObject() ? Module::Current()->AsBuffer(info[0].As<Napi::Object>()) : nullptr;
        std::string fmt = info.Length() >= 2 && info[1].IsString() ? info[1].As<Napi::String>().Utf8Value() : "uint32";
        wgpu::IndexFormat f = fmt == "uint16" ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32;
        uint64_t offset = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int64Value() : 0;
        if (b != nullptr) m_pass.SetIndexBuffer(b->Handle(), f, b->BaseOffset() + offset);
        return info.Env().Undefined();
    }
    Napi::Value RenderPassEncoder::Draw(const Napi::CallbackInfo& info)
    {
        uint32_t vertexCount = info[0].As<Napi::Number>().Uint32Value();
        uint32_t instanceCount = info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Uint32Value() : 1;
        uint32_t firstVertex = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Uint32Value() : 0;
        uint32_t firstInstance = info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Uint32Value() : 0;
        m_pass.Draw(vertexCount, instanceCount, firstVertex, firstInstance);
        return info.Env().Undefined();
    }
    Napi::Value RenderPassEncoder::DrawIndexed(const Napi::CallbackInfo& info)
    {
        uint32_t indexCount = info[0].As<Napi::Number>().Uint32Value();
        uint32_t instanceCount = info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Uint32Value() : 1;
        uint32_t firstIndex = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Uint32Value() : 0;
        int32_t baseVertex = info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 0;
        uint32_t firstInstance = info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Uint32Value() : 0;
        m_pass.DrawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
        return info.Env().Undefined();
    }
    Napi::Value RenderPassEncoder::SetViewport(const Napi::CallbackInfo& info)
    {
        m_pass.SetViewport(
            info[0].As<Napi::Number>().FloatValue(), info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(), info[3].As<Napi::Number>().FloatValue(),
            info[4].As<Napi::Number>().FloatValue(), info[5].As<Napi::Number>().FloatValue());
        return info.Env().Undefined();
    }
    Napi::Value RenderPassEncoder::SetScissorRect(const Napi::CallbackInfo& info)
    {
        m_pass.SetScissorRect(
            info[0].As<Napi::Number>().Uint32Value(), info[1].As<Napi::Number>().Uint32Value(),
            info[2].As<Napi::Number>().Uint32Value(), info[3].As<Napi::Number>().Uint32Value());
        return info.Env().Undefined();
    }
    Napi::Value RenderPassEncoder::ExecuteBundles(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_executeBundles);
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsArray()) return env.Undefined();
        const Module* mod = Module::Current();
        Napi::Array arr = info[0].As<Napi::Array>();
        std::vector<wgpu::RenderBundle> bundles;
        bundles.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
            if (!arr.Get(i).IsObject()) continue;
            RenderBundle* rb = mod->AsRenderBundle(arr.Get(i).As<Napi::Object>());
            if (rb != nullptr) bundles.push_back(rb->Handle());
        }
        if (!bundles.empty())
            m_pass.ExecuteBundles(bundles.size(), bundles.data());
        return env.Undefined();
    }
    Napi::Value RenderPassEncoder::End(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_endPass);
        m_pass.End();
        return info.Env().Undefined();
    }

    // ================================================================ RenderBundle
    Napi::Function RenderBundle::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<RenderBundle>::DefineClass(env, "GPURenderBundle", {});
    }
    RenderBundle::RenderBundle(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RenderBundle>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_bundle = *info[0].As<Napi::External<wgpu::RenderBundle>>().Data();
    }

    // ========================================================= RenderBundleEncoder
    Napi::Function RenderBundleEncoder::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<RenderBundleEncoder>::DefineClass(env, "GPURenderBundleEncoder", {
            Napi::ObjectWrap<RenderBundleEncoder>::InstanceMethod<&RenderBundleEncoder::SetPipeline>("setPipeline"),
            Napi::ObjectWrap<RenderBundleEncoder>::InstanceMethod<&RenderBundleEncoder::SetBindGroup>("setBindGroup"),
            Napi::ObjectWrap<RenderBundleEncoder>::InstanceMethod<&RenderBundleEncoder::SetVertexBuffer>("setVertexBuffer"),
            Napi::ObjectWrap<RenderBundleEncoder>::InstanceMethod<&RenderBundleEncoder::SetIndexBuffer>("setIndexBuffer"),
            Napi::ObjectWrap<RenderBundleEncoder>::InstanceMethod<&RenderBundleEncoder::Draw>("draw"),
            Napi::ObjectWrap<RenderBundleEncoder>::InstanceMethod<&RenderBundleEncoder::DrawIndexed>("drawIndexed"),
            Napi::ObjectWrap<RenderBundleEncoder>::InstanceMethod<&RenderBundleEncoder::Finish>("finish"),
        });
    }
    RenderBundleEncoder::RenderBundleEncoder(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RenderBundleEncoder>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_encoder = *info[0].As<Napi::External<wgpu::RenderBundleEncoder>>().Data();
    }
    Napi::Value RenderBundleEncoder::SetPipeline(const Napi::CallbackInfo& info)
    {
        RenderPipeline* p = Module::Current()->AsRenderPipeline(info[0].As<Napi::Object>());
        if (p != nullptr) m_encoder.SetPipeline(p->Handle());
        return info.Env().Undefined();
    }
    Napi::Value RenderBundleEncoder::SetBindGroup(const Napi::CallbackInfo& info)
    {
        uint32_t index = info[0].As<Napi::Number>().Uint32Value();
        BindGroup* bg = info[1].IsObject() ? Module::Current()->AsBindGroup(info[1].As<Napi::Object>()) : nullptr;
        if (bg != nullptr) m_encoder.SetBindGroup(index, bg->Handle());
        return info.Env().Undefined();
    }
    Napi::Value RenderBundleEncoder::SetVertexBuffer(const Napi::CallbackInfo& info)
    {
        uint32_t slot = info[0].As<Napi::Number>().Uint32Value();
        Buffer* b = info[1].IsObject() ? Module::Current()->AsBuffer(info[1].As<Napi::Object>()) : nullptr;
        uint64_t offset = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int64Value() : 0;
        if (b != nullptr) m_encoder.SetVertexBuffer(slot, b->Handle(), b->BaseOffset() + offset);
        return info.Env().Undefined();
    }
    Napi::Value RenderBundleEncoder::SetIndexBuffer(const Napi::CallbackInfo& info)
    {
        Buffer* b = info[0].IsObject() ? Module::Current()->AsBuffer(info[0].As<Napi::Object>()) : nullptr;
        std::string fmt = info.Length() >= 2 && info[1].IsString() ? info[1].As<Napi::String>().Utf8Value() : "uint32";
        wgpu::IndexFormat f = fmt == "uint16" ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32;
        uint64_t offset = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int64Value() : 0;
        if (b != nullptr) m_encoder.SetIndexBuffer(b->Handle(), f, b->BaseOffset() + offset);
        return info.Env().Undefined();
    }
    Napi::Value RenderBundleEncoder::Draw(const Napi::CallbackInfo& info)
    {
        uint32_t vertexCount = info[0].As<Napi::Number>().Uint32Value();
        uint32_t instanceCount = info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Uint32Value() : 1;
        uint32_t firstVertex = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Uint32Value() : 0;
        uint32_t firstInstance = info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Uint32Value() : 0;
        m_encoder.Draw(vertexCount, instanceCount, firstVertex, firstInstance);
        return info.Env().Undefined();
    }
    Napi::Value RenderBundleEncoder::DrawIndexed(const Napi::CallbackInfo& info)
    {
        uint32_t indexCount = info[0].As<Napi::Number>().Uint32Value();
        uint32_t instanceCount = info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Uint32Value() : 1;
        uint32_t firstIndex = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Uint32Value() : 0;
        int32_t baseVertex = info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 0;
        uint32_t firstInstance = info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Uint32Value() : 0;
        m_encoder.DrawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
        return info.Env().Undefined();
    }
    Napi::Value RenderBundleEncoder::Finish(const Napi::CallbackInfo& info)
    {
        wgpu::RenderBundleDescriptor desc{};
        wgpu::RenderBundle bundle = m_encoder.Finish(&desc);
        return Module::Current()->WrapRenderBundle(info.Env(), bundle);
    }

    // =============================================================== CommandEncoder
    Napi::Function CommandEncoder::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<CommandEncoder>::DefineClass(env, "GPUCommandEncoder", {
            Napi::ObjectWrap<CommandEncoder>::InstanceMethod<&CommandEncoder::BeginRenderPass>("beginRenderPass"),
            Napi::ObjectWrap<CommandEncoder>::InstanceMethod<&CommandEncoder::BeginComputePass>("beginComputePass"),
            Napi::ObjectWrap<CommandEncoder>::InstanceMethod<&CommandEncoder::CopyTextureToTexture>("copyTextureToTexture"),
            Napi::ObjectWrap<CommandEncoder>::InstanceMethod<&CommandEncoder::CopyBufferToBuffer>("copyBufferToBuffer"),
            Napi::ObjectWrap<CommandEncoder>::InstanceMethod<&CommandEncoder::Finish>("finish"),
        });
    }
    CommandEncoder::CommandEncoder(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CommandEncoder>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_encoder = *info[0].As<Napi::External<wgpu::CommandEncoder>>().Data();
    }
    Napi::Value CommandEncoder::BeginRenderPass(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_beginRenderPass);
        Napi::Env env = info.Env();
        const Module* mod = Module::Current();
        Napi::Object d = info[0].As<Napi::Object>();

        std::vector<wgpu::RenderPassColorAttachment> colors;
        if (d.Has("colorAttachments") && d.Get("colorAttachments").IsArray())
        {
            Napi::Array atts = d.Get("colorAttachments").As<Napi::Array>();
            colors.resize(atts.Length());
            for (uint32_t i = 0; i < atts.Length(); ++i)
            {
                Napi::Object a = atts.Get(i).As<Napi::Object>();
                wgpu::RenderPassColorAttachment& c = colors[i];
                if (a.Get("view").IsObject())
                {
                    TextureView* v = mod->AsTextureView(a.Get("view").As<Napi::Object>());
                    if (v != nullptr) c.view = v->Handle();
                }
                if (a.Get("resolveTarget").IsObject())
                {
                    TextureView* rv = mod->AsTextureView(a.Get("resolveTarget").As<Napi::Object>());
                    if (rv != nullptr) c.resolveTarget = rv->Handle();
                }
                c.loadOp = ParseLoadOp(GetString(a, "loadOp", "clear"));
                c.storeOp = ParseStoreOp(GetString(a, "storeOp", "store"));
                if (a.Has("clearValue") && a.Get("clearValue").IsObject())
                {
                    Napi::Object cv = a.Get("clearValue").As<Napi::Object>();
                    c.clearValue = wgpu::Color{GetNumber(cv, "r", 0), GetNumber(cv, "g", 0),
                        GetNumber(cv, "b", 0), GetNumber(cv, "a", 1)};
                }
            }
        }

        wgpu::RenderPassDepthStencilAttachment depth{};
        bool hasDepth = d.Has("depthStencilAttachment") && d.Get("depthStencilAttachment").IsObject();
        if (hasDepth)
        {
            Napi::Object da = d.Get("depthStencilAttachment").As<Napi::Object>();
            if (da.Get("view").IsObject())
            {
                TextureView* v = mod->AsTextureView(da.Get("view").As<Napi::Object>());
                if (v != nullptr) depth.view = v->Handle();
            }
            if (da.Has("depthLoadOp")) depth.depthLoadOp = ParseLoadOp(GetString(da, "depthLoadOp", "clear"));
            if (da.Has("depthStoreOp")) depth.depthStoreOp = ParseStoreOp(GetString(da, "depthStoreOp", "store"));
            depth.depthClearValue = static_cast<float>(GetNumber(da, "depthClearValue", 1.0));
            depth.depthReadOnly = GetBool(da, "depthReadOnly", false);
            if (da.Has("stencilLoadOp")) depth.stencilLoadOp = ParseLoadOp(GetString(da, "stencilLoadOp", "clear"));
            if (da.Has("stencilStoreOp")) depth.stencilStoreOp = ParseStoreOp(GetString(da, "stencilStoreOp", "store"));
            depth.stencilClearValue = static_cast<uint32_t>(GetNumber(da, "stencilClearValue", 0));
        }

        wgpu::RenderPassDescriptor desc{};
        desc.colorAttachmentCount = colors.size();
        desc.colorAttachments = colors.empty() ? nullptr : colors.data();
        desc.depthStencilAttachment = hasDepth ? &depth : nullptr;

        wgpu::RenderPassEncoder pass = m_encoder.BeginRenderPass(&desc);
        return mod->WrapRenderPassEncoder(env, pass);
    }
    Napi::Value CommandEncoder::Finish(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_finish);
        wgpu::CommandBuffer cb = m_encoder.Finish();
        return Module::Current()->WrapCommandBuffer(info.Env(), cb);
    }
    // beginComputePass(descriptor?) — timestampWrites in the descriptor are ignored (GPU
    // timing isn't wired here); the common no-arg form is what Lite's prefilter uses.
    Napi::Value CommandEncoder::BeginComputePass(const Napi::CallbackInfo& info)
    {
        wgpu::ComputePassDescriptor desc{};
        wgpu::ComputePassEncoder pass = m_encoder.BeginComputePass(&desc);
        return Module::Current()->WrapComputePassEncoder(info.Env(), pass);
    }
    // copyTextureToTexture(source, destination, copySize) — source/destination are
    // { texture, mipLevel?, origin?:{x,y,z} }. Lite's env prefilter copies compute-written
    // mips into the specular cubemap's mip chain.
    Napi::Value CommandEncoder::CopyTextureToTexture(const Napi::CallbackInfo& info)
    {
        const Module* mod = Module::Current();
        auto parseCopyTex = [&](const Napi::Object& o) -> wgpu::TexelCopyTextureInfo {
            wgpu::TexelCopyTextureInfo t{};
            Texture* tex = o.Get("texture").IsObject() ? mod->AsTexture(o.Get("texture").As<Napi::Object>()) : nullptr;
            if (tex != nullptr) t.texture = tex->Handle();
            t.mipLevel = static_cast<uint32_t>(GetNumber(o, "mipLevel", 0));
            if (o.Get("origin").IsObject())
            {
                Napi::Object org = o.Get("origin").As<Napi::Object>();
                t.origin.x = static_cast<uint32_t>(GetNumber(org, "x", 0));
                t.origin.y = static_cast<uint32_t>(GetNumber(org, "y", 0));
                t.origin.z = static_cast<uint32_t>(GetNumber(org, "z", 0));
            }
            return t;
        };
        wgpu::TexelCopyTextureInfo src = parseCopyTex(info[0].As<Napi::Object>());
        wgpu::TexelCopyTextureInfo dst = parseCopyTex(info[1].As<Napi::Object>());
        wgpu::Extent3D extent = ParseExtent(info[2]);
        m_encoder.CopyTextureToTexture(&src, &dst, &extent);
        return info.Env().Undefined();
    }
    // copyBufferToBuffer(src, srcOffset, dst, dstOffset, size).
    Napi::Value CommandEncoder::CopyBufferToBuffer(const Napi::CallbackInfo& info)
    {
        const Module* mod = Module::Current();
        Buffer* src = info[0].IsObject() ? mod->AsBuffer(info[0].As<Napi::Object>()) : nullptr;
        uint64_t srcOffset = info.Length() >= 2 && info[1].IsNumber() ? static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value()) : 0;
        Buffer* dst = info.Length() >= 3 && info[2].IsObject() ? mod->AsBuffer(info[2].As<Napi::Object>()) : nullptr;
        uint64_t dstOffset = info.Length() >= 4 && info[3].IsNumber() ? static_cast<uint64_t>(info[3].As<Napi::Number>().Int64Value()) : 0;
        uint64_t size = info.Length() >= 5 && info[4].IsNumber() ? static_cast<uint64_t>(info[4].As<Napi::Number>().Int64Value()) : 0;
        if (src != nullptr && dst != nullptr)
            m_encoder.CopyBufferToBuffer(src->Handle(), src->BaseOffset() + srcOffset,
                dst->Handle(), dst->BaseOffset() + dstOffset, size);
        return info.Env().Undefined();
    }

    Napi::Function Device::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Device>::DefineClass(env, "GPUDevice", {
            Napi::ObjectWrap<Device>::InstanceAccessor<&Device::GetQueue>("queue"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateBuffer>("createBuffer"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateTexture>("createTexture"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateSampler>("createSampler"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateShaderModule>("createShaderModule"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateBindGroupLayout>("createBindGroupLayout"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreatePipelineLayout>("createPipelineLayout"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateBindGroup>("createBindGroup"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateRenderPipeline>("createRenderPipeline"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateComputePipeline>("createComputePipeline"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateRenderBundleEncoder>("createRenderBundleEncoder"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::CreateCommandEncoder>("createCommandEncoder"),
            Napi::ObjectWrap<Device>::InstanceAccessor<&Device::GetFeatures>("features"),
            Napi::ObjectWrap<Device>::InstanceMethod<&Device::Destroy>("destroy"),
        });
    }

    Device::Device(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Device>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_device = *info[0].As<Napi::External<wgpu::Device>>().Data();
        if (info.Length() >= 2 && info[1].IsObject())
            m_queue = Napi::Persistent(info[1].As<Napi::Object>());
    }

    Napi::Value Device::GetQueue(const Napi::CallbackInfo& info)
    {
        return m_queue.IsEmpty() ? info.Env().Undefined() : m_queue.Value();
    }

    // Pack small, non-host-visible buffers into shared arenas to dodge D3D12's 64 KB
    // per-buffer placement floor. Returns true (with arena+offset) when suballocated.
    bool Device::SubAllocate(uint64_t size, uint32_t usage, wgpu::Buffer& outArena, uint64_t& outOffset)
    {
        // Only small buffers benefit; large ones already amortize the 64 KB floor. Skip
        // host-visible buffers (MAP_READ=0x1 / MAP_WRITE=0x2) — those need a real mappable
        // allocation, not a slice of a DEFAULT-heap arena.
        constexpr uint64_t kSubAllocMax = 8192;   // bytes; cubes' buffers are ≤256 B
        constexpr uint64_t kArenaSize = 4 * 1024 * 1024;
        constexpr uint64_t kAlign = 256;          // satisfies uniform/storage bind offset alignment
        if (size == 0 || size > kSubAllocMax) return false;
        if (usage & 0x3u) return false;           // MAP_READ | MAP_WRITE

        if (m_arenaQueue == nullptr) m_arenaQueue = m_device.GetQueue();
        const uint64_t need = (size + kAlign - 1) / kAlign * kAlign;

        for (auto& a : m_arenas)
        {
            if (a.usage == usage && a.capacity - a.used >= need)
            {
                outArena = a.buffer;
                outOffset = a.used;
                a.used += need;
                return true;
            }
        }
        // No arena for this usage with room — make a new one (CopyDst so we can upload).
        SubArena arena{};
        arena.usage = usage;
        arena.capacity = need > kArenaSize ? need : kArenaSize;
        wgpu::BufferDescriptor ad{};
        ad.size = arena.capacity;
        ad.usage = static_cast<wgpu::BufferUsage>(usage | 0x8u); // | COPY_DST
        ad.mappedAtCreation = false;
        arena.buffer = m_device.CreateBuffer(&ad);
        if (arena.buffer == nullptr) return false;
        arena.used = need;
        outArena = arena.buffer;
        outOffset = 0;
        m_arenas.push_back(std::move(arena));
        return true;
    }

    Napi::Value Device::CreateBuffer(const Napi::CallbackInfo& info)
    {
        auto _t0 = std::chrono::steady_clock::now();
        Napi::Env env = info.Env();
        Napi::Object d = info[0].As<Napi::Object>();
        const uint64_t size = static_cast<uint64_t>(GetNumber(d, "size", 0));
        const uint32_t usage = static_cast<uint32_t>(GetNumber(d, "usage", 0));
        const bool mappedAtCreation = GetBool(d, "mappedAtCreation", false);
        g_mem.addBuffer(size, usage, mappedAtCreation);

        wgpu::Buffer arena;
        uint64_t arenaOffset = 0;
        if (SubAllocate(size, usage, arena, arenaOffset))
        {
            BufferInit init{};
            init.buffer = arena;
            init.baseOffset = arenaOffset;
            init.size = size;
            init.isSub = true;
            init.queue = m_arenaQueue;
            Napi::Object r = Module::Current()->WrapBuffer(env, init);
            if (g_mem.enabled) g_mem.createBufferNs += std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - _t0).count();
            return r;
        }

        wgpu::BufferDescriptor desc{};
        desc.size = size;
        desc.usage = static_cast<wgpu::BufferUsage>(usage);
        desc.mappedAtCreation = mappedAtCreation;
        wgpu::Buffer buffer = m_device.CreateBuffer(&desc);
        BufferInit init{};
        init.buffer = buffer;
        init.size = size;
        Napi::Object r = Module::Current()->WrapBuffer(env, init);
        if (g_mem.enabled) g_mem.createBufferNs += std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - _t0).count();
        return r;
    }

    Napi::Value Device::CreateTexture(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        Napi::Object d = info[0].As<Napi::Object>();
        wgpu::TextureDescriptor desc{};
        desc.size = ParseExtent(d.Get("size"));
        desc.format = ParseFormat(GetString(d, "format", "rgba8unorm"));
        desc.usage = static_cast<wgpu::TextureUsage>(static_cast<uint32_t>(GetNumber(d, "usage", 0)));
        desc.mipLevelCount = static_cast<uint32_t>(GetNumber(d, "mipLevelCount", 1));
        desc.sampleCount = static_cast<uint32_t>(GetNumber(d, "sampleCount", 1));
        desc.dimension = wgpu::TextureDimension::e2D;
        wgpu::Texture texture = m_device.CreateTexture(&desc);
        {
            // Rough VRAM estimate: w*h*layers*bytesPerPixel(format-agnostic 4B approx)*mips(~1.33).
            uint64_t bpp = 4;
            uint64_t px = static_cast<uint64_t>(desc.size.width) * desc.size.height *
                desc.size.depthOrArrayLayers;
            uint64_t bytes = px * bpp;
            if (desc.mipLevelCount > 1) bytes = bytes * 4 / 3;
            g_mem.addTexture(bytes);
        }
        return Module::Current()->WrapTexture(env, texture);
    }

    Napi::Value Device::CreateSampler(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        Napi::Object d = info.Length() >= 1 && info[0].IsObject() ? info[0].As<Napi::Object>() : Napi::Object::New(env);
        wgpu::SamplerDescriptor desc{};
        desc.magFilter = ParseFilter(GetString(d, "magFilter", "nearest"));
        desc.minFilter = ParseFilter(GetString(d, "minFilter", "nearest"));
        desc.mipmapFilter = ParseMipmapFilter(GetString(d, "mipmapFilter", "nearest"));
        desc.addressModeU = ParseAddressMode(GetString(d, "addressModeU", "clamp-to-edge"));
        desc.addressModeV = ParseAddressMode(GetString(d, "addressModeV", "clamp-to-edge"));
        desc.addressModeW = ParseAddressMode(GetString(d, "addressModeW", "clamp-to-edge"));
        desc.lodMinClamp = static_cast<float>(GetNumber(d, "lodMinClamp", 0));
        desc.lodMaxClamp = static_cast<float>(GetNumber(d, "lodMaxClamp", 32));
        desc.maxAnisotropy = static_cast<uint16_t>(GetNumber(d, "maxAnisotropy", 1));
        if (d.Has("compare") && d.Get("compare").IsString())
            desc.compare = ParseCompare(GetString(d, "compare", "less"));
        wgpu::Sampler sampler = m_device.CreateSampler(&desc);
        return Module::Current()->WrapSampler(env, sampler);
    }

    Napi::Value Device::CreateShaderModule(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        Napi::Object d = info[0].As<Napi::Object>();
        std::string code = GetString(d, "code", "");
        wgpu::ShaderSourceWGSL wgsl{};
        wgsl.code = code.c_str();
        wgpu::ShaderModuleDescriptor desc{};
        desc.nextInChain = &wgsl;
        wgpu::ShaderModule module = m_device.CreateShaderModule(&desc);
        return Module::Current()->WrapShaderModule(env, module);
    }

    Napi::Value Device::CreateBindGroupLayout(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        Napi::Object d = info[0].As<Napi::Object>();
        Napi::Array entries = d.Get("entries").As<Napi::Array>();
        std::vector<wgpu::BindGroupLayoutEntry> wEntries(entries.Length());
        for (uint32_t i = 0; i < entries.Length(); ++i)
        {
            Napi::Object e = entries.Get(i).As<Napi::Object>();
            wgpu::BindGroupLayoutEntry& w = wEntries[i];
            w.binding = static_cast<uint32_t>(GetNumber(e, "binding", i));
            w.visibility = static_cast<wgpu::ShaderStage>(static_cast<uint32_t>(GetNumber(e, "visibility", 0)));
            if (e.Has("buffer") && e.Get("buffer").IsObject())
            {
                Napi::Object b = e.Get("buffer").As<Napi::Object>();
                w.buffer.type = ParseBufferBindingType(GetString(b, "type", "uniform"));
                w.buffer.hasDynamicOffset = GetBool(b, "hasDynamicOffset", false);
                w.buffer.minBindingSize = static_cast<uint64_t>(GetNumber(b, "minBindingSize", 0));
            }
            else if (e.Has("sampler") && e.Get("sampler").IsObject())
            {
                Napi::Object s = e.Get("sampler").As<Napi::Object>();
                w.sampler.type = ParseSamplerBindingType(GetString(s, "type", "filtering"));
            }
            else if (e.Has("texture") && e.Get("texture").IsObject())
            {
                Napi::Object t = e.Get("texture").As<Napi::Object>();
                w.texture.sampleType = ParseTextureSampleType(GetString(t, "sampleType", "float"));
                w.texture.viewDimension = ParseViewDimension(GetString(t, "viewDimension", "2d"));
                w.texture.multisampled = GetBool(t, "multisampled", false);
            }
            else if (e.Has("storageTexture") && e.Get("storageTexture").IsObject())
            {
                // Storage texture binding (compute prefilter writes to it). access defaults
                // to write-only; format + viewDimension from the descriptor.
                Napi::Object st = e.Get("storageTexture").As<Napi::Object>();
                std::string access = GetString(st, "access", "write-only");
                w.storageTexture.access = access == "read-write" ? wgpu::StorageTextureAccess::ReadWrite
                    : access == "read-only" ? wgpu::StorageTextureAccess::ReadOnly
                    : wgpu::StorageTextureAccess::WriteOnly;
                w.storageTexture.format = ParseFormat(GetString(st, "format", "rgba8unorm"));
                w.storageTexture.viewDimension = ParseViewDimension(GetString(st, "viewDimension", "2d"));
            }
        }
        wgpu::BindGroupLayoutDescriptor desc{};
        desc.entryCount = wEntries.size();
        desc.entries = wEntries.data();
        wgpu::BindGroupLayout layout = m_device.CreateBindGroupLayout(&desc);
        return Module::Current()->WrapBindGroupLayout(env, layout);
    }

    Napi::Value Device::CreatePipelineLayout(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        Napi::Object d = info[0].As<Napi::Object>();
        Napi::Array bgls = d.Get("bindGroupLayouts").As<Napi::Array>();
        std::vector<wgpu::BindGroupLayout> layouts;
        layouts.reserve(bgls.Length());
        for (uint32_t i = 0; i < bgls.Length(); ++i)
        {
            BindGroupLayout* bgl = Module::Current()->AsBindGroupLayout(bgls.Get(i).As<Napi::Object>());
            if (bgl != nullptr) layouts.push_back(bgl->Handle());
        }
        wgpu::PipelineLayoutDescriptor desc{};
        desc.bindGroupLayoutCount = layouts.size();
        desc.bindGroupLayouts = layouts.data();
        wgpu::PipelineLayout layout = m_device.CreatePipelineLayout(&desc);
        return Module::Current()->WrapPipelineLayout(env, layout);
    }

    Napi::Value Device::CreateBindGroup(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        const Module* mod = Module::Current();
        Napi::Object d = info[0].As<Napi::Object>();
        BindGroupLayout* layout = mod->AsBindGroupLayout(d.Get("layout").As<Napi::Object>());
        Napi::Array entries = d.Get("entries").As<Napi::Array>();
        std::vector<wgpu::BindGroupEntry> wEntries(entries.Length());
        for (uint32_t i = 0; i < entries.Length(); ++i)
        {
            Napi::Object e = entries.Get(i).As<Napi::Object>();
            wgpu::BindGroupEntry& w = wEntries[i];
            w.binding = static_cast<uint32_t>(GetNumber(e, "binding", i));
            Napi::Value res = e.Get("resource");
            if (res.IsObject())
            {
                Napi::Object ro = res.As<Napi::Object>();
                // Buffer binding: { buffer, offset?, size? }.
                if (ro.Has("buffer") && ro.Get("buffer").IsObject())
                {
                    Buffer* buf = mod->AsBuffer(ro.Get("buffer").As<Napi::Object>());
                    if (buf != nullptr)
                    {
                        w.buffer = buf->Handle();
                        w.offset = buf->BaseOffset() + static_cast<uint64_t>(GetNumber(ro, "offset", 0));
                        if (ro.Has("size")) w.size = static_cast<uint64_t>(GetNumber(ro, "size", 0));
                        else if (buf->IsSub()) w.size = buf->Size(); // must bound the binding to
                            // this logical sub-buffer; otherwise it spans the whole arena and
                            // (for uniforms) exceeds maxUniformBufferBindingSize.
                    }
                }
                else if (Sampler* samp = mod->AsSampler(ro))
                {
                    w.sampler = samp->Handle();
                }
                else if (TextureView* view = mod->AsTextureView(ro))
                {
                    w.textureView = view->Handle();
                }
            }
        }
        wgpu::BindGroupDescriptor desc{};
        desc.layout = layout != nullptr ? layout->Handle() : nullptr;
        desc.entryCount = wEntries.size();
        desc.entries = wEntries.data();
        wgpu::BindGroup group = m_device.CreateBindGroup(&desc);
        return mod->WrapBindGroup(env, group);
    }

    Napi::Value Device::CreateRenderPipeline(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        const Module* mod = Module::Current();
        Napi::Object d = info[0].As<Napi::Object>();

        // ---- layout ----
        wgpu::PipelineLayout layout;
        if (d.Get("layout").IsObject())
        {
            PipelineLayout* pl = mod->AsPipelineLayout(d.Get("layout").As<Napi::Object>());
            if (pl != nullptr) layout = pl->Handle();
        }

        // ---- vertex ----
        Napi::Object vtx = d.Get("vertex").As<Napi::Object>();
        ShaderModule* vsMod = mod->AsShaderModule(vtx.Get("module").As<Napi::Object>());
        std::string vsEntry = GetString(vtx, "entryPoint", "vs_main");

        // Vertex buffer layouts. Keep attribute storage alive until the create call.
        std::vector<std::vector<wgpu::VertexAttribute>> attribStore;
        std::vector<wgpu::VertexBufferLayout> vbLayouts;
        if (vtx.Has("buffers") && vtx.Get("buffers").IsArray())
        {
            Napi::Array buffers = vtx.Get("buffers").As<Napi::Array>();
            attribStore.reserve(buffers.Length());
            vbLayouts.reserve(buffers.Length());
            for (uint32_t b = 0; b < buffers.Length(); ++b)
            {
                Napi::Object bl = buffers.Get(b).As<Napi::Object>();
                Napi::Array attrs = bl.Get("attributes").As<Napi::Array>();
                std::vector<wgpu::VertexAttribute> a(attrs.Length());
                for (uint32_t i = 0; i < attrs.Length(); ++i)
                {
                    Napi::Object ao = attrs.Get(i).As<Napi::Object>();
                    a[i].format = ParseVertexFormat(GetString(ao, "format", "float32x3"));
                    a[i].offset = static_cast<uint64_t>(GetNumber(ao, "offset", 0));
                    a[i].shaderLocation = static_cast<uint32_t>(GetNumber(ao, "shaderLocation", i));
                }
                attribStore.push_back(std::move(a));
            }
            for (uint32_t b = 0; b < buffers.Length(); ++b)
            {
                Napi::Object bl = buffers.Get(b).As<Napi::Object>();
                wgpu::VertexBufferLayout vb{};
                vb.arrayStride = static_cast<uint64_t>(GetNumber(bl, "arrayStride", 0));
                vb.stepMode = GetString(bl, "stepMode", "vertex") == "instance"
                    ? wgpu::VertexStepMode::Instance : wgpu::VertexStepMode::Vertex;
                vb.attributeCount = attribStore[b].size();
                vb.attributes = attribStore[b].data();
                vbLayouts.push_back(vb);
            }
        }

        // ---- fragment ----
        bool hasFragment = d.Has("fragment") && d.Get("fragment").IsObject();
        Napi::Object frag;
        ShaderModule* fsMod = nullptr;
        std::string fsEntry = "fs_main";
        std::vector<wgpu::ColorTargetState> targets;
        std::vector<wgpu::BlendState> blends; // keep alive
        if (hasFragment)
        {
            frag = d.Get("fragment").As<Napi::Object>();
            fsMod = mod->AsShaderModule(frag.Get("module").As<Napi::Object>());
            fsEntry = GetString(frag, "entryPoint", "fs_main");
            Napi::Array tgts = frag.Get("targets").As<Napi::Array>();
            targets.resize(tgts.Length());
            blends.resize(tgts.Length());
            for (uint32_t i = 0; i < tgts.Length(); ++i)
            {
                Napi::Object t = tgts.Get(i).As<Napi::Object>();
                targets[i].format = ParseFormat(GetString(t, "format", "rgba8unorm"));
                // writeMask: honor the descriptor (PBR/standard pass GPUColorWrite.ALL = 0xF;
                // depth/shadow passes pass 0 to disable color writes). Default ALL.
                targets[i].writeMask = t.Has("writeMask") && t.Get("writeMask").IsNumber()
                    ? static_cast<wgpu::ColorWriteMask>(t.Get("writeMask").As<Napi::Number>().Uint32Value())
                    : wgpu::ColorWriteMask::All;
                if (t.Has("blend") && t.Get("blend").IsObject())
                {
                    Napi::Object bl = t.Get("blend").As<Napi::Object>();
                    auto parseComp = [](const Napi::Object& c, wgpu::BlendComponent& out) {
                        auto factor = [](const std::string& s) {
                            if (s == "one") return wgpu::BlendFactor::One;
                            if (s == "src-alpha") return wgpu::BlendFactor::SrcAlpha;
                            if (s == "one-minus-src-alpha") return wgpu::BlendFactor::OneMinusSrcAlpha;
                            if (s == "dst-alpha") return wgpu::BlendFactor::DstAlpha;
                            if (s == "one-minus-dst-alpha") return wgpu::BlendFactor::OneMinusDstAlpha;
                            return wgpu::BlendFactor::Zero;
                        };
                        out.srcFactor = factor(GetString(c, "srcFactor", "one"));
                        out.dstFactor = factor(GetString(c, "dstFactor", "zero"));
                        out.operation = wgpu::BlendOperation::Add;
                    };
                    if (bl.Get("color").IsObject()) parseComp(bl.Get("color").As<Napi::Object>(), blends[i].color);
                    if (bl.Get("alpha").IsObject()) parseComp(bl.Get("alpha").As<Napi::Object>(), blends[i].alpha);
                    targets[i].blend = &blends[i];
                }
            }
        }

        // ---- primitive ----
        wgpu::PrimitiveState primitive{};
        primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        primitive.cullMode = wgpu::CullMode::None;
        primitive.frontFace = wgpu::FrontFace::CCW;
        if (d.Has("primitive") && d.Get("primitive").IsObject())
        {
            Napi::Object p = d.Get("primitive").As<Napi::Object>();
            primitive.topology = ParseTopology(GetString(p, "topology", "triangle-list"));
            primitive.cullMode = ParseCullMode(GetString(p, "cullMode", "none"));
            primitive.frontFace = ParseFrontFace(GetString(p, "frontFace", "ccw"));
        }

        // ---- depthStencil ----
        wgpu::DepthStencilState depth{};
        bool hasDepth = d.Has("depthStencil") && d.Get("depthStencil").IsObject();
        if (hasDepth)
        {
            Napi::Object ds = d.Get("depthStencil").As<Napi::Object>();
            depth.format = ParseFormat(GetString(ds, "format", "depth24plus"));
            depth.depthWriteEnabled = GetBool(ds, "depthWriteEnabled", true)
                ? wgpu::OptionalBool::True : wgpu::OptionalBool::False;
            depth.depthCompare = ParseCompare(GetString(ds, "depthCompare", "less"));
        }

        // ---- multisample ----
        wgpu::MultisampleState multisample{};
        multisample.count = 1;
        if (d.Has("multisample") && d.Get("multisample").IsObject())
        {
            Napi::Object ms = d.Get("multisample").As<Napi::Object>();
            multisample.count = static_cast<uint32_t>(GetNumber(ms, "count", 1));
            multisample.alphaToCoverageEnabled = GetBool(ms, "alphaToCoverageEnabled", false);
        }

        // ---- assemble ----
        wgpu::FragmentState fragment{};
        if (hasFragment)
        {
            fragment.module = fsMod != nullptr ? fsMod->Handle() : nullptr;
            fragment.entryPoint = fsEntry.c_str();
            fragment.targetCount = targets.size();
            fragment.targets = targets.data();
        }

        wgpu::RenderPipelineDescriptor desc{};
        desc.layout = layout;
        desc.vertex.module = vsMod != nullptr ? vsMod->Handle() : nullptr;
        desc.vertex.entryPoint = vsEntry.c_str();
        desc.vertex.bufferCount = vbLayouts.size();
        desc.vertex.buffers = vbLayouts.empty() ? nullptr : vbLayouts.data();
        desc.primitive = primitive;
        desc.depthStencil = hasDepth ? &depth : nullptr;
        desc.multisample = multisample;
        desc.fragment = hasFragment ? &fragment : nullptr;

        wgpu::RenderPipeline pipeline = m_device.CreateRenderPipeline(&desc);
        return mod->WrapRenderPipeline(env, pipeline);
    }

    // device.createComputePipeline({ layout, compute: { module, entryPoint } }) — layout is
    // either a GPUPipelineLayout or the string "auto" (Dawn auto-derives bind group layouts
    // from the shader; left null here). Used by Lite's env prefilter + GPU culling.
    Napi::Value Device::CreateComputePipeline(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        const Module* mod = Module::Current();
        Napi::Object d = info[0].As<Napi::Object>();

        wgpu::ComputePipelineDescriptor desc{};
        if (d.Get("layout").IsObject())
        {
            PipelineLayout* pl = mod->AsPipelineLayout(d.Get("layout").As<Napi::Object>());
            if (pl != nullptr) desc.layout = pl->Handle();
        }
        std::string entry = "main";
        if (d.Get("compute").IsObject())
        {
            Napi::Object c = d.Get("compute").As<Napi::Object>();
            ShaderModule* sm = c.Get("module").IsObject() ? mod->AsShaderModule(c.Get("module").As<Napi::Object>()) : nullptr;
            entry = GetString(c, "entryPoint", "main");
            if (sm != nullptr) desc.compute.module = sm->Handle();
            desc.compute.entryPoint = entry.c_str();
        }
        wgpu::ComputePipeline pipeline = m_device.CreateComputePipeline(&desc);
        return mod->WrapComputePipeline(env, pipeline);
    }

    Napi::Value Device::CreateCommandEncoder(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_createCommandEncoder);
        wgpu::CommandEncoderDescriptor desc{};
        wgpu::CommandEncoder encoder = m_device.CreateCommandEncoder(&desc);
        return Module::Current()->WrapCommandEncoder(info.Env(), encoder);
    }

    // device.createRenderBundleEncoder({ colorFormats, depthStencilFormat?, sampleCount? })
    // — the attachment state the bundle's pipelines were built against. Lite builds the
    // opaque bundle once with the task's target signature.
    Napi::Value Device::CreateRenderBundleEncoder(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        Napi::Object d = info[0].As<Napi::Object>();
        std::vector<wgpu::TextureFormat> colorFormats;
        if (d.Has("colorFormats") && d.Get("colorFormats").IsArray())
        {
            Napi::Array arr = d.Get("colorFormats").As<Napi::Array>();
            colorFormats.reserve(arr.Length());
            for (uint32_t i = 0; i < arr.Length(); ++i)
            {
                Napi::Value f = arr.Get(i);
                colorFormats.push_back(f.IsString() ? ParseFormat(f.As<Napi::String>().Utf8Value())
                                                    : wgpu::TextureFormat::Undefined);
            }
        }
        wgpu::RenderBundleEncoderDescriptor desc{};
        desc.colorFormatCount = colorFormats.size();
        desc.colorFormats = colorFormats.empty() ? nullptr : colorFormats.data();
        if (d.Has("depthStencilFormat") && d.Get("depthStencilFormat").IsString())
            desc.depthStencilFormat = ParseFormat(d.Get("depthStencilFormat").As<Napi::String>().Utf8Value());
        if (d.Has("sampleCount") && d.Get("sampleCount").IsNumber())
            desc.sampleCount = d.Get("sampleCount").As<Napi::Number>().Uint32Value();
        wgpu::RenderBundleEncoder encoder = m_device.CreateRenderBundleEncoder(&desc);
        return Module::Current()->WrapRenderBundleEncoder(env, encoder);
    }

    // device.features — a setlike with .has(name). Real Lite calls
    // `device.features.has("timestamp-query")`. Returns a JS Set of feature strings.
    Napi::Value Device::GetFeatures(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        wgpu::SupportedFeatures supported{};
        m_device.GetFeatures(&supported);
        return MakeFeatureSet(env, supported);
    }

    Napi::Value Device::Destroy(const Napi::CallbackInfo& info)
    {
        m_device.Destroy();
        return info.Env().Undefined();
    }

    // ================================================================ CanvasContext
    Napi::Function CanvasContext::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<CanvasContext>::DefineClass(env, "GPUCanvasContext", {
            Napi::ObjectWrap<CanvasContext>::InstanceMethod<&CanvasContext::Configure>("configure"),
            Napi::ObjectWrap<CanvasContext>::InstanceMethod<&CanvasContext::Unconfigure>("unconfigure"),
            Napi::ObjectWrap<CanvasContext>::InstanceMethod<&CanvasContext::GetCurrentTexture>("getCurrentTexture"),
        });
    }
    CanvasContext::CanvasContext(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CanvasContext>{info}
    {
        // info[0]: the owning canvas object (read width/height at configure time).
        if (info.Length() >= 1 && info[0].IsObject())
        {
            Napi::Object canvas = info[0].As<Napi::Object>();
            m_width = static_cast<uint32_t>(GetNumber(canvas, "width", 0));
            m_height = static_cast<uint32_t>(GetNumber(canvas, "height", 0));
        }
    }
    Napi::Value CanvasContext::Configure(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        const Module* mod = Module::Current();
        Napi::Object d = info[0].As<Napi::Object>();
        Device* dev = d.Get("device").IsObject()
            ? Napi::ObjectWrap<Device>::Unwrap(d.Get("device").As<Napi::Object>())
            : nullptr;
        wgpu::TextureFormat format = ParseFormat(GetString(d, "format", "bgra8unorm"));
        if (dev != nullptr)
            const_cast<Module*>(mod)->ConfigureSurface(dev->Handle(), format, m_width, m_height);
        return env.Undefined();
    }
    Napi::Value CanvasContext::Unconfigure(const Napi::CallbackInfo& info)
    {
        Module::Current()->Surface().Unconfigure();
        return info.Env().Undefined();
    }
    Napi::Value CanvasContext::GetCurrentTexture(const Napi::CallbackInfo& info)
    {
        ScopedProf _p(P_getCurrentTexture);
        Napi::Env env = info.Env();
        const Module* mod = Module::Current();
        // First swapchain acquire = the render loop has begun (setup is done). Arm threaded
        // submit deferral here so all setup submits ran synchronously and stayed valid.
        const_cast<Module*>(mod)->MarkRenderLoopStarted();
        wgpu::SurfaceTexture surfaceTexture{};
        mod->Surface().GetCurrentTexture(&surfaceTexture);
        if (surfaceTexture.texture == nullptr)
        {
            Napi::Error::New(env, "getCurrentTexture: no swapchain texture (configure first)")
                .ThrowAsJavaScriptException();
            return env.Undefined();
        }
        const_cast<Module*>(mod)->NoteAcquiredTexture(surfaceTexture.texture);
        return mod->WrapTexture(env, surfaceTexture.texture);
    }

    // ====================================================================== Adapter
    Napi::Function Adapter::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<Adapter>::DefineClass(env, "GPUAdapter", {
            Napi::ObjectWrap<Adapter>::InstanceAccessor<&Adapter::GetFeatures>("features"),
            Napi::ObjectWrap<Adapter>::InstanceMethod<&Adapter::RequestDevice>("requestDevice"),
        });
    }
    Adapter::Adapter(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Adapter>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
            m_adapter = *info[0].As<Napi::External<wgpu::Adapter>>().Data();
    }
    Napi::Value Adapter::GetFeatures(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        wgpu::SupportedFeatures supported{};
        m_adapter.GetFeatures(&supported);
        return MakeFeatureSet(env, supported);
    }
    // requestDevice(descriptor?) -> Promise<GPUDevice>. Dawn's request is synchronous
    // (WaitAny), so we resolve the promise immediately. requiredFeatures are honored;
    // requiredLimits are accepted but not yet plumbed.
    Napi::Value Adapter::RequestDevice(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        const Module* mod = Module::Current();
        auto deferred = Napi::Promise::Deferred::New(env);

        wgpu::DeviceDescriptor desc{};
        desc.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
                std::fprintf(stderr, "[webgpu] device error (%d): %.*s\n",
                    static_cast<int>(type), static_cast<int>(message.length), message.data);
            });
        std::vector<wgpu::FeatureName> features;
        if (info.Length() >= 1 && info[0].IsObject())
        {
            Napi::Object d = info[0].As<Napi::Object>();
            if (d.Has("requiredFeatures") && d.Get("requiredFeatures").IsArray())
            {
                Napi::Array reqs = d.Get("requiredFeatures").As<Napi::Array>();
                for (uint32_t i = 0; i < reqs.Length(); ++i)
                {
                    if (!reqs.Get(i).IsString()) continue;
                    wgpu::FeatureName f = StringToFeatureName(reqs.Get(i).As<Napi::String>().Utf8Value());
                    if (static_cast<uint32_t>(f) != 0) features.push_back(f);
                }
            }
        }
        if (!features.empty())
        {
            desc.requiredFeatureCount = features.size();
            desc.requiredFeatures = features.data();
        }
        static const char* kEnableDxc = "use_dxc";
        static const char* kImplicitSync = "implicit_device_synchronization";
        // Threaded submit is ON by default (it moves the expensive Dawn->D3D12 command
        // translation off the JS thread; measured a consistent CPU win across light and
        // draw-heavy scenes with correctness preserved). Opt out with LITE_THREAD_SUBMIT=0.
        bool threaded = ThreadedSubmitDefault();
        const char* enabledToggles[2] = { kEnableDxc, kImplicitSync };
        wgpu::DawnTogglesDescriptor toggles{};
        toggles.enabledToggleCount = threaded ? 2 : 1; // add device thread-safety only when needed
        toggles.enabledToggles = enabledToggles;
        desc.nextInChain = &toggles;

        wgpu::Device device;
        wgpu::Future future = m_adapter.RequestDevice(
            &desc, wgpu::CallbackMode::WaitAnyOnly,
            [&device](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message) {
                if (status == wgpu::RequestDeviceStatus::Success) device = std::move(result);
                else std::fprintf(stderr, "[webgpu] requestDevice failed: %.*s\n",
                    static_cast<int>(message.length), message.data);
            });
        mod->Instance().WaitAny(future, UINT64_MAX);

        if (device == nullptr)
        {
            deferred.Reject(Napi::Error::New(env, "requestDevice failed").Value());
        }
        else
        {
            deferred.Resolve(mod->CreateDevice(env, device));
        }
        return deferred.Promise();
    }

    // =================================================================== ImageBitmap
    Napi::Function ImageBitmap::DefineClass(Napi::Env env)
    {
        return Napi::ObjectWrap<ImageBitmap>::DefineClass(env, "ImageBitmap", {
            Napi::ObjectWrap<ImageBitmap>::InstanceAccessor<&ImageBitmap::GetWidth>("width"),
            Napi::ObjectWrap<ImageBitmap>::InstanceAccessor<&ImageBitmap::GetHeight>("height"),
            Napi::ObjectWrap<ImageBitmap>::InstanceMethod<&ImageBitmap::Close>("close"),
            Napi::ObjectWrap<ImageBitmap>::InstanceMethod<&ImageBitmap::GetPixels>("_getPixels"),
        });
    }
    // Constructed from an External<DecodedImage> smuggled by createImageBitmap (so the
    // decoded pixels move into the wrapper without an extra copy across N-API).
    ImageBitmap::ImageBitmap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ImageBitmap>{info}
    {
        if (info.Length() >= 1 && info[0].IsExternal())
        {
            auto* decoded = info[0].As<Napi::External<DecodedImage>>().Data();
            if (decoded != nullptr)
            {
                m_width = decoded->width;
                m_height = decoded->height;
                m_pixels = std::move(decoded->pixels);
            }
        }
    }
    Napi::Value ImageBitmap::GetWidth(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), m_width); }
    Napi::Value ImageBitmap::GetHeight(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), m_height); }
    Napi::Value ImageBitmap::Close(const Napi::CallbackInfo& info)
    {
        m_pixels.clear();
        m_pixels.shrink_to_fit();
        return info.Env().Undefined();
    }
    Napi::Value ImageBitmap::GetPixels(const Napi::CallbackInfo& info)
    {
        Napi::Env env = info.Env();
        // Hand JS a fresh Uint8ClampedArray copy of the RGBA8 pixels (ChakraCore's N-API
        // doesn't alias external memory, so a copy is required anyway).
        Napi::ArrayBuffer ab = Napi::ArrayBuffer::New(env, m_pixels.size());
        if (!m_pixels.empty()) std::memcpy(ab.Data(), m_pixels.data(), m_pixels.size());
        return Napi::Uint8Array::New(env, m_pixels.size(), ab, 0, napi_uint8_clamped_array);
    }

    // ======================================================================= Module
    const Module* Module::s_current = nullptr;

    Module::Module(Napi::Env env, WindowHandle window)
        : m_window(window)
    {
        auto persist = [](Napi::Function fn) {
            Napi::FunctionReference ref = Napi::Persistent(fn);
            ref.SuppressDestruct();
            return ref;
        };
        m_bufferCtor = persist(Buffer::DefineClass(env));
        m_textureCtor = persist(Texture::DefineClass(env));
        m_textureViewCtor = persist(TextureView::DefineClass(env));
        m_samplerCtor = persist(Sampler::DefineClass(env));
        m_shaderModuleCtor = persist(ShaderModule::DefineClass(env));
        m_bindGroupLayoutCtor = persist(BindGroupLayout::DefineClass(env));
        m_pipelineLayoutCtor = persist(PipelineLayout::DefineClass(env));
        m_bindGroupCtor = persist(BindGroup::DefineClass(env));
        m_renderPipelineCtor = persist(RenderPipeline::DefineClass(env));
        m_computePipelineCtor = persist(ComputePipeline::DefineClass(env));
        m_computePassEncoderCtor = persist(ComputePassEncoder::DefineClass(env));
        m_renderBundleCtor = persist(RenderBundle::DefineClass(env));
        m_renderBundleEncoderCtor = persist(RenderBundleEncoder::DefineClass(env));
        m_queueCtor = persist(Queue::DefineClass(env));
        m_deviceCtor = persist(Device::DefineClass(env));
        m_commandEncoderCtor = persist(CommandEncoder::DefineClass(env));
        m_renderPassEncoderCtor = persist(RenderPassEncoder::DefineClass(env));
        m_commandBufferCtor = persist(CommandBuffer::DefineClass(env));
        m_adapterCtor = persist(Adapter::DefineClass(env));
        m_canvasContextCtor = persist(CanvasContext::DefineClass(env));
        m_imageBitmapCtor = persist(ImageBitmap::DefineClass(env));

        // Dawn instance with TimedWaitAny so the synchronous request/wait pattern works.
        static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
        wgpu::InstanceDescriptor instanceDesc{};
        instanceDesc.requiredFeatureCount = 1;
        instanceDesc.requiredFeatures = &kTimedWaitAny;
        m_instance = wgpu::CreateInstance(&instanceDesc);

        // The HWND-bound swapchain surface (configured later via GPUCanvasContext).
        if (m_window.hwnd != nullptr)
            m_surface = CreateSurfaceForWindow(m_instance, m_window.hwnd, m_window.hinstance);

        // Threaded-submit experiment: move queue.Submit + surface.Present off the JS thread
        // onto a dedicated render thread (mirrors the browser's GPU-process split). The render
        // thread is started lazily on the first submit so the device exists.
        {
            m_threadedSubmit = ThreadedSubmitDefault();
            if (const char* af = std::getenv("LITE_THREAD_ARM_FRAME"))
            {
                int n = std::atoi(af);
                if (n >= 0 && n <= 10000) m_armFrame = n;
            }
        }

        s_current = this;
    }

    Module::~Module()
    {
        StopRenderThread();
        if (s_current == this) s_current = nullptr;
    }

    void Module::ConfigureSurface(const wgpu::Device& device, wgpu::TextureFormat format,
        uint32_t width, uint32_t height)
    {
        if (m_surface == nullptr || width == 0 || height == 0) return;
        m_surfaceFormat = format;
        m_surfaceDevice = device;
        if (m_readbackFrame < 0)
        {
            if (const char* rb = std::getenv("LITE_READBACK"))
                m_readbackFrame = std::atoi(rb);
        }
        wgpu::SurfaceConfiguration config{};
        config.device = device;
        config.format = format;
        config.usage = wgpu::TextureUsage::RenderAttachment;
        // Add CopySrc so the readback path can copy the presented texture back to the CPU,
        // but only if the surface actually supports it (else Configure would fail).
        m_surfaceCanCopySrc = false;
        if (m_readbackFrame >= 0 && m_adapter != nullptr)
        {
            wgpu::SurfaceCapabilities caps{};
            if (m_surface.GetCapabilities(m_adapter, &caps) == wgpu::Status::Success &&
                (caps.usages & wgpu::TextureUsage::CopySrc))
            {
                config.usage = config.usage | wgpu::TextureUsage::CopySrc;
                m_surfaceCanCopySrc = true;
            }
        }
        config.width = width;
        config.height = height;
        config.presentMode = wgpu::PresentMode::Fifo;
        if (m_noVsync)
        {
            // Pick an uncapped present mode for benchmarking: prefer Mailbox (low-latency,
            // no tearing), else Immediate, else fall back to Fifo. Query what the surface
            // actually supports so Configure never fails on an unsupported mode.
            wgpu::PresentMode chosen = wgpu::PresentMode::Fifo;
            wgpu::SurfaceCapabilities caps{};
            if (m_adapter != nullptr &&
                m_surface.GetCapabilities(m_adapter, &caps) == wgpu::Status::Success)
            {
                bool hasMailbox = false, hasImmediate = false;
                for (size_t i = 0; i < caps.presentModeCount; ++i)
                {
                    if (caps.presentModes[i] == wgpu::PresentMode::Mailbox) hasMailbox = true;
                    if (caps.presentModes[i] == wgpu::PresentMode::Immediate) hasImmediate = true;
                }
                chosen = hasMailbox ? wgpu::PresentMode::Mailbox
                       : hasImmediate ? wgpu::PresentMode::Immediate
                       : wgpu::PresentMode::Fifo;
            }
            else
            {
                chosen = wgpu::PresentMode::Mailbox; // best-effort if caps query unavailable
            }
            config.presentMode = chosen;
        }
        config.alphaMode = wgpu::CompositeAlphaMode::Auto;
        m_surface.Configure(&config);
        m_surfaceConfigured = true;
    }

    void Module::Present()
    {
        if (ThreadedSubmit())
        {
            // Threaded path: the render thread is running this frame's queue.Submit (the
            // expensive Dawn->D3D12 translation) OFF the JS thread. Surface ops stay on the
            // JS thread (getCurrentTexture already ran here; Present runs here) so the
            // swapchain is never touched cross-thread. Wait for this frame's submit to finish
            // (it must complete before we can present the backbuffer it rendered into), then
            // present on the JS thread. The wait is OUTSIDE the per-frame CPU metric (the host
            // times only the rAF callback = record + submit-enqueue), so the measured JS
            // render-loop CPU reflects only the work that stays on the JS critical path —
            // exactly what the browser's main thread pays (its GPU process owns submit).
            {
                std::unique_lock<std::mutex> lk(m_renderMutex);
                m_drainCv.wait(lk, [this] {
                    return m_pendingSubmits.load() == 0 || m_renderThreadStop;
                });
            }
            {
                ScopedProf _p(P_present);
                if (m_surfaceConfigured && m_surface != nullptr)
                {
                    if (m_readbackFrame >= 0 && ++m_presentCount == m_readbackFrame)
                        ReadbackAndLog("threaded");
                    m_surface.Present();
                }
                m_instance.ProcessEvents();
            }
            g_prof.endFrame();
            return;
        }

        {
            ScopedProf _p(P_present);
            if (m_surfaceConfigured && m_surface != nullptr)
            {
                if (m_readbackFrame >= 0 && ++m_presentCount == m_readbackFrame)
                    ReadbackAndLog("direct");
                m_surface.Present();
            }
            m_instance.ProcessEvents();
        }
        g_prof.endFrame(); // present is once per frame → frame boundary for the profiler
        // One-shot GPU-allocation dump shortly after load (frame 10) so all setup buffers
        // are counted but we don't spam every frame.
        if (g_mem.enabled) { static uint64_t s_memFrame = 0; if (++s_memFrame == 10) g_mem.dump("post-load"); }
    }

    void Module::ReadbackAndLog(const char* label)
    {
        if (!m_surfaceCanCopySrc || m_surfaceDevice == nullptr)
        {
            std::fprintf(stderr, "[readback:%s] surface lacks CopySrc usage — cannot read back\n", label);
            return;
        }
        wgpu::Texture tex = m_lastAcquiredTexture;
        if (tex == nullptr)
        {
            // Fallback: re-acquire (returns the current texture if no fresh acquire happened).
            wgpu::SurfaceTexture st{};
            m_surface.GetCurrentTexture(&st);
            tex = st.texture;
        }
        if (tex == nullptr)
        {
            std::fprintf(stderr, "[readback:%s] no current surface texture\n", label);
            return;
        }
        const uint32_t w = tex.GetWidth();
        const uint32_t h = tex.GetHeight();
        if (w == 0 || h == 0) return;
        const uint32_t bytesPerRow = (w * 4u + 255u) & ~255u; // 256-aligned for CopyTextureToBuffer
        const uint64_t bufSize = static_cast<uint64_t>(bytesPerRow) * h;

        wgpu::BufferDescriptor bd{};
        bd.size = bufSize;
        bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        wgpu::Buffer readback = m_surfaceDevice.CreateBuffer(&bd);

        wgpu::TexelCopyTextureInfo src{};
        src.texture = tex;
        wgpu::TexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = bytesPerRow;
        dst.layout.rowsPerImage = h;
        wgpu::Extent3D extent{w, h, 1};

        wgpu::CommandEncoder enc = m_surfaceDevice.CreateCommandEncoder();
        enc.CopyTextureToBuffer(&src, &dst, &extent);
        wgpu::CommandBuffer cb = enc.Finish();
        m_surfaceDevice.GetQueue().Submit(1, &cb);

        wgpu::Future f = readback.MapAsync(wgpu::MapMode::Read, 0, bufSize,
            wgpu::CallbackMode::WaitAnyOnly,
            [label](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                if (status != wgpu::MapAsyncStatus::Success)
                    std::fprintf(stderr, "[readback:%s] map failed: %.*s\n", label,
                        static_cast<int>(message.length), message.data);
            });
        m_instance.WaitAny(f, UINT64_MAX);

        const uint8_t* data = static_cast<const uint8_t*>(readback.GetConstMappedRange(0, bufSize));
        if (data == nullptr)
        {
            std::fprintf(stderr, "[readback:%s] mapped range null\n", label);
            readback.Unmap();
            return;
        }

        // The surface format may be BGRA8 or RGBA8; report raw byte order channels [0..3].
        auto px = [&](uint32_t x, uint32_t y, int c) -> int {
            return data[static_cast<size_t>(y) * bytesPerRow + static_cast<size_t>(x) * 4 + c];
        };
        const uint32_t cx = w / 2, cy = h / 2;
        // Coarse histogram: count "clear-ish navy" vs non-clear pixels (sample every 4th).
        size_t total = 0, nonClear = 0;
        int b0 = px(0, 0, 0), b1 = px(0, 0, 1), b2 = px(0, 0, 2); // corner = assumed clear color
        for (uint32_t y = 0; y < h; y += 4)
            for (uint32_t x = 0; x < w; x += 4)
            {
                ++total;
                int d = std::abs(px(x, y, 0) - b0) + std::abs(px(x, y, 1) - b1) + std::abs(px(x, y, 2) - b2);
                if (d > 24) ++nonClear;
            }
        const double pctNon = total ? (100.0 * nonClear / total) : 0.0;
        std::fprintf(stderr,
            "[readback:%s] frame=%d %ux%u corner=(%d,%d,%d) center=(%d,%d,%d,%d) non-clear=%.1f%%\n",
            label, m_presentCount, w, h, b0, b1, b2,
            px(cx, cy, 0), px(cx, cy, 1), px(cx, cy, 2), px(cx, cy, 3), pctNon);
        readback.Unmap();
    }

    void Module::EnqueueSubmit(const wgpu::Queue& queue, std::vector<wgpu::CommandBuffer>&& buffers)
    {
        StartRenderThread(); // lazy-start on first submit (device now exists)
        {
            std::lock_guard<std::mutex> lk(m_renderMutex);
            RenderWork w;
            w.queue = queue;
            w.buffers = std::move(buffers);
            m_renderQueue.push_back(std::move(w));
            m_pendingSubmits.fetch_add(1);
        }
        m_renderCv.notify_one();
    }

    void Module::StartRenderThread()
    {
        if (m_renderThread.joinable()) return; // already running
        m_renderThreadStop = false;
        m_renderThread = std::thread([this] { RenderThreadMain(); });
    }

    void Module::StopRenderThread()
    {
        if (!m_renderThread.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(m_renderMutex);
            m_renderThreadStop = true;
        }
        m_renderCv.notify_one();
        m_drainCv.notify_all();
        m_renderThread.join();
    }

    void Module::RenderThreadMain()
    {
        for (;;)
        {
            RenderWork work;
            {
                std::unique_lock<std::mutex> lk(m_renderMutex);
                m_renderCv.wait(lk, [this] { return !m_renderQueue.empty() || m_renderThreadStop; });
                if (m_renderThreadStop && m_renderQueue.empty()) return;
                work = std::move(m_renderQueue.front());
                m_renderQueue.pop_front();
            }

            // The expensive op: Dawn->D3D12 command-list generation happens here, now OFF the
            // JS thread. Device/queue thread-safety is provided by the
            // implicit_device_synchronization toggle enabled at requestDevice. (Surface ops
            // are NOT done here — they stay on the JS thread.)
            {
                ScopedProf _p(P_submit);
                if (!work.buffers.empty())
                    work.queue.Submit(work.buffers.size(), work.buffers.data());
            }
            m_pendingSubmits.fetch_sub(1);
            m_drainCv.notify_one();
        }
    }

    // An offscreen-style canvas: width/height data props + getContext("webgpu"). No
    // clientWidth/clientHeight, so Lite's isDomCanvas() returns false (OffscreenCanvas
    // path) — no DOM layout, size is the backing-store width/height we seed here.
    Napi::Object Module::CreateCanvas(Napi::Env env, uint32_t width, uint32_t height) const
    {
        Napi::Object canvas = Napi::Object::New(env);
        canvas.Set("width", Napi::Number::New(env, width));
        canvas.Set("height", Napi::Number::New(env, height));
        Napi::FunctionReference& ctxCtor = const_cast<Module*>(this)->m_canvasContextCtor;
        canvas.Set("getContext", Napi::Function::New(env,
            [&ctxCtor](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                std::string type = info.Length() >= 1 && info[0].IsString()
                    ? info[0].As<Napi::String>().Utf8Value() : "";
                if (type != "webgpu") return env.Null();
                // Pass the canvas (info.This) so the context reads its width/height.
                return ctxCtor.New({info.This()});
            },
            "getContext"));
        canvas.Set("setAttribute", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value { return info.Env().Undefined(); },
            "setAttribute"));
        // getAttribute/hasAttribute/removeAttribute — some setup paths probe canvas attributes
        // (e.g. engine feature detection). No real attribute store; report "absent".
        canvas.Set("getAttribute", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value { return info.Env().Null(); },
            "getAttribute"));
        canvas.Set("hasAttribute", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value { return Napi::Boolean::New(info.Env(), false); },
            "hasAttribute"));
        canvas.Set("removeAttribute", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value { return info.Env().Undefined(); },
            "removeAttribute"));
        // Pointer/keyboard input is intentionally unsupported in the native host (no DOM
        // event loop). Provide no-op addEventListener/removeEventListener so scene code that
        // calls attachControl(camera, canvas, scene) runs unmodified — it just receives no
        // input events. (Skipping interactive camera control is fine for the benchmark/render
        // corpus.)
        canvas.Set("addEventListener", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value { return info.Env().Undefined(); },
            "addEventListener"));
        canvas.Set("removeEventListener", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value { return info.Env().Undefined(); },
            "removeEventListener"));
        // getBoundingClientRect — some control/setup paths read canvas rect. Report the
        // backing-store size at origin.
        canvas.Set("getBoundingClientRect", Napi::Function::New(env,
            [width, height](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                Napi::Object r = Napi::Object::New(env);
                r.Set("x", Napi::Number::New(env, 0));
                r.Set("y", Napi::Number::New(env, 0));
                r.Set("left", Napi::Number::New(env, 0));
                r.Set("top", Napi::Number::New(env, 0));
                r.Set("right", Napi::Number::New(env, width));
                r.Set("bottom", Napi::Number::New(env, height));
                r.Set("width", Napi::Number::New(env, width));
                r.Set("height", Napi::Number::New(env, height));
                return r;
            },
            "getBoundingClientRect"));
        // dataset / style — plain bags so scene code that stashes diagnostics on the canvas
        // (e.g. canvas.dataset.drawCalls = ..., canvas.style.cursor = ...) doesn't throw.
        canvas.Set("dataset", Napi::Object::New(env));
        canvas.Set("style", Napi::Object::New(env));
        return canvas;
    }

    // navigator.gpu — requestAdapter() (Promise<GPUAdapter>) + getPreferredCanvasFormat().
    void Module::InstallNavigatorGpu(Napi::Env env) const
    {
        Napi::Object gpu = Napi::Object::New(env);

        gpu.Set("requestAdapter", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                const Module* mod = Module::Current();
                auto deferred = Napi::Promise::Deferred::New(env);
                wgpu::Adapter adapter = RequestAdapterSync(mod->Instance());
                if (adapter == nullptr)
                    deferred.Resolve(env.Null()); // spec: resolves null when unavailable
                else
                    deferred.Resolve(mod->WrapAdapter(env, adapter));
                return deferred.Promise();
            },
            "requestAdapter"));

        gpu.Set("getPreferredCanvasFormat", Napi::Function::New(env,
            [](const Napi::CallbackInfo& info) -> Napi::Value {
                // D3D12 swapchains prefer BGRA8Unorm.
                return Napi::String::New(info.Env(), "bgra8unorm");
            },
            "getPreferredCanvasFormat"));

        Napi::Value navVal = env.Global().Get("navigator");
        Napi::Object navigator = navVal.IsObject() ? navVal.As<Napi::Object>() : Napi::Object::New(env);
        navigator.Set("gpu", gpu);
        env.Global().Set("navigator", navigator);
    }

    // globalThis.createImageBitmap(source, options?) — decodes PNG/JPEG/etc. bytes to an
    // ImageBitmap (RGBA8) via stb_image. `source` may be a Blob (native polyfill — read via
    // its async arrayBuffer()), an ArrayBuffer, or a TypedArray. Returns Promise<ImageBitmap>
    // per the web API. Decode options (premultiplyAlpha/colorSpaceConversion "none") match
    // raw RGBA decode, so they need no special handling here.
    void Module::InstallCreateImageBitmap(Napi::Env env) const
    {
        // Decodes a byte range to an ImageBitmap and resolves `deferred`. Shared by the
        // direct (ArrayBuffer/TypedArray) and Blob (post-arrayBuffer) paths.
        auto decodeAndResolve = [](Napi::Env env, const uint8_t* data, size_t size,
            Napi::Promise::Deferred deferred) {
            DecodedImage decoded;
            if (!DecodeImageWIC(data, size, decoded.pixels, decoded.width, decoded.height))
            {
                deferred.Reject(Napi::Error::New(env, "createImageBitmap: failed to decode image").Value());
                return;
            }
            Napi::Object bitmap = Module::Current()->m_imageBitmapCtor.New(
                {Napi::External<DecodedImage>::New(env, &decoded)});
            deferred.Resolve(bitmap);
        };

        Napi::Function fn = Napi::Function::New(env,
            [decodeAndResolve](const Napi::CallbackInfo& info) -> Napi::Value {
                Napi::Env env = info.Env();
                auto deferred = Napi::Promise::Deferred::New(env);
                if (info.Length() < 1 || !info[0].IsObject())
                {
                    deferred.Reject(Napi::Error::New(env, "createImageBitmap: source required").Value());
                    return deferred.Promise();
                }
                Napi::Object source = info[0].As<Napi::Object>();

                // ImageBitmap passthrough.
                if (Module::Current()->AsImageBitmap(source) != nullptr)
                {
                    deferred.Resolve(source);
                    return deferred.Promise();
                }

                // Direct ArrayBuffer / TypedArray.
                const uint8_t* data = nullptr;
                size_t size = 0;
                if (ReadBufferSource(source, data, size))
                {
                    decodeAndResolve(env, data, size, deferred);
                    return deferred.Promise();
                }

                // Blob (native polyfill): read bytes via its async arrayBuffer(), then decode.
                if (source.Get("arrayBuffer").IsFunction())
                {
                    Napi::Function abFn = source.Get("arrayBuffer").As<Napi::Function>();
                    Napi::Value abPromiseVal = abFn.Call(source, {});
                    if (abPromiseVal.IsObject() && abPromiseVal.As<Napi::Object>().Get("then").IsFunction())
                    {
                        Napi::Object abPromise = abPromiseVal.As<Napi::Object>();
                        Napi::Function thenFn = abPromise.Get("then").As<Napi::Function>();
                        // Capture the deferred so the async continuation resolves the same promise.
                        Napi::Function onFulfilled = Napi::Function::New(env,
                            [decodeAndResolve, deferred](const Napi::CallbackInfo& cb) -> Napi::Value {
                                Napi::Env env = cb.Env();
                                const uint8_t* d = nullptr;
                                size_t s = 0;
                                if (cb.Length() >= 1 && ReadBufferSource(cb[0], d, s))
                                {
                                    decodeAndResolve(env, d, s, deferred);
                                }
                                else
                                {
                                    deferred.Reject(Napi::Error::New(env, "createImageBitmap: blob.arrayBuffer() did not yield bytes").Value());
                                }
                                return env.Undefined();
                            });
                        thenFn.Call(abPromise, {onFulfilled});
                        return deferred.Promise();
                    }
                }

                deferred.Reject(Napi::Error::New(env, "createImageBitmap: unsupported source").Value());
                return deferred.Promise();
            },
            "createImageBitmap");
        env.Global().Set("createImageBitmap", fn);
    }

    Napi::Object Module::WrapAdapter(Napi::Env env, const wgpu::Adapter& a) const
    {
        wgpu::Adapter local = a;
        m_adapter = a; // retain for surface-capabilities queries (present-mode selection)
        return m_adapterCtor.New({Napi::External<wgpu::Adapter>::New(env, &local)});
    }

    Napi::Object Module::WrapBuffer(Napi::Env env, const BufferInit& init) const
    {
        BufferInit local = init;
        return m_bufferCtor.New({Napi::External<BufferInit>::New(env, &local)});
    }
    Napi::Object Module::WrapBuffer(Napi::Env env, const wgpu::Buffer& buffer) const
    {
        BufferInit init{};
        init.buffer = buffer;
        init.size = buffer != nullptr ? buffer.GetSize() : 0;
        return WrapBuffer(env, init);
    }
    Napi::Object Module::WrapTexture(Napi::Env env, const wgpu::Texture& t) const
    {
        wgpu::Texture local = t;
        return m_textureCtor.New({Napi::External<wgpu::Texture>::New(env, &local)});
    }
    Napi::Object Module::WrapTextureView(Napi::Env env, const wgpu::TextureView& v) const
    {
        wgpu::TextureView local = v;
        return m_textureViewCtor.New({Napi::External<wgpu::TextureView>::New(env, &local)});
    }
    Napi::Object Module::WrapSampler(Napi::Env env, const wgpu::Sampler& s) const
    {
        wgpu::Sampler local = s;
        return m_samplerCtor.New({Napi::External<wgpu::Sampler>::New(env, &local)});
    }
    Napi::Object Module::WrapShaderModule(Napi::Env env, const wgpu::ShaderModule& m) const
    {
        wgpu::ShaderModule local = m;
        return m_shaderModuleCtor.New({Napi::External<wgpu::ShaderModule>::New(env, &local)});
    }
    Napi::Object Module::WrapBindGroupLayout(Napi::Env env, const wgpu::BindGroupLayout& l) const
    {
        wgpu::BindGroupLayout local = l;
        return m_bindGroupLayoutCtor.New({Napi::External<wgpu::BindGroupLayout>::New(env, &local)});
    }
    Napi::Object Module::WrapPipelineLayout(Napi::Env env, const wgpu::PipelineLayout& l) const
    {
        wgpu::PipelineLayout local = l;
        return m_pipelineLayoutCtor.New({Napi::External<wgpu::PipelineLayout>::New(env, &local)});
    }
    Napi::Object Module::WrapBindGroup(Napi::Env env, const wgpu::BindGroup& g) const
    {
        wgpu::BindGroup local = g;
        return m_bindGroupCtor.New({Napi::External<wgpu::BindGroup>::New(env, &local)});
    }
    Napi::Object Module::WrapRenderPipeline(Napi::Env env, const wgpu::RenderPipeline& p) const
    {
        wgpu::RenderPipeline local = p;
        return m_renderPipelineCtor.New({Napi::External<wgpu::RenderPipeline>::New(env, &local)});
    }
    Napi::Object Module::WrapComputePipeline(Napi::Env env, const wgpu::ComputePipeline& p) const
    {
        wgpu::ComputePipeline local = p;
        return m_computePipelineCtor.New({Napi::External<wgpu::ComputePipeline>::New(env, &local)});
    }
    Napi::Object Module::WrapComputePassEncoder(Napi::Env env, const wgpu::ComputePassEncoder& p) const
    {
        wgpu::ComputePassEncoder local = p;
        return m_computePassEncoderCtor.New({Napi::External<wgpu::ComputePassEncoder>::New(env, &local)});
    }
    Napi::Object Module::WrapRenderBundle(Napi::Env env, const wgpu::RenderBundle& b) const
    {
        wgpu::RenderBundle local = b;
        return m_renderBundleCtor.New({Napi::External<wgpu::RenderBundle>::New(env, &local)});
    }
    Napi::Object Module::WrapRenderBundleEncoder(Napi::Env env, const wgpu::RenderBundleEncoder& e) const
    {
        wgpu::RenderBundleEncoder local = e;
        return m_renderBundleEncoderCtor.New({Napi::External<wgpu::RenderBundleEncoder>::New(env, &local)});
    }
    Napi::Object Module::WrapQueue(Napi::Env env, const wgpu::Queue& q) const
    {
        wgpu::Queue local = q;
        return m_queueCtor.New({Napi::External<wgpu::Queue>::New(env, &local)});
    }
    Napi::Object Module::WrapCommandEncoder(Napi::Env env, const wgpu::CommandEncoder& e) const
    {
        wgpu::CommandEncoder local = e;
        return m_commandEncoderCtor.New({Napi::External<wgpu::CommandEncoder>::New(env, &local)});
    }
    Napi::Object Module::WrapRenderPassEncoder(Napi::Env env, const wgpu::RenderPassEncoder& p) const
    {
        wgpu::RenderPassEncoder local = p;
        return m_renderPassEncoderCtor.New({Napi::External<wgpu::RenderPassEncoder>::New(env, &local)});
    }
    Napi::Object Module::WrapCommandBuffer(Napi::Env env, const wgpu::CommandBuffer& cb) const
    {
        wgpu::CommandBuffer local = cb;
        return m_commandBufferCtor.New({Napi::External<wgpu::CommandBuffer>::New(env, &local)});
    }

    Napi::Object Module::CreateDevice(Napi::Env env, const wgpu::Device& device) const
    {
        wgpu::Device local = device;
        Napi::Object queueObj = WrapQueue(env, local.GetQueue());
        return m_deviceCtor.New({Napi::External<wgpu::Device>::New(env, &local), queueObj});
    }

    Buffer* Module::AsBuffer(const Napi::Object& o) const
    {
        return o.InstanceOf(m_bufferCtor.Value()) ? Napi::ObjectWrap<Buffer>::Unwrap(o) : nullptr;
    }
    Texture* Module::AsTexture(const Napi::Object& o) const
    {
        return o.InstanceOf(m_textureCtor.Value()) ? Napi::ObjectWrap<Texture>::Unwrap(o) : nullptr;
    }
    TextureView* Module::AsTextureView(const Napi::Object& o) const
    {
        return o.InstanceOf(m_textureViewCtor.Value()) ? Napi::ObjectWrap<TextureView>::Unwrap(o) : nullptr;
    }
    Sampler* Module::AsSampler(const Napi::Object& o) const
    {
        return o.InstanceOf(m_samplerCtor.Value()) ? Napi::ObjectWrap<Sampler>::Unwrap(o) : nullptr;
    }
    ShaderModule* Module::AsShaderModule(const Napi::Object& o) const
    {
        return o.InstanceOf(m_shaderModuleCtor.Value()) ? Napi::ObjectWrap<ShaderModule>::Unwrap(o) : nullptr;
    }
    BindGroupLayout* Module::AsBindGroupLayout(const Napi::Object& o) const
    {
        return o.InstanceOf(m_bindGroupLayoutCtor.Value()) ? Napi::ObjectWrap<BindGroupLayout>::Unwrap(o) : nullptr;
    }
    PipelineLayout* Module::AsPipelineLayout(const Napi::Object& o) const
    {
        return o.InstanceOf(m_pipelineLayoutCtor.Value()) ? Napi::ObjectWrap<PipelineLayout>::Unwrap(o) : nullptr;
    }
    BindGroup* Module::AsBindGroup(const Napi::Object& o) const
    {
        return o.InstanceOf(m_bindGroupCtor.Value()) ? Napi::ObjectWrap<BindGroup>::Unwrap(o) : nullptr;
    }
    RenderPipeline* Module::AsRenderPipeline(const Napi::Object& o) const
    {
        return o.InstanceOf(m_renderPipelineCtor.Value()) ? Napi::ObjectWrap<RenderPipeline>::Unwrap(o) : nullptr;
    }
    ComputePipeline* Module::AsComputePipeline(const Napi::Object& o) const
    {
        return o.InstanceOf(m_computePipelineCtor.Value()) ? Napi::ObjectWrap<ComputePipeline>::Unwrap(o) : nullptr;
    }
    RenderBundle* Module::AsRenderBundle(const Napi::Object& o) const
    {
        return o.InstanceOf(m_renderBundleCtor.Value()) ? Napi::ObjectWrap<RenderBundle>::Unwrap(o) : nullptr;
    }
    CommandBuffer* Module::AsCommandBuffer(const Napi::Object& o) const
    {
        return o.InstanceOf(m_commandBufferCtor.Value()) ? Napi::ObjectWrap<CommandBuffer>::Unwrap(o) : nullptr;
    }
    ImageBitmap* Module::AsImageBitmap(const Napi::Object& o) const
    {
        return o.InstanceOf(m_imageBitmapCtor.Value()) ? Napi::ObjectWrap<ImageBitmap>::Unwrap(o) : nullptr;
    }
}
