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

            Napi::Function func = DefineClass(
                env,
                JS_CLASS_NAME,
                {
                    InstanceAccessor("width", &XRCPUDepthInformation::GetWidth, nullptr),
                    InstanceAccessor("height", &XRCPUDepthInformation::GetHeight, nullptr),
                    InstanceAccessor("data", &XRCPUDepthInformation::GetData, nullptr),
                    InstanceAccessor("rawValueToMeters", &XRCPUDepthInformation::GetRawValueToMeters, nullptr),
                    InstanceAccessor("normDepthBufferFromNormView", &XRCPUDepthInformation::GetNormDepthBufferFromNormView, nullptr),
                    InstanceMethod("getDepthInMeters", &XRCPUDepthInformation::GetDepthInMeters),
                });

            env.Global().Set(JS_CLASS_NAME, func);
        }

        static Napi::Object New(const Napi::Env& env)
        {
            return env.Global().Get(JS_CLASS_NAME).As<Napi::Function>().New({});
        }

        XRCPUDepthInformation(const Napi::CallbackInfo& info)
            : Napi::ObjectWrap<XRCPUDepthInformation>{info}
            , m_normTransform{Napi::Persistent(XRRigidTransform::New(info.Env()))}
        {
        }

        void Update(const xr::System::Session::Frame::DepthSensingData& data)
        {
            m_width = data.Width;
            m_height = data.Height;
            m_rawValueToMeters = data.RawValueToMeters;

            // Copy depth buffer to ArrayBuffer (reuse if same size)
            size_t byteLength = data.DepthBuffer.size() * sizeof(uint16_t);
            if (!m_dataBuffer || m_dataBuffer.Value().ByteLength() != byteLength)
            {
                m_dataBuffer = Napi::Persistent(Napi::ArrayBuffer::New(m_normTransform.Env(), byteLength));
            }
            std::memcpy(m_dataBuffer.Value().Data(), data.DepthBuffer.data(), byteLength);

            // Update the normDepthBufferFromNormView matrix directly
            auto matrixArray = m_normTransform.Value().Get("matrix").As<Napi::Float32Array>();
            std::memcpy(matrixArray.Data(), data.NormDepthBufferFromNormView.data(), 16 * sizeof(float));

            // Cache the matrix locally for getDepthInMeters
            m_normMatrix = data.NormDepthBufferFromNormView;
        }

    private:
        uint32_t m_width{0};
        uint32_t m_height{0};
        float m_rawValueToMeters{0.001f};
        Napi::Reference<Napi::ArrayBuffer> m_dataBuffer{};
        Napi::ObjectReference m_normTransform{};
        std::array<float, 16> m_normMatrix{
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };

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
            {
                return info.Env().Null();
            }
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
            if (info.Length() < 2)
            {
                throw Napi::RangeError::New(info.Env(), "getDepthInMeters requires (x, y) arguments");
            }

            float x = info[0].As<Napi::Number>().FloatValue();
            float y = info[1].As<Napi::Number>().FloatValue();

            if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f)
            {
                throw Napi::RangeError::New(info.Env(), "x and y must be in [0, 1] range");
            }

            if (m_width == 0 || m_height == 0 || !m_dataBuffer)
            {
                return Napi::Number::New(info.Env(), 0.0f);
            }

            // Apply normDepthBufferFromNormView transform (column-major)
            float nx = m_normMatrix[0] * x + m_normMatrix[4] * y + m_normMatrix[12];
            float ny = m_normMatrix[1] * x + m_normMatrix[5] * y + m_normMatrix[13];

            // Scale to buffer coordinates, truncate and clamp
            int col = static_cast<int>(nx * m_width);
            int row = static_cast<int>(ny * m_height);
            col = std::max(0, std::min(col, static_cast<int>(m_width) - 1));
            row = std::max(0, std::min(row, static_cast<int>(m_height) - 1));

            size_t index = static_cast<size_t>(row) * m_width + col;
            size_t bufferSize = m_dataBuffer.Value().ByteLength() / sizeof(uint16_t);
            if (index >= bufferSize)
            {
                return Napi::Number::New(info.Env(), 0.0f);
            }

            const uint16_t* depthPtr = static_cast<const uint16_t*>(m_dataBuffer.Value().Data());
            float depthInMeters = depthPtr[index] * m_rawValueToMeters;
            return Napi::Number::New(info.Env(), depthInMeters);
        }
    };
} // Babylon
