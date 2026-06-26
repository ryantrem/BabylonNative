#pragma once

#include <string>

namespace lite
{
    // A small translucent "heads-up display" rendered as a separate layered popup
    // window composited by the DWM ON TOP of the render window. Using a distinct HWND
    // (rather than drawing GDI onto the render window's DC) is what makes the text
    // survive the DXGI flip-model swapchain's Present — the swapchain owns the render
    // window's surface, but the DWM composites this layered window over it every frame.
    //
    // Click-through (WS_EX_TRANSPARENT) and non-activating (WS_EX_NOACTIVATE) so it never
    // steals input from the render window. Owned by the render window so it stacks above
    // it and is destroyed with it. All methods run on the main (window) thread.
    class Overlay
    {
    public:
        ~Overlay();

        // Create the layered popup owned by `ownerHwnd`. Returns false on failure (the
        // app still runs, just without the HUD).
        bool Create(void* ownerHwnd, void* hinstance);

        // Reposition over the owner's client top-left and set/redraw the HUD text.
        // Multi-line text is supported (separate lines with '\n').
        void Update(const std::wstring& text);

        void Destroy();

    private:
        long long HandleMessage(unsigned int msg, unsigned long long wParam, long long lParam);
        static long long __stdcall WndProcThunk(void* hwnd, unsigned int msg,
            unsigned long long wParam, long long lParam);
        void Paint();

        void* m_hwnd = nullptr;
        void* m_ownerHwnd = nullptr;
        void* m_hinstance = nullptr;
        void* m_font = nullptr; // HFONT
        std::wstring m_text;
    };
}
