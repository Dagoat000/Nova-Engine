// File: src/Graphics/DescriptorHeap.h
//
// A minimal bump-allocator over a single ID3D12DescriptorHeap. RTV/DSV
// heaps in this engine are owned directly by SwapChain since they're small
// and fixed-size; this class is for the CBV_SRV_UAV heap, which is
// shader-visible and will grow to hold per-material/per-texture
// descriptors in later phases. Keeping allocation behind one type now
// means later phases (bindless SRV tables, etc.) only change this file.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <stdexcept>

namespace gfx
{
    using Microsoft::WRL::ComPtr;

    class DescriptorHeap
    {
    public:
        DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible)
            : m_type(type)
            , m_capacity(capacity)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = type;
            desc.NumDescriptors = capacity;
            desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

            if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap))))
            {
                throw std::runtime_error("Failed to create descriptor heap");
            }

            m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
            m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
            if (shaderVisible)
            {
                m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
            }
        }

        // Bump-allocates the next slot. Phase 1 never frees individual
        // descriptors - a free-list is added once materials/textures make
        // that necessary.
        uint32_t Allocate()
        {
            if (m_nextFree >= m_capacity)
            {
                throw std::runtime_error("Descriptor heap exhausted");
            }
            return m_nextFree++;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(uint32_t index) const
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
            handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
            return handle;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(uint32_t index) const
        {
            D3D12_GPU_DESCRIPTOR_HANDLE handle = m_gpuStart;
            handle.ptr += static_cast<UINT64>(index) * m_descriptorSize;
            return handle;
        }

        ID3D12DescriptorHeap* Get() const { return m_heap.Get(); }

    private:
        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_DESCRIPTOR_HEAP_TYPE m_type;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};
        UINT m_descriptorSize = 0;
        uint32_t m_capacity = 0;
        uint32_t m_nextFree = 0;
    };
}
