cmake_minimum_required(VERSION 3.12)

set(DYNAMIC_LOADER OFF CACHE BOOL "Build the loader as a .dll library")

set(SOURCES "Include/XR.h")

if (ANDROID)
    set(SOURCES ${SOURCES}
        "Source/ARCore/XR.cpp")
else()
    add_subdirectory(Dependencies/OpenXR-SDK)

    set(SOURCES ${SOURCES}
        "Source/OpenXR/XR.cpp"
        "Source/OpenXR/XrPlatformCommon.h")

    if(WIN32 OR WINDOWS_STORE)
        set(SOURCES ${SOURCES} "Source/OpenXR/Windows/XrPlatform.h")
    else()
        message(FATAL_ERROR "OpenXR support not available for platform ${BABYLON_NATIVE_PLATFORM}")
    endif()
endif()

add_library(xr ${SOURCES})

if (ANDROID)
    
else()
    if(WIN32 OR WINDOWS_STORE)
        target_include_directories(xr PRIVATE "Source/OpenXR/Windows")
    endif()

    target_include_directories(xr PRIVATE "Dependencies/OpenXR-SDK/include")
    target_include_directories(xr PRIVATE "Source/OpenXR")

    target_link_libraries(xr PRIVATE openxr_loader)
endif()

target_include_directories(xr PUBLIC "Include")