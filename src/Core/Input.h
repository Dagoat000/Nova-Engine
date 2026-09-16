// File: src/Core/Input.h
//
// Polling-based input for continuous per-frame state (movement keys, mouse
// look), separate from Window's event-based key callback (which stays for
// one-shot toggles like F1/F11/Esc). GetAsyncKeyState is the pragmatic
// choice for Phase 2 - raw input only pays for itself once multiple
// keyboards/mice or high-frequency polling >1kHz matter, which is a later
// concern.
#pragma once

#include <Windows.h>
#include <DirectXMath.h>

namespace core
{
    class Input
    {
    public:
        static bool IsKeyDown(int vkCode)
        {
            return (GetAsyncKeyState(vkCode) & 0x8000) != 0;
        }

        // Call once per frame while look mode is active. Re-centers the
        // cursor to the middle of `hwnd` each call so the mouse can never
        // physically leave the screen edge and clamp - the classic FPS
        // mouse-look trick. Returns the pixel delta since the last call.
        static DirectX::XMFLOAT2 PollMouseDelta(HWND hwnd)
        {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            POINT center{ (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
            ClientToScreen(hwnd, &center);

            POINT current{};
            GetCursorPos(&current);

            SetCursorPos(center.x, center.y);

            return DirectX::XMFLOAT2(
                static_cast<float>(current.x - center.x),
                static_cast<float>(current.y - center.y));
        }

        static void SetCursorVisible(bool visible)
        {
            // ShowCursor uses an internal display counter, not a boolean, so
            // it must be called exactly once per state change to stay balanced.
            ShowCursor(visible ? TRUE : FALSE);
        }
    };
}
