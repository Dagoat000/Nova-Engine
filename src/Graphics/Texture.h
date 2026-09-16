// File: src/Graphics/Texture.h
//
// GPU texture creation for the two cases Phase 2 needs: a file loaded via
// WIC (Windows Imaging Component - already on every Windows install, so no
// external image-decoding dependency), and a procedurally generated
// fallback so the demo always has something to show even with an empty
// assets/ folder.
//
// Both paths land in the same place: a DEFAULT-heap 2D texture, uploaded
// through a temporary staging buffer, with an SRV allocated from the
// renderer's shader-visible descriptor heap. This is the same
// default-heap/upload-heap split Buffer.h uses for vertex/index data, for
// the same reason: textures sampled every frame must live in fast VRAM.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace gfx
{
    using Microsoft::WRL::ComPtr;
    class DescriptorHeap;

    class Texture
    {
    public:
        // Loads a PNG/JPG/BMP (anything WIC has a codec for) from disk,
        // decodes to RGBA8, and uploads it. Returns false (leaving the
        // texture unusable) if the file doesn't exist or fails to decode -
        // the caller is expected to fall back to CreateCheckerboard.
        bool LoadFromFile(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* uploadCmdList,
            ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
            const std::wstring& path);

        // Procedural fallback: a two-tone checkerboard. Useful both as a
        // "no asset found" default and as a classic UV-sanity-check pattern.
        void CreateCheckerboard(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* uploadCmdList,
            ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
            uint32_t size = 256,
            uint32_t checkerSize = 32);

        // Procedural fallback for non-albedo material slots (normal map,
        // metallic-roughness, AO): a tiny flat-color texture. e.g. (128,128,
        // 255,255) decodes to tangent-space (0,0,1) - "no bump" - for a
        // default normal map; (0,128,0,255) gives roughness=0.5, metallic=0
        // for a default metallic-roughness map (G=roughness, B=metallic,
        // matching the glTF convention).
        void CreateSolidColor(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* uploadCmdList,
            ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
            uint8_t r, uint8_t g, uint8_t b, uint8_t a);

        // Allocates a slot in `heap` and writes the SRV there. Call once,
        // after the texture resource above has been created.
        void CreateShaderResourceView(ID3D12Device* device, DescriptorHeap& heap, uint32_t& outDescriptorIndex);

        ID3D12Resource* Get() const { return m_resource.Get(); }

    private:
        void UploadPixels(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* uploadCmdList,
            ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
            const uint8_t* rgba,
            uint32_t width,
            uint32_t height);

        ComPtr<ID3D12Resource> m_resource;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
    };
}
