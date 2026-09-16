// File: src/Graphics/SwapChain.cpp
#include "Graphics/SwapChain.h"
#include "Graphics/Device.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"

namespace gfx
{
    SwapChain::SwapChain(Device& device, HWND hwnd, uint32_t width, uint32_t height)
        : m_device(device)
        , m_width(width)
        , m_height(height)
        , m_tearingSupported(device.GetCapabilities().tearingSupported)
    {
        CreateSwapChain(hwnd);
        CreateRtvHeap();
        CreateDsvHeap();
        CreateRenderTargetViews();
        CreateDepthBuffer();
    }

    SwapChain::~SwapChain() = default;

    void SwapChain::CreateSwapChain(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.Format = kBackBufferFormat;
        desc.Stereo = FALSE;
        desc.SampleDesc = { 1, 0 }; // MSAA is done via an offscreen resolve target in later phases, not here.
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBackBufferCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        // Tearing flag is required to ever call Present with the ALLOW_TEARING
        // flag later (uncapped / variable refresh presentation).
        desc.Flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        ComPtr<IDXGISwapChain1> swapChain1;
        DX_CHECK(m_device.GetFactory()->CreateSwapChainForHwnd(
            m_device.GetGraphicsQueue(), // swap chain needs the queue that presents, not the device
            hwnd, &desc, nullptr, nullptr, &swapChain1));

        // We handle Alt+Enter ourselves (ToggleFullscreen) so the GPU can be
        // flushed safely first; let DXGI's automatic handler stay out of it.
        DX_CHECK(m_device.GetFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

        DX_CHECK(swapChain1.As(&m_swapChain));
        m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    void SwapChain::CreateRtvHeap()
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = kBackBufferCount;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // RTV/DSV heaps are never shader-visible.
        DX_CHECK(m_device.GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvDescriptorSize = m_device.GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    void SwapChain::CreateDsvHeap()
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = 1;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        DX_CHECK(m_device.GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_dsvHeap)));
    }

    void SwapChain::CreateRenderTargetViews()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (uint32_t i = 0; i < kBackBufferCount; ++i)
        {
            DX_CHECK(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
            m_device.GetDevice()->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, handle);
            m_backBuffers[i]->SetName(L"BackBuffer");
            handle.ptr += m_rtvDescriptorSize;
        }
    }

    void SwapChain::CreateDepthBuffer()
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = m_width;
        desc.Height = m_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kDepthBufferFormat;
        desc.SampleDesc = { 1, 0 };
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = kDepthBufferFormat;
        clearValue.DepthStencil = { 1.0f, 0 };

        DX_CHECK(m_device.GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
            IID_PPV_ARGS(&m_depthBuffer)));
        m_depthBuffer->SetName(L"DepthBuffer");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kDepthBufferFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device.GetDevice()->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    void SwapChain::ReleaseBackBuffers()
    {
        for (auto& buffer : m_backBuffers)
        {
            buffer.Reset();
        }
        m_depthBuffer.Reset();
    }

    void SwapChain::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0) return;

        m_width = width;
        m_height = height;

        // The caller (D3D12Renderer) is responsible for flushing the GPU
        // before calling this - resizing while the GPU still references
        // these resources is undefined behavior.
        ReleaseBackBuffers();

        DXGI_SWAP_CHAIN_DESC1 desc{};
        DX_CHECK(m_swapChain->GetDesc1(&desc));
        DX_CHECK(m_swapChain->ResizeBuffers(kBackBufferCount, width, height, kBackBufferFormat, desc.Flags));

        m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
        CreateRenderTargetViews();
        CreateDepthBuffer();
    }

    void SwapChain::Present(bool vsyncOn)
    {
        UINT syncInterval = vsyncOn ? 1 : 0;
        UINT flags = (!vsyncOn && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;

        DX_CHECK(m_swapChain->Present(syncInterval, flags));
        m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::GetCurrentRtv() const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(m_currentBackBufferIndex) * m_rtvDescriptorSize;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::GetDsv() const
    {
        return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12_VIEWPORT SwapChain::GetViewport() const
    {
        return D3D12_VIEWPORT{ 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    }

    D3D12_RECT SwapChain::GetScissorRect() const
    {
        return D3D12_RECT{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    }

    void SwapChain::ToggleFullscreen()
    {
        m_fullscreen = !m_fullscreen;
        DX_CHECK(m_swapChain->SetFullscreenState(m_fullscreen ? TRUE : FALSE, nullptr));
    }
}
