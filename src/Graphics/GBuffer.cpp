// File: src/Graphics/GBuffer.cpp
#include "Graphics/GBuffer.h"
#include "Graphics/DescriptorHeap.h"
#include "Graphics/DXHelper.h"
#include <cstring>

namespace gfx
{
    namespace
    {
        ComPtr<ID3D12Resource> CreateColorTarget(ID3D12Device* device, DXGI_FORMAT format, uint32_t width, uint32_t height, const float clearColor[4], const wchar_t* name)
        {
            D3D12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc = { 1, 0 };
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clearValue{};
            clearValue.Format = format;
            std::memcpy(clearValue.Color, clearColor, sizeof(clearValue.Color));

            ComPtr<ID3D12Resource> resource;
            DX_CHECK(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&resource)));
            resource->SetName(name);
            return resource;
        }
    }

    GBuffer::GBuffer(ID3D12Device* device, uint32_t width, uint32_t height)
        : m_width(width)
        , m_height(height)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = 3;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        DX_CHECK(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        DX_CHECK(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

        CreateResources(device);
        CreateViews(device);
    }

    void GBuffer::CreateResources(ID3D12Device* device)
    {
        // Cleared to the same dark background used everywhere else, so a
        // pixel the geometry pass never touches (there is none, in this
        // single-object demo, but a real scene will have background pixels)
        // still reads as the intended background rather than black.
        const float bgClear[4] = { 0.04f, 0.045f, 0.06f, 1.0f };
        const float zeroClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        m_albedo = CreateColorTarget(device, kGBufferAlbedoFormat, m_width, m_height, bgClear, L"GBuffer_Albedo");
        m_normal = CreateColorTarget(device, kGBufferNormalFormat, m_width, m_height, zeroClear, L"GBuffer_Normal");
        m_mrao = CreateColorTarget(device, kGBufferMRAOFormat, m_width, m_height, zeroClear, L"GBuffer_MetallicRoughnessAO");

        D3D12_HEAP_PROPERTIES depthHeapProps{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = m_width;
        depthDesc.Height = m_height;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_R32_TYPELESS; // lets DSV (D32_FLOAT) and SRV (R32_FLOAT) both view this resource
        depthDesc.SampleDesc = { 1, 0 };
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthClearValue{};
        depthClearValue.Format = kGBufferDepthDsvFormat;
        depthClearValue.DepthStencil = { 1.0f, 0 };

        DX_CHECK(device->CreateCommittedResource(
            &depthHeapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &depthClearValue, IID_PPV_ARGS(&m_depth)));
        m_depth->SetName(L"GBuffer_Depth");
    }

    void GBuffer::CreateViews(ID3D12Device* device)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(m_albedo.Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
        device->CreateRenderTargetView(m_normal.Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
        device->CreateRenderTargetView(m_mrao.Get(), nullptr, rtvHandle);

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kGBufferDepthDsvFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(m_depth.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    void GBuffer::WriteShaderResourceViews(ID3D12Device* device, DescriptorHeap& srvHeap)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        srvDesc.Format = kGBufferAlbedoFormat;
        device->CreateShaderResourceView(m_albedo.Get(), &srvDesc, srvHeap.GetCpuHandle(m_srvBaseIndex + 0));

        srvDesc.Format = kGBufferNormalFormat;
        device->CreateShaderResourceView(m_normal.Get(), &srvDesc, srvHeap.GetCpuHandle(m_srvBaseIndex + 1));

        srvDesc.Format = kGBufferMRAOFormat;
        device->CreateShaderResourceView(m_mrao.Get(), &srvDesc, srvHeap.GetCpuHandle(m_srvBaseIndex + 2));

        srvDesc.Format = kGBufferDepthSrvFormat;
        device->CreateShaderResourceView(m_depth.Get(), &srvDesc, srvHeap.GetCpuHandle(m_srvBaseIndex + 3));
    }

    void GBuffer::CreateShaderResourceViews(ID3D12Device* device, DescriptorHeap& srvHeap)
    {
        // Allocate once - Resize() re-creates the underlying resources but
        // always writes back into these same 4 slots, so the root
        // signature's base descriptor table handle never needs updating.
        if (!m_srvAllocated)
        {
            m_srvBaseIndex = srvHeap.Allocate();
            srvHeap.Allocate();
            srvHeap.Allocate();
            srvHeap.Allocate();
            m_srvAllocated = true;
        }
        WriteShaderResourceViews(device, srvHeap);
    }

    void GBuffer::Resize(ID3D12Device* device, uint32_t width, uint32_t height, DescriptorHeap& srvHeap)
    {
        if (width == 0 || height == 0 || (width == m_width && height == m_height)) return;

        m_width = width;
        m_height = height;

        m_albedo.Reset();
        m_normal.Reset();
        m_mrao.Reset();
        m_depth.Reset();

        CreateResources(device);
        CreateViews(device);
        WriteShaderResourceViews(device, srvHeap);
    }

    void GBuffer::GetRtvHandles(D3D12_CPU_DESCRIPTOR_HANDLE outHandles[3]) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        outHandles[0] = handle;
        handle.ptr += m_rtvDescriptorSize;
        outHandles[1] = handle;
        handle.ptr += m_rtvDescriptorSize;
        outHandles[2] = handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetDsv() const
    {
        return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GBuffer::GetSrvTableGpuHandle(DescriptorHeap& srvHeap) const
    {
        return srvHeap.GetGpuHandle(m_srvBaseIndex);
    }
}
