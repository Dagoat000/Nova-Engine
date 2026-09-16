// File: src/Core/Timer.h
//
// QueryPerformanceCounter-based timer. This avoids std::chrono's
// steady_clock overhead concerns on older CRTs and gives us direct access
// to the counter frequency, which we need for stable delta-time and for the
// CPU/GPU timing shown in the performance overlay.
#pragma once

#include <Windows.h>

namespace core
{
    class Timer
    {
    public:
        Timer()
        {
            QueryPerformanceFrequency(&m_frequency);
            QueryPerformanceCounter(&m_startTime);
            m_lastTime = m_startTime;
        }

        // Call once per frame. Returns delta time in seconds.
        double Tick()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            const double delta = static_cast<double>(now.QuadPart - m_lastTime.QuadPart) /
                static_cast<double>(m_frequency.QuadPart);

            m_lastTime = now;

            // Guard against huge spikes (e.g. breakpoint hit, window drag)
            // so Update() never receives a delta that would blow up physics
            // or animation state.
            constexpr double kMaxDelta = 0.25;
            return delta > kMaxDelta ? kMaxDelta : delta;
        }

        double TotalSeconds() const
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return static_cast<double>(now.QuadPart - m_startTime.QuadPart) /
                static_cast<double>(m_frequency.QuadPart);
        }

    private:
        LARGE_INTEGER m_frequency{};
        LARGE_INTEGER m_startTime{};
        LARGE_INTEGER m_lastTime{};
    };
}
