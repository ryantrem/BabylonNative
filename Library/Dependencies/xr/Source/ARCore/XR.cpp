#include <XR.h>

#include <assert.h>
#include <optional>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>

#include <android/native_window.h>
#include <android/log.h>
#include <arcore_c_api.h>

#include <jni.h>

extern ANativeWindow* xrWindow;
extern uint32_t xrWindowWidth;
extern uint32_t xrWindowHeight;

extern JNIEnv* g_env;
extern jobject g_appContext;

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

    namespace
    {
        const GLfloat kVertices[] = { -1.0f, -1.0f, +1.0f, -1.0f, -1.0f, +1.0f, +1.0f, +1.0f, };
        const GLfloat kUVs[] =      { +0.0f, +0.0f, +1.0f, +0.0f, +0.0f, +1.0f, +1.0f, +1.0f, };

        constexpr char QUAD_VERT_SHADER[] = R"(#version 300 es
            precision highp float;
            out vec2 v_TexCoord;
            void main() {
                float x = -1.0 + float((gl_VertexID & 1) << 2);
                float y = -1.0 + float((gl_VertexID & 2) << 1);
                gl_Position = vec4(x, y, 0., 1.);
                v_TexCoord = vec2(x+1.,y+1.) * 0.5;
            }
        )";

        const char QUAD_FRAG_SHADER[] = R"(#version 300 es
            precision mediump float;
            in vec2 v_TexCoord;
            uniform sampler2D texture_color;
            out vec4 oFragColor;
            void main() {
                vec4 texColor = texture(texture_color, v_TexCoord);
                oFragColor = vec4(1.0,1.0,1.0,1.0) - texColor;
            }
        )";

        GLuint LoadShader(GLenum shader_type, const char* shader_source)
        {
            GLuint shader = glCreateShader(shader_type);
            if (!shader) {
                return shader;
            }

            glShaderSource(shader, 1, &shader_source, nullptr);
            glCompileShader(shader);
            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

            if (!compiled) {
                GLint info_len = 0;

                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
                if (!info_len) {
                    return shader;
                }

                char* buf = reinterpret_cast<char*>(malloc(static_cast<size_t>(info_len)));
                if (!buf) {
                    return shader;
                }

                glGetShaderInfoLog(shader, info_len, nullptr, buf);
                __android_log_write(ANDROID_LOG_ERROR, "BabylonNative", buf);
                // TODO: Throw exception
                free(buf);
                glDeleteShader(shader);
                shader = 0;
            }

            return shader;
        }

        GLuint CreateShaderProgram()
        {
            GLuint vertShader = LoadShader(GL_VERTEX_SHADER, QUAD_VERT_SHADER);
            GLuint fragShader = LoadShader(GL_FRAGMENT_SHADER, QUAD_FRAG_SHADER);

            GLuint program = glCreateProgram();
            if (program)
            {
                glAttachShader(program, vertShader);
                glAttachShader(program, fragShader);

                glLinkProgram(program);
                GLint link_status = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &link_status);

                if (link_status != GL_TRUE)
                {
                    GLint buf_length = 0;
                    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &buf_length);
                    if (buf_length) {
                        char* buf = reinterpret_cast<char*>(malloc(static_cast<size_t>(buf_length)));
                        if (buf) {
                            glGetProgramInfoLog(program, buf_length, nullptr, buf);
                            // TODO: Throw exception
                            free(buf);
                        }
                    }
                    glDeleteProgram(program);
                    program = 0;
                }
            }
            return program;
        }
    }

    class System::Session::Impl
    {
    public:
        const System::Impl& HmdImpl;
        std::vector<Frame::View> ActiveFrameViews{ {} };
        std::vector<Frame::InputSource> InputSources;
        float DepthNearZ{ DEFAULT_DEPTH_NEAR_Z };
        float DepthFarZ{ DEFAULT_DEPTH_FAR_Z };
        bool SessionEnded{ false };

        EGLContext OriginalContext{};
        EGLContext RenderContext{};
        EGLDisplay Display{};
        EGLSurface Surface{};

        GLuint shader_program_;
        GLint attribute_vertices_;
        GLint attribute_uvs_;
        GLint uniform_texture_;
        GLuint vertexArray;
        GLuint vertexBuffer;

        ArSession* session;

        Impl(System::Impl& hmdImpl, void* graphicsContext)
            : HmdImpl{ hmdImpl }
        {
            
            // graphicsContext is an EGLContext
            // grab and store the ANativeWindow pointer (the drawing surface)
            OriginalContext = graphicsContext;
            Display = eglGetCurrentDisplay();
            Surface = eglGetCurrentSurface(EGL_DRAW);
            size_t width, height;
            {
                EGLint _width, _height;
                eglQuerySurface(eglGetDisplay(EGL_DEFAULT_DISPLAY), Surface, EGL_WIDTH, &_width);
                eglQuerySurface(eglGetDisplay(EGL_DEFAULT_DISPLAY), Surface, EGL_HEIGHT, &_height);
                width = static_cast<size_t>(_width);
                height = static_cast<size_t >(_height);
            }

            EGLint attributes[] =
            {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_DEPTH_SIZE, 16,
                EGL_STENCIL_SIZE, 8,
                EGL_NONE
            };
/*
            TODO cg: create a shared context, bind it for resources creation and bind original context before leaving function
            EGLConfig config;
            EGLint numConfig = 0;
            bool success;
            auto success = eglChooseConfig(Display, attributes, &config, 1, &numConfig);
            RenderContext = eglCreateContext(Display, config, OriginalContext, nullptr);
            success = eglMakeCurrent(Display, Surface, Surface, RenderContext);
            success = eglMakeCurrent(Display, Surface, Surface, OriginalContext);
*/
            // Allocate and store the render texture and camera texture
            GLuint colorTextureId;
            glGenTextures(1, &colorTextureId);
            glBindTexture(GL_TEXTURE_2D, colorTextureId);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            ActiveFrameViews[0].ColorTexturePointer = reinterpret_cast<void*>(colorTextureId);
            ActiveFrameViews[0].ColorTextureFormat = TextureFormat::RGBA8_SRGB;
            ActiveFrameViews[0].ColorTextureSize = {width, height};

            GLuint depthTextureId;
            glGenTextures(1, &depthTextureId);
            glBindTexture(GL_TEXTURE_2D, depthTextureId);
            //glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);
            //glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24_OES, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8_OES, width, height, 0, GL_DEPTH_STENCIL_OES, GL_UNSIGNED_INT_24_8_OES, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);
            ActiveFrameViews[0].DepthTexturePointer = reinterpret_cast<void*>(depthTextureId);
            ActiveFrameViews[0].DepthTextureFormat = TextureFormat::D24S8;
            ActiveFrameViews[0].DepthTextureSize = {width, height};

            shader_program_ = CreateShaderProgram();

            // Call ArCoreApk_requestInstall, and possibly throw an exception if the user declines ArCore installation
            // Call ArSession_create and ArFrame_create and ArSession_setDisplayGeometry, and probably ArSession_resume

            // This needs to be called from the UI thread (e.g. an Activity's onResume)
            ArStatus status = ArSession_create(g_env, g_appContext, &session);
            int x = 5;
        }

        std::unique_ptr<System::Session::Frame> GetNextFrame(bool& shouldEndSession, bool& shouldRestartSession)
        {
            shouldEndSession = SessionEnded;
            shouldRestartSession = false;
            // Call ArSession_setCameraTextureName and ArSession_update
            return std::make_unique<Frame>(*this);
        }

        void RequestEndSession()
        {
            // Call ArSession_destroy and ArFrame_destroy, or maybe do this in the destructor
            SessionEnded = true;
        }

        Size GetWidthAndHeightForViewIndex(size_t viewIndex) const
        {
            // Just return the window/surface width/height
            return {1,1};
        }
    };

    System::Session::Frame::Frame(Session::Impl& sessionImpl)
        : Views{ sessionImpl.ActiveFrameViews }
        , m_sessionImpl{ sessionImpl }
        , InputSources{ sessionImpl.InputSources}
    {
        Views[0].DepthNearZ = sessionImpl.DepthNearZ;
        Views[0].DepthFarZ = sessionImpl.DepthFarZ;

        Views[0].Space.Position = {0, 0, -10};
        // https://quaternions.online/
        Views[0].Space.Orientation = {0.707f, 0, -.707f, 0};
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
/*
        TODO cg: bind gl context used for rendering
        auto success = eglMakeCurrent(m_sessionImpl.Display, m_sessionImpl.Surface, m_sessionImpl.Surface, m_sessionImpl.RenderContext);
*/
        GLint drawFboId;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &drawFboId);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, Views[0].ColorTextureSize.Width, Views[0].ColorTextureSize.Height);

        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(m_sessionImpl.shader_program_);
        glDepthMask(GL_FALSE);

        glActiveTexture(GL_TEXTURE0);
        GLuint texId = (GLuint)(size_t)Views[0].ColorTexturePointer;
        glBindTexture(GL_TEXTURE_2D, texId);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 3);

        eglSwapBuffers(m_sessionImpl.Display, m_sessionImpl.Surface);

        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glBindFramebuffer(GL_FRAMEBUFFER, drawFboId);

        /* TODO CG: set back original gl context with eglMakeCurrent*/
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