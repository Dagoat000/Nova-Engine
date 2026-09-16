// File: src/Graphics/Buffer.h
//
// Two distinct resource kinds, matching how D3D12 memory actually works:
//
//  - GpuBuffer:    lives in a DEFAULT heap (fast VRAM, not CPU-visible).
//                  Used for vertex/index buffers. Filled once via a
//                  temporary upload buffer + CopyBufferRegion, then never
//                  touched by the CPU again.
//
//  - UploadBuffer: lives in an UPLOAD heap (CPU-writable, GPU-readable but
//                  slower to read from). Persistently mapped so writing
//                  per-frame constants is a plain memcpy with no
//                  Map/Unmap overhead in the hot path. Used for the
//                  per-frame constant buffer.
//
// Mixing these up - e.g. using an upload heap as permanent GPU storage for
// something read every draw call - is the classic D3D12 performance trap
// this split is designed to prevent.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace gfx
{
    using Microsoft::WRL::ComPtr;

    class GpuBuffer
    {
    public:
        // Uploads `data` (sizeInBytes) into a DEFAULT-heap buffer using
        // `uploadCmdList`. `keepAliveUploadBuffer` must be kept alive by the
        // caller until the copy command has executed on the GPU (i.e. until
        // the next fence signal after this command list is submitted).
        static GpuBuffer Create(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* uploadCmdList,
            const void* data,
            uint64_t sizeInBytes,
            D3D12_RESOURCE_STATES finalState,
            ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
            const wchar_t* debugName)
        {
            GpuBuffer result;

            D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = sizeInBytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc = { 1, 0 };
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if (FAILED(device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result.m_resource))))
            {
                throw std::runtime_error("Failed to create default-heap buffer");
            }
            result.m_resource->SetName(debugName);
            result.m_sizeInBytes = sizeInBytes;

            // Staging buffer: CPU writes here, GPU copies from here to the
            // default-heap resource above.
            D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
            if (FAILED(device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outUploadBufferKeepAlive))))
            {
                throw std::runtime_error("Failed to create upload staging buffer");
            }

            void* mapped = nullptr;
            outUploadBufferKeepAlive->Map(0, nullptr, &mapped);
            std::memcpy(mapped, data, static_cast<size_t>(sizeInBytes));
            outUploadBufferKeepAlive->Unmap(0, nullptr);

            uploadCmdList->CopyBufferRegion(result.m_resource.Get(), 0, outUploadBufferKeepAlive.Get(), 0, sizeInBytes);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = result.m_resource.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = finalState;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            uploadCmdList->ResourceBarrier(1, &barrier);

            return result;
        }

        ID3D12Resource* Get() const { return m_resource.Get(); }
        uint64_t GetSize() const { return m_sizeInBytes; }

    private:
        ComPtr<ID3D12Resource> m_resource;
        uint64_t m_sizeInBytes = 0;
    };

    // Persistently-mapped ring of upload-heap allocations, one per frame in
    // flight, sized/aligned for a single constant buffer struct T.
    template <typename T>
    class ConstantBuffer
    {
    public:
        void Create(ID3D12Device* device, uint32_t frameCount, const wchar_t* debugName)
        {
            // CBVs must be sized in multiples of 256 bytes.
            m_alignedSize = (sizeof(T) + 255) & ~255u;

            D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = static_cast<UINT64>(m_alignedSize) * frameCount;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc = { 1, 0 };
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if (FAILED(device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource))))
            {
                throw std::runtime_error("Failed to create constant buffer");
            }
            m_resource->SetName(debugName);

            // GENERIC_READ upload resources may be persistently mapped for
            // their entire lifetime - no per-frame Map/Unmap cost.
            D3D12_RANGE noRead{ 0, 0 };
            m_resource->Map(0, &noRead, reinterpret_cast<void**>(&m_mappedBase));
            m_frameCount = frameCount;
        }

        void Update(uint32_t frameIndex, const T& data)
        {
            std::memcpy(m_mappedBase + static_cast<size_t>(frameIndex) * m_alignedSize, &data, sizeof(T));
        }

        D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress(uint32_t frameIndex) const
        {
            return m_resource->GetGPUVirtualAddress() + static_cast<UINT64>(frameIndex) * m_alignedSize;
        }

    private:
        ComPtr<ID3D12Resource> m_resource;
        uint8_t* m_mappedBase = nullptr;
        uint32_t m_alignedSize = 0;
        uint32_t m_frameCount = 0;
    };
}
