#include "Graphics/LensFlare.h"
#include "Graphics/DXHelper.h"
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace DirectX;

namespace gfx
{
    namespace
    {
        struct PatentFormat
        {
            float r;
            float d;
            float n;
            bool flat;
            float w;
            float h;
            float coating;
        };

        constexpr float kD6 = 53.142f;
        constexpr float kD10 = 7.063f;
        constexpr float kD14 = 1.532f;
        constexpr float kDAp = 2.800f;
        constexpr float kD20 = 16.889f;
        constexpr float kBf = 39.683f;

        std::vector<PatentFormat> CreateNikon28To75()
        {
            return {
                { 72.747f, 2.300f, 1.60300f, false, 0.2f, 29.0f, 530.0f },
                { 37.000f, 13.000f, 1.00000f, false, 0.2f, 29.0f, 600.0f },
                {-172.809f, 2.100f, 1.58913f, false, 2.7f, 26.2f, 570.0f },
                { 39.894f, 1.000f, 1.00000f, false, 2.7f, 26.2f, 660.0f },
                { 49.820f, 4.400f, 1.86074f, false, 0.5f, 20.0f, 330.0f },
                { 74.750f, kD6, 1.00000f, false, 0.5f, 20.0f, 544.0f },
                { 63.402f, 1.600f, 1.86074f, false, 0.5f, 16.1f, 740.0f },
                { 37.530f, 8.600f, 1.51680f, false, 0.5f, 16.1f, 411.0f },
                {-75.887f, 1.600f, 1.80458f, false, 0.5f, 16.0f, 580.0f },
                {-97.792f, kD10, 1.00000f, false, 0.5f, 16.5f, 730.0f },
                { 96.034f, 3.600f, 1.62041f, false, 0.5f, 18.0f, 700.0f },
                {261.743f, 0.100f, 1.00000f, false, 0.5f, 18.0f, 440.0f },
                { 54.262f, 6.000f, 1.69680f, false, 0.5f, 18.0f, 800.0f },
                {-5995.277f, kD14, 1.00000f, false, 0.5f, 18.0f, 300.0f },
                { 0.0f, kDAp, 1.00000f, true, 18.0f, 7.0f, 440.0f },
                {-74.414f, 2.200f, 1.90265f, false, 0.5f, 13.0f, 500.0f },
                {-62.929f, 1.450f, 1.51680f, false, 0.1f, 13.0f, 770.0f },
                {121.380f, 2.500f, 1.00000f, false, 4.0f, 13.1f, 820.0f },
                {-85.723f, 1.400f, 1.49782f, false, 4.0f, 13.0f, 200.0f },
                {31.093f, 2.600f, 1.80458f, false, 4.0f, 13.1f, 540.0f },
                {84.758f, kD20, 1.00000f, false, 0.5f, 13.0f, 580.0f },
                {459.690f, 1.400f, 1.86074f, false, 1.0f, 15.0f, 533.0f },
                {40.240f, 7.300f, 1.49782f, false, 1.0f, 15.0f, 666.0f },
                {-49.771f, 0.100f, 1.00000f, false, 1.0f, 15.2f, 500.0f },
                {62.369f, 7.000f, 1.67025f, false, 1.0f, 16.0f, 487.0f },
                {-76.454f, 5.200f, 1.00000f, false, 1.0f, 16.0f, 671.0f },
                {-32.524f, 2.000f, 1.80454f, false, 0.5f, 17.0f, 487.0f },
                {-50.194f, kBf, 1.00000f, false, 0.5f, 17.0f, 732.0f },
                {0.0f, 5.0f, 1.00000f, true, 10.0f, 10.0f, 500.0f }
            };
        }
    }

    void LensFlare::Initialize(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        DescriptorHeap& srvHeap,
        std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive)
    {
        CreateLensData(device, cmdList, uploadKeepAlive);
        m_constantBuffer.Create(device, 3, L"LensFlareConstants");
        CreateRayBuffer(device);
        CreatePatchIndexBuffer(device, cmdList, uploadKeepAlive);
        CreatePipelines(device);
        CreateDescriptors(device, srvHeap);
    }

    void LensFlare::Resize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
    }

    void LensFlare::CreateLensData(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive)
    {
        const auto patent = CreateNikon28To75();
        const int apertureId = 14;

        std::vector<LensInterfaceGpu> interfaces(patent.size());
        float totalDistance = 0.0f;

        for (int i = static_cast<int>(patent.size()) - 1; i >= 0; --i)
        {
            const PatentFormat& entry = patent[i];
            totalDistance += entry.d;
            const float leftIor = i == 0 ? 1.0f : patent[i - 1].n;
            const float rightIor = entry.n;

            LensInterfaceGpu out{};
            out.center = { 0.0f, 0.0f, totalDistance - entry.r };
            out.radius = entry.r;
            out.n = { leftIor, 1.0f, rightIor };
            out.sa = entry.h;
            out.d1 = entry.coating;
            out.flat = entry.flat ? 1.0f : 0.0f;
            out.pos = totalDistance;
            out.w = entry.w;
            interfaces[i] = out;
        }

        interfaces[apertureId].sa = m_apertureOpening;
        m_plateSize = interfaces.back().sa;
        m_lensCount = static_cast<uint32_t>(interfaces.size());

        std::vector<GhostDataGpu> ghosts;
        for (int bounce1 = 2, bounce2 = 1;; ++bounce1)
        {
            if (bounce1 >= static_cast<int>(interfaces.size()) - 1)
            {
                ++bounce2;
                bounce1 = bounce2 + 1;
            }
            if (bounce2 >= static_cast<int>(interfaces.size()) - 1)
                break;
            GhostDataGpu g{};
            g.bounce1 = static_cast<float>(bounce1);
            g.bounce2 = static_cast<float>(bounce2);
            ghosts.push_back(g);
        }

        m_ghostCount = static_cast<uint32_t>(ghosts.size());

        ComPtr<ID3D12Resource> lensUpload;
        GpuBuffer lens = GpuBuffer::Create(
            device, cmdList, interfaces.data(), interfaces.size() * sizeof(LensInterfaceGpu),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, lensUpload, L"LensFlareLensInterfaces");
        m_lensBuffer = lens.Get();
        m_lensUpload = lensUpload;
        uploadKeepAlive.push_back(lensUpload);

        ComPtr<ID3D12Resource> ghostUpload;
        GpuBuffer ghost = GpuBuffer::Create(
            device, cmdList, ghosts.data(), ghosts.size() * sizeof(GhostDataGpu),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ghostUpload, L"LensFlareGhostData");
        m_ghostBuffer = ghost.Get();
        m_ghostUpload = ghostUpload;
        uploadKeepAlive.push_back(ghostUpload);
    }

    void LensFlare::CreateRayBuffer(ID3D12Device* device)
    {
        const uint64_t count = static_cast<uint64_t>(m_ghostCount) * m_patchSize * m_patchSize;
        const uint64_t bytes = count * sizeof(FlareVertexGpu);

        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc = { 1, 0 };
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        DX_CHECK(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_rayBuffer)));

        m_rayBufferState = D3D12_RESOURCE_STATE_COMMON;
        m_rayBuffer->SetName(L"LensFlareRayBundle");
    }
    void LensFlare::TransitionRayBuffer(
        ID3D12GraphicsCommandList* cmdList,
        D3D12_RESOURCE_STATES newState)
    {
        if (m_rayBufferState == newState)
            return;

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_rayBuffer.Get();
        barrier.Transition.StateBefore = m_rayBufferState;
        barrier.Transition.StateAfter = newState;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmdList->ResourceBarrier(1, &barrier);

        m_rayBufferState = newState;
    }
    void LensFlare::CreatePatchIndexBuffer(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive)
    {
        std::vector<uint32_t> indices;
        indices.reserve((m_patchSize - 1) * (m_patchSize - 1) * 6);

        for (uint32_t y = 0; y < m_patchSize - 1; ++y)
        {
            for (uint32_t x = 0; x < m_patchSize - 1; ++x)
            {
                const uint32_t i1 = y * m_patchSize + x;
                const uint32_t i2 = i1 + 1;
                const uint32_t i3 = i1 + m_patchSize;
                const uint32_t i4 = i2 + m_patchSize;
                indices.push_back(i3);
                indices.push_back(i1);
                indices.push_back(i2);
                indices.push_back(i2);
                indices.push_back(i4);
                indices.push_back(i3);
            }
        }

        ComPtr<ID3D12Resource> upload;
        GpuBuffer ib = GpuBuffer::Create(
            device, cmdList, indices.data(), indices.size() * sizeof(uint32_t),
            D3D12_RESOURCE_STATE_INDEX_BUFFER, upload, L"LensFlarePatchIndexBuffer");
        m_indexBuffer = ib.Get();
        uploadKeepAlive.push_back(upload);

        m_ibv.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
        m_ibv.SizeInBytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        m_ibv.Format = DXGI_FORMAT_R32_UINT;
    }

    void LensFlare::CreatePipelines(ID3D12Device* device)
    {
        D3D12_DESCRIPTOR_RANGE1 computeSrvs{};

        computeSrvs.RangeType =
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        computeSrvs.NumDescriptors = 2;
        computeSrvs.BaseShaderRegister = 0;
        computeSrvs.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 computeUav{};
        computeUav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        computeUav.NumDescriptors = 1;
        computeUav.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER1 cp[3]{};
        cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cp[0].Descriptor.ShaderRegister = 0;
        cp[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        cp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cp[1].DescriptorTable.NumDescriptorRanges = 1;
        cp[1].DescriptorTable.pDescriptorRanges = &computeSrvs;
        cp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cp[2].DescriptorTable.NumDescriptorRanges = 1;
        cp[2].DescriptorTable.pDescriptorRanges = &computeUav;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC csDesc{};
        csDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        csDesc.Desc_1_1.NumParameters = 3;
        csDesc.Desc_1_1.pParameters = cp;
        csDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3D10Blob> blob, error;
        DX_CHECK(D3D12SerializeVersionedRootSignature(&csDesc, &blob, &error));
        DX_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature)));

        ComPtr<ID3D10Blob> cs = CompileShader(L"shaders/LensFlareCS.hlsl", "CS", "cs_5_1");
        D3D12_COMPUTE_PIPELINE_STATE_DESC cps{};
        cps.pRootSignature = m_computeRootSignature.Get();
        cps.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        DX_CHECK(device->CreateComputePipelineState(&cps, IID_PPV_ARGS(&m_computePso)));

        D3D12_DESCRIPTOR_RANGE1 graphicsSrv{};
        graphicsSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        graphicsSrv.NumDescriptors = 1;
        graphicsSrv.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER1 gp[2]{};
        gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        gp[0].Descriptor.ShaderRegister = 0;
        gp[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        gp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        gp[1].DescriptorTable.NumDescriptorRanges = 1;
        gp[1].DescriptorTable.pDescriptorRanges = &graphicsSrv;
        gp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC gDesc{};
        gDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        gDesc.Desc_1_1.NumParameters = 2;
        gDesc.Desc_1_1.pParameters = gp;
        gDesc.Desc_1_1.NumStaticSamplers = 1;
        gDesc.Desc_1_1.pStaticSamplers = &sampler;
        gDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        blob.Reset();
        error.Reset();
        DX_CHECK(D3D12SerializeVersionedRootSignature(&gDesc, &blob, &error));
        DX_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_graphicsRootSignature)));

        ComPtr<ID3D10Blob> vs = CompileShader(L"shaders/LensFlareComposite.hlsl", "VS", "vs_5_1");
        ComPtr<ID3D10Blob> ps = CompileShader(L"shaders/LensFlareComposite.hlsl", "PS", "ps_5_1");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_graphicsRootSignature.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.SampleDesc = { 1, 0 };
        DX_CHECK(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_graphicsPso)));
    }

    void LensFlare::CreateDescriptors(ID3D12Device* device, DescriptorHeap& srvHeap)
    {
        m_lensSrvIndex = srvHeap.Allocate();
        m_ghostSrvIndex = srvHeap.Allocate();
        m_raySrvIndex = srvHeap.Allocate();
        m_rayUavIndex = srvHeap.Allocate();

        D3D12_SHADER_RESOURCE_VIEW_DESC lensSrv{};
        lensSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        lensSrv.Format = DXGI_FORMAT_UNKNOWN;
        lensSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lensSrv.Buffer.FirstElement = 0;
        lensSrv.Buffer.NumElements = m_lensCount;
        lensSrv.Buffer.StructureByteStride = sizeof(LensInterfaceGpu);
        device->CreateShaderResourceView(m_lensBuffer.Get(), &lensSrv, srvHeap.GetCpuHandle(m_lensSrvIndex));

        D3D12_SHADER_RESOURCE_VIEW_DESC ghostSrv{};
        ghostSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        ghostSrv.Format = DXGI_FORMAT_UNKNOWN;
        ghostSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ghostSrv.Buffer.NumElements = m_ghostCount;
        ghostSrv.Buffer.StructureByteStride = sizeof(GhostDataGpu);
        device->CreateShaderResourceView(m_ghostBuffer.Get(), &ghostSrv, srvHeap.GetCpuHandle(m_ghostSrvIndex));

        D3D12_SHADER_RESOURCE_VIEW_DESC raySrv{};
        raySrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        raySrv.Format = DXGI_FORMAT_UNKNOWN;
        raySrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raySrv.Buffer.NumElements = m_ghostCount * m_patchSize * m_patchSize;
        raySrv.Buffer.StructureByteStride = sizeof(FlareVertexGpu);
        device->CreateShaderResourceView(m_rayBuffer.Get(), &raySrv, srvHeap.GetCpuHandle(m_raySrvIndex));

        D3D12_UNORDERED_ACCESS_VIEW_DESC rayUav{};
        rayUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        rayUav.Format = DXGI_FORMAT_UNKNOWN;
        rayUav.Buffer.NumElements = m_ghostCount * m_patchSize * m_patchSize;
        rayUav.Buffer.StructureByteStride = sizeof(FlareVertexGpu);
        device->CreateUnorderedAccessView(m_rayBuffer.Get(), nullptr, &rayUav, srvHeap.GetCpuHandle(m_rayUavIndex));
    }

    void LensFlare::Render(
        ID3D12GraphicsCommandList* cmdList,
        DescriptorHeap& srvHeap,
        uint32_t frameIndex,
        XMFLOAT2 sunScreenPos,
        float sunVisible,
        XMMATRIX viewMatrix,
        XMFLOAT3 sunDirectionWS)
    {
        const XMVECTOR sunWS = XMVector3Normalize(XMLoadFloat3(&sunDirectionWS));
        XMVECTOR sunCamera = XMVector3Normalize(XMVector3TransformNormal(sunWS, viewMatrix));
        sunCamera = XMVectorNegate(sunCamera);

        XMFLOAT3 lightDirCamera;
        XMStoreFloat3(&lightDirCamera, sunCamera);

        LensConstants constants{};
        constants.lightDirCamera = { lightDirCamera.x, lightDirCamera.y, lightDirCamera.z, 0.0f };
        constants.sunScreenPos = sunScreenPos;
        constants.sunVisible = sunVisible;
        constants.spread = m_spread;
        constants.plateSize = m_plateSize;
        constants.numInterfaces = static_cast<float>(m_lensCount);
        constants.coatingQuality = m_coatingQuality;
        constants.apertureOpening = m_apertureOpening;
        constants.numberOfBlades = 5.0f;
        constants.flareScale = m_flareScale;
        constants.strength = m_strength;
        m_constantBuffer.Update(frameIndex, constants);
        TransitionRayBuffer(
            cmdList,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->SetPipelineState(m_computePso.Get());
        cmdList->SetComputeRootSignature(m_computeRootSignature.Get());
        cmdList->SetComputeRootConstantBufferView(0, m_constantBuffer.GetGpuAddress(frameIndex));
        cmdList->SetComputeRootDescriptorTable(1, srvHeap.GetGpuHandle(m_lensSrvIndex));
        cmdList->SetComputeRootDescriptorTable(2, srvHeap.GetGpuHandle(m_rayUavIndex));
        cmdList->Dispatch(m_ghostCount, 1, 1);
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = m_rayBuffer.Get();
        cmdList->ResourceBarrier(1, &uavBarrier);

        TransitionRayBuffer(
            cmdList,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        cmdList->SetPipelineState(m_graphicsPso.Get());
        cmdList->SetGraphicsRootSignature(m_graphicsRootSignature.Get());
        cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer.GetGpuAddress(frameIndex));
        cmdList->SetGraphicsRootDescriptorTable(1, srvHeap.GetGpuHandle(m_raySrvIndex));
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetIndexBuffer(&m_ibv);
        cmdList->DrawIndexedInstanced((m_patchSize - 1) * (m_patchSize - 1) * 6, m_ghostCount, 0, 0, 0);

        TransitionRayBuffer(
            cmdList,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

}
