#include <Babylon/JsRuntime.h>

#include <wasm3_cpp.h>

namespace
{

}

namespace Babylon::Polyfills::Internal
{
    class WebAssembly : public Napi::ObjectWrap<WebAssembly>
    {
    public:
        static constexpr auto JS_CLASS_NAME = "WebAssembly";

        static void Initialize(Napi::Env env)
        {
            Napi::HandleScope scope{env};

            Napi::Function func = WebAssembly::DefineClass(
                env,
                JS_CLASS_NAME,
                {
                    
                });

            env.Global().Set(JS_CLASS_NAME, func);
        }

        WebAssembly(const Napi::CallbackInfo& info)
            : Napi::ObjectWrap<WebAssembly>{info}
            , m_runtime{JsRuntime::GetFromJavaScript(info.Env())}
        {
           
        }

        ~WebAssembly()
        {
            
        }

    private:

        JsRuntime& m_runtime;
    };
}

namespace Babylon::Polyfills::WebAssembly
{
    void Initialize(Napi::Env env)
    {
        Babylon::Polyfills::Internal::WebAssembly::Initialize(env);
    }
}
