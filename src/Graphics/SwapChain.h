// File: src/Graphics/SwapChain.h
//
// Wraps the DXGI swap chain and the RTVs for its back buffers. Uses
// FLIP_DISCARD with 3 buffers, which is the model modern DWM composition
// and variable-refresh (tearing) presentation are built around - BitBlt-style
// swap effects are legacy and cost an extra copy.
//
// Also owns a per-swap-chain depth buffer, historically used by the
// forward rendering pass. The deferred pipeline (Graphics/GBuffer) has its
// own dedicated, SRV-readable depth buffer instead, so this one currently
// goes unused by D3D12Renderer - kept rather than removed because a future
// forward pass (e.g. transparent objects drawn after the deferred opaque
// pass, which deferred shading cannot handle on its own) will need a
// depth buffer to test against, and this is already exactly that.
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace gfx
{
    using Microsoft::WRL::ComPtr;

    class Device;

    // Number of back buffers == number of frames-in-flight. 3 gives the
    // GPU a full extra frame of slack versus double buffering, which is
    // what keeps frame pacing smooth when a frame occasionally spikes.
    constexpr uint32_t kBackBufferCount = 3;
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT kDepthBufferFormat = DXGI_FORMAT_D32_FLOAT;

    class SwapChain
    {
    public:
        SwapChain(Device& device, HWND hwnd, uint32_t width, uint32_t height);
        ~SwapChain();

        SwapChain(const SwapChain&) = delete;
        SwapChain& operator=(const SwapChain&) = delete;

        // Must be called with the GPU idle (caller flushes first). Releases
        // old buffers, resizes the swap chain, and recreates RTVs/DSV.
        void Resize(uint32_t width, uint32_t height);

        // vsyncOn=true -> Present(1,0) (locked to display refresh).
        // vsyncOn=false -> Present(0, ALLOW_TEARING) when supported, else 0.
        void Present(bool vsyncOn);

        ID3D12Resource* GetCurrentBackBuffer() const { return m_backBuffers[m_currentBackBufferIndex].Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRtv() const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetDsv() const;
        ID3D12Resource* GetDepthBuffer() const { return m_depthBuffer.Get(); }

        uint32_t GetCurrentBackBufferIndex() const { return m_currentBackBufferIndex; }
        uint32_t GetWidth() const { return m_width; }
        uint32_t GetHeight() const { return m_height; }

        D3D12_VIEWPORT GetViewport() const;
        D3D12_RECT GetScissorRect() const;

        void ToggleFullscreen();
        bool IsFullscreen() const { return m_fullscreen; }

    private:
        void CreateSwapChain(HWND hwnd);
        void CreateRtvHeap();
        void CreateDsvHeap();
        void CreateRenderTargetViews();
        void CreateDepthBuffer();
        void ReleaseBackBuffers();

        Device& m_device;
        ComPtr<IDXGISwapChain4> m_swapChain;

        ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        UINT m_rtvDescriptorSize = 0;

        std::array<ComPtr<ID3D12Resource>, kBackBufferCount> m_backBuffers;
        ComPtr<ID3D12Resource> m_depthBuffer;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_currentBackBufferIndex = 0;
        bool m_tearingSupported = false;
        bool m_fullscreen = false;
    };
}
