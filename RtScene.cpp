// File: src/Graphics/RtScene.cpp
#include "Graphics/RtScene.h"
#include "Graphics/Scene.h"
#include "Graphics/DescriptorHeap.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"
#include <cstring>

namespace gfx
{
    RtScene::RtScene(ID3D12Device* device)
    {
        ComPtr<ID3D12Device5> device5;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))))
        {
            LOG_WARN("ID3D12Device5 unavailable - DXR disabled, every ray-traced effect falls back to its non-RT path.");
            return;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
            options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {
            LOG_WARN("D3D12_RAYTRACING_TIER_NOT_SUPPORTED - DXR disabled, every ray-traced effect falls back to its non-RT path.");
            return;
        }

        m_usable = true;
    }

    void RtScene::BuildBottomLevelStructures(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const std::vector<RenderObject>& objects, DescriptorHeap& srvHeap)
    {
        if (!m_usable) return;

        ComPtr<ID3D12Device5> device5;
        DX_CHECK(device->QueryInterface(IID_PPV_ARGS(&device5)));
        ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        DX_CHECK(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4)));

        m_blasEntries.clear();
        m_blasEntries.reserve(objects.size());

        for (const RenderObject& obj : objects)
        {
            // Raw (byte-address) SRVs on the same vertex/index buffers used
            // for rasterization - closest-hit shaders fetch triangle data
            // straight out of them via manual byte offsets (see
            // shaders/RtCommon.hlsli's FetchHitObjectSpaceNormal).
            BlasEntry entry;
            entry.vertexSrvIndex = srvHeap.Allocate();
            entry.indexSrvIndex = srvHeap.Allocate();

            D3D12_SHADER_RESOURCE_VIEW_DESC vbSrvDesc{};
            vbSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            vbSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            vbSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            vbSrvDesc.Buffer.NumElements = obj.vbv.SizeInBytes / 4;
            vbSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            device->CreateShaderResourceView(obj.vertexBuffer.Get(), &vbSrvDesc, srvHeap.GetCpuHandle(entry.vertexSrvIndex));

            D3D12_SHADER_RESOURCE_VIEW_DESC ibSrvDesc = vbSrvDesc;
            ibSrvDesc.Buffer.NumElements = obj.ibv.SizeInBytes / 4;
            device->CreateShaderResourceView(obj.indexBuffer.Get(), &ibSrvDesc, srvHeap.GetCpuHandle(entry.indexSrvIndex));

            // --- BLAS ---
            D3D12_RAYTRACING_GEOMETRY_DESC geomDesc{};
            geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geomDesc.Triangles.VertexBuffer.StartAddress = obj.vertexBuffer->GetGPUVirtualAddress();
            geomDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(rendering::Vertex);
            geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geomDesc.Triangles.VertexCount = obj.vertexCount;
            geomDesc.Triangles.IndexBuffer = obj.indexBuffer->GetGPUVirtualAddress();
            geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geomDesc.Triangles.IndexCount = obj.indexCount;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
            blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            blasInputs.NumDescs = 1;
            blasInputs.pGeometryDescs = &geomDesc;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &prebuildInfo);

            D3D12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_DEFAULT };

            D3D12_RESOURCE_DESC resultDesc{};
            resultDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resultDesc.Width = prebuildInfo.ResultDataMaxSizeInBytes;
            resultDesc.Height = 1;
            resultDesc.DepthOrArraySize = 1;
            resultDesc.MipLevels = 1;
            resultDesc.SampleDesc = { 1, 0 };
            resultDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            resultDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            DX_CHECK(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resultDesc,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&entry.blas)));
            entry.blas->SetName(L"BLAS");

            D3D12_RESOURCE_DESC scratchDesc = resultDesc;
            scratchDesc.Width = prebuildInfo.ScratchDataSizeInBytes;
            ComPtr<ID3D12Resource> scratch;
            DX_CHECK(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &scratchDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&scratch)));
            m_blasScratchKeepAlive.push_back(scratch);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
            buildDesc.Inputs = blasInputs;
            buildDesc.DestAccelerationStructureData = entry.blas->GetGPUVirtualAddress();
            buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();

            cmdList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

            D3D12_RESOURCE_BARRIER uavBarrier{};
            uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavBarrier.UAV.pResource = entry.blas.Get();
            cmdList4->ResourceBarrier(1, &uavBarrier);

            m_blasEntries.push_back(entry);
        }

        // --- Persistently-mapped instance-desc upload buffer, rewritten every frame ---
        D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC instanceDescBufferDesc{};
        instanceDescBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        instanceDescBufferDesc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * m_blasEntries.size();
        instanceDescBufferDesc.Height = 1;
        instanceDescBufferDesc.DepthOrArraySize = 1;
        instanceDescBufferDesc.MipLevels = 1;
        instanceDescBufferDesc.SampleDesc = { 1, 0 };
        instanceDescBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        DX_CHECK(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &instanceDescBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_instanceDescBuffer)));

        // --- TLAS result + scratch, sized once for the fixed object count ---
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD; // rebuilt every frame - build speed matters more than trace speed here
        tlasInputs.NumDescs = static_cast<UINT>(m_blasEntries.size());
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild{};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuild);

        D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };

        D3D12_RESOURCE_DESC tlasResultDesc{};
        tlasResultDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        tlasResultDesc.Width = tlasPrebuild.ResultDataMaxSizeInBytes;
        tlasResultDesc.Height = 1;
        tlasResultDesc.DepthOrArraySize = 1;
        tlasResultDesc.MipLevels = 1;
        tlasResultDesc.SampleDesc = { 1, 0 };
        tlasResultDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        tlasResultDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        DX_CHECK(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &tlasResultDesc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&m_tlas)));
        m_tlas->SetName(L"TLAS");

        D3D12_RESOURCE_DESC tlasScratchDesc = tlasResultDesc;
        tlasScratchDesc.Width = tlasPrebuild.ScratchDataSizeInBytes;
        DX_CHECK(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &tlasScratchDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_tlasScratch)));
    }

    void RtScene::UpdateTopLevelStructure(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const std::vector<RenderObject>& objects, double totalTimeSeconds)
    {
        if (!m_usable) return;
        (void)device;

        ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        DX_CHECK(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4)));

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances(objects.size());
        for (size_t i = 0; i < objects.size(); ++i)
        {
            DirectX::XMMATRIX world = objects[i].GetWorldMatrix(totalTimeSeconds);
            DirectX::XMFLOAT4X4 m;
            DirectX::XMStoreFloat4x4(&m, world);

            D3D12_RAYTRACING_INSTANCE_DESC& inst = instances[i];
            // DXR wants the transform's linear 3x3 part transposed relative
            // to DirectXMath's row-vector-convention matrix (translation in
            // m._41/_42/_43 here), with translation moved into column 3 of
            // each row - the classic row-vector vs column-vector layout
            // difference, not a bug.
            inst.Transform[0][0] = m._11; inst.Transform[0][1] = m._21; inst.Transform[0][2] = m._31; inst.Transform[0][3] = m._41;
            inst.Transform[1][0] = m._12; inst.Transform[1][1] = m._22; inst.Transform[1][2] = m._32; inst.Transform[1][3] = m._42;
            inst.Transform[2][0] = m._13; inst.Transform[2][1] = m._23; inst.Transform[2][2] = m._33; inst.Transform[2][3] = m._43;

            inst.InstanceID = static_cast<UINT>(i);
            inst.InstanceMask = 0xFF;
            inst.InstanceContributionToHitGroupIndex = static_cast<UINT>(i);
            inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            inst.AccelerationStructure = m_blasEntries[i].blas->GetGPUVirtualAddress();
        }

        void* mapped = nullptr;
        m_instanceDescBuffer->Map(0, nullptr, &mapped);
        std::memcpy(mapped, instances.data(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instances.size());
        D3D12_RANGE noRead{ 0, 0 };
        m_instanceDescBuffer->Unmap(0, &noRead);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        tlasInputs.NumDescs = static_cast<UINT>(instances.size());
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.InstanceDescs = m_instanceDescBuffer->GetGPUVirtualAddress();

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
        buildDesc.Inputs = tlasInputs;
        buildDesc.DestAccelerationStructureData = m_tlas->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();

        cmdList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = m_tlas.Get();
        cmdList4->ResourceBarrier(1, &uavBarrier);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE RtScene::GetInstanceBuffersGpuHandle(uint32_t objectIndex, DescriptorHeap& srvHeap) const
    {
        return srvHeap.GetGpuHandle(m_blasEntries[objectIndex].vertexSrvIndex);
    }
}