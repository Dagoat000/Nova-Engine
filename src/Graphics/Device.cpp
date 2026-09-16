// File: src/Graphics/Device.cpp
#include "Graphics/Device.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"
#include <string>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace gfx
{
    Device::Device(bool enableDebugLayer)
    {
        CreateFactory(enableDebugLayer);
        SelectAdapterAndCreateDevice();
        QueryCapabilities();
        CreateGraphicsQueue();
    }

    Device::~Device() = default;

    void Device::CreateFactory(bool debugLayer)
    {
        UINT factoryFlags = 0;

#if defined(_DEBUG)
        if (debugLayer)
        {
            // The debug layer must be enabled BEFORE the device is created,
            // otherwise it has no effect. GPU-based validation catches
            // resource-state and descriptor mistakes the CPU validation
            // layer alone would miss - invaluable while barriers and
            // descriptor heaps are still being built out.
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&m_debugController))))
            {
                m_debugController->EnableDebugLayer();

                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(m_debugController.As(&debug1)))
                {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                }
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                LOG_INFO("D3D12 debug layer + GPU-based validation enabled.");
            }
            else
            {
                LOG_WARN("D3D12 debug layer requested but unavailable (enable the 'Graphics Tools' optional Windows feature).");
            }
        }
#else
        (void)debugLayer;
#endif

        DX_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));
    }

    void Device::SelectAdapterAndCreateDevice()
    {
        // We pick the hardware adapter with the most dedicated VRAM that is
        // actually capable of D3D12 (feature level 11_0 minimum). This is a
        // cheap, dependency-free heuristic that does the right thing on the
        // overwhelming majority of desktops, including hybrid-GPU laptops
        // where EnumAdapterByGpuPreference below does the heavy lifting.
        ComPtr<IDXGIAdapter1> bestAdapter;
        ComPtr<IDXGIAdapter1> bestRaytracingAdapter;
        SIZE_T bestVram = 0;
        SIZE_T bestRaytracingVram = 0;
        DXGI_ADAPTER_DESC1 bestDesc{};
        DXGI_ADAPTER_DESC1 bestRaytracingDesc{};

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;
             m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++i)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            // Skip the software (WARP) adapter and remoting adapters - we
            // want real hardware for a performance-first renderer.
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                continue;
            }

                ComPtr<ID3D12Device> candidateDevice;
                if (SUCCEEDED(D3D12CreateDevice(
                    adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&candidateDevice))))
            {
                LogAdapterInfo(adapter.Get(), desc);

                if (desc.DedicatedVideoMemory > bestVram)
                {
                    bestVram = desc.DedicatedVideoMemory;
                    bestAdapter = adapter;
                    bestDesc = desc;
                }

                D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
                const bool supportsRaytracing =
                    SUCCEEDED(candidateDevice->CheckFeatureSupport(
                        D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) &&
                    options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

                if (supportsRaytracing && desc.DedicatedVideoMemory > bestRaytracingVram)
                {
                    bestRaytracingVram = desc.DedicatedVideoMemory;
                    bestRaytracingAdapter = adapter;
                    bestRaytracingDesc = desc;
                }
            }
        }

        if (!bestAdapter)
        {
            LOG_ERROR("No D3D12-capable hardware adapter found.");
            throw std::runtime_error("No suitable GPU adapter found");
        }

        if (bestRaytracingAdapter)
        {
            m_adapter = bestRaytracingAdapter;
            bestDesc = bestRaytracingDesc;
            LOG_INFO("Selected the highest-VRAM DXR-capable adapter.");
        }
        else
        {
            m_adapter = bestAdapter;
            LOG_WARN("No DXR-capable adapter found; using the highest-VRAM D3D12 adapter.");
        }

        // Try feature levels from newest to oldest so m_caps.featureLevel
        // reflects what the GPU truly supports, not just our minimum bar.
        static const D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_12_2,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        for (D3D_FEATURE_LEVEL level : kLevels)
        {
            if (SUCCEEDED(D3D12CreateDevice(m_adapter.Get(), level, IID_PPV_ARGS(&m_device))))
            {
                m_caps.featureLevel = level;
                break;
            }
        }

        if (!m_device)
        {
            throw std::runtime_error("D3D12CreateDevice failed for selected adapter");
        }

#if defined(_DEBUG)
        // Route D3D12 debug-layer messages through our logger and break on
        // corruption/errors immediately instead of discovering them three
        // frames later as a garbled screen.
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue)))
        {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }
#endif

        LOG_INFO("Selected adapter: " + std::to_string(bestDesc.DedicatedVideoMemory / (1024 * 1024)) + " MB VRAM");
    }

    void Device::QueryCapabilities()
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
        {
            m_caps.raytracingTier = options5.RaytracingTier;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
        {
            m_caps.meshShaderTier = options7.MeshShaderTier;
        }

        BOOL allowTearing = FALSE;
        if (SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
        {
            m_caps.tearingSupported = (allowTearing == TRUE);
        }

        const char* rtTierStr =
            m_caps.raytracingTier == D3D12_RAYTRACING_TIER_1_1 ? "1.1" :
            m_caps.raytracingTier == D3D12_RAYTRACING_TIER_1_0 ? "1.0" : "Not supported";

        const char* meshTierStr =
            m_caps.meshShaderTier == D3D12_MESH_SHADER_TIER_1 ? "1" : "Not supported";

        LOG_INFO(std::string("Feature Level: ") +
            (m_caps.featureLevel == D3D_FEATURE_LEVEL_12_2 ? "12_2" :
             m_caps.featureLevel == D3D_FEATURE_LEVEL_12_1 ? "12_1" :
             m_caps.featureLevel == D3D_FEATURE_LEVEL_12_0 ? "12_0" :
             m_caps.featureLevel == D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0"));
        LOG_INFO(std::string("Ray Tracing Tier: ") + rtTierStr);
        LOG_INFO(std::string("Mesh Shader Tier: ") + meshTierStr);
        LOG_INFO(std::string("Tearing (variable refresh) supported: ") + (m_caps.tearingSupported ? "yes" : "no"));
    }

    void Device::CreateGraphicsQueue()
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

        DX_CHECK(m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_graphicsQueue)));
        m_graphicsQueue->SetName(L"GraphicsQueue");
    }

    void Device::LogAdapterInfo(IDXGIAdapter1* adapter, const DXGI_ADAPTER_DESC1& desc) const
    {
        (void)adapter;

        // desc.Description is UTF-16; naively copying wchar_t -> char
        // truncates every code point and corrupts non-ASCII GPU names.
        // WideCharToMultiByte does a real UTF-16 -> UTF-8 conversion.
        std::string name;
        int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
        if (bytesNeeded > 0)
        {
            name.resize(static_cast<size_t>(bytesNeeded) - 1); // exclude the null terminator
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), bytesNeeded, nullptr, nullptr);
        }

        LOG_INFO("GPU: " + name);
        LOG_INFO("  Dedicated VRAM: " + std::to_string(desc.DedicatedVideoMemory / (1024 * 1024)) + " MB");
        LOG_INFO("  Shared Memory:  " + std::to_string(desc.SharedSystemMemory / (1024 * 1024)) + " MB");
    }
}
