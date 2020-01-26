#include <XR.h>

#include <assert.h>
#include <optional>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <android/native_window.h>

extern ANativeWindow* xrWindow;
extern uint32_t xrWindowWidth;
extern uint32_t xrWindowHeight;

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
            return true;
        }

        bool TryInitialize()
        {
            // Perhaps call eglGetCurrentSurface to get the render surface *before* XR render loop starts and changes to rendering to an FBO?
            return true;
        }
    };

    class System::Session::Impl
    {
    public:
        const System::Impl& HmdImpl;
        std::vector<Frame::View> ActiveFrameViews{ {} };
        float DepthNearZ{ DEFAULT_DEPTH_NEAR_Z };
        float DepthFarZ{ DEFAULT_DEPTH_FAR_Z };
        bool sessionEnded{ false };

        Impl(System::Impl& hmdImpl, void* graphicsContext)
            : HmdImpl{ hmdImpl }
        {
            // graphicsContext is an EGLContext
            // grab and store the ANativeWindow pointer (the drawing surface)
            
            // Allocate and store the render texture and camera texture
            GLuint colorTextureId;
            glGenTextures(1, &colorTextureId);
            glBindTexture(GL_TEXTURE_2D, colorTextureId);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1024, 768, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            ActiveFrameViews[0].ColorTexturePointer = reinterpret_cast<void*>(colorTextureId);
            ActiveFrameViews[0].ColorTextureFormat = TextureFormat::RGBA8_SRGB;
            ActiveFrameViews[0].ColorTextureSize = {1024, 768};

            GLuint depthTextureId;
            glGenTextures(1, &depthTextureId);
            glBindTexture(GL_TEXTURE_2D, depthTextureId);
            //glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, 1024, 768, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);
            //glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24_OES, 1024, 768, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8_OES, 1024, 768, 0, GL_DEPTH_STENCIL_OES, GL_UNSIGNED_INT_24_8_OES, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);
            ActiveFrameViews[0].DepthTexturePointer = reinterpret_cast<void*>(depthTextureId);
            ActiveFrameViews[0].DepthTextureFormat = TextureFormat::D24S8;
            ActiveFrameViews[0].DepthTextureSize = {1024, 768};

            // Call ArCoreApk_requestInstall, and possibly throw an exception if the user declines ArCore installation
            // Call ArSession_create and ArFrame_create and ArSession_setDisplayGeometry, and probably ArSession_resume
        }

        std::unique_ptr<System::Session::Frame> GetNextFrame(bool& shouldEndSession, bool& shouldRestartSession)
        {
            shouldEndSession = sessionEnded;
            shouldRestartSession = false;
            // Call ArSession_setCameraTextureName and ArSession_update
            return std::make_unique<Frame>(*this);
        }

        void RequestEndSession()
        {
            // Call ArSession_destroy and ArFrame_destroy, or maybe do this in the destructor
            sessionEnded = true;
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
        Views[0].DepthNearZ = sessionImpl.DepthNearZ;
        Views[0].DepthFarZ = sessionImpl.DepthFarZ;

        Views[0].Position = {0, 0, -10};
        // https://quaternions.online/
        Views[0].Orientation = {0.707f, 0, -.707f, 0};
        Views[0].FieldOfView.AngleLeft = 0.4;
        Views[0].FieldOfView.AngleRight = 0.4;
        Views[0].FieldOfView.AngleUp = 0.4;
        Views[0].FieldOfView.AngleDown = 0.4;

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

    System::Session::Session(System& system, void* graphicsDevice)
        : m_impl{ std::make_unique<System::Session::Impl>(*system.m_impl, graphicsDevice) }
    {}

    System::Session::~Session()
    {
        // Free textures
    }

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