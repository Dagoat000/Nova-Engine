// File: src/Graphics/Material.h
//
// A metallic-roughness PBR material: 4 textures (albedo, normal,
// metallic-roughness, ambient occlusion) allocated as one contiguous block
// in the shader-visible SRV heap, so the root signature can bind them as a
// single 4-descriptor table (t0..t3) instead of 4 separate root
// parameters. Each texture falls back to a small procedural default if the
// corresponding file isn't found under assets/, so the demo always renders
// something reasonable with zero required art assets.
//
// The scene (Graphics/Scene) gives each object its own Material instance -
// simple and fine for a handful of objects; a real material *library* with
// deduplication/sharing across many objects is a later concern once the
// object count actually makes that matter.
#pragma once

#include "Graphics/Texture.h"
#include "Graphics/DescriptorHeap.h"
#include <DirectXMath.h>
#include <vector>
#include <string>

namespace gfx
{
    // Mirrors the HLSL cbuffer layout exactly (see shaders/GBufferPS.hlsl) -
    // 16-byte-aligned throughout so no implicit HLSL packing surprises.
    struct MaterialConstants
    {
        DirectX::XMFLOAT4 baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f }; // multiplies the albedo texture
        float metallicFactor = 1.0f;   // multiplies the metallic-roughness texture's B channel
        float roughnessFactor = 1.0f;  // multiplies the metallic-roughness texture's G channel
        float aoStrength = 1.0f;       // lerp weight between "no AO" and the AO texture
        float _pad = 0.0f;
    };

    class Material
    {
    public:
        // assetDir is scanned for "albedo.png", "normal.png",
        // "metallicRoughness.png", "ao.png" (first of .png/.jpg/.bmp found);
        // any missing file gets its procedural fallback instead. If
        // flatAlbedoColor is provided, a missing albedo falls back to that
        // flat color instead of the checkerboard - used for procedural
        // scene objects (spheres, floor) that want a clean, distinguishable
        // color rather than a debug pattern. Upload staging buffers are
        // appended to uploadKeepAlive, which the caller must keep alive
        // until the command list recording these uploads has finished
        // executing on the GPU.
        void Load(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* uploadCmdList,
            std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive,
            DescriptorHeap& srvHeap,
            const std::string& assetDir = "assets",
            const DirectX::XMFLOAT3* flatAlbedoColor = nullptr);

        // GPU handle of the FIRST of the 4 contiguous descriptors (albedo).
        // Valid as the base handle for a t0..t3 descriptor table because
        // Load() allocates all 4 back-to-back with nothing else in between.
        D3D12_GPU_DESCRIPTOR_HANDLE GetTableGpuHandle(DescriptorHeap& srvHeap) const
        {
            return srvHeap.GetGpuHandle(m_baseDescriptorIndex);
        }

        MaterialConstants factors;

    private:
        static bool TryLoad(
            Texture& tex, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
            std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive, const std::string& basePath);

        Texture m_albedo;
        Texture m_normal;
        Texture m_metallicRoughness;
        Texture m_ao;
        uint32_t m_baseDescriptorIndex = 0;
    };
}
