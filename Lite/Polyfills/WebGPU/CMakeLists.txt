# Dawn exposed to JavaScript as real WebGPU — N-API ObjectWrap classes.
add_library(LiteWebGPU STATIC
    Source/WebGPU.cpp)

target_include_directories(LiteWebGPU PUBLIC Include)

# webgpu_dawn for the Dawn handles, napi for the N-API C++ ObjectWrap classes.
target_link_libraries(LiteWebGPU PUBLIC webgpu_dawn napi)

if(WIN32)
    # WIC image decoder used by createImageBitmap.
    target_link_libraries(LiteWebGPU PRIVATE windowscodecs ole32)
elseif(APPLE)
    # ImageIO image decoder (WebGPU_AppleImage.mm) used by createImageBitmap.
    target_sources(LiteWebGPU PRIVATE Source/WebGPU_AppleImage.mm)
    find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(LiteWebGPU PRIVATE
        ${IMAGEIO_FRAMEWORK} ${COREGRAPHICS_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    # ARC off: the .mm uses manual CFRelease/CGImageRelease (no Obj-C object ownership).
    set_source_files_properties(Source/WebGPU_AppleImage.mm PROPERTIES
        COMPILE_OPTIONS "-fno-objc-arc")
endif()

if(MSVC)
    target_compile_options(LiteWebGPU PRIVATE /Zc:__cplusplus /Zc:preprocessor /W3 /MP)
endif()
