#include "NativeCapture.h"
#include "AviWriter.h"
#include "Bitmap.hpp"

#include <Babylon/JsRuntime.h>
#include <GraphicsImpl.h>

#include <vector>

namespace Babylon::Plugins::Internal
{
    namespace
    {
//        void write_bitmap(uint32_t width, uint32_t height, const std::vector<uint8_t>&)
//        {
//            typedef struct                       /**** BMP file header structure ****/
//            {
//                unsigned int   bfSize;           /* Size of file */
//                unsigned short bfReserved1;      /* Reserved */
//                unsigned short bfReserved2;      /* ... */
//                unsigned int   bfOffBits;        /* Offset to bitmap data */
//            } BITMAPFILEHEADER;
//
//            typedef struct                       /**** BMP file info structure ****/
//            {
//                unsigned int   biSize;           /* Size of info header */
//                int            biWidth;          /* Width of image */
//                int            biHeight;         /* Height of image */
//                unsigned short biPlanes;         /* Number of color planes */
//                unsigned short biBitCount;       /* Number of bits per pixel */
//                unsigned int   biCompression;    /* Type of compression to use */
//                unsigned int   biSizeImage;      /* Size of image data */
//                int            biXPelsPerMeter;  /* X pixels per meter */
//                int            biYPelsPerMeter;  /* Y pixels per meter */
//                unsigned int   biClrUsed;        /* Number of colors used */
//                unsigned int   biClrImportant;   /* Number of important colors */
//            } BITMAPINFOHEADER;
//
//            BITMAPFILEHEADER bfh;
//            BITMAPINFOHEADER bih;
//
///* Magic number for file. It does not fit in the header structure due to alignment requirements, so put it outside */
//            unsigned short bfType=0x4d42;
//            bfh.bfReserved1 = 0;
//            bfh.bfReserved2 = 0;
//            bfh.bfSize = 2+sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)+width*height*3;
//            bfh.bfOffBits = 0x36;
//
//            bih.biSize = sizeof(BITMAPINFOHEADER);
//            bih.biWidth = width;
//            bih.biHeight = height;
//            bih.biPlanes = 1;
//            bih.biBitCount = 24;
//            bih.biCompression = 0;
//            bih.biSizeImage = 0;
//            bih.biSizeImage = 0;
//            bih.biXPelsPerMeter = 5000;
//            bih.biYPelsPerMeter = 5000;
//            bih.biClrUsed = 0;
//            bih.biClrImportant = 0;
//
//            FILE *file = fopen("/data/data/com.playground/files/temp2.bmp", "wb");
//            if (!file)
//            {
//                printf("Could not write file\n");
//                return;
//            }
//
///*Write headers*/
//            fwrite(&bfType,1,sizeof(bfType),file);
//            fwrite(&bfh, 1, sizeof(bfh), file);
//            fwrite(&bih, 1, sizeof(bih), file);
//
///*Write bitmap*/
//            for (int y = bih.biHeight-1; y>=0; y--) /*Scanline loop backwards*/
//            {
//                for (int x = 0; x < bih.biWidth; x++) /*Column loop forwards*/
//                {
////                    auto index = y * bih.biWidth * 4 + x;
////                    auto b = bytes[index + 0];
////                    auto g = bytes[index + 1];
////                    auto r = bytes[index + 2];
//                    unsigned char r = 255*((float)x/bih.biWidth);
//                    unsigned char g = 255*((float)y/bih.biHeight);
//                    unsigned char b = 0;
//                    fwrite(&b, 1, 1, file);
//                    fwrite(&g, 1, 1, file);
//                    fwrite(&r, 1, 1, file);
//                }
//            }
//            fclose(file);
//        }
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
        {
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
            auto listener = info[0].As<Napi::Function>();
            m_callbacks.push_back(Napi::Persistent(listener));
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
                bmp.save_image("/data/data/com.playground/files/temp.bmp");
            }

            //write_bitmap(data.Width, data.Height, bytes);

            m_runtime.Dispatch([this, data{data}, bytes{std::move(bytes)}](Napi::Env env) mutable {
                data.Data = bytes.data();

                auto external = Napi::External<BgfxCallback::CaptureData>::New(env, &data);
                for (const auto& callback : m_callbacks)
                {
                    callback.Call({external});
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
    };
}

namespace Babylon::Plugins::NativeCapture
{
    void Initialize(Napi::Env env)
    {
        Babylon::Plugins::Internal::NativeCapture::Initialize(env);
    }
}
