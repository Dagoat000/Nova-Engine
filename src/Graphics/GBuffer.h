// File: src/Graphics/GBuffer.h
//
// Owns the deferred pipeline's G-buffer: 3 color render targets (albedo,
// world-space normal, packed metallic-roughness-AO) plus its own depth
// buffer, all sized to the swap chain's resolution and resized alongside
// it. All 4 are sampled as regular textures by the lighting pass, so the
// depth buffer here uses the same TYPELESS trick as Graphics/ShadowMap
// (D32_FLOAT for the DSV, R32_FLOAT for the SRV) rather than reusing
// SwapChain's own per-backbuffer depth buffer, which cannot be read as an
// SRV at all.
//
// A single (not per-frame-in-flight) set of G-buffer resources is correct
// here for the same reason the shadow map is single-buffered: every read
// of this frame's G-buffer happens later in the SAME command list that
// wrote it, and the GPU executes commands from one queue in submission
// order, so frame N+1's geometry pass cannot begin writing before frame
// N's lighting pass has finished reading.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace gfx
{
    using Microsoft::WRL::ComPtr;
    class DescriptorHeap;

    constexpr DXGI_FORMAT kGBufferAlbedoFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT kGBufferNormalFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kGBufferMRAOFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT kGBufferDepthDsvFormat = DXGI_FORMAT_D32_FLOAT;
    constexpr DXGI_FORMAT kGBufferDepthSrvFormat = DXGI_FORMAT_R32_FLOAT;

    class GBuffer
    {
    public:
        GBuffer(ID3D12Device* device, uint32_t width, uint32_t height);

        // Recreates all 4 resources at the new size and rewrites the SRVs
        // in place at their already-allocated heap slots (see
        // CreateShaderResourceViews) - call after the GPU is idle (the
        // caller, D3D12Renderer::Resize, already flushes before this).
        void Resize(ID3D12Device* device, uint32_t width, uint32_t height, DescriptorHeap& srvHeap);

        // Allocates 4 contiguous SRV slots (albedo, normal, mrao, depth) in
        // the shared shader-visible heap. Call once during startup, after
        // construction.
        void CreateShaderResourceViews(ID3D12Device* device, DescriptorHeap& srvHeap);

        ID3D12Resource* GetAlbedoResource() const { return m_albedo.Get(); }
        ID3D12Resource* GetNormalResource() const { return m_normal.Get(); }
        ID3D12Resource* GetMRAOResource() const { return m_mrao.Get(); }
        ID3D12Resource* GetDepthResource() const { return m_depth.Get(); }

        // 3 contiguous RTV handles for OMSetRenderTargets (albedo, normal, mrao).
        void GetRtvHandles(D3D12_CPU_DESCRIPTOR_HANDLE outHandles[3]) const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetDsv() const;

        // Base GPU handle of the 4 contiguous SRVs (t0..t3 in the lighting
        // pass's root signature: albedo, normal, mrao, depth in that order).
        D3D12_GPU_DESCRIPTOR_HANDLE GetSrvTableGpuHandle(DescriptorHeap& srvHeap) const;

        uint32_t GetWidth() const { return m_width; }
        uint32_t GetHeight() const { return m_height; }
        D3D12_VIEWPORT GetViewport() const { return D3D12_VIEWPORT{ 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f }; }
        D3D12_RECT GetScissorRect() const { return D3D12_RECT{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) }; }

    private:
        void CreateResources(ID3D12Device* device);
        void CreateViews(ID3D12Device* device);
        void WriteShaderResourceViews(ID3D12Device* device, DescriptorHeap& srvHeap);

        ComPtr<ID3D12Resource> m_albedo;
        ComPtr<ID3D12Resource> m_normal;
        ComPtr<ID3D12Resource> m_mrao;
        ComPtr<ID3D12Resource> m_depth;

        ComPtr<ID3D12DescriptorHeap> m_rtvHeap; // 3 descriptors: albedo, normal, mrao
        ComPtr<ID3D12DescriptorHeap> m_dsvHeap; // 1 descriptor
        UINT m_rtvDescriptorSize = 0;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_srvBaseIndex = 0;
        bool m_srvAllocated = false;
    };
}
