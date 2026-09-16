// File: src/Graphics/ShadowMap.cpp
#include "Graphics/ShadowMap.h"
#include "Graphics/DescriptorHeap.h"
#include "Graphics/DXHelper.h"

namespace gfx
{
    ShadowMap::ShadowMap(ID3D12Device* device, uint32_t size)
        : m_size(size)
    {
        D3D12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_DEFAULT };

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = size;
        desc.Height = size;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS; // lets DSV (D32_FLOAT) and SRV (R32_FLOAT) both view this resource
        desc.SampleDesc = { 1, 0 };
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = { 1.0f, 0 };

        // Created directly in PIXEL_SHADER_RESOURCE state (rather than
        // DEPTH_WRITE) so RenderFrame's per-frame barrier sequence -
        // PIXEL_SHADER_RESOURCE -> DEPTH_WRITE -> PIXEL_SHADER_RESOURCE -
        // is uniform from the very first frame, with no special-cased
        // "first use" branch.
        DX_CHECK(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&m_resource)));
        m_resource->SetName(L"ShadowMap");

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        DX_CHECK(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(m_resource.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    void ShadowMap::CreateShaderResourceView(ID3D12Device* device, DescriptorHeap& srvHeap)
    {
        m_srvIndex = srvHeap.Allocate();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(m_resource.Get(), &srvDesc, srvHeap.GetCpuHandle(m_srvIndex));
    }

    D3D12_GPU_DESCRIPTOR_HANDLE ShadowMap::GetSrvGpuHandle(DescriptorHeap& srvHeap) const
    {
        return srvHeap.GetGpuHandle(m_srvIndex);
    }
}
