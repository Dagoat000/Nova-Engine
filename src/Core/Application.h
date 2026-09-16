// File: src/Core/Application.h
//
// Owns the Window + Renderer and drives the ProcessInput / Update / Render
// loop. This is the only place that knows about both Core/ and Graphics/ -
// everything else keeps a one-directional dependency.
#pragma once

#include "Core/Window.h"
#include "Core/Timer.h"
#include "Core/Input.h"
#include "Graphics/D3D12Renderer.h"
#include <memory>

namespace core
{
    class Application
    {
    public:
        Application();
        ~Application();

        int Run();

    private:
        void ProcessInput();
        void Update(double deltaSeconds);
        void Render();
        void UpdateTitleOverlay();

        std::unique_ptr<Window> m_window;
        std::unique_ptr<gfx::D3D12Renderer> m_renderer;
        Timer m_timer;

        double m_totalTime = 0.0;
        uint32_t m_framesSinceTitleUpdate = 0;

        // FPS camera-look state: only active while the right mouse button is
        // held, so the cursor stays free for everything else (matching the
        // convention most DCC / engine viewports use).
        bool m_lookModeActive = false;
        float m_mouseSensitivity = 0.0025f;
        float m_moveSpeed = 3.0f; // meters/second; Shift doubles it (see Update()).
    };
}
