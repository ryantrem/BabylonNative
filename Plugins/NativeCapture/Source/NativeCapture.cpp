#include "NativeCapture.h"
#include "Bitmap.hpp"

#include <Babylon/JsRuntime.h>
#include <GraphicsImpl.h>

#include <vector>

namespace Babylon::Plugins::Internal
{
    namespace
    {
        std::string g_saveDir{};
    }

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
            , m_ticket{std::make_unique<TicketT>(m_graphicsImpl.AddCaptureCallback([this](auto& data) { CaptureDataReceived(data); }))}
            , m_jsData{Napi::Persistent(Napi::Object::New(info.Env()))}
        {
            Napi::Object jsData = m_jsData.Value();
            jsData.Set("data", Napi::ArrayBuffer::New(info.Env(), 0));
        }

        ~NativeCapture()
        {
            if (m_ticket != nullptr)
            {
                // If m_ticket is still active, this object is being garbage collected without
                // having been disposed, so it must dispose itself.
                Dispose();
            }
        }

    private:
        void AddCallback(const Napi::CallbackInfo& info)
        {
            auto saveDir = info[0].As<Napi::String>();
            g_saveDir = saveDir.Utf8Value().append("/temp.bmp");
//            auto listener = info[0].As<Napi::Function>();
//            m_callbacks.push_back(Napi::Persistent(listener));
        }

        void CaptureDataReceived(const BgfxCallback::CaptureData& data)
        {
            std::vector<uint8_t> bytes{};
            bytes.resize(data.DataSize);
            std::memcpy(bytes.data(), data.Data, data.DataSize);

            {
                bitmap_image bmp{data.Width, data.Height};
                bmp.clear();
                for (uint32_t y = 0; y < data.Height; y++)
                {
                    for (uint32_t x = 0; x < data.Width; x++)
                    {
                        auto index = y * data.Width * 4 + x * 4;
                        bmp.set_pixel(x, y, bytes[index + 2], bytes[index + 1], bytes[index]);
                    }
                }
                // View this on the dev machine by doing the following in Android Studio:
                // 1. Select: View -> Tool Windows -> Device File Explorer
                // 2. Double click: data -> data -> com.playground -> files -> temp.bmp
                //bmp.save_image("/data/data/com.playground/files/temp.bmp");

                bmp.save_image(g_saveDir.c_str());
                //bmp.save_image("Library/temp.bmp");
                //bmp.save_image("/var/mobile/Containers/Data/Application/8FA3EDB0-67E6-4AD3-8295-EBAE1975EF07/Documents/temp.bmp");
            }

            //write_bitmap(data.Width, data.Height, bytes);

            m_runtime.Dispatch([this, data{data}, bytes{std::move(bytes)}](Napi::Env env) mutable {
                data.Data = bytes.data();

                Napi::Object jsData = m_jsData.Value();
                jsData.Set("width", static_cast<double>(data.Width));
                jsData.Set("height", static_cast<double>(data.Height));
                jsData.Set("pitch", static_cast<double>(data.Pitch));
                constexpr auto FORMAT_MEMBER_NAME = "format";
                switch (data.Format)
                {
                    case bgfx::TextureFormat::BGRA8:
                        jsData.Set(FORMAT_MEMBER_NAME, "BGRA8");
                        break;
                    default:
                        jsData.Set(FORMAT_MEMBER_NAME, env.Undefined());
                        break;
                }
                jsData.Set("yFlip", data.YFlip);

                auto jsBytes = jsData.Get("data").As<Napi::ArrayBuffer>();
                if (data.DataSize != jsBytes.ByteLength())
                {
                    jsBytes = Napi::ArrayBuffer::New(env, data.DataSize);
                    jsData.Set("data", jsBytes);
                }
                std::memcpy(jsBytes.Data(), bytes.data(), data.DataSize);
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
    };
}

namespace Babylon::Plugins::NativeCapture
{
    void Initialize(Napi::Env env)
    {
        Babylon::Plugins::Internal::NativeCapture::Initialize(env);
    }
}
