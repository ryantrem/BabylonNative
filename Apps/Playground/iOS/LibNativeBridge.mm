#include "LibNativeBridge.h"

#import <Babylon/AppRuntime.h>
#import <Babylon/Graphics/Device.h>
#import <Babylon/ScriptLoader.h>
#import <Babylon/Plugins/NativeCamera.h>
#import <Babylon/Plugins/NativeEngine.h>
#import <Babylon/Plugins/NativeInput.h>
#import <Babylon/Plugins/NativeOptimizations.h>
#import <Babylon/Plugins/NativeXr.h>
#import <Babylon/Polyfills/Canvas.h>
#import <Babylon/Polyfills/Console.h>
#import <Babylon/Polyfills/Window.h>
#import <Babylon/Polyfills/XMLHttpRequest.h>
#import <Babylon/ShaderCache.h>
#import <Babylon/DebugTrace.h>
#import <optional>
#import <fstream>

#define ENABLE_PERSISTENT_SHADER_CACHE 1
#ifdef BABYLON_DEBUG_TRACE
#define ON_DEBUG_TRACE(x) x
#else
#define ON_DEBUG_TRACE(x)
#endif

std::optional<Babylon::Graphics::Device> device{};
std::optional<Babylon::Graphics::DeviceUpdate> update{};
std::optional<Babylon::AppRuntime> runtime{};
std::optional<Babylon::Polyfills::Canvas> nativeCanvas{};
std::optional<Babylon::Plugins::NativeXr> nativeXr{};
Babylon::Plugins::NativeInput* nativeInput{};
bool isXrActive{};
float screenScale{1.0f};
std::string appCacheFilePath;

@implementation LibNativeBridge

- (instancetype)init
{
    self = [super init];
    return self;
}

- (void)saveShaderCache
{
#if ENABLE_PERSISTENT_SHADER_CACHE
   // Try to save the shader cache, but only if have some shaders as Uninitialize called first on init
   if (device && Babylon::ShaderCache::Enabled() && !appCacheFilePath.empty())
   {
       std::ofstream fileSerialize(appCacheFilePath, std::ios::binary);
       if (fileSerialize.good())
       {
           ON_DEBUG_TRACE( uint32_t shaderCount = ) Babylon::ShaderCache::Serialize(fileSerialize);
           DEBUG_TRACE("Saved %d shaders to %s", shaderCount, appCacheFilePath.c_str());
       }
       else
       {
           DEBUG_TRACE("Could not save shaders to %s", appCacheFilePath.c_str());
       }
   }
#endif
}

// called from applicationWillTerminate
- (void)terminate
{
    [self saveShaderCache];
    
    // would normally call dealloc here too, but will trigger exceptions currently when called on iOS during termination
}

- (void)dealloc
{
    if (device)
    {
        update->Finish();
        device->FinishRenderingCurrentFrame();
    }
    
    nativeInput = {};
    nativeXr.reset();
    nativeCanvas.reset();
    runtime.reset();
    update.reset();
    device.reset();
}

- (void)init:(MTKView*)view screenScale:(float)inScreenScale width:(int)inWidth height:(int)inHeight xrView:(void*)xrView
{
    screenScale = inScreenScale;
    float width = inWidth;
    float height = inHeight;

    Babylon::DebugTrace::EnableDebugTrace(true);
    Babylon::DebugTrace::SetTraceOutput([](const char* trace) { NSLog(@"%s", trace); });

    Babylon::Graphics::Configuration graphicsConfig{};
    graphicsConfig.Window = view;
    graphicsConfig.Width = static_cast<size_t>(width);
    graphicsConfig.Height = static_cast<size_t>(height);

    device.emplace(graphicsConfig);
    update.emplace(device->GetUpdate("update"));

    Babylon::ShaderCache::Enabled(true);
    
#if ENABLE_PERSISTENT_SHADER_CACHE
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString *cacheDirectory = [paths objectAtIndex:0];
    if (cacheDirectory)
    {
        appCacheFilePath = [cacheDirectory UTF8String];
        appCacheFilePath.append("/");
        appCacheFilePath.append("PlaygroundShaderCache.bin");
    }
    if (!appCacheFilePath.empty())
    {
        std::ifstream file(appCacheFilePath, std::ios::binary);
        if (file.good())
        {
            ON_DEBUG_TRACE( uint32_t deserializedCount = ) Babylon::ShaderCache::Deserialize(file);
            DEBUG_TRACE("Loaded %d shaders from %s", deserializedCount, appCacheFilePath.c_str());
        }
        else
        {
            DEBUG_TRACE("Could not load shaders from %s", appCacheFilePath.c_str());
        }
    }
#endif
    
    device->StartRenderingCurrentFrame();
    update->Start();

    runtime.emplace();

    runtime->Dispatch([xrView](Napi::Env env)
    {
        device->AddToJavaScript(env);

        Babylon::Polyfills::Console::Initialize(env, [](const char* message, auto) {
            NSLog(@"%s", message);
        });

        nativeCanvas.emplace(Babylon::Polyfills::Canvas::Initialize(env));

        Babylon::Polyfills::Window::Initialize(env);

        Babylon::Polyfills::XMLHttpRequest::Initialize(env);

        Babylon::Plugins::NativeCamera::Initialize(env);

        Babylon::Plugins::NativeEngine::Initialize(env);

        Babylon::Plugins::NativeOptimizations::Initialize(env);

        nativeXr.emplace(Babylon::Plugins::NativeXr::Initialize(env));
        nativeXr->UpdateWindow(xrView);
        nativeXr->SetSessionStateChangedCallback([](bool isXrActive){ ::isXrActive = isXrActive; });

        nativeInput = &Babylon::Plugins::NativeInput::CreateForJavaScript(env);
    });

    Babylon::ScriptLoader loader{ *runtime };
    // loader.LoadScript("app:///Scripts/ammo.js");
    // loader.LoadScript("app:///Scripts/recast.js");
    // loader.LoadScript("app:///Scripts/babylon.max.js");
    // loader.LoadScript("app:///Scripts/babylonjs.loaders.js");
    // loader.LoadScript("app:///Scripts/babylonjs.materials.js");
    // loader.LoadScript("app:///Scripts/babylon.gui.js");
    loader.LoadScript("app:///Scripts/experience.js");
}

- (void)resize:(int)inWidth height:(int)inHeight
{
    if (device)
    {
        update->Finish();
        device->FinishRenderingCurrentFrame();

        device->UpdateSize(static_cast<size_t>(inWidth), static_cast<size_t>(inHeight));

        device->StartRenderingCurrentFrame();
        update->Start();
    }
}

- (void)render
{
    if (device)
    {
        update->Finish();
        device->FinishRenderingCurrentFrame();
        device->StartRenderingCurrentFrame();
        update->Start();
    }
}

- (void)setTouchDown:(int)pointerId x:(int)inX y:(int)inY
{
    if (nativeInput != nullptr) {
        nativeInput->TouchDown(pointerId, inX * screenScale, inY * screenScale);
    }
}

- (void)setTouchMove:(int)pointerId x:(int)inX y:(int)inY
{
    if (nativeInput != nullptr) {
        nativeInput->TouchMove(pointerId, inX * screenScale, inY * screenScale);
    }
}

- (void)setTouchUp:(int)pointerId x:(int)inX y:(int)inY
{
    if (nativeInput != nullptr) {
        nativeInput->TouchUp(pointerId, inX * screenScale, inY * screenScale);
    }
}

- (bool)isXRActive
{
    return ::isXrActive;
}

@end
