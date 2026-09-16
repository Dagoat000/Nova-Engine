// File: src/Graphics/Texture.cpp
#include "Graphics/Texture.h"
#include "Graphics/DescriptorHeap.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"
#include <wincodec.h>
#include <vector>
#include <stdexcept>
#include <string>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")

namespace gfx
{
    namespace
    {
        // WIC needs COM. Every real-world D3D12 app already initializes COM
        // somewhere (input, file dialogs, ...); we do it lazily here so
        // Texture has no ordering dependency on the rest of startup.
        void EnsureComInitialized()
        {
            static bool initialized = false;
            if (!initialized)
            {
                HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                // RPC_E_CHANGED_MODE means COM was already initialized with a
                // different threading model elsewhere - harmless for our use.
                if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
                {
                    throw std::runtime_error("Failed to initialize COM for WIC");
                }
                initialized = true;
            }
        }
    }

    bool Texture::LoadFromFile(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
        const std::wstring& path)
    {
        EnsureComInitialized();

        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        {
            LOG_ERROR("Failed to create WIC imaging factory.");
            return false;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
        {
            LOG_WARN("Texture not found or unreadable, will fall back to procedural texture.");
            return false;
        }

        ComPtr<IWICBitmapFrameDecode> frame;
        DX_CHECK(decoder->GetFrame(0, &frame));

        // Normalize every input format to 32bpp RGBA so the rest of the
        // pipeline only ever deals with one texel format in Phase 2.
        ComPtr<IWICFormatConverter> converter;
        DX_CHECK(factory->CreateFormatConverter(&converter));
        DX_CHECK(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));

        UINT width = 0, height = 0;
        DX_CHECK(converter->GetSize(&width, &height));

        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        DX_CHECK(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));

        UploadPixels(device, uploadCmdList, outUploadBufferKeepAlive, pixels.data(), width, height);
        LOG_INFO("Loaded texture (" + std::to_string(width) + "x" + std::to_string(height) + ")");
        return true;
    }

    void Texture::CreateCheckerboard(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
        uint32_t size,
        uint32_t checkerSize)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);

        // Two desaturated neutral tones rather than pure black/white - looks
        // intentional next to the accent-blue object instead of like a raw
        // debug pattern.
        const uint8_t colorA[4] = { 235, 235, 240, 255 };
        const uint8_t colorB[4] = { 60, 65, 75, 255 };

        for (uint32_t y = 0; y < size; ++y)
        {
            for (uint32_t x = 0; x < size; ++x)
            {
                bool isA = ((x / checkerSize) + (y / checkerSize)) % 2 == 0;
                const uint8_t* c = isA ? colorA : colorB;
                size_t offset = (static_cast<size_t>(y) * size + x) * 4;
                pixels[offset + 0] = c[0];
                pixels[offset + 1] = c[1];
                pixels[offset + 2] = c[2];
                pixels[offset + 3] = c[3];
            }
        }

        UploadPixels(device, uploadCmdList, outUploadBufferKeepAlive, pixels.data(), size, size);
    }

    void Texture::CreateSolidColor(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
        uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        // 4x4 rather than 1x1: some hardware/driver combinations are picky
        // about minification/mip behavior on true 1x1 textures, and 4x4 is
        // negligible memory either way.
        constexpr uint32_t kSize = 4;
        std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4);
        for (uint32_t i = 0; i < kSize * kSize; ++i)
        {
            pixels[i * 4 + 0] = r;
            pixels[i * 4 + 1] = g;
            pixels[i * 4 + 2] = b;
            pixels[i * 4 + 3] = a;
        }

        UploadPixels(device, uploadCmdList, outUploadBufferKeepAlive, pixels.data(), kSize, kSize);
    }

    void Texture::UploadPixels(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        ComPtr<ID3D12Resource>& outUploadBufferKeepAlive,
        const uint8_t* rgba,
        uint32_t width,
        uint32_t height)
    {
        m_width = width;
        m_height = height;

        D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1; // Mip generation is a Phase 3 concern (needs a compute or render-pass downsample).
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc = { 1, 0 };
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        DX_CHECK(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource)));
        m_resource->SetName(L"Texture2D");

        UINT64 uploadSize = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &uploadSize);

        D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc = { 1, 0 };
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        DX_CHECK(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outUploadBufferKeepAlive)));

        // The upload buffer's row pitch must be 256-byte aligned per D3D12
        // rules, which is generally NOT the same as the source image's tight
        // row pitch (width * 4) - GetCopyableFootprints already computed the
        // correct aligned pitch for us in footprint.Footprint.RowPitch.
        uint8_t* mapped = nullptr;
        outUploadBufferKeepAlive->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        const uint32_t srcRowPitch = width * 4;
        for (UINT row = 0; row < numRows; ++row)
        {
            std::memcpy(mapped + footprint.Footprint.RowPitch * row, rgba + srcRowPitch * row, srcRowPitch);
        }
        outUploadBufferKeepAlive->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = outUploadBufferKeepAlive.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;

        uploadCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        uploadCmdList->ResourceBarrier(1, &barrier);
    }

    void Texture::CreateShaderResourceView(ID3D12Device* device, DescriptorHeap& heap, uint32_t& outDescriptorIndex)
    {
        outDescriptorIndex = heap.Allocate();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(m_resource.Get(), &srvDesc, heap.GetCpuHandle(outDescriptorIndex));
    }
}
