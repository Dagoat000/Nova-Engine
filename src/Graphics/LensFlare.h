#pragma once

#include "Graphics/Buffer.h"
#include "Graphics/DescriptorHeap.h"
#include "Graphics/Pipeline.h"
#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

namespace gfx
{
    using Microsoft::WRL::ComPtr;

    class LensFlare
    {
    public:
        LensFlare() = default;

        void Initialize(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* cmdList,
            DescriptorHeap& srvHeap,
            std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive);

        void Resize(uint32_t width, uint32_t height);

        void Render(
            ID3D12GraphicsCommandList* cmdList,
            DescriptorHeap& srvHeap,
            uint32_t frameIndex,
            DirectX::XMFLOAT2 sunScreenPos,
            float sunVisible,
            DirectX::XMMATRIX viewMatrix,
            DirectX::XMFLOAT3 sunDirectionWS);

        ID3D12Resource* GetOutputResource() const { return m_rayBuffer.Get(); }

    private:
        struct alignas(16) LensInterfaceGpu
        {
            DirectX::XMFLOAT3 center;
            float radius = 0.0f;
            DirectX::XMFLOAT3 n;
            float sa = 0.0f;
            float d1 = 0.0f;
            float flat = 0.0f;
            float pos = 0.0f;
            float w = 0.0f;
        };

        struct alignas(16) GhostDataGpu
        {
            float bounce1 = 0.0f;
            float bounce2 = 0.0f;
            float padding0 = 0.0f;
            float padding1 = 0.0f;
        };

        struct alignas(16) FlareVertexGpu
        {
            DirectX::XMFLOAT4 pos;
            DirectX::XMFLOAT4 color;
            DirectX::XMFLOAT4 coordinates;
            DirectX::XMFLOAT4 reflectance;
        };

        struct alignas(16) LensConstants
        {
            DirectX::XMFLOAT4 lightDirCamera;
            DirectX::XMFLOAT2 sunScreenPos;
            float sunVisible = 0.0f;
            float time = 0.0f;
            float spread = 0.75f;
            float plateSize = 10.0f;
            float numInterfaces = 0.0f;
            float coatingQuality = 1.25f;
            float apertureOpening = 7.0f;
            float numberOfBlades = 5.0f;
            float flareScale = 0.75f;
            float strength = 1.25f;
        };
        D3D12_RESOURCE_STATES m_rayBufferState =
            D3D12_RESOURCE_STATE_COMMON;
        void CreateLensData(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* cmdList,
            std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive);
        void TransitionRayBuffer(
            ID3D12GraphicsCommandList* cmdList,
            D3D12_RESOURCE_STATES newState);
        void CreateRayBuffer(ID3D12Device* device);
        void CreatePatchIndexBuffer(
            ID3D12Device* device,
            ID3D12GraphicsCommandList* cmdList,
            std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive);
        void CreatePipelines(ID3D12Device* device);
        void CreateDescriptors(ID3D12Device* device, DescriptorHeap& srvHeap);

        ComPtr<ID3D12RootSignature> m_computeRootSignature;
        ComPtr<ID3D12PipelineState> m_computePso;
        ComPtr<ID3D12RootSignature> m_graphicsRootSignature;
        ComPtr<ID3D12PipelineState> m_graphicsPso;

        ComPtr<ID3D12Resource> m_lensBuffer;
        ComPtr<ID3D12Resource> m_ghostBuffer;
        ComPtr<ID3D12Resource> m_rayBuffer;
        ComPtr<ID3D12Resource> m_indexBuffer;

        ComPtr<ID3D12Resource> m_lensUpload;
        ComPtr<ID3D12Resource> m_ghostUpload;

        uint32_t m_lensSrvIndex = 0;
        uint32_t m_ghostSrvIndex = 0;
        uint32_t m_raySrvIndex = 0;
        uint32_t m_rayUavIndex = 0;

        D3D12_INDEX_BUFFER_VIEW m_ibv{};

        ConstantBuffer<LensConstants> m_constantBuffer;

        uint32_t m_width = 1;
        uint32_t m_height = 1;
        uint32_t m_lensCount = 0;
        uint32_t m_ghostCount = 0;
        float m_plateSize = 10.0f;
        float m_apertureOpening = 7.0f;
        float m_coatingQuality = 1.25f;
        float m_spread = 0.75f;
        float m_flareScale = 0.75f;
        float m_strength = 1.25f;
        uint32_t m_patchSize = 32;
    };
}
