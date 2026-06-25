# Pure native Dawn renderer — no JavaScript/N-API dependency.
add_library(LiteRenderer STATIC
    Source/Renderer.cpp
    Source/ShaderComposer.cpp)

target_include_directories(LiteRenderer PUBLIC Include)

target_link_libraries(LiteRenderer PUBLIC webgpu_dawn)

if(MSVC)
    target_compile_options(LiteRenderer PRIVATE /Zc:__cplusplus /Zc:preprocessor /W3 /MP)
endif()
