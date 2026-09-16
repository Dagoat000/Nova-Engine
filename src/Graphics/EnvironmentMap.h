// File: src/Graphics/EnvironmentMap.h
//
// Image-based lighting: a procedural sky cubemap plus its two split-sum
// precomputed derivatives (Karis, "Real Shading in Unreal Engine 4", 2013):
// a diffuse irradiance cubemap and a roughness-mipped prefiltered specular
// cubemap, plus the shared BRDF integration LUT. Together these replace
// the flat ambient constant every earlier phase used with a real,
// sky-colored ambient term - the single biggest visual-realism gap left
// after Phase 4's direct lighting.
//
// Everything here is a one-time GPU compute precompute, run once at
// startup (see Generate()) since the sky is currently static; a moving
// time-of-day would re-run this same pipeline periodically rather than
// needing a different one.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace gfx
{
    using Microsoft::WRL::ComPtr;
    class DescriptorHeap;

    constexpr uint32_t kSkyResolution = 128;
    constexpr uint32_t kIrradianceResolution = 32;
    constexpr uint32_t kPrefilterBaseResolution = 128;
    constexpr uint32_t kPrefilterMipCount = 6; // 128,64,32,16,8,4
    constexpr uint32_t kBrdfLutResolution = 128;
    constexpr DXGI_FORMAT kEnvMapFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kBrdfLutFormat = DXGI_FORMAT_R16G16_FLOAT;

    class EnvironmentMap
    {
    public:
        EnvironmentMap(ID3D12Device* device);

        // Records the sky generation + irradiance convolution + specular
        // prefilter + BRDF LUT compute dispatches into cmdList, and binds
        // its own temporary UAV/SRV heap to do so (via SetDescriptorHeaps) -
        // call this on a command list that isn't relying on a different
        // heap being bound afterward without re-binding it. Call once
        // during startup, before Flush().
        void Generate(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::XMFLOAT3 sunDirection, DirectX::XMFLOAT3 sunColor, float sunIntensity);

        // Allocates 4 contiguous SRV slots (sky, irradiance, prefiltered,
        // brdfLUT) in the shared shader-visible heap used by the lighting
        // pass. Call once during startup, after Generate().
        void CreateShaderResourceViews(ID3D12Device* device, DescriptorHeap& srvHeap);
        D3D12_GPU_DESCRIPTOR_HANDLE GetSrvTableGpuHandle(DescriptorHeap& srvHeap) const;

        static constexpr uint32_t GetPrefilteredMipCount() { return kPrefilterMipCount; }

    private:
        ComPtr<ID3D12Resource> m_sky;
        ComPtr<ID3D12Resource> m_irradiance;
        ComPtr<ID3D12Resource> m_prefiltered;
        ComPtr<ID3D12Resource> m_brdfLut;

        // Root signatures + PSOs for the 4 precompute passes.
        ComPtr<ID3D12RootSignature> m_skyRootSig;
        ComPtr<ID3D12PipelineState> m_skyPso;
        ComPtr<ID3D12RootSignature> m_irradianceRootSig;
        ComPtr<ID3D12PipelineState> m_irradiancePso;
        ComPtr<ID3D12RootSignature> m_prefilterRootSig;
        ComPtr<ID3D12PipelineState> m_prefilterPso;
        ComPtr<ID3D12RootSignature> m_brdfLutRootSig;
        ComPtr<ID3D12PipelineState> m_brdfLutPso;

        // Temporary heap for the UAVs written during Generate() plus the
        // sky's intermediate SRV read by the irradiance/prefilter passes.
        // Kept alive for the object's lifetime (cheap, ~50 descriptors)
        // rather than freed after use, since the GPU must not lose access
        // to it before the one-time precompute command list has finished
        // executing - freeing it right after recording, before that
        // Flush(), would be a use-after-free from the GPU's perspective.
        std::unique_ptr<DescriptorHeap> m_uavHeap;

        // The tiny upload-heap CBs created during Generate() (face index,
        // roughness, etc.) must outlive that command list's execution. For
        // a one-time startup cost of a few dozen 256-byte buffers, keeping
        // them for the engine's lifetime is simpler and cheaper than
        // tracking a fence value just to free them slightly earlier.
        std::vector<ComPtr<ID3D12Resource>> m_startupUploadsLeakedIntentionally;

        uint32_t m_srvBaseIndex = 0;
    };
}
