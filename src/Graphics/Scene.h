// File: src/Graphics/Scene.h
//
// A minimal, fixed-size scene: a handful of RenderObjects, each with its
// own mesh (vertex/index buffer), material, and transform. This replaces
// the single hardcoded mesh every earlier phase rendered - not a general
// entity-component scene graph (that's further out than DXR reflections
// need), just enough structure for the G-buffer pass and the DXR
// acceleration structures to both iterate the same object list.
#pragma once

#include "Graphics/Material.h"
#include "Graphics/Buffer.h"
#include "Rendering/Mesh.h"
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <string>

namespace gfx
{
    class DescriptorHeap;

    struct RenderObject
    {
        ComPtr<ID3D12Resource> vertexBuffer;
        ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        D3D12_INDEX_BUFFER_VIEW ibv{};
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0; // needed by the DXR BLAS geometry desc, which the index buffer alone doesn't give us

        std::unique_ptr<Material> material;

        DirectX::XMFLOAT3 position{ 0, 0, 0 };
        float uniformScale = 1.0f;
        bool rotatesWithTime = false;
        float rotationSpeed = 0.0f;

        DirectX::XMMATRIX GetWorldMatrix(double totalTimeSeconds) const
        {
            using namespace DirectX;
            XMMATRIX scale = XMMatrixScaling(uniformScale, uniformScale, uniformScale);
            XMMATRIX rotate = rotatesWithTime
                ? XMMatrixRotationY(static_cast<float>(totalTimeSeconds) * rotationSpeed) * XMMatrixRotationX(0.15f)
                : XMMatrixIdentity();
            XMMATRIX translate = XMMatrixTranslation(position.x, position.y, position.z);
            return scale * rotate * translate;
        }
    };

    class Scene
    {
    public:
        // Builds the default demo scene (floor + the showcase mesh + two
        // spheres) and uploads every object's geometry + material textures
        // in one batch on cmdList. Call once during startup, before Flush().
        void Load(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* cmdList,
            std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive,
            DescriptorHeap& srvHeap);

        std::vector<RenderObject>& GetObjects() { return m_objects; }
        const std::vector<RenderObject>& GetObjects() const { return m_objects; }

    private:
        void AddObject(
            ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
            std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive, DescriptorHeap& srvHeap,
            rendering::MeshData mesh, const std::string& assetDir, const DirectX::XMFLOAT3* flatAlbedo,
            DirectX::XMFLOAT3 position, float scale, float metallic, float roughness,
            bool rotates = false, float rotationSpeed = 0.0f);

        std::vector<RenderObject> m_objects;
    };
}
