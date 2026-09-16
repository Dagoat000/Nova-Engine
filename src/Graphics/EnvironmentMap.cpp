// File: src/Graphics/EnvironmentMap.cpp
#include "Graphics/EnvironmentMap.h"
#include "Graphics/DescriptorHeap.h"
#include "Graphics/Pipeline.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"
#include <string>
#include <vector>
#include <cstring>
#include <memory>

namespace gfx
{
    namespace
    {
        // Read-by-both-stages state for a resource sampled by both compute
        // (irradiance/prefilter passes reading the sky) and pixel shaders
        // (the lighting pass reading all 4 final maps).
        constexpr D3D12_RESOURCE_STATES kShaderReadAll =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        ComPtr<ID3D12Resource> CreateCubemapArray(ID3D12Device* device, DXGI_FORMAT format, uint32_t size, uint32_t mipLevels, const wchar_t* name)
        {
            D3D12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = size;
            desc.Height = size;
            desc.DepthOrArraySize = 6; // 6 cubemap faces, addressed as a Texture2DArray for UAV access
            desc.MipLevels = static_cast<UINT16>(mipLevels);
            desc.Format = format;
            desc.SampleDesc = { 1, 0 };
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            ComPtr<ID3D12Resource> resource;
            DX_CHECK(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)));
            resource->SetName(name);
            return resource;
        }

        ComPtr<ID3D12RootSignature> SerializeRootSignature(ID3D12Device* device, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, const wchar_t* name)
        {
            ComPtr<ID3D10Blob> signatureBlob;
            ComPtr<ID3D10Blob> errorBlob;
            HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &signatureBlob, &errorBlob);
            if (FAILED(hr))
            {
                if (errorBlob)
                {
                    LOG_ERROR(std::string("Root signature serialize error: ") + static_cast<const char*>(errorBlob->GetBufferPointer()));
                }
                DX_CHECK(hr);
            }
            ComPtr<ID3D12RootSignature> rootSignature;
            DX_CHECK(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
            rootSignature->SetName(name);
            return rootSignature;
        }

        ComPtr<ID3D12PipelineState> CreateComputePso(ID3D12Device* device, ID3D12RootSignature* rootSig, const wchar_t* shaderPath, const wchar_t* name)
        {
            ComPtr<ID3D10Blob> cs = CompileShader(shaderPath, "main", "cs_5_1");
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = rootSig;
            desc.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
            ComPtr<ID3D12PipelineState> pso;
            DX_CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
            pso->SetName(name);
            return pso;
        }

        // Every precompute root signature shares this shape: one or two CBVs,
        // optionally an SRV table + sampler, and a single-descriptor UAV
        // table - so a small builder keeps the 4 near-identical definitions
        // from turning into 300 lines of copy-pasted D3D12_ROOT_PARAMETER1 setup.
        struct RootSigDesc
        {
            uint32_t numCbvs = 0;      // registers b0..b(numCbvs-1)
            bool hasSrv = false;        // t0, if true
            uint32_t numUavDescriptors = 1; // u0 table size (always 1 here - one slice per dispatch)
        };

        ComPtr<ID3D12RootSignature> BuildRootSignature(ID3D12Device* device, const RootSigDesc& d, const wchar_t* name)
        {
            std::vector<D3D12_ROOT_PARAMETER1> params;
            D3D12_DESCRIPTOR_RANGE1 srvRange{};
            D3D12_DESCRIPTOR_RANGE1 uavRange{};

            for (uint32_t i = 0; i < d.numCbvs; ++i)
            {
                D3D12_ROOT_PARAMETER1 p{};
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                p.Descriptor.ShaderRegister = i;
                p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                params.push_back(p);
            }

            if (d.hasSrv)
            {
                srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                srvRange.NumDescriptors = 1;
                srvRange.BaseShaderRegister = 0; // t0
                srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
                srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                D3D12_ROOT_PARAMETER1 p{};
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                p.DescriptorTable.NumDescriptorRanges = 1;
                p.DescriptorTable.pDescriptorRanges = &srvRange;
                p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                params.push_back(p);
            }

            uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            uavRange.NumDescriptors = d.numUavDescriptors;
            uavRange.BaseShaderRegister = 0; // u0
            uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE; // rebound to a different heap slot per dispatch
            uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            D3D12_ROOT_PARAMETER1 uavParam{};
            uavParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            uavParam.DescriptorTable.NumDescriptorRanges = 1;
            uavParam.DescriptorTable.pDescriptorRanges = &uavRange;
            uavParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.push_back(uavParam);

            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = 0; // s0
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
            desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            desc.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
            desc.Desc_1_1.pParameters = params.data();
            desc.Desc_1_1.NumStaticSamplers = d.hasSrv ? 1 : 0;
            desc.Desc_1_1.pStaticSamplers = d.hasSrv ? &sampler : nullptr;

            return SerializeRootSignature(device, desc, name);
        }
    }

    EnvironmentMap::EnvironmentMap(ID3D12Device* device)
    {
        m_sky = CreateCubemapArray(device, kEnvMapFormat, kSkyResolution, 1, L"EnvSky");
        m_irradiance = CreateCubemapArray(device, kEnvMapFormat, kIrradianceResolution, 1, L"EnvIrradiance");
        m_prefiltered = CreateCubemapArray(device, kEnvMapFormat, kPrefilterBaseResolution, kPrefilterMipCount, L"EnvPrefiltered");

        D3D12_HEAP_PROPERTIES lutHeapProps{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC lutDesc{};
        lutDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        lutDesc.Width = kBrdfLutResolution;
        lutDesc.Height = kBrdfLutResolution;
        lutDesc.DepthOrArraySize = 1;
        lutDesc.MipLevels = 1;
        lutDesc.Format = kBrdfLutFormat;
        lutDesc.SampleDesc = { 1, 0 };
        lutDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        DX_CHECK(device->CreateCommittedResource(
            &lutHeapProps, D3D12_HEAP_FLAG_NONE, &lutDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_brdfLut)));
        m_brdfLut->SetName(L"EnvBrdfLut");

        m_skyRootSig = BuildRootSignature(device, { /*numCbvs*/ 2, /*hasSrv*/ false, /*numUav*/ 1 }, L"SkyRootSig");
        m_skyPso = CreateComputePso(device, m_skyRootSig.Get(), L"shaders/SkyCS.hlsl", L"SkyPSO");

        m_irradianceRootSig = BuildRootSignature(device, { 1, true, 1 }, L"IrradianceRootSig");
        m_irradiancePso = CreateComputePso(device, m_irradianceRootSig.Get(), L"shaders/IrradianceCS.hlsl", L"IrradiancePSO");

        m_prefilterRootSig = BuildRootSignature(device, { 1, true, 1 }, L"PrefilterRootSig");
        m_prefilterPso = CreateComputePso(device, m_prefilterRootSig.Get(), L"shaders/PrefilterCS.hlsl", L"PrefilterPSO");

        m_brdfLutRootSig = BuildRootSignature(device, { 1, false, 1 }, L"BrdfLutRootSig");
        m_brdfLutPso = CreateComputePso(device, m_brdfLutRootSig.Get(), L"shaders/BrdfLutCS.hlsl", L"BrdfLutPSO");

        // 6 sky faces + 6 irradiance faces + 6*kPrefilterMipCount prefilter
        // slices + 1 BRDF LUT + 1 sky-as-SRV (read by irradiance/prefilter) -
        // all allocated up front from one small heap.
        const uint32_t uavCount = 6 + 6 + 6 * kPrefilterMipCount + 1;
        const uint32_t heapCapacity = uavCount + 1 /* sky SRV */;
        m_uavHeap = std::make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, heapCapacity, /*shaderVisible*/ true);
    }

    void EnvironmentMap::Generate(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::XMFLOAT3 sunDirection, DirectX::XMFLOAT3 sunColor, float sunIntensity)
    {
        using namespace DirectX;

        ID3D12DescriptorHeap* heaps[] = { m_uavHeap->Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        // --- Allocate all descriptor slots up front (fixed layout) ---
        uint32_t skyFaceUav[6], irradianceFaceUav[6], prefilterUav[kPrefilterMipCount][6];
        for (int i = 0; i < 6; ++i) skyFaceUav[i] = m_uavHeap->Allocate();
        for (int i = 0; i < 6; ++i) irradianceFaceUav[i] = m_uavHeap->Allocate();
        for (uint32_t m = 0; m < kPrefilterMipCount; ++m)
            for (int f = 0; f < 6; ++f) prefilterUav[m][f] = m_uavHeap->Allocate();
        uint32_t brdfLutUav = m_uavHeap->Allocate();
        uint32_t skySrvIndex = m_uavHeap->Allocate();

        // --- Create the actual views at those slots ---
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uavDesc.Texture2DArray.ArraySize = 1;

        uavDesc.Format = kEnvMapFormat;
        uavDesc.Texture2DArray.MipSlice = 0;
        for (int i = 0; i < 6; ++i)
        {
            uavDesc.Texture2DArray.FirstArraySlice = i;
            device->CreateUnorderedAccessView(m_sky.Get(), nullptr, &uavDesc, m_uavHeap->GetCpuHandle(skyFaceUav[i]));
        }
        for (int i = 0; i < 6; ++i)
        {
            uavDesc.Texture2DArray.FirstArraySlice = i;
            device->CreateUnorderedAccessView(m_irradiance.Get(), nullptr, &uavDesc, m_uavHeap->GetCpuHandle(irradianceFaceUav[i]));
        }
        for (uint32_t m = 0; m < kPrefilterMipCount; ++m)
        {
            uavDesc.Texture2DArray.MipSlice = m;
            for (int f = 0; f < 6; ++f)
            {
                uavDesc.Texture2DArray.FirstArraySlice = f;
                device->CreateUnorderedAccessView(m_prefiltered.Get(), nullptr, &uavDesc, m_uavHeap->GetCpuHandle(prefilterUav[m][f]));
            }
        }
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC lutUavDesc{};
            lutUavDesc.Format = kBrdfLutFormat;
            lutUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(m_brdfLut.Get(), nullptr, &lutUavDesc, m_uavHeap->GetCpuHandle(brdfLutUav));
        }
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC skySrvDesc{};
            skySrvDesc.Format = kEnvMapFormat;
            skySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            skySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            skySrvDesc.TextureCube.MipLevels = 1;
            device->CreateShaderResourceView(m_sky.Get(), &skySrvDesc, m_uavHeap->GetCpuHandle(skySrvIndex));
        }

        // --- Tiny upload-heap constant buffers for this one-time precompute.
        // These don't need the triple-buffered ConstantBuffer<T> machinery -
        // nothing here runs more than once per resource. ---
        auto makeUploadCb = [&](const void* data, size_t size, const wchar_t* name) -> ComPtr<ID3D12Resource>
        {
            size_t aligned = (size + 255) & ~size_t(255);
            D3D12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = aligned;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc = { 1, 0 };
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> resource;
            DX_CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)));
            resource->SetName(name);
            void* mapped = nullptr;
            resource->Map(0, nullptr, &mapped);
            std::memcpy(mapped, data, size);
            resource->Unmap(0, nullptr);
            return resource;
        };

        struct SkyConstants { XMFLOAT3 sunDir; float sunIntensity; XMFLOAT3 sunColor; float sunSharpness; XMFLOAT3 horizon; float _p0; XMFLOAT3 zenith; float _p1; };
        SkyConstants skyConstants{ sunDirection, sunIntensity, sunColor, 256.0f, { 0.55f, 0.62f, 0.72f }, 0.0f, { 0.10f, 0.22f, 0.45f }, 0.0f };
        m_startupUploadsLeakedIntentionally.push_back(makeUploadCb(&skyConstants, sizeof(skyConstants), L"SkyConstantsCB"));

        // --- Pass 1: sky (6 dispatches, one per face) ---
        cmdList->SetPipelineState(m_skyPso.Get());
        cmdList->SetComputeRootSignature(m_skyRootSig.Get());
        cmdList->SetComputeRootConstantBufferView(0, m_startupUploadsLeakedIntentionally.back()->GetGPUVirtualAddress());

        const uint32_t threadGroups = (kSkyResolution + 7) / 8;
        for (uint32_t face = 0; face < 6; ++face)
        {
            struct FaceConstants { uint32_t faceIndex; uint32_t resolution; } fc{ face, kSkyResolution };
            m_startupUploadsLeakedIntentionally.push_back(makeUploadCb(&fc, sizeof(fc), L"SkyFaceCB"));
            cmdList->SetComputeRootConstantBufferView(1, m_startupUploadsLeakedIntentionally.back()->GetGPUVirtualAddress());
            cmdList->SetComputeRootDescriptorTable(2, m_uavHeap->GetGpuHandle(skyFaceUav[face]));
            cmdList->Dispatch(threadGroups, threadGroups, 1);
        }

        // Sky must finish writing AND transition out of UNORDERED_ACCESS
        // before the next two passes can sample it as an SRV - a resource
        // transition barrier is itself a full sync point (it makes prior
        // writes visible before the state change takes effect), so no
        // separate UAV barrier is needed on top of it here.
        D3D12_RESOURCE_BARRIER skyToShaderRead{};
        skyToShaderRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        skyToShaderRead.Transition.pResource = m_sky.Get();
        skyToShaderRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        skyToShaderRead.Transition.StateAfter = kShaderReadAll;
        skyToShaderRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &skyToShaderRead);

        // --- Pass 2: diffuse irradiance convolution (6 dispatches) ---
        cmdList->SetPipelineState(m_irradiancePso.Get());
        cmdList->SetComputeRootSignature(m_irradianceRootSig.Get());
        const uint32_t irradianceGroups = (kIrradianceResolution + 7) / 8;
        for (uint32_t face = 0; face < 6; ++face)
        {
            struct FaceConstants { uint32_t faceIndex; uint32_t resolution; } fc{ face, kIrradianceResolution };
            m_startupUploadsLeakedIntentionally.push_back(makeUploadCb(&fc, sizeof(fc), L"IrradianceFaceCB"));
            cmdList->SetComputeRootConstantBufferView(0, m_startupUploadsLeakedIntentionally.back()->GetGPUVirtualAddress());
            cmdList->SetComputeRootDescriptorTable(1, m_uavHeap->GetGpuHandle(skySrvIndex));
            cmdList->SetComputeRootDescriptorTable(2, m_uavHeap->GetGpuHandle(irradianceFaceUav[face]));
            cmdList->Dispatch(irradianceGroups, irradianceGroups, 1);
        }

        // --- Pass 3: specular prefilter (6 faces x kPrefilterMipCount mips) ---
        cmdList->SetPipelineState(m_prefilterPso.Get());
        cmdList->SetComputeRootSignature(m_prefilterRootSig.Get());
        for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
        {
            uint32_t mipRes = kPrefilterBaseResolution >> mip;
            float roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMipCount - 1);
            uint32_t groups = (mipRes + 7) / 8;

            for (uint32_t face = 0; face < 6; ++face)
            {
                struct FaceConstants { uint32_t faceIndex; uint32_t mipResolution; float roughness; } fc{ face, mipRes, roughness };
                m_startupUploadsLeakedIntentionally.push_back(makeUploadCb(&fc, sizeof(fc), L"PrefilterFaceCB"));
                cmdList->SetComputeRootConstantBufferView(0, m_startupUploadsLeakedIntentionally.back()->GetGPUVirtualAddress());
                cmdList->SetComputeRootDescriptorTable(1, m_uavHeap->GetGpuHandle(skySrvIndex));
                cmdList->SetComputeRootDescriptorTable(2, m_uavHeap->GetGpuHandle(prefilterUav[mip][face]));
                cmdList->Dispatch(groups, groups, 1);
            }
        }

        // --- Pass 4: BRDF integration LUT (single dispatch) ---
        cmdList->SetPipelineState(m_brdfLutPso.Get());
        cmdList->SetComputeRootSignature(m_brdfLutRootSig.Get());
        struct LutConstants { uint32_t resolution; } lutConstants{ kBrdfLutResolution };
        m_startupUploadsLeakedIntentionally.push_back(makeUploadCb(&lutConstants, sizeof(lutConstants), L"BrdfLutCB"));
        cmdList->SetComputeRootConstantBufferView(0, m_startupUploadsLeakedIntentionally.back()->GetGPUVirtualAddress());
        cmdList->SetComputeRootDescriptorTable(1, m_uavHeap->GetGpuHandle(brdfLutUav));
        uint32_t lutGroups = (kBrdfLutResolution + 7) / 8;
        cmdList->Dispatch(lutGroups, lutGroups, 1);

        // The lighting pass reads all 3 of these as regular SRVs - transition
        // each out of UNORDERED_ACCESS now that their compute writes are done.
        D3D12_RESOURCE_BARRIER finalBarriers[3]{};
        finalBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        finalBarriers[0].Transition.pResource = m_irradiance.Get();
        finalBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        finalBarriers[0].Transition.StateAfter = kShaderReadAll;
        finalBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        finalBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        finalBarriers[1].Transition.pResource = m_prefiltered.Get();
        finalBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        finalBarriers[1].Transition.StateAfter = kShaderReadAll;
        finalBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        finalBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        finalBarriers[2].Transition.pResource = m_brdfLut.Get();
        finalBarriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        finalBarriers[2].Transition.StateAfter = kShaderReadAll;
        finalBarriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmdList->ResourceBarrier(3, finalBarriers);
    }

    void EnvironmentMap::CreateShaderResourceViews(ID3D12Device* device, DescriptorHeap& srvHeap)
    {
        m_srvBaseIndex = srvHeap.Allocate();
        srvHeap.Allocate();
        srvHeap.Allocate();
        srvHeap.Allocate();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        srvDesc.Format = kEnvMapFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = 1;
        device->CreateShaderResourceView(m_sky.Get(), &srvDesc, srvHeap.GetCpuHandle(m_srvBaseIndex + 0));
        device->CreateShaderResourceView(m_irradiance.Get(), &srvDesc, srvHeap.GetCpuHandle(m_srvBaseIndex + 1));

        srvDesc.TextureCube.MipLevels = kPrefilterMipCount;
        device->CreateShaderResourceView(m_prefiltered.Get(), &srvDesc, srvHeap.GetCpuHandle(m_srvBaseIndex + 2));

        D3D12_SHADER_RESOURCE_VIEW_DESC lutSrvDesc{};
        lutSrvDesc.Format = kBrdfLutFormat;
        lutSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        lutSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lutSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_brdfLut.Get(), &lutSrvDesc, srvHeap.GetCpuHandle(m_srvBaseIndex + 3));
    }

    D3D12_GPU_DESCRIPTOR_HANDLE EnvironmentMap::GetSrvTableGpuHandle(DescriptorHeap& srvHeap) const
    {
        return srvHeap.GetGpuHandle(m_srvBaseIndex);
    }
}
