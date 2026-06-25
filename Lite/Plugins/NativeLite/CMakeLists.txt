# The JavaScript-facing native engine surface (Engine ObjectWrap +
# BabylonNativeLite global). Depends on the renderer, the WebGPU wrappers, and
# AppRuntime (for the JS-thread event loop the render pump re-dispatches onto).
add_library(LiteNativeLite STATIC
    Source/NativeLite.cpp
    Source/Microbench.cpp)

target_include_directories(LiteNativeLite PUBLIC Include)

target_link_libraries(LiteNativeLite PUBLIC
    LiteRenderer
    LiteWebGPU
    AppRuntime
    napi)

# The Microbench probe is V8-specific (raw v8::FunctionTemplate + v8 fast API calls).
# Only enable its body when building against V8; on other engines (Chakra/QuickJS) it
# compiles to a no-op InstallMicrobench so the V8-only headers are never included.
if(NAPI_JAVASCRIPT_ENGINE STREQUAL "V8")
    target_compile_definitions(LiteNativeLite PRIVATE LITE_ENGINE_V8=1)
endif()

if(MSVC)
    target_compile_options(LiteNativeLite PRIVATE /Zc:__cplusplus /Zc:preprocessor /W3 /MP)
endif()

