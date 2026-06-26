# The Win32 host executable. Links the NativeLite surface (which transitively
# brings the Renderer, WebGPU wrappers, AppRuntime and Dawn) plus the Console
# polyfill used for JS logging.
add_executable(LiteApp
    Main.cpp
    Window.cpp
    Overlay.cpp)

target_link_libraries(LiteApp PRIVATE
    LiteNativeLite
    Console
    Fetch
    Blob
    URL)

target_compile_definitions(LiteApp PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(LiteApp PRIVATE /Zc:__cplusplus /Zc:preprocessor /W3 /MP)
endif()

# Dawn's D3D backend loads the FXC shader compiler (d3dcompiler_47.dll) at device
# creation. By default (LITE_SHIP_FXC unset) the top-level CMake sets
# DAWN_FORCE_SYSTEM_COMPONENT_LOAD=ON, so Dawn loads FXC from the OS (System32) and
# NO DLL needs to ship beside the exe — saving ~4.5 MB. Only when LITE_SHIP_FXC is set
# do we copy the SDK/OS FXC next to the exe (Dawn then uses the exe-dir copy; its bare
# LoadLibraryEx with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR otherwise rejects a path-less
# name with ERROR_INVALID_PARAMETER (87)). Shipping the latest SDK FXC is preferable
# for older Windows where the OS FXC has known heap-corruption bugs.
if(LITE_SHIP_FXC)
    find_file(D3DCOMPILER_47_DLL
        NAMES d3dcompiler_47.dll
        PATHS "$ENV{SystemRoot}/System32"
        NO_DEFAULT_PATH)
    if(D3DCOMPILER_47_DLL)
        add_custom_command(TARGET LiteApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${D3DCOMPILER_47_DLL}" "$<TARGET_FILE_DIR:LiteApp>"
            COMMENT "Copying d3dcompiler_47.dll next to LiteApp.exe")
    else()
        message(WARNING "d3dcompiler_47.dll not found; LiteApp will fail to create a device until it is placed next to the executable.")
    endif()
endif()

# Copy any runtime DLLs LiteApp depends on (e.g. the V8 redist: v8.dll,
# v8_libbase.dll, v8_libplatform.dll, icu DLLs + icudtl.dat, abseil, zlib) next to
# the executable. For the Chakra engine this list is empty (system Chakra.dll), so
# the command is a no-op. Requires CMake >= 3.21 for $<TARGET_RUNTIME_DLLS:>.
add_custom_command(TARGET LiteApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:LiteApp>>,copy,true>
        $<TARGET_RUNTIME_DLLS:LiteApp> "$<TARGET_FILE_DIR:LiteApp>"
    COMMAND_EXPAND_LISTS
    COMMENT "Copying runtime DLLs (V8 redist, if any) next to LiteApp.exe")
