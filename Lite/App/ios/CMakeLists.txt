# The iOS UIKit host executable (an .app bundle). Links the NativeLite surface (which
# transitively brings the Renderer, WebGPU wrappers, AppRuntime and Dawn/Metal) plus the
# Console/Fetch/Blob/URL polyfills, exactly like the Win32 App — only the host shell differs
# (UIKit + CAMetalLayer instead of a Win32 window + message pump).

# --------------------------------------------------------------------------
# Bundle identity (override on the CMake command line as needed).
# --------------------------------------------------------------------------
set(LITE_IOS_BUNDLE_ID "com.babylonjs.nativelite" CACHE STRING "iOS bundle identifier")
set(MACOSX_BUNDLE_GUI_IDENTIFIER "${LITE_IOS_BUNDLE_ID}")
set(MACOSX_BUNDLE_BUNDLE_NAME "NativeLite")
set(MACOSX_BUNDLE_BUNDLE_VERSION "1")
set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")

# --------------------------------------------------------------------------
# Bundle resources: everything dropped into App/ios/Resources/ is copied into the .app's
# Resources directory (the scene .js + cubes.glb + brdf-lut.png + environment .env). The
# asset-staging step (see Lite/bundler) or the README's manual copy populates this folder.
# --------------------------------------------------------------------------
set(LITE_IOS_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Resources")
file(GLOB LITE_IOS_RESOURCES CONFIGURE_DEPENDS "${LITE_IOS_RESOURCE_DIR}/*")

add_executable(LiteApp MACOSX_BUNDLE
    main.mm
    LiteAppDelegate.mm
    LiteViewController.mm
    LiteMetalView.mm
    LiteHost.mm
    ${LITE_IOS_RESOURCES})

# Mark the dropped files as bundle resources so Xcode copies them into Foo.app/.
set_source_files_properties(${LITE_IOS_RESOURCES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")

target_link_libraries(LiteApp PRIVATE
    LiteNativeLite
    Console
    Fetch
    Blob
    URL)

# iOS system frameworks. JavaScriptCore is linked transitively by the Node-API target
# (find_library JavaScriptCore in JsRuntimeHost), and Metal/QuartzCore by Dawn — but link
# them explicitly here too so the host's own Obj-C++ (CAMetalLayer) resolves.
find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
target_link_libraries(LiteApp PRIVATE
    ${UIKIT_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK} ${METAL_FRAMEWORK})

# Info.plist + bundle settings. Xcode generator fills in code-signing from the command-line
# cache vars (CODE_SIGN_IDENTITY / DEVELOPMENT_TEAM) — see App/ios/README.md.
set_target_properties(LiteApp PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${LITE_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"   # iPhone + iPad
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES")

# The host Obj-C++ uses ARC (UIKit objects); LiteHost.mm too. The WebGPU ImageIO decoder
# opts OUT of ARC in its own target (manual CFRelease), so there's no conflict.
