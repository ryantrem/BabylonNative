# Dawn exposed to JavaScript as real WebGPU — N-API ObjectWrap classes.
add_library(LiteWebGPU STATIC
    Source/WebGPU.cpp)

target_include_directories(LiteWebGPU PUBLIC Include)

# webgpu_dawn for the Dawn handles, napi for the N-API C++ ObjectWrap classes.
# windowscodecs + ole32 for the WIC image decoder used by createImageBitmap.
target_link_libraries(LiteWebGPU PUBLIC webgpu_dawn napi)
target_link_libraries(LiteWebGPU PRIVATE windowscodecs ole32)

if(MSVC)
    target_compile_options(LiteWebGPU PRIVATE /Zc:__cplusplus /Zc:preprocessor /W3 /MP)
endif()
