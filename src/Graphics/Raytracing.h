// File: src/Graphics/Raytracing.h
//
// Ray-traced reflections: one bottom-level acceleration structure (BLAS)
// per scene object (built once - the geometry is static), one top-level
// acceleration structure (TLAS) rebuilt every frame (objects move/rotate),
// and a small raytracing pipeline that shoots one reflection ray per
// eligible G-buffer pixel (see the roughness threshold in
// DeferredLightingPS.hlsl - rough surfaces keep using the prefiltered IBL
// cubemap from the Image-Based Lighting step instead of being ray traced;
// tracing every pixel regardless of whether it would show a sharp
// reflection would be pure waste).
//
// Gracefully degrades to "unsupported" (IsUsable() == false) on hardware
// or drivers without DXR (< D3D12_RAYTRACING_TIER_1_0) - the renderer is
// expected to skip acceleration-structure building and dispatching
// entirely in that case and fall back to IBL-only reflections, which is
// exactly what every earlier phase already did.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
#include <memory>

namespace gfx
{
    using Microsoft::WRL::ComPtr;
    class DescriptorHeap;
    struct RenderObject;

    class RaytracingContext
    {
    public:
        RaytracingContext(ID3D12Device* device, uint32_t width, uint32_t height);

        bool IsUsable() const { return m_usable; }

        // Builds one BLAS per object plus the per-instance raw vertex/index
        // buffer SRVs the closest-hit shader needs for normal interpolation.
        // The geometry is static, so this runs once, during startup.
        void BuildBottomLevelStructures(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const std::vector<RenderObject>& objects, DescriptorHeap& srvHeap);

        // Rebuilds the TLAS with each object's current world transform.
        // Called every frame - a full rebuild (not a refit) because the
        // whole acceleration structure is small (a handful of objects) and
        // simplicity beats the marginal win a refit would give here.
        void UpdateTopLevelStructure(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const std::vector<RenderObject>& objects, double totalTimeSeconds);

        // Recreates the output texture at the new resolution. Call after
        // the GPU is idle (the caller already flushes before this).
        void Resize(ID3D12Device* device, uint32_t width, uint32_t height, DescriptorHeap& srvHeap);

        // Traces one reflection ray per pixel via the ray generation
        // shader's own roughness check against the G-buffer, writing
        // shaded results (or the sky, on a miss) into the output texture.
        // Caller is responsible for the output resource's UNORDERED_ACCESS
        // <-> shader-read transitions (see D3D12Renderer::RenderFrame),
        // matching how SwapChain/ShadowMap/GBuffer's resources are handled.
        void DispatchReflections(
            ID3D12GraphicsCommandList* cmdList, DescriptorHeap& srvHeap,
            D3D12_GPU_VIRTUAL_ADDRESS screenCbAddress, D3D12_GPU_VIRTUAL_ADDRESS lightCbAddress,
            D3D12_GPU_DESCRIPTOR_HANDLE gbufferTable, D3D12_GPU_DESCRIPTOR_HANDLE envTable);

        ID3D12Resource* GetOutputResource() const { return m_output.Get(); }
        void CreateOutputShaderResourceView(ID3D12Device* device, DescriptorHeap& srvHeap);
        D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSrvGpuHandle(DescriptorHeap& srvHeap) const;

    private:
        void CreateOutputResource(ID3D12Device* device, uint32_t width, uint32_t height);
        void CreateRootSignatures(ID3D12Device* device);
        void CreateStateObjectAndTable(ID3D12Device* device);

        bool m_usable = false;
        uint32_t m_width = 0;
        uint32_t m_height = 0;

        ComPtr<ID3D12Resource> m_output; // RGBA16_FLOAT, UAV + SRV
        uint32_t m_outputUavIndex = 0;
        uint32_t m_outputSrvIndex = 0;
        bool m_outputSrvAllocated = false;

        ComPtr<ID3D12RootSignature> m_globalRootSig;
        ComPtr<ID3D12RootSignature> m_localRootSig;
        ComPtr<ID3D12StateObject> m_stateObject;
        ComPtr<ID3D12StateObjectProperties> m_stateObjectProps;

        struct BlasEntry
        {
            ComPtr<ID3D12Resource> blas;
            uint32_t vertexSrvIndex = 0;
            uint32_t indexSrvIndex = 0;
        };
        std::vector<BlasEntry> m_blasEntries;
        // BLAS scratch buffers only need to live until the build command
        // list has executed, but - same reasoning as EnvironmentMap's
        // startup upload buffers - keeping them for the engine's lifetime
        // is simpler and cheap enough for a one-time startup cost.
        std::vector<ComPtr<ID3D12Resource>> m_blasScratchKeepAlive;

        ComPtr<ID3D12Resource> m_tlas;
        ComPtr<ID3D12Resource> m_tlasScratch;
        ComPtr<ID3D12Resource> m_instanceDescBuffer; // persistently-mapped upload buffer, rewritten every frame

        ComPtr<ID3D12Resource> m_shaderTable;
        UINT64 m_raygenRecordOffset = 0;
        UINT64 m_missRecordOffset = 0;
        UINT64 m_hitGroupTableOffset = 0;
        UINT m_hitGroupRecordStride = 0;
        uint32_t m_hitGroupCount = 0;
    };
}
