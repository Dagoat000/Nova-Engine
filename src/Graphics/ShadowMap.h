// File: src/Graphics/ShadowMap.h
//
// A single square depth texture rendered from the light's point of view,
// then sampled as a regular texture in the main pass. This needs a
// TYPELESS resource format because DSV and SRV need different concrete
// views of the same bits (D32_FLOAT for the depth-write view,
// R32_FLOAT for the shader-read view) - a plain D32_FLOAT resource
// cannot be given an SRV at all.
//
// One shadow map, one directional light, for now - point-light shadows
// (cube maps) and cascaded shadow maps for the directional light are both
// scoped out here; this establishes the resource/binding pattern they'd
// both extend.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace gfx
{
    using Microsoft::WRL::ComPtr;
    class DescriptorHeap;

    class ShadowMap
    {
    public:
        ShadowMap(ID3D12Device* device, uint32_t size = 2048);

        // Allocates one SRV slot in the shared shader-visible heap. Call
        // once during startup, after the resource above already exists.
        void CreateShaderResourceView(ID3D12Device* device, DescriptorHeap& srvHeap);

        D3D12_CPU_DESCRIPTOR_HANDLE GetDsv() const { return m_dsvHeap->GetCPUDescriptorHandleForHeapStart(); }
        D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle(DescriptorHeap& srvHeap) const;
        ID3D12Resource* GetResource() const { return m_resource.Get(); }

        uint32_t GetSize() const { return m_size; }
        D3D12_VIEWPORT GetViewport() const { return D3D12_VIEWPORT{ 0.0f, 0.0f, static_cast<float>(m_size), static_cast<float>(m_size), 0.0f, 1.0f }; }
        D3D12_RECT GetScissorRect() const { return D3D12_RECT{ 0, 0, static_cast<LONG>(m_size), static_cast<LONG>(m_size) }; }

    private:
        ComPtr<ID3D12Resource> m_resource;
        ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        uint32_t m_size;
        uint32_t m_srvIndex = 0;
    };
}
