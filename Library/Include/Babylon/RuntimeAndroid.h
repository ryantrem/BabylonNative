#pragma once

#include "Runtime.h"

#include <jni.h>

class ANativeWindow;

namespace Babylon
{
    class RuntimeAndroid final : public Runtime
    {
    public:

        explicit RuntimeAndroid(JavaVM* javaVM, ANativeWindow* nativeWindowPtr, float width, float height, ResourceLoadingCallback resourceLoadingCallback);
        explicit RuntimeAndroid(JavaVM* javaVM, ANativeWindow* nativeWindowPtr, const std::string& rootUrl, float width, float height, ResourceLoadingCallback resourceLoadingCallback);
        RuntimeAndroid(const RuntimeAndroid&) = delete;
        void UpdateSurface(float width, float height, ANativeWindow* nativeWindowPtr);
    };
}
