// File: src/Core/Window.h
//
// Thin Win32 wrapper. Kept deliberately dumb: it owns the HWND and forwards
// the handful of messages the renderer cares about (resize, destroy,
// keyboard) via std::function callbacks, so Graphics/ never has to know
// about Win32 message pumps and Core/ never has to know about D3D12.
#pragma once

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace core
{
    struct WindowDesc
    {
        std::wstring title = L"Engine";
        uint32_t width = 1920;
        uint32_t height = 1080;
    };

    class Window
    {
    public:
        using ResizeCallback = std::function<void(uint32_t width, uint32_t height)>;
        using KeyCallback = std::function<void(uint32_t vkCode, bool isDown)>;

        explicit Window(const WindowDesc& desc);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        // Pumps all pending Win32 messages. Returns false once WM_QUIT has
        // been posted (e.g. the user closed the window).
        bool PumpMessages();

        void SetTitle(const std::wstring& title);

        HWND GetHandle() const { return m_hwnd; }
        uint32_t GetWidth() const { return m_width; }
        uint32_t GetHeight() const { return m_height; }
        bool IsMinimized() const { return m_minimized; }

        void SetResizeCallback(ResizeCallback cb) { m_onResize = std::move(cb); }
        void SetKeyCallback(KeyCallback cb) { m_onKey = std::move(cb); }

    private:
        static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        HWND m_hwnd = nullptr;
        HINSTANCE m_hinstance = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_minimized = false;
        bool m_running = true;

        ResizeCallback m_onResize;
        KeyCallback m_onKey;

        static constexpr wchar_t kClassName[] = L"EngineWindowClass";
    };
}
