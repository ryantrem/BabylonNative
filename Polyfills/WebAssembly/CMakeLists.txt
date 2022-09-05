set(SOURCES
    "Include/Babylon/Polyfills/WebAssembly.h"
    "Source/WebAssembly.cpp")

add_library(WebAssembly ${SOURCES})
warnings_as_errors(WebAssembly)

target_include_directories(WebAssembly PUBLIC "Include")

target_link_to_dependencies(WebAssembly
    PUBLIC JsRuntime
    PRIVATE m3
    PRIVATE wasm3_cpp)

set_property(TARGET WebAssembly PROPERTY FOLDER Polyfills)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})
