// File: src/Graphics/Material.cpp
#include "Graphics/Material.h"
#include "Core/Logger.h"
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

namespace gfx
{
    namespace
    {
        bool FileExists(const std::string& path)
        {
            std::ifstream f(path);
            return f.good();
        }

        // Tries basePath + each of the given extensions in turn, returns the
        // first that exists on disk, or an empty string if none do.
        std::string FindWithExtension(const std::string& basePath)
        {
            for (const char* ext : { ".png", ".jpg", ".jpeg", ".bmp" })
            {
                std::string candidate = basePath + ext;
                if (FileExists(candidate)) return candidate;
            }
            return {};
        }

        std::wstring ToWide(const std::string& s)
        {
            return std::wstring(s.begin(), s.end()); // paths here are ASCII by construction (assets/<name>.<ext>)
        }
    }

    bool Material::TryLoad(
        Texture& tex, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
        std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive, const std::string& basePath)
    {
        std::string found = FindWithExtension(basePath);
        if (found.empty()) return false;

        ComPtr<ID3D12Resource> upload;
        bool ok = tex.LoadFromFile(device, cmdList, upload, ToWide(found));
        if (ok) uploadKeepAlive.push_back(upload);
        return ok;
    }

    void Material::Load(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive,
        DescriptorHeap& srvHeap,
        const std::string& assetDir,
        const DirectX::XMFLOAT3* flatAlbedoColor)
    {
        // --- Albedo: flat color if requested, else checkerboard (visually informative if no art exists) ---
        if (!TryLoad(m_albedo, device, uploadCmdList, uploadKeepAlive, assetDir + "/albedo"))
        {
            ComPtr<ID3D12Resource> upload;
            if (flatAlbedoColor)
            {
                auto toByte = [](float c) { return static_cast<uint8_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f); };
                m_albedo.CreateSolidColor(device, uploadCmdList, upload,
                    toByte(flatAlbedoColor->x), toByte(flatAlbedoColor->y), toByte(flatAlbedoColor->z), 255);
            }
            else
            {
                LOG_INFO("No albedo texture found, using procedural checkerboard.");
                m_albedo.CreateCheckerboard(device, uploadCmdList, upload);
            }
            uploadKeepAlive.push_back(upload);
        }

        // --- Normal map: flat tangent-space (0,0,1) = "no bump" ---
        if (!TryLoad(m_normal, device, uploadCmdList, uploadKeepAlive, assetDir + "/normal"))
        {
            ComPtr<ID3D12Resource> upload;
            m_normal.CreateSolidColor(device, uploadCmdList, upload, 128, 128, 255, 255);
            uploadKeepAlive.push_back(upload);
        }

        // --- Metallic-roughness: G=roughness, B=metallic (glTF convention). ---
        // Default: roughness 0.5, metallic 0 - a plausible, non-mirror,
        // non-metal starting point that still shows the BRDF clearly.
        if (!TryLoad(m_metallicRoughness, device, uploadCmdList, uploadKeepAlive, assetDir + "/metallicRoughness"))
        {
            ComPtr<ID3D12Resource> upload;
            m_metallicRoughness.CreateSolidColor(device, uploadCmdList, upload, 0, 128, 0, 255);
            uploadKeepAlive.push_back(upload);
        }

        // --- Ambient occlusion: flat white = "fully unoccluded" ---
        if (!TryLoad(m_ao, device, uploadCmdList, uploadKeepAlive, assetDir + "/ao"))
        {
            ComPtr<ID3D12Resource> upload;
            m_ao.CreateSolidColor(device, uploadCmdList, upload, 255, 255, 255, 255);
            uploadKeepAlive.push_back(upload);
        }

        // Allocate all 4 SRVs back-to-back so GetTableGpuHandle()'s single
        // base handle covers the whole t0..t3 range the root signature expects.
        uint32_t albedoIdx = 0, normalIdx = 0, mrIdx = 0, aoIdx = 0;
        m_albedo.CreateShaderResourceView(device, srvHeap, albedoIdx);
        m_normal.CreateShaderResourceView(device, srvHeap, normalIdx);
        m_metallicRoughness.CreateShaderResourceView(device, srvHeap, mrIdx);
        m_ao.CreateShaderResourceView(device, srvHeap, aoIdx);
        m_baseDescriptorIndex = albedoIdx;
    }
}
