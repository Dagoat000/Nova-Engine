// File: src/Core/Window.cpp
#include "Core/Window.h"
#include "Core/Logger.h"
#include <windowsx.h>
#include <stdexcept>
#include <string>

namespace core
{
    Window::Window(const WindowDesc& desc)
        : m_hinstance(GetModuleHandle(nullptr))
        , m_width(desc.width)
        , m_height(desc.height)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Window::StaticWndProc;
        wc.hInstance = m_hinstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        RECT rect{ 0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height) };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        m_hwnd = CreateWindowExW(
            0, kClassName, desc.title.c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, m_hinstance, this);

        if (!m_hwnd)
        {
            LOG_ERROR("Failed to create Win32 window.");
            throw std::runtime_error("CreateWindowExW failed");
        }

        // Stash "this" so StaticWndProc can retrieve it before WM_NCCREATE.
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
    }

    Window::~Window()
    {
        if (m_hwnd)
        {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        UnregisterClassW(kClassName, m_hinstance);
    }

    void Window::SetTitle(const std::wstring& title)
    {
        SetWindowTextW(m_hwnd, title.c_str());
    }

    bool Window::PumpMessages()
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_running = false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return m_running;
    }

    LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        Window* self = nullptr;

        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<Window*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self)
        {
            return self->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT Window::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE:
        {
            m_minimized = (wParam == SIZE_MINIMIZED);
            const uint32_t width = LOWORD(lParam);
            const uint32_t height = HIWORD(lParam);
            if (!m_minimized && (width != m_width || height != m_height))
            {
                m_width = width;
                m_height = height;
                if (m_onResize) m_onResize(width, height);
            }
            return 0;
        }

        // Block the default system key beep and Alt+Enter's automatic
        // fullscreen switch - the renderer owns fullscreen transitions so
        // it can flush the GPU first.
        case WM_SYSCHAR:
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (m_onKey) m_onKey(static_cast<uint32_t>(wParam), true);
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (m_onKey) m_onKey(static_cast<uint32_t>(wParam), false);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
