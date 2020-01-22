#include <XR.h>

#include <assert.h>
#include <optional>

namespace xr
{
    // TODO: Move this to a common cpp
    Exception::Exception(const char* message)
        : m_message{ message }
    {}

    const char* Exception::what() const noexcept
    {
        return m_message.c_str();
    }

    class System::Impl
    {
    public:
        Impl(const std::string& applicationName)
        {}

        bool IsInitialized() const
        {
            return false;
        }

        bool TryInitialize()
        {
            return false;
        }
    };

    class System::Session::Impl
    {
    public:
        const System::Impl& HmdImpl;
        std::vector<Frame::View> ActiveFrameViews{{}};
        float DepthNearZ{ DEFAULT_DEPTH_NEAR_Z };
        float DepthFarZ{ DEFAULT_DEPTH_FAR_Z };

        Impl(System::Impl& hmdImpl, void* graphicsContext)
            : HmdImpl{ hmdImpl }
        {
            // graphicsContext is an EGLContext
            // grab and store the ANativeWindow pointer (the drawing surface)
            // Allocate and store the render texture and camera texture
            // Call ArCoreApk_requestInstall, and possibly throw an exception if the user declines ArCore installation
            // Call ArSession_create and ArFrame_create and ArSession_setDisplayGeometry, and probably ArSession_resume
        }

        std::unique_ptr<System::Session::Frame> GetNextFrame(bool& shouldEndSession, bool& shouldRestartSession)
        {
            // Call ArSession_setCameraTextureName and ArSession_update
            return nullptr;
        }

        void RequestEndSession()
        {
            // Probably call ArSession_destroy and ArFrame_destroy
        }

        Size GetWidthAndHeightForViewIndex(size_t viewIndex) const
        {
            // Just return the window/surface width/height
            return {0,0};
        }
    };

    System::Session::Frame::Frame(Session::Impl& sessionImpl)
        : Views{ sessionImpl.ActiveFrameViews }
        , m_sessionImpl{ sessionImpl }
    {
        // Call ArFrame_acquireCamera 
        // Call ArCamera_getPose and ArCamera_getProjectionMatrix and mash state into the single View (from Views)
        // Call ArCamera_release
    }

    System::Session::Frame::~Frame()
    {
        // Probably draw to the xr surface here
    }

    System::System(const char* appName)
        : m_impl{ std::make_unique<System::Impl>(appName) }
    {}

    System::~System() {}

    bool System::IsInitialized() const
    {
        return m_impl->IsInitialized();
    }

    bool System::TryInitialize()
    {
        return m_impl->TryInitialize();
    }

    System::Session::Session(System& headMountedDisplay, void* graphicsDevice)
        : m_impl{ std::make_unique<System::Session::Impl>(*headMountedDisplay.m_impl, graphicsDevice) }
    {}

    System::Session::~Session() {}

    std::unique_ptr<System::Session::Frame> System::Session::GetNextFrame(bool& shouldEndSession, bool& shouldRestartSession)
    {
        return m_impl->GetNextFrame(shouldEndSession, shouldRestartSession);
    }

    void System::Session::RequestEndSession()
    {
        m_impl->RequestEndSession();
    }

    Size System::Session::GetWidthAndHeightForViewIndex(size_t viewIndex) const
    {
        return m_impl->GetWidthAndHeightForViewIndex(viewIndex);
    }

    void System::Session::SetDepthsNearFar(float depthNear, float depthFar)
    {
        m_impl->DepthNearZ = depthNear;
        m_impl->DepthFarZ = depthFar;
    }
}