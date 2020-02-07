#include <Babylon/RuntimeAndroid.h>
#include "RuntimeImpl.h"

#include "NativeEngine.h"
#include "NativeXr.h"

JNIEnv* g_env;

namespace Babylon
{
    namespace
    {
        JavaVM* g_javaVM{};
    }

    RuntimeAndroid::RuntimeAndroid(JavaVM* javaVM, ANativeWindow* nativeWindowPtr, float width, float height, ResourceLoadingCallback resourceLoadingCallback)
        : RuntimeAndroid{javaVM, nativeWindowPtr, ".", width, height, std::move(resourceLoadingCallback)} // todo : GetModulePath().parent_path() std::fs experimental not available with ndk
    {
    }

    RuntimeAndroid::RuntimeAndroid(JavaVM* javaVM, ANativeWindow* nativeWindowPtr, const std::string& rootUrl, float width, float height, ResourceLoadingCallback resourceLoadingCallback)
        : Runtime{std::make_unique<RuntimeImpl>(nativeWindowPtr, rootUrl, std::move(resourceLoadingCallback))}
    {
        g_javaVM = javaVM;
        //NativeEngine::InitializeWindow(nativeWindowPtr, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }

    void RuntimeAndroid::UpdateSurface(float width, float height, ANativeWindow* nativeWindowPtr)
    {
        m_impl->UpdateSurface(width, height, nativeWindowPtr);
    }

    void RuntimeImpl::ThreadProcedure()
    {
        auto result = g_javaVM->AttachCurrentThread(&g_env, nullptr);

        this->Dispatch([](Env& env) {
            InitializeNativeXr(env);
        });

        RuntimeImpl::BaseThreadProcedure();

        g_javaVM->DetachCurrentThread();
    }
}
