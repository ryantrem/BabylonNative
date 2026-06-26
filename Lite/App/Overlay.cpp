#include "Overlay.h"

#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lite
{
    namespace
    {
        constexpr const wchar_t* kOverlayClass = L"BabylonNativeLiteOverlay";
        constexpr int kPadX = 10;   // text inset from the box edge
        constexpr int kPadY = 8;
        constexpr int kMarginX = 12; // box offset from the client top-left
        constexpr int kMarginY = 12;
        constexpr BYTE kAlpha = 210; // box translucency (0..255)
    }

    Overlay::~Overlay()
    {
        Destroy();
    }

    bool Overlay::Create(void* ownerHwnd, void* hinstance)
    {
        m_ownerHwnd = ownerHwnd;
        m_hinstance = hinstance;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Overlay::WndProcThunk);
        wc.hInstance = static_cast<HINSTANCE>(m_hinstance);
        wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kOverlayClass;
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        ATOM atom = ::RegisterClassExW(&wc);
        if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::fprintf(stderr, "[overlay] RegisterClassExW failed (%lu)\n", ::GetLastError());
            return false;
        }

        // Layered (translucent), transparent (click-through), non-activating, tool window
        // (no taskbar entry), topmost-within-owner. Owned by the render window.
        DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        HWND hwnd = ::CreateWindowExW(
            exStyle, kOverlayClass, L"", WS_POPUP,
            0, 0, 10, 10,
            static_cast<HWND>(m_ownerHwnd), nullptr, static_cast<HINSTANCE>(m_hinstance), this);
        if (hwnd == nullptr)
        {
            std::fprintf(stderr, "[overlay] CreateWindowExW failed (%lu)\n", ::GetLastError());
            return false;
        }
        m_hwnd = hwnd;
        ::SetLayeredWindowAttributes(hwnd, 0, kAlpha, LWA_ALPHA);

        // Fixed-pitch font so the digits don't jitter as the numbers change.
        m_font = ::CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");

        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return true;
    }

    void Overlay::Destroy()
    {
        if (m_font != nullptr)
        {
            ::DeleteObject(static_cast<HFONT>(m_font));
            m_font = nullptr;
        }
        if (m_hwnd != nullptr)
        {
            ::DestroyWindow(static_cast<HWND>(m_hwnd));
            m_hwnd = nullptr;
        }
    }

    void Overlay::Update(const std::wstring& text)
    {
        if (m_hwnd == nullptr || m_ownerHwnd == nullptr)
        {
            return;
        }
        m_text = text;

        HWND hwnd = static_cast<HWND>(m_hwnd);
        HWND owner = static_cast<HWND>(m_ownerHwnd);

        // Measure the text to size the box.
        HDC dc = ::GetDC(hwnd);
        HFONT prev = static_cast<HFONT>(::SelectObject(dc, static_cast<HFONT>(m_font)));
        RECT measure{0, 0, 0, 0};
        ::DrawTextW(dc, m_text.c_str(), -1, &measure, DT_CALCRECT | DT_NOPREFIX | DT_LEFT);
        ::SelectObject(dc, prev);
        ::ReleaseDC(hwnd, dc);

        int boxW = (measure.right - measure.left) + kPadX * 2;
        int boxH = (measure.bottom - measure.top) + kPadY * 2;
        if (boxW < 60) boxW = 60;
        if (boxH < 24) boxH = 24;

        // Position over the owner's client top-left (in screen coordinates).
        POINT origin{kMarginX, kMarginY};
        ::ClientToScreen(owner, &origin);

        ::SetWindowPos(hwnd, HWND_TOPMOST, origin.x, origin.y, boxW, boxH,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        ::UpdateWindow(hwnd);
    }

    void Overlay::Paint()
    {
        HWND hwnd = static_cast<HWND>(m_hwnd);
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);

        RECT rc{};
        ::GetClientRect(hwnd, &rc);

        // Double-buffer to avoid flicker.
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bmp = ::CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
        HBITMAP oldBmp = static_cast<HBITMAP>(::SelectObject(mem, bmp));

        // Dark box background (the whole popup is the box; LWA_ALPHA makes it translucent).
        HBRUSH bg = ::CreateSolidBrush(RGB(18, 18, 22));
        ::FillRect(mem, &rc, bg);
        ::DeleteObject(bg);

        // White text.
        HFONT prevFont = static_cast<HFONT>(::SelectObject(mem, static_cast<HFONT>(m_font)));
        ::SetBkMode(mem, TRANSPARENT);
        ::SetTextColor(mem, RGB(235, 235, 240));
        RECT textRc{rc.left + kPadX, rc.top + kPadY, rc.right - kPadX, rc.bottom - kPadY};
        ::DrawTextW(mem, m_text.c_str(), -1, &textRc, DT_NOPREFIX | DT_LEFT | DT_TOP);
        ::SelectObject(mem, prevFont);

        ::BitBlt(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);

        ::SelectObject(mem, oldBmp);
        ::DeleteObject(bmp);
        ::DeleteDC(mem);
        ::EndPaint(hwnd, &ps);
    }

    long long Overlay::HandleMessage(unsigned int msg, unsigned long long wParam, long long lParam)
    {
        HWND hwnd = static_cast<HWND>(m_hwnd);
        switch (msg)
        {
            case WM_PAINT:
                Paint();
                return 0;
            case WM_NCHITTEST:
                return HTTRANSPARENT; // click-through
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
    }

    long long __stdcall Overlay::WndProcThunk(void* hwnd, unsigned int msg,
        unsigned long long wParam, long long lParam)
    {
        Overlay* self = nullptr;
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Overlay*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(static_cast<HWND>(hwnd), GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self != nullptr)
            {
                self->m_hwnd = hwnd;
            }
        }
        else
        {
            self = reinterpret_cast<Overlay*>(::GetWindowLongPtrW(static_cast<HWND>(hwnd), GWLP_USERDATA));
        }
        if (self != nullptr)
        {
            return self->HandleMessage(msg, wParam, lParam);
        }
        return ::DefWindowProcW(static_cast<HWND>(hwnd), msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
    }
}
