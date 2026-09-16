// File: src/Core/Application.cpp
#include "Core/Application.h"
#include "Core/Logger.h"
#include <sstream>
#include <iomanip>
#include <string>
#include <memory>

namespace core
{
    Application::Application()
    {
        WindowDesc desc;
        desc.title = L"Engine - Phase 1 (D3D12 Foundation)";
        desc.width = 1600;
        desc.height = 900;

        m_window = std::make_unique<Window>(desc);

#if defined(_DEBUG)
        constexpr bool kEnableDebugLayer = true;
#else
        constexpr bool kEnableDebugLayer = false;
#endif
        m_renderer = std::make_unique<gfx::D3D12Renderer>(m_window->GetHandle(), desc.width, desc.height, kEnableDebugLayer);

        m_window->SetResizeCallback([this](uint32_t w, uint32_t h)
        {
            m_renderer->Resize(w, h);
        });

        m_window->SetKeyCallback([this](uint32_t vkCode, bool isDown)
        {
            if (!isDown) return;
            if (vkCode == VK_F1)
            {
                m_renderer->SetVSync(!m_renderer->IsVSyncEnabled());
                LOG_INFO(std::string("VSync: ") + (m_renderer->IsVSyncEnabled() ? "ON" : "OFF"));
            }
            else if (vkCode == VK_F11 || vkCode == VK_RETURN) // Alt+Enter arrives as VK_RETURN with alt down
            {
                if (vkCode == VK_RETURN && !(GetKeyState(VK_MENU) & 0x8000)) return;
                m_renderer->ToggleFullscreen();
            }
            else if (vkCode == VK_ESCAPE)
            {
                PostQuitMessage(0);
            }
        });
    }

    Application::~Application() = default;

    int Application::Run()
    {
        LOG_INFO("Entering main loop.");

        while (m_window->PumpMessages())
        {
            const double dt = m_timer.Tick();
            m_totalTime += dt;

            ProcessInput();

            if (!m_window->IsMinimized())
            {
                Update(dt);
                Render();
            }

            UpdateTitleOverlay();
        }

        return 0;
    }

    void Application::ProcessInput()
    {
        // Keyboard toggles (F1/F11/Esc) are handled via the Window
        // key-callback registered in the constructor. Continuous input
        // (movement, mouse-look) needs delta time, so it lives in Update().
    }

    void Application::Update(double deltaSeconds)
    {
        auto& camera = m_renderer->GetCamera();
        const float dt = static_cast<float>(deltaSeconds);

        const bool rmbHeld = Input::IsKeyDown(VK_RBUTTON);
        if (rmbHeld != m_lookModeActive)
        {
            m_lookModeActive = rmbHeld;
            Input::SetCursorVisible(!m_lookModeActive);
            if (m_lookModeActive)
            {
                // Prime the re-centering so the first frame of holding RMB
                // doesn't register a huge jump from wherever the cursor was.
                Input::PollMouseDelta(m_window->GetHandle());
            }
        }

        if (m_lookModeActive)
        {
            DirectX::XMFLOAT2 delta = Input::PollMouseDelta(m_window->GetHandle());
            camera.Rotate(delta.x * m_mouseSensitivity, -delta.y * m_mouseSensitivity);

            float speed = m_moveSpeed * (Input::IsKeyDown(VK_SHIFT) ? 2.5f : 1.0f) * dt;
            if (Input::IsKeyDown('W')) camera.MoveForward(speed);
            if (Input::IsKeyDown('S')) camera.MoveForward(-speed);
            if (Input::IsKeyDown('D')) camera.MoveRight(speed);
            if (Input::IsKeyDown('A')) camera.MoveRight(-speed);
            if (Input::IsKeyDown('E') || Input::IsKeyDown(VK_SPACE)) camera.MoveUp(speed);
            if (Input::IsKeyDown('Q') || Input::IsKeyDown(VK_CONTROL)) camera.MoveUp(-speed);
        }
    }

    void Application::Render()
    {
        m_renderer->RenderFrame(m_totalTime);
    }

    void Application::UpdateTitleOverlay()
    {
        // A lightweight, dependency-free performance overlay: the window
        // title bar. Updated at most twice a second so it never itself
        // becomes measurable overhead, and never uses Sleep() to pace -
        // pacing is entirely the swap chain's job (VSync or tearing).
        ++m_framesSinceTitleUpdate;

        static double lastUpdate = 0.0;
        if (m_totalTime - lastUpdate >= 0.5)
        {
            const auto& stats = m_renderer->GetStats();
            const double avgFrameMs = (m_totalTime - lastUpdate) * 1000.0 / m_framesSinceTitleUpdate;
            const double fps = 1000.0 / (avgFrameMs > 0.0 ? avgFrameMs : 1.0);

            std::wostringstream ss;
            ss << L"Engine  |  FPS: " << std::fixed << std::setprecision(0) << fps
               << L"  |  Frame: " << std::setprecision(2) << avgFrameMs << L" ms"
               << L"  |  GPU: " << std::setprecision(2) << stats.gpuFrameMs << L" ms"
               << L"  |  " << m_window->GetWidth() << L"x" << m_window->GetHeight()
               << L"  |  VSync: " << (m_renderer->IsVSyncEnabled() ? L"On" : L"Off")
               << L"  |  DXR: " << (m_renderer->IsRaytracingEnabled() ? L"On" : L"Off")
               << L"  (F1 VSync, F11/Alt+Enter fullscreen, hold RMB + WASD to fly)";

            m_window->SetTitle(ss.str());

            lastUpdate = m_totalTime;
            m_framesSinceTitleUpdate = 0;
        }
    }
}
