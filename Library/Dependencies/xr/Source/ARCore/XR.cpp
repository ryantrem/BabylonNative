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

    
}