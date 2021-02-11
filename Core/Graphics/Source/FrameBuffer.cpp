#include "FrameBuffer.h"
#include "FrameBufferManager.h"

namespace Babylon
{
    FrameBuffer::FrameBuffer(FrameBufferManager& manager, bgfx::FrameBufferHandle handle, uint16_t width, uint16_t height, bool backBuffer)
        : m_manager{manager}
        , m_handle{handle}
        , m_width{width}
        , m_height{height}
        , m_backBuffer{backBuffer}
        , m_viewId{}
        , m_viewPort{}
        , m_requestedViewPort{}
    {
    }

    FrameBuffer::~FrameBuffer()
    {
        if (bgfx::isValid(m_handle))
        {
            bgfx::destroy(m_handle);
        }
    }

    uint16_t FrameBuffer::Width() const
    {
        return (m_width == 0 ? bgfx::getStats()->width : m_width);
    }

    uint16_t FrameBuffer::Height() const
    {
        return (m_height == 0 ? bgfx::getStats()->height : m_height);
    }

    bool FrameBuffer::BackBuffer() const
    {
        return m_backBuffer;
    }

    void FrameBuffer::Bind()
    {
        m_manager.BindFrameBuffer(this);
    }

    void FrameBuffer::Clear(bgfx::Encoder* encoder, uint16_t flags, uint32_t rgba, float depth, uint8_t stencil)
    {
        NewView(encoder, {});

        bgfx::setViewClear(m_viewId.value(), flags, rgba, depth, stencil);
    }

    void FrameBuffer::SetViewPort(float x, float y, float width, float height)
    {
        m_requestedViewPort = {x, y, width, height};
    }

    void FrameBuffer::Submit(bgfx::Encoder* encoder, bgfx::ProgramHandle programHandle, uint8_t flags)
    {
        if (m_requestedViewPort.has_value() && !m_requestedViewPort->Equals(m_viewPort))
        {
            NewView(encoder, m_requestedViewPort.value());
        }
        else if (!m_viewId.has_value())
        {
            NewView(encoder, {});
        }

        encoder->submit(m_viewId.value(), programHandle, 0, flags);
    }

    void FrameBuffer::Reset()
    {
        m_viewId.reset();
        m_viewPort = {};
        m_requestedViewPort.reset();
    }

    bool FrameBuffer::ViewPort::Equals(const ViewPort& other) const
    {
        return
            std::abs(X - other.X) < std::numeric_limits<float>::epsilon() &&
            std::abs(Y - other.Y) < std::numeric_limits<float>::epsilon() &&
            std::abs(Width - other.Width) < std::numeric_limits<float>::epsilon() &&
            std::abs(Height - other.Height) < std::numeric_limits<float>::epsilon();
    }

    void FrameBuffer::NewView(bgfx::Encoder* encoder, const ViewPort& viewPort)
    {
        m_viewId = m_manager.NewViewId();
        m_viewPort = viewPort;

        bgfx::setViewMode(m_viewId.value(), bgfx::ViewMode::Sequential);
        bgfx::setViewClear(m_viewId.value(), BGFX_CLEAR_NONE);
        bgfx::setViewFrameBuffer(m_viewId.value(), m_handle);
        bgfx::setViewRect(m_viewId.value(),
            static_cast<uint16_t>(m_viewPort.X * Width()),
            static_cast<uint16_t>(m_viewPort.Y * Height()),
            static_cast<uint16_t>(m_viewPort.Width * Width()),
            static_cast<uint16_t>(m_viewPort.Height * Height()));

        // This dummy draw call is here to make sure that the view is cleared
        // if no other draw calls are submitted to the view.
        encoder->touch(m_viewId.value());
    }
}
