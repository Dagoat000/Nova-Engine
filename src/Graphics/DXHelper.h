// File: src/Graphics/DXHelper.h
//
// Centralized HRESULT checking. Every D3D12/DXGI call in the engine is
// wrapped in DX_CHECK so a failure is never silently swallowed - it throws
// an exception carrying the HRESULT, the failing expression, the file and
// the line, which the top-level message loop / debugger can catch.
#pragma once

#include <Windows.h>
#include <comdef.h>
#include <stdexcept>
#include <string>

namespace gfx
{
    // Thin exception type so callers can catch DirectX failures specifically
    // instead of a generic std::runtime_error.
    class DxException : public std::runtime_error
    {
    public:
        DxException(HRESULT hr, const char* expression, const char* file, int line)
            : std::runtime_error(Format(hr, expression, file, line))
            , m_hr(hr)
        {
        }

        HRESULT GetHResult() const { return m_hr; }

    private:
        static std::string Format(HRESULT hr, const char* expression, const char* file, int line)
        {
            _com_error err(hr);
            char buffer[1024];
            sprintf_s(buffer,
                "DirectX call failed.\n  Expression: %s\n  File:       %s\n  Line:       %d\n  HRESULT:    0x%08X\n  Message:    %ws\n",
                expression, file, line, static_cast<unsigned int>(hr), err.ErrorMessage());
            return std::string(buffer);
        }

        HRESULT m_hr;
    };
}

// Every DX12/DXGI creation and submission call in the engine goes through
// this macro. In Debug builds this is the only safety net we have besides
// the debug layer, so it must never be compiled out.
#define DX_CHECK(expr)                                                        \
    do                                                                        \
    {                                                                         \
        HRESULT _hr_ = (expr);                                                \
        if (FAILED(_hr_))                                                     \
        {                                                                     \
            throw ::gfx::DxException(_hr_, #expr, __FILE__, __LINE__);        \
        }                                                                     \
    } while (0)
