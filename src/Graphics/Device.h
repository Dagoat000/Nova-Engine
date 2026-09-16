// File: src/Graphics/Device.h
//
// Owns the DXGI factory, the chosen hardware adapter, the ID3D12Device and
// the single graphics command queue. Everything else in Graphics/ takes a
// Device& rather than talking to D3D12 globals directly, which is what
// lets us later add a second (e.g. copy or compute) queue without touching
// unrelated code.
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>

namespace gfx
{
    using Microsoft::WRL::ComPtr;

    struct GpuCapabilities
    {
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        D3D12_MESH_SHADER_TIER meshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        bool tearingSupported = false;
    };

    class Device
    {
    public:
        // enableDebugLayer should be true in Debug builds only - it is
        // compiled out entirely by the caller via #if defined(_DEBUG) so
        // there is zero chance of it leaking into a Release binary.
        explicit Device(bool enableDebugLayer);
        ~Device();

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        IDXGIFactory6* GetFactory() const { return m_factory.Get(); }
        ID3D12Device* GetDevice() const { return m_device.Get(); }
        ID3D12CommandQueue* GetGraphicsQueue() const { return m_graphicsQueue.Get(); }
        const GpuCapabilities& GetCapabilities() const { return m_caps; }

        UINT GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const
        {
            return m_device->GetDescriptorHandleIncrementSize(type);
        }

    private:
        void CreateFactory(bool debugLayer);
        void SelectAdapterAndCreateDevice();
        void CreateGraphicsQueue();
        void QueryCapabilities();
        void LogAdapterInfo(IDXGIAdapter1* adapter, const DXGI_ADAPTER_DESC1& desc) const;

        ComPtr<IDXGIFactory6> m_factory;
        ComPtr<IDXGIAdapter1> m_adapter;
        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12CommandQueue> m_graphicsQueue;
        ComPtr<ID3D12Debug> m_debugController;

        GpuCapabilities m_caps;
    };
}
