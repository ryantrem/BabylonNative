#include <jni.h>
#include <android/native_window.h> // requires ndk r5 or newer
#include <android/native_window_jni.h> // requires ndk r5 or newer
#include <android/log.h>

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <optional>
#include <fstream>

#include <AndroidExtensions/Globals.h>
#include <Babylon/AppRuntime.h>
#include <Babylon/Graphics/Device.h>
#include <Babylon/ScriptLoader.h>
#include <Babylon/Plugins/NativeEngine.h>
#include <Babylon/Plugins/NativeInput.h>
#include <Babylon/Plugins/NativeXr.h>
#include <Babylon/Plugins/NativeCamera.h>
#include <Babylon/Plugins/NativeOptimizations.h>
#include <Babylon/Polyfills/Console.h>
#include <Babylon/Polyfills/Window.h>
#include <Babylon/Polyfills/XMLHttpRequest.h>
#include <Babylon/Polyfills/Canvas.h>
#include <Babylon/DebugTrace.h>
#include <Babylon/ShaderCache.h>

#define ENABLE_PERSISTENT_SHADER_CACHE 1

#ifdef BABYLON_DEBUG_TRACE
#   define ON_DEBUG_TRACE(x) x
#else
#   define ON_DEBUG_TRACE(x)
#endif

namespace
{
    std::optional<Babylon::Graphics::Device> device{};
    std::optional<Babylon::Graphics::DeviceUpdate> deviceUpdate{};
    std::optional<Babylon::AppRuntime> runtime{};
    std::optional<Babylon::Plugins::NativeXr> nativeXr{};
    Babylon::Plugins::NativeInput* nativeInput{};
    std::optional<Babylon::Polyfills::Canvas> nativeCanvas{};
    std::optional<Babylon::ScriptLoader> scriptLoader{};
    bool isXrActive{};

    const char* shaderCacheFileName = "PlaygroundShaderCache.bin";
    std::string appCachePath;
}

extern "C"
{
    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_initEngine(JNIEnv* env, jclass clazz)
    {
    }

    void SaveShaderCache()
    {
#if ENABLE_PERSISTENT_SHADER_CACHE
        // Try to save the shader cache, but only if have some shaders as Uninitialize called first on init
        if (device && Babylon::ShaderCache::Enabled() && !appCachePath.empty())
        {
            std::ofstream fileSerialize(appCachePath, std::ios::binary);
            if (fileSerialize.good())
            {
                ON_DEBUG_TRACE( uint32_t shaderCount = ) Babylon::ShaderCache::Serialize(fileSerialize);
                DEBUG_TRACE("Saved %d shaders to %s", shaderCount, appCachePath.c_str());
            }
            else
            {
                DEBUG_TRACE("Could not save shaders to %s", appCachePath.c_str());
            }
        }
#endif   
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_finishEngine(JNIEnv* env, jclass clazz)
    {
        SaveShaderCache();

        if (device)
        {
            deviceUpdate->Finish();
            device->FinishRenderingCurrentFrame();
        }

        isXrActive = false;

        scriptLoader.reset();

        nativeInput = {};
        nativeXr.reset();
        scriptLoader.reset();
        runtime.reset();
        device.reset();
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_surfaceCreated(JNIEnv* env, jclass clazz, jobject surface, jobject context, jstring cacheDir)
    {
        if (!runtime)
        {
            JavaVM* javaVM{};
            if (env->GetJavaVM(&javaVM) != JNI_OK)
            {
                throw std::runtime_error("Failed to get Java VM");
            }

            android::global::Initialize(javaVM, context);

            Babylon::DebugTrace::EnableDebugTrace(true);
            Babylon::DebugTrace::SetTraceOutput([](const char* trace) { __android_log_write(ANDROID_LOG_INFO, "BabylonNative",trace);});

            ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
            int32_t width  = ANativeWindow_getWidth(window);
            int32_t height = ANativeWindow_getHeight(window);

            Babylon::Graphics::Configuration graphicsConfig{};
            graphicsConfig.Window = window;
            graphicsConfig.Width = static_cast<size_t>(width);
            graphicsConfig.Height = static_cast<size_t>(height);
            device.emplace(graphicsConfig);
            deviceUpdate.emplace(device->GetUpdate("update"));

            Babylon::ShaderCache::Enabled(true);

        #if ENABLE_PERSISTENT_SHADER_CACHE
            // see if we can prime the ShaderCache from last run
            {
                const char *cacheDirStr = env->GetStringUTFChars(cacheDir, nullptr);

                appCachePath = cacheDirStr;
                appCachePath.append("/");
                appCachePath.append(shaderCacheFileName);
                env->ReleaseStringUTFChars(cacheDir, cacheDirStr);

                std::ifstream file(appCachePath, std::ios::binary);
                if (file.good())
                {
                    ON_DEBUG_TRACE(uint32_t deserializedCount = ) Babylon::ShaderCache::Deserialize(file);
                    DEBUG_TRACE("Loaded %d shaders from %s", deserializedCount, appCachePath.c_str());
                }
                else
                {
                    DEBUG_TRACE("Could not load shaders from %s", appCachePath.c_str());
                }
             }
        #endif

            device->StartRenderingCurrentFrame();
            deviceUpdate->Start();

            runtime.emplace();

            runtime->Dispatch([](Napi::Env env)
            {
                device->AddToJavaScript(env);

                Babylon::Polyfills::Console::Initialize(env, [](const char* message, Babylon::Polyfills::Console::LogLevel level)
                {
                    switch (level)
                    {
                    case Babylon::Polyfills::Console::LogLevel::Log:
                        __android_log_write(ANDROID_LOG_INFO, "BabylonNative", message);
                        break;
                    case Babylon::Polyfills::Console::LogLevel::Warn:
                        __android_log_write(ANDROID_LOG_WARN, "BabylonNative", message);
                        break;
                    case Babylon::Polyfills::Console::LogLevel::Error:
                        __android_log_write(ANDROID_LOG_ERROR, "BabylonNative", message);
                        break;
                    }
                });

                Babylon::Plugins::NativeEngine::Initialize(env);
                Babylon::Plugins::NativeOptimizations::Initialize(env);

                nativeXr.emplace(Babylon::Plugins::NativeXr::Initialize(env));
                nativeXr->SetSessionStateChangedCallback([](bool isXrActive){ ::isXrActive = isXrActive; });
                
                nativeInput = &Babylon::Plugins::NativeInput::CreateForJavaScript(env);

                Babylon::Plugins::NativeCamera::Initialize(env);
                Babylon::Polyfills::Window::Initialize(env);

                Babylon::Polyfills::XMLHttpRequest::Initialize(env);
                nativeCanvas.emplace(Babylon::Polyfills::Canvas::Initialize(env));
            });

            scriptLoader.emplace(*runtime);
//            scriptLoader->LoadScript("app:///Scripts/ammo.js");
//            scriptLoader->LoadScript("app:///Scripts/recast.js");
//            scriptLoader->LoadScript("app:///Scripts/babylon.max.js");
//            scriptLoader->LoadScript("app:///Scripts/babylonjs.loaders.js");
//            scriptLoader->LoadScript("app:///Scripts/babylonjs.materials.js");
//            scriptLoader->LoadScript("app:///Scripts/babylon.gui.js");
        }
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_surfaceChanged(JNIEnv* env, jclass clazz, jint width, jint height, jobject surface)
    {
        if (runtime)
        {
            ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
            runtime->Dispatch([window, width = static_cast<size_t>(width), height = static_cast<size_t>(height)](auto) {
                device->UpdateWindow(window);
                device->UpdateSize(width, height);
            });
        }
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_setCurrentActivity(JNIEnv* env, jclass clazz, jobject currentActivity)
    {
        android::global::SetCurrentActivity(currentActivity);
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_activityOnPause(JNIEnv* env, jclass clazz)
    {
        SaveShaderCache();
        
        android::global::Pause();
        if (runtime)
        {
            runtime->Suspend();
        }
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_activityOnResume(JNIEnv* env, jclass clazz)
    {
        if (runtime)
        {
            runtime->Resume();
        }
        android::global::Resume();
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_activityOnRequestPermissionsResult(JNIEnv* env, jclass clazz, jint requestCode, jobjectArray permissions, jintArray grantResults)
    {
        std::vector<std::string> nativePermissions{};
        for (int i = 0; i < env->GetArrayLength(permissions); i++)
        {
            jstring permission = (jstring)env->GetObjectArrayElement(permissions, i);
            const char* utfString{env->GetStringUTFChars(permission, nullptr)};
            nativePermissions.push_back(utfString);
            env->ReleaseStringUTFChars(permission, utfString);
        }

        auto grantResultElements{env->GetIntArrayElements(grantResults, nullptr)};
        auto grantResultElementCount = env->GetArrayLength(grantResults);
        std::vector<int32_t> nativeGrantResults{grantResultElements, grantResultElements + grantResultElementCount};
        env->ReleaseIntArrayElements(grantResults, grantResultElements, 0);

        android::global::RequestPermissionsResult(requestCode, nativePermissions, nativeGrantResults);
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_loadScript(JNIEnv* env, jclass clazz, jstring path)
    {
        if (scriptLoader)
        {
            scriptLoader->LoadScript(env->GetStringUTFChars(path, nullptr));
        }
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_eval(JNIEnv* env, jclass clazz, jstring source, jstring sourceURL)
    {
        if (runtime)
        {
            std::string url = env->GetStringUTFChars(sourceURL, nullptr);
            std::string src = env->GetStringUTFChars(source, nullptr);
            scriptLoader->Eval(std::move(src), std::move(url));
        }
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_setTouchInfo(JNIEnv* env, jclass clazz, jint pointerId, jfloat x, jfloat y, jboolean buttonAction, jint buttonValue)
    {
        if (nativeInput != nullptr)
        {
            if (buttonAction)
            {
                if (buttonValue == 1)
                    nativeInput->TouchDown(pointerId, x, y);
                else
                    nativeInput->TouchUp(pointerId, x, y);
            }
            else {
                nativeInput->TouchMove(pointerId, x, y);
            }
        }
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_renderFrame(JNIEnv* env, jclass clazz)
    {
        if (device)
        {
            deviceUpdate->Finish();
            device->FinishRenderingCurrentFrame();
            device->StartRenderingCurrentFrame();
            deviceUpdate->Start();
        }
    }

    JNIEXPORT void JNICALL
    Java_com_library_babylonnative_Wrapper_xrSurfaceChanged(JNIEnv* env, jclass clazz, jobject surface)
    {
        if (nativeXr)
        {
            ANativeWindow* window{};
            if (surface)
            {
                window = ANativeWindow_fromSurface(env, surface);
            }
            nativeXr->UpdateWindow(window);
        }
    }

    JNIEXPORT jboolean JNICALL
    Java_com_library_babylonnative_Wrapper_isXRActive(JNIEnv* env, jclass clazz)
    {
        return isXrActive;
    }
}
