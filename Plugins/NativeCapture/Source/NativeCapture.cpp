#include "NativeCapture.h"

#include <arcana/containers/ticketed_collection.h>

#include <Babylon/JsRuntime.h>
#include <NativeEngine.h>
#include <GraphicsImpl.h>
#include <FrameBuffer.h>

#include <vector>

namespace
{
//    gsl::final_action<std::function<void()>> StartDefaultFrameBufferCapture(std::function<void(uint32_t width, uint32_t height, uint32_t pitch, bgfx::TextureFormat::Enum format, bool yFlip, gsl::span<const uint8_t> data)> callback)
//    {
//        using TicketT = arcana::ticketed_collection<std::function<void(const Babylon::BgfxCallback::CaptureData&)>>::ticket;
////        auto ticket{std::make_unique<TicketT>(m_graphicsImpl.AddCaptureCallback([](auto& data) { }));};
//
//        return gsl::finally<std::function<void()>>([]{
//
//        });
//    }

//    gsl::final_action<std::function<void()>> StartOffScreenFrameBufferCapture(std::function<void(uint32_t width, uint32_t height, uint32_t pitch, bgfx::TextureFormat::Enum format, bool yFlip, gsl::span<const uint8_t> data)> callback)
//    {
//
//    }

    class FrameProvider
    {
    public:
        virtual void Stop() = 0;
        virtual ~FrameProvider()
        {
        }
        //static FrameProvider Create(....)
        
    private:
//            class DefaultBufferFrameProvider : FrameProvider
//            {
//
//            };
//            class DefaultBufferFrameProvider : public FrameProvider
//            {
//            public:
//                DefaultBufferFrameProvider(uint32_t width, uint32_t height, uint32_t pitch, bgfx::TextureFormat::Enum format, bool yFlip, gsl::span<const uint8_t> data)
//                {
//
//                }
//
//                ~DefaultBufferFrameProvider()
//                {
//
//                }
//
//                void Stop()
//                {
//
//                }
//            };
//
//            class OffScreenBufferFrameProvider : public FrameProvider
//            {
//            public:
//                OffScreenBufferFrameProvider(uint32_t width, uint32_t height, uint32_t pitch, bgfx::TextureFormat::Enum format, bool yFlip, gsl::span<const uint8_t> data)
//                {
//
//                }
//
//                ~OffScreenBufferFrameProvider()
//                {
//
//                }
//
//                void Stop()
//                {
//
//                }
//            };
    };

    using FrameCallback = std::function<void(uint32_t width, uint32_t height, uint32_t pitch, bgfx::TextureFormat::Enum format, bool yFlip, gsl::span<const uint8_t> data)>;

    class DefaultBufferFrameProvider : public FrameProvider, std::enable_shared_from_this<DefaultBufferFrameProvider>
    {
    public:
        DefaultBufferFrameProvider(Babylon::Graphics::Impl& graphicsImpl, FrameCallback callback)
//            : m_graphicsImpl{graphicsImpl}
        {
            m_ticket = std::make_unique<Babylon::Graphics::Impl::CaptureCallbackTicketT>(graphicsImpl.AddCaptureCallback([thisRef{shared_from_this()}, callback{std::move(callback)}](auto& data) {
                callback(data.Width, data.Height, data.Pitch, data.Format, data.YFlip, {static_cast<const uint8_t*>(data.Data), static_cast<std::ptrdiff_t>(data.DataSize)});
            }));
        }

//        ~DefaultBufferFrameProvider()
//        {
//
//        }

        void Stop() override
        {
            m_ticket.reset();
        }
        
    private:
//        void CaptureDataReceived(const Babylon::BgfxCallback::CaptureData& data)
//        {
//
//        }

    private:
        //Babylon::Graphics::Impl& m_graphicsImpl;
        std::unique_ptr<Babylon::Graphics::Impl::CaptureCallbackTicketT> m_ticket{};
    };

    class OffScreenBufferFrameProvider : public FrameProvider, std::enable_shared_from_this<OffScreenBufferFrameProvider>
    {
    public:
        OffScreenBufferFrameProvider(Babylon::Graphics::Impl& graphicsImpl, bgfx::FrameBufferHandle frameBufferHandle, FrameCallback callback)
            : m_graphicsImpl{graphicsImpl}
            , m_frameBufferTextureHandle{bgfx::getTexture(frameBufferHandle)}
            , m_textureInfo{graphicsImpl.GetTextureInfo(m_frameBufferTextureHandle)}
            , m_blitTextureHandle{bgfx::createTexture2D(m_textureInfo.Width, m_textureInfo.Height, m_textureInfo.HasMips, m_textureInfo.NumLayers, m_textureInfo.Format, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK)}
        {
            bgfx::TextureInfo textureInfo{};
            bgfx::calcTextureSize(textureInfo, m_textureInfo.Width, m_textureInfo.Height, 1, false, m_textureInfo.HasMips, m_textureInfo.NumLayers, m_textureInfo.Format);
            m_textureBuffer.resize(textureInfo.storageSize);

            ReadTextureAsync(std::move(callback));
        }

        ~OffScreenBufferFrameProvider()
        {
            bgfx::destroy(m_blitTextureHandle);
        }

        void Stop() override
        {
            m_cancellationToken.cancel();
        }

    private:
        arcana::task<void, std::exception_ptr> ReadTextureAsync(FrameCallback callback)
        {
            return arcana::make_task(m_graphicsImpl.AfterRenderScheduler(), m_cancellationToken, [thisRef{shared_from_this()}, callback{std::move(callback)}]{
                bgfx::blit(bgfx::getCaps()->limits.maxViews - 1, thisRef->m_blitTextureHandle, 0, 0, thisRef->m_frameBufferTextureHandle);
                // todo: arcana::when_all
                thisRef->m_graphicsImpl.ReadTextureAsync(thisRef->m_blitTextureHandle, thisRef->m_textureBuffer).then(arcana::inline_scheduler, thisRef->m_cancellationToken, [thisRef, callback]{
                    callback(thisRef->m_textureInfo.Width, thisRef->m_textureInfo.Height, 0 /*todo*/, thisRef->m_textureInfo.Format, true /*todo*/, thisRef->m_textureBuffer);
                });
                return thisRef->ReadTextureAsync(callback);
            });
        }

    private:
        Babylon::Graphics::Impl& m_graphicsImpl;
        bgfx::TextureHandle m_frameBufferTextureHandle{bgfx::kInvalidHandle};
        Babylon::Graphics::Impl::TextureInfo m_textureInfo{};
        bgfx::TextureHandle m_blitTextureHandle{bgfx::kInvalidHandle};
        std::vector<uint8_t> m_textureBuffer{};
        arcana::cancellation_source m_cancellationToken{};
    };

    std::shared_ptr<FrameProvider> CreateDefaultBufferFrameProvider(Babylon::Graphics::Impl& graphicsImpl, FrameCallback callback)
    {
        return std::shared_ptr<FrameProvider>{std::make_shared<DefaultBufferFrameProvider>(graphicsImpl, callback)};
    }

    std::shared_ptr<FrameProvider> CreateOffScreenBufferFrameProvider(Babylon::Graphics::Impl& graphicsImpl, bgfx::FrameBufferHandle frameBufferHandle, FrameCallback callback)
    {
        return std::shared_ptr<FrameProvider>{std::make_shared<OffScreenBufferFrameProvider>(graphicsImpl, frameBufferHandle, callback)};
    }
}

namespace Babylon::Plugins::Internal
{
    class NativeCapture : public Napi::ObjectWrap<NativeCapture>
    {
        using TicketT = arcana::ticketed_collection<std::function<void(const BgfxCallback::CaptureData&)>>::ticket;

    public:
        static constexpr auto JS_CLASS_NAME = "NativeCapture";

        static void Initialize(Napi::Env env)
        {
            Napi::HandleScope scope{env};

            Napi::Function func = NativeCapture::DefineClass(
                env,
                JS_CLASS_NAME,
                {
                    NativeCapture::InstanceMethod("addCallback", &NativeCapture::AddCallback),
                    NativeCapture::InstanceMethod("dispose", &NativeCapture::Dispose),
                });

            env.Global().Set(JS_CLASS_NAME, func);
        }

        NativeCapture(const Napi::CallbackInfo& info)
            : Napi::ObjectWrap<NativeCapture>{info}
            , m_runtime{JsRuntime::GetFromJavaScript(info.Env())}
            , m_graphicsImpl(Graphics::Impl::GetFromJavaScript(info.Env()))
            , m_jsData{Napi::Persistent(Napi::Object::New(info.Env()))}
            , m_cancellationToken{std::make_shared<arcana::cancellation_source>()}
        {
            Napi::Object jsData = m_jsData.Value();
            jsData.Set("data", Napi::ArrayBuffer::New(info.Env(), 0));

            if (info.Length() == 0)
            {
                m_ticket = std::make_unique<TicketT>(m_graphicsImpl.AddCaptureCallback([this](auto& data) { CaptureDataReceived(data); }));
            }
            else
            {
                auto& frameBuffer = *info[0].As<Napi::External<FrameBuffer>>().Data();
                auto textureHandle = bgfx::getTexture(frameBuffer.Handle());
                auto textureInfo = m_graphicsImpl.GetTextureInfo(textureHandle);
                m_textureData = new TextureData();
                m_textureData->Handle = textureHandle;
                m_textureData->Width = frameBuffer.Width();
                m_textureData->Height = frameBuffer.Height();
                //m_textureData->Format = bgfx::TextureFormat::BGRA8;
                m_textureData->Format = textureInfo.Format;
                m_textureData->StorageSize = frameBuffer.Width() * frameBuffer.Height() * 4;

                m_blitTexture = bgfx::createTexture2D(m_textureData->Width, m_textureData->Height, false, 1, m_textureData->Format, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);

//                m_textureData = info[0].As<Napi::External<TextureData>>().Data();
                m_textureBuffer.resize(m_textureData->StorageSize);
                ReadTextureAsync();
            }
        }

        ~NativeCapture()
        {
            if (!m_cancellationToken->cancelled())
            {
                // If m_cancellationToken is not cancelled, this object is being garbage collected without
                // having been disposed, so it must dispose itself.
                Dispose();
            }
        }

    private:
        void AddCallback(const Napi::CallbackInfo& info)
        {
            auto listener = info[0].As<Napi::Function>();
            m_callbacks.push_back(Napi::Persistent(listener));
        }

        arcana::task<void, std::exception_ptr> ReadTextureAsync()
        {
            return arcana::make_task(m_graphicsImpl.AfterRenderScheduler(), *m_cancellationToken, [this, cancellation{m_cancellationToken}]{
                bgfx::blit(bgfx::getCaps()->limits.maxViews - 1, m_blitTexture, 0, 0, m_textureData->Handle);
                m_graphicsImpl.ReadTextureAsync(m_blitTexture, m_textureBuffer).then(arcana::inline_scheduler, *m_cancellationToken, [this]{
                    CaptureDataReceived(m_textureData->Width, m_textureData->Height, m_textureData->StorageSize / m_textureData->Height, m_textureData->Format, true, m_textureBuffer);
                    //return ReadTextureAsync();
                });
                return ReadTextureAsync();
            });
        }

        void CaptureDataReceived(const BgfxCallback::CaptureData& data)
        {
            CaptureDataReceived(data.Width, data.Height, data.Pitch, data.Format, data.YFlip, {static_cast<const uint8_t*>(data.Data), static_cast<std::ptrdiff_t>(data.DataSize)});
        }

        void CaptureDataReceived(uint32_t width, uint32_t height, uint32_t pitch, bgfx::TextureFormat::Enum format, bool yFlip, gsl::span<const uint8_t> data)
        {
            std::vector<uint8_t> bytes{};
            bytes.resize(data.size());
            std::memcpy(bytes.data(), data.data(), data.size());
            m_runtime.Dispatch([this, width, height, pitch, format, yFlip, bytes{std::move(bytes)}](Napi::Env env) mutable {
                Napi::Object jsData = m_jsData.Value();
                jsData.Set("width", static_cast<double>(width));
                jsData.Set("height", static_cast<double>(height));
                jsData.Set("pitch", static_cast<double>(pitch));
                constexpr auto FORMAT_MEMBER_NAME = "format";
                switch (format)
                {
                    case bgfx::TextureFormat::BGRA8:
                        jsData.Set(FORMAT_MEMBER_NAME, "BGRA8");
                        break;
                    default:
                        jsData.Set(FORMAT_MEMBER_NAME, env.Undefined());
                        break;
                }
                jsData.Set("yFlip", yFlip);

                auto jsBytes = jsData.Get("data").As<Napi::ArrayBuffer>();
                if (static_cast<size_t>(bytes.size()) != jsBytes.ByteLength())
                {
                    jsBytes = Napi::ArrayBuffer::New(env, bytes.size());
                    jsData.Set("data", jsBytes);
                }
                std::memcpy(jsBytes.Data(), bytes.data(), bytes.size());
                bytes.clear();

                for (const auto& callback : m_callbacks)
                {
                    callback.Call({jsData});
                }
            });
        }

        void Dispose()
        {
            m_callbacks.clear();
            m_ticket.reset();
            //m_textureBuffer.clear();
            //m_textureBuffer.shrink_to_fit();
            m_cancellationToken->cancel();
        }

        void Dispose(const Napi::CallbackInfo&)
        {
            Dispose();
        }

        JsRuntime& m_runtime;
        Graphics::Impl& m_graphicsImpl;
        std::vector<Napi::FunctionReference> m_callbacks{};
        std::unique_ptr<TicketT> m_ticket{};
        Napi::ObjectReference m_jsData{};
        TextureData* m_textureData{};
        std::vector<uint8_t> m_textureBuffer{};
        std::shared_ptr<arcana::cancellation_source> m_cancellationToken{};
        bgfx::TextureHandle m_blitTexture{};
    };
}

namespace Babylon::Plugins::NativeCapture
{
    void Initialize(Napi::Env env)
    {
        Babylon::Plugins::Internal::NativeCapture::Initialize(env);
    }
}
