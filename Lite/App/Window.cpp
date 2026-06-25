#include "Window.h"

#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

namespace lite
{
    namespace
    {
        constexpr const wchar_t* kClassName = L"BabylonNativeLiteWindow";
    }

    Window::~Window()
    {
        Destroy();
    }

    bool Window::Create(int width, int height, const std::string& title)
    {
        m_hinstance = ::GetModuleHandleW(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::WndProcThunk);
        wc.hInstance = static_cast<HINSTANCE>(m_hinstance);
        wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));

        ATOM atom = ::RegisterClassExW(&wc);
        if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::fprintf(stderr, "[window] RegisterClassExW failed (%lu)\n", ::GetLastError());
            return false;
        }

        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), nullptr, 0);
        std::wstring wtitle(static_cast<size_t>(wlen), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), wtitle.data(), wlen);

        // Size the window so the client area is exactly width x height.
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT rc{0, 0, width, height};
        ::AdjustWindowRect(&rc, style, FALSE);

        HWND hwnd = ::CreateWindowExW(
            0, kClassName, wtitle.c_str(), style,
            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, static_cast<HINSTANCE>(m_hinstance), this);
        if (hwnd == nullptr)
        {
            std::fprintf(stderr, "[window] CreateWindowExW failed (%lu)\n", ::GetLastError());
            return false;
        }

        m_hwnd = hwnd;
        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);
        return true;
    }

    void Window::Destroy()
    {
        if (m_hwnd != nullptr)
        {
            ::DestroyWindow(static_cast<HWND>(m_hwnd));
            m_hwnd = nullptr;
        }
    }

    bool Window::PumpEvents()
    {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_shouldClose = true;
                return false;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        return !m_shouldClose;
    }

    bool Window::PumpEventsBlocking()
    {
        MSG msg;
        // Block until at least one message is available, then process it.
        BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1)
        {
            m_shouldClose = true;
            return false;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);

        // Drain any remaining queued messages without blocking.
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_shouldClose = true;
                return false;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        return !m_shouldClose;
    }

    void Window::GetClientSize(int& widthPx, int& heightPx) const
    {
        widthPx = 0;
        heightPx = 0;
        if (m_hwnd == nullptr)
        {
            return;
        }
        RECT rc;
        if (::GetClientRect(static_cast<HWND>(m_hwnd), &rc))
        {
            widthPx = rc.right - rc.left;
            heightPx = rc.bottom - rc.top;
        }
    }

    long long __stdcall Window::WndProcThunk(void* hwnd, unsigned int msg, unsigned long long wParam, long long lParam)
    {
        Window* self = nullptr;
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Window*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(static_cast<HWND>(hwnd), GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self != nullptr)
            {
                self->m_hwnd = hwnd;
            }
        }
        else
        {
            self = reinterpret_cast<Window*>(::GetWindowLongPtrW(static_cast<HWND>(hwnd), GWLP_USERDATA));
        }

        if (self != nullptr)
        {
            return self->HandleMessage(msg, wParam, lParam);
        }
        return ::DefWindowProcW(static_cast<HWND>(hwnd), msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
    }

    long long Window::HandleMessage(unsigned int msg, unsigned long long wParam, long long lParam)
    {
        HWND hwnd = static_cast<HWND>(m_hwnd);
        switch (msg)
        {
            case WM_CLOSE:
                m_shouldClose = true;
                return 0;
            case WM_DESTROY:
                ::PostQuitMessage(0);
                return 0;
            case WM_SIZE:
                if (m_resizeCb)
                {
                    m_resizeCb(LOWORD(lParam), HIWORD(lParam));
                }
                return 0;
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE)
                {
                    m_shouldClose = true;
                }
                return 0;
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
    }
}
