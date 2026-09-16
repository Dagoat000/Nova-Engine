// File: src/Graphics/Scene.cpp
#include "Graphics/Scene.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"
#include <string>
#include <vector>
#include <memory>
#include <utility>

namespace gfx
{
    void Scene::AddObject(
        ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
        std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive, DescriptorHeap& srvHeap,
        rendering::MeshData mesh, const std::string& assetDir, const DirectX::XMFLOAT3* flatAlbedo,
        DirectX::XMFLOAT3 position, float scale, float metallic, float roughness,
        bool rotates, float rotationSpeed)
    {
        rendering::ComputeTangents(mesh);

        RenderObject obj;
        obj.indexCount = static_cast<uint32_t>(mesh.indices.size());
        obj.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        obj.position = position;
        obj.uniformScale = scale;
        obj.rotatesWithTime = rotates;
        obj.rotationSpeed = rotationSpeed;

        ComPtr<ID3D12Resource> vbUpload, ibUpload;
        GpuBuffer vb = GpuBuffer::Create(
            device, cmdList, mesh.vertices.data(), mesh.vertices.size() * sizeof(rendering::Vertex),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, vbUpload, L"SceneObjectVB");
        GpuBuffer ib = GpuBuffer::Create(
            device, cmdList, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t),
            D3D12_RESOURCE_STATE_INDEX_BUFFER, ibUpload, L"SceneObjectIB");
        uploadKeepAlive.push_back(vbUpload);
        uploadKeepAlive.push_back(ibUpload);

        obj.vertexBuffer = vb.Get();
        obj.indexBuffer = ib.Get();
        obj.vbv.BufferLocation = obj.vertexBuffer->GetGPUVirtualAddress();
        obj.vbv.SizeInBytes = static_cast<UINT>(mesh.vertices.size() * sizeof(rendering::Vertex));
        obj.vbv.StrideInBytes = sizeof(rendering::Vertex);
        obj.ibv.BufferLocation = obj.indexBuffer->GetGPUVirtualAddress();
        obj.ibv.SizeInBytes = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
        obj.ibv.Format = DXGI_FORMAT_R32_UINT;

        obj.material = std::make_unique<Material>();
        obj.material->Load(device, cmdList, uploadKeepAlive, srvHeap, assetDir, flatAlbedo);
        obj.material->factors.metallicFactor = metallic;
        obj.material->factors.roughnessFactor = roughness;

        m_objects.push_back(std::move(obj));
    }

    void Scene::Load(
        ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
        std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive, DescriptorHeap& srvHeap)
    {
        using namespace DirectX;

        // --- Floor: large plane, checkerboard for scale/grounding, mostly rough ---
        AddObject(device, cmdList, uploadKeepAlive, srvHeap,
            rendering::CreatePlane(14.0f, 14.0f, 6.0f), "assets/floor", nullptr,
            { 0.0f, -1.0f, 0.0f }, 1.0f, /*metallic*/ 0.0f, /*roughness*/ 0.85f);

        // --- Showcase object: the original rotating mesh (OBJ if present, else a cube) ---
        rendering::MeshData showcaseMesh;
        std::string showcaseAssetDir = "assets";
        if (!rendering::LoadObj("assets/model.obj", showcaseMesh))
        {
            LOG_INFO("assets/model.obj not found, using built-in textured cube.");
            showcaseMesh = rendering::CreateTexturedCube(1.0f);
        }
        else
        {
            LOG_INFO("Loaded assets/model.obj (" + std::to_string(showcaseMesh.vertices.size()) + " vertices).");
        }
        AddObject(device, cmdList, uploadKeepAlive, srvHeap,
            std::move(showcaseMesh), showcaseAssetDir, nullptr,
            { 0.0f, 0.3f, 0.0f }, 1.0f, /*metallic*/ 0.0f, /*roughness*/ 0.5f,
            /*rotates*/ true, /*rotationSpeed*/ 0.8f);

        // --- Mirror sphere: the hero object for ray-traced reflections ---
        XMFLOAT3 mirrorColor{ 0.92f, 0.92f, 0.95f };
        AddObject(device, cmdList, uploadKeepAlive, srvHeap,
            rendering::CreateUVSphere(0.7f), "assets/mirror", &mirrorColor,
            { -2.2f, -0.3f, -0.5f }, 1.0f, /*metallic*/ 1.0f, /*roughness*/ 0.03f);

        // --- Gold sphere: rougher metal, stays on the prefiltered-IBL path (see the
        //     RT roughness threshold in DeferredLightingPS.hlsl) - a deliberate visual
        //     contrast against the mirror sphere's sharp ray-traced reflection. ---
        XMFLOAT3 goldColor{ 0.83f, 0.68f, 0.30f };
        AddObject(device, cmdList, uploadKeepAlive, srvHeap,
            rendering::CreateUVSphere(0.7f), "assets/gold", &goldColor,
            { 2.2f, -0.3f, -0.5f }, 1.0f, /*metallic*/ 1.0f, /*roughness*/ 0.35f);
    }
}
