#include <napi/env.h>
#include <napi/js_native_api_types.h>
#include "js_native_api_JavaScriptInterface.h"

namespace Napi
{
    template<>
    Napi::Env Attach(facebook::jsi::Runtime* context)
    {
        napi_env env_ptr{new napi_env__{context}};
        return {env_ptr};
    }

    void Detach(Napi::Env env)
    {
        napi_env env_ptr{env};
        delete env_ptr;
    }

    template<> facebook::jsi::Runtime* GetContext(Napi::Env env)
    {
        napi_env env_ptr{env};
        return env_ptr->context;
    }
}

