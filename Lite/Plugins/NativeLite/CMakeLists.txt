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

if(MSVC)
    target_compile_options(LiteNativeLite PRIVATE /Zc:__cplusplus /Zc:preprocessor /W3 /MP)
endif()

