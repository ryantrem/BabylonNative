#pragma once

#include <functional>
#include <string>

namespace lite
{
    // Minimal Win32 window for the Dawn milestone. No input plumbing yet — just
    // creation, a message pump, client-size queries, and a resize notification so
    // the swapchain can be reconfigured.
    class Window
    {
    public:
        Window() = default;
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        bool Create(int width, int height, const std::string& title);
        void Destroy();

        // Pumps all pending messages. Returns false once the window should close.
        bool PumpEvents();

        // Blocks until at least one message arrives, processes it and any others
        // queued, then returns. Returns false once the window should close. Use
        // this on the main thread when rendering happens on another thread, so the
        // main thread sleeps instead of spinning.
        bool PumpEventsBlocking();

        void GetClientSize(int& widthPx, int& heightPx) const;

        void* Hwnd() const { return m_hwnd; }
        void* Hinstance() const { return m_hinstance; }
        bool ShouldClose() const { return m_shouldClose; }

        // Invoked on WM_SIZE with the new client pixel size.
        void SetResizeCallback(std::function<void(int, int)> cb) { m_resizeCb = std::move(cb); }

        // Invoked on WM_TIMER. Use with StartTick() to drive periodic main-thread work
        // (e.g. refreshing the HUD overlay) even while PumpEventsBlocking sleeps.
        void SetTickCallback(std::function<void()> cb) { m_tickCb = std::move(cb); }

        // Start a periodic WM_TIMER every intervalMs milliseconds.
        void StartTick(unsigned int intervalMs);

    private:
        static long long __stdcall WndProcThunk(void* hwnd, unsigned int msg, unsigned long long wParam, long long lParam);
        long long HandleMessage(unsigned int msg, unsigned long long wParam, long long lParam);

        void* m_hwnd = nullptr;
        void* m_hinstance = nullptr;
        bool m_shouldClose = false;
        std::function<void(int, int)> m_resizeCb;
        std::function<void()> m_tickCb;
    };
}
