// File: src/Graphics/Raytracing.cpp
//
// NOTE ON THE DXC DEPENDENCY: DXR shader libraries must be DXIL (shader
// model 6.3+), which the legacy D3DCompiler (fxc, used by every other
// shader in this engine via Pipeline.cpp's CompileShader) cannot produce.
// This file therefore uses DXC directly (dxcapi.h + dxcompiler.lib), which
// ships with the Windows 10/11 SDK (present since a broadly-available SDK
// version - the same one this engine already requires for the ray tracing
// tier / mesh shader capability checks in Device.cpp). At runtime,
// dxcompiler.dll and dxil.dll must sit next to the executable - see
// CMakeLists.txt's post-build copy step and the README if that step
// doesn't find them on a given machine.
#include "Graphics/Raytracing.h"
#include "Graphics/Scene.h"
#include "Graphics/DescriptorHeap.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"
#include <dxcapi.h>
#include <string>
#include <vector>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace gfx
{
    namespace
    {
        constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr UINT kShaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32

        UINT64 AlignUp(UINT64 value, UINT64 alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        ComPtr<IDxcBlob> CompileRaytracingLibrary(const wchar_t* path)
        {
            ComPtr<IDxcUtils> utils;
            ComPtr<IDxcCompiler3> compiler;
            DX_CHECK(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
            DX_CHECK(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

            ComPtr<IDxcBlobEncoding> source;
            DX_CHECK(utils->LoadFile(path, nullptr, &source));

            ComPtr<IDxcIncludeHandler> includeHandler;
            DX_CHECK(utils->CreateDefaultIncludeHandler(&includeHandler));

            DxcBuffer sourceBuffer{};
            sourceBuffer.Ptr = source->GetBufferPointer();
            sourceBuffer.Size = source->GetBufferSize();
            sourceBuffer.Encoding = DXC_CP_ACP;

            const wchar_t* args[] = { L"-I", L"shaders", L"-T", L"lib_6_3" };

            ComPtr<IDxcResult> result;
            DX_CHECK(compiler->Compile(&sourceBuffer, args, _countof(args), includeHandler.Get(), IID_PPV_ARGS(&result)));

            HRESULT compileStatus = S_OK;
            result->GetStatus(&compileStatus);
            if (FAILED(compileStatus))
            {
                ComPtr<IDxcBlobUtf8> errors;
                result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
                if (errors && errors->GetStringLength() > 0)
                {
                    LOG_ERROR(std::string("DXC compile error: ") + errors->GetStringPointer());
                }
                DX_CHECK(compileStatus);
            }

            ComPtr<IDxcBlob> objectBlob;
            DX_CHECK(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objectBlob), nullptr));
            return objectBlob;
        }
    }

    RaytracingContext::RaytracingContext(ID3D12Device* device, uint32_t width, uint32_t height)
        : m_width(width)
        , m_height(height)
    {
        // The output texture is created regardless of DXR support so the
        // lighting pass always has a valid, descriptor-backed texture to
        // (conditionally) sample - see DeferredLightingPS.hlsl's roughness
        // gate, which is what actually prevents reading it when unusable.
        CreateOutputResource(device, width, height);

        ComPtr<ID3D12Device5> device5;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))))
        {
            LOG_WARN("ID3D12Device5 unavailable - ray-traced reflections disabled, falling back to IBL-only reflections.");
            return;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
            options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {
            LOG_WARN("D3D12_RAYTRACING_TIER_NOT_SUPPORTED - ray-traced reflections disabled, falling back to IBL-only reflections.");
            return;
        }

        try
        {
            CreateRootSignatures(device);
            CreateStateObjectAndTable(device);
            m_usable = true;
            LOG_INFO("DXR ray-traced reflections enabled.");
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(std::string("DXR setup failed, falling back to IBL-only reflections: ") + e.what());
            m_usable = false;
        }
    }

    void RaytracingContext::CreateOutputResource(ID3D12Device* device, uint32_t width, uint32_t height)
    {
        D3D12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kOutputFormat;
        desc.SampleDesc = { 1, 0 };
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        // Created in a shader-read state (not UNORDERED_ACCESS) so the
        // per-frame barrier sequence the renderer applies around
        // DispatchReflections - shader-read -> UNORDERED_ACCESS (for the
        // dispatch) -> shader-read (for the lighting pass to sample) - is
        // uniform from frame 1 onward, with no special-cased first use.
        // This mirrors ShadowMap's identical fix for the same class of bug.
        DX_CHECK(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_output)));
        m_output->SetName(L"RtReflectionsOutput");
    }

    void RaytracingContext::CreateRootSignatures(ID3D12Device* device)
    {
        // --- Global root signature ---
        // t0: TLAS, bound as a plain root SRV (acceleration structures are
        // not a distinct root parameter type - the HLSL side declaring it
        // as RaytracingAccelerationStructure is what gives the GPU address
        // its special meaning).
        D3D12_DESCRIPTOR_RANGE1 gbufferRange{};
        gbufferRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        gbufferRange.NumDescriptors = 4;
        gbufferRange.BaseShaderRegister = 1; // t1..t4
        gbufferRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        gbufferRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 envRange{};
        envRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        envRange.NumDescriptors = 4;
        envRange.BaseShaderRegister = 5; // t5..t8
        envRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
        envRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 outputRange{};
        outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        outputRange.NumDescriptors = 1;
        outputRange.BaseShaderRegister = 0; // u0
        outputRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        outputRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 globalParams[6]{};
        globalParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t0: TLAS
        globalParams[0].Descriptor.ShaderRegister = 0;
        globalParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        globalParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b0: ScreenConstants
        globalParams[1].Descriptor.ShaderRegister = 0;
        globalParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        globalParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        globalParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b1: LightConstants
        globalParams[2].Descriptor.ShaderRegister = 1;
        globalParams[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        globalParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        globalParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // t1..t4: G-buffer
        globalParams[3].DescriptorTable.NumDescriptorRanges = 1;
        globalParams[3].DescriptorTable.pDescriptorRanges = &gbufferRange;
        globalParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        globalParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // t5..t8: environment map
        globalParams[4].DescriptorTable.NumDescriptorRanges = 1;
        globalParams[4].DescriptorTable.pDescriptorRanges = &envRange;
        globalParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        globalParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // u0: output
        globalParams[5].DescriptorTable.NumDescriptorRanges = 1;
        globalParams[5].DescriptorTable.pDescriptorRanges = &outputRange;
        globalParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister = 0; // s0
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1; // s1
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC globalDesc{};
        globalDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        globalDesc.Desc_1_1.NumParameters = _countof(globalParams);
        globalDesc.Desc_1_1.pParameters = globalParams;
        globalDesc.Desc_1_1.NumStaticSamplers = _countof(samplers);
        globalDesc.Desc_1_1.pStaticSamplers = samplers;
        globalDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        {
            ComPtr<ID3D10Blob> blob, error;
            HRESULT hr = D3D12SerializeVersionedRootSignature(&globalDesc, &blob, &error);
            if (FAILED(hr))
            {
                if (error) LOG_ERROR(std::string("RT global root sig error: ") + static_cast<const char*>(error->GetBufferPointer()));
                DX_CHECK(hr);
            }
            DX_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSig)));
            m_globalRootSig->SetName(L"RtGlobalRootSignature");
        }

        // --- Local root signature (space1, one shader-table record per instance) ---
        D3D12_DESCRIPTOR_RANGE1 localRange{};
        localRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        localRange.NumDescriptors = 2; // t0,space1 vertex buffer ; t1,space1 index buffer
        localRange.BaseShaderRegister = 0;
        localRange.RegisterSpace = 1;
        localRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
        localRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 localParams[2]{};
        localParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        localParams[0].DescriptorTable.NumDescriptorRanges = 1;
        localParams[0].DescriptorTable.pDescriptorRanges = &localRange;
        localParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        localParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b0,space1: InstanceMaterial
        localParams[1].Constants.ShaderRegister = 0;
        localParams[1].Constants.RegisterSpace = 1;
        localParams[1].Constants.Num32BitValues = 8; // float3 albedo, float metallic, float roughness, float3 pad
        localParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC localDesc{};
        localDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        localDesc.Desc_1_1.NumParameters = _countof(localParams);
        localDesc.Desc_1_1.pParameters = localParams;
        localDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

        {
            ComPtr<ID3D10Blob> blob, error;
            HRESULT hr = D3D12SerializeVersionedRootSignature(&localDesc, &blob, &error);
            if (FAILED(hr))
            {
                if (error) LOG_ERROR(std::string("RT local root sig error: ") + static_cast<const char*>(error->GetBufferPointer()));
                DX_CHECK(hr);
            }
            DX_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_localRootSig)));
            m_localRootSig->SetName(L"RtLocalRootSignature");
        }
    }

    void RaytracingContext::CreateStateObjectAndTable(ID3D12Device* device)
    {
        ComPtr<ID3D12Device5> device5;
        DX_CHECK(device->QueryInterface(IID_PPV_ARGS(&device5)));

        ComPtr<IDxcBlob> library = CompileRaytracingLibrary(L"shaders/RaytracingReflections.hlsl");

        // Subobjects reference each other by pointer, so every struct fed
        // into the subobject array must outlive CreateStateObject() and
        // must not move - plain stack locals, not a vector that could
        // reallocate, for every struct whose address is taken below.
        D3D12_DXIL_LIBRARY_DESC libraryDesc{};
        libraryDesc.DXILLibrary.pShaderBytecode = library->GetBufferPointer();
        libraryDesc.DXILLibrary.BytecodeLength = library->GetBufferSize();
        // NumExports=0 / pExports=nullptr exports every symbol the library
        // defines (RayGen, ClosestHit, Miss) under its own name.

        D3D12_HIT_GROUP_DESC hitGroupDesc{};
        hitGroupDesc.HitGroupExport = L"HitGroup";
        hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroupDesc.ClosestHitShaderImport = L"ClosestHit";

        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
        shaderConfig.MaxPayloadSizeInBytes = 16; // ReflectionPayload: float3 color + pad
        shaderConfig.MaxAttributeSizeInBytes = 8; // BuiltInTriangleIntersectionAttributes

        D3D12_LOCAL_ROOT_SIGNATURE localRootSigSubobject{};
        localRootSigSubobject.pLocalRootSignature = m_localRootSig.Get();

        const wchar_t* hitGroupExportName = L"HitGroup";
        // Filled in below, once the local-root-signature subobject has been
        // pushed into the array and its stable address is known.
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION localRootSigAssociation{};

        D3D12_GLOBAL_ROOT_SIGNATURE globalRootSigSubobject{};
        globalRootSigSubobject.pGlobalRootSignature = m_globalRootSig.Get();

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        pipelineConfig.MaxTraceRecursionDepth = 1; // single-bounce reflections only, see the .hlsl file's header comment

        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
        subobjects.reserve(7);

        subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libraryDesc });
        subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroupDesc });
        subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig });

        D3D12_STATE_SUBOBJECT localRootSigStateObject{ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &localRootSigSubobject };
        subobjects.push_back(localRootSigStateObject);
        const D3D12_STATE_SUBOBJECT* localRootSigPtr = &subobjects.back();

        localRootSigAssociation.pSubobjectToAssociate = localRootSigPtr;
        localRootSigAssociation.NumExports = 1;
        localRootSigAssociation.pExports = &hitGroupExportName;
        subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &localRootSigAssociation });

        subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSigSubobject });
        subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig });

        D3D12_STATE_OBJECT_DESC stateObjectDesc{};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
        stateObjectDesc.pSubobjects = subobjects.data();

        DX_CHECK(device5->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_stateObject)));
        DX_CHECK(m_stateObject.As(&m_stateObjectProps));

        // Table start alignment and per-record stride both follow the D3D12
        // raytracing alignment rules. RayGen/Miss have no local root
        // arguments (only the global root signature applies to them), so
        // their records are just the 32-byte shader identifier. The actual
        // identifiers are fetched and written into the table buffer later,
        // in BuildBottomLevelStructures, once the buffer itself exists.
        m_raygenRecordOffset = 0;

        m_missRecordOffset = AlignUp(
            kShaderIdentifierSize,
            D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

        m_hitGroupTableOffset = AlignUp(
            m_missRecordOffset + kShaderIdentifierSize,
            D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
       

        // Hit group record: identifier + local root arguments (one 8-byte
        // descriptor-table GPU handle + 8x4-byte root constants = 40 bytes),
        // rounded up to the shader-record alignment.
        UINT localArgsSize = sizeof(D3D12_GPU_DESCRIPTOR_HANDLE) + 8 * sizeof(UINT32);
        m_hitGroupRecordStride = static_cast<UINT>(AlignUp(kShaderIdentifierSize + localArgsSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT));

        // m_hitGroupCount and the shader table buffer itself are filled in
        // by BuildBottomLevelStructures, which runs after this and knows
        // the actual object count and per-instance descriptor handles.
    }

    void RaytracingContext::BuildBottomLevelStructures(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const std::vector<RenderObject>& objects, DescriptorHeap& srvHeap)
    {
        if (!m_usable) return;

        ComPtr<ID3D12Device5> device5;
        DX_CHECK(device->QueryInterface(IID_PPV_ARGS(&device5)));
        ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        DX_CHECK(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4)));

        m_blasEntries.clear();
        m_blasEntries.reserve(objects.size());
        m_hitGroupCount = static_cast<uint32_t>(objects.size());

        for (const RenderObject& obj : objects)
        {
            // Raw (byte-address) SRVs on the same vertex/index buffers used
            // for rasterization - the closest-hit shader fetches triangle
            // data straight out of them via manual byte offsets.
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

            // DXR requires the geometry buffers to be readable
// in NON_PIXEL_SHADER_RESOURCE state.
            D3D12_RESOURCE_BARRIER geometryBarriers[2]{};

            geometryBarriers[0].Type =
                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            geometryBarriers[0].Transition.pResource =
                obj.vertexBuffer.Get();
            geometryBarriers[0].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            geometryBarriers[0].Transition.StateBefore =
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            geometryBarriers[0].Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            geometryBarriers[1].Type =
                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            geometryBarriers[1].Transition.pResource =
                obj.indexBuffer.Get();
            geometryBarriers[1].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            geometryBarriers[1].Transition.StateBefore =
                D3D12_RESOURCE_STATE_INDEX_BUFFER;
            geometryBarriers[1].Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            cmdList4->ResourceBarrier(2, geometryBarriers);

            cmdList4->BuildRaytracingAccelerationStructure(
                &buildDesc, 0, nullptr);

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

        // --- Shader table, now that per-instance descriptor handles are known ---
        UINT64 tableSize = m_hitGroupTableOffset + static_cast<UINT64>(m_hitGroupRecordStride) * m_blasEntries.size();
        D3D12_RESOURCE_DESC tableDesc{};
        tableDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        tableDesc.Width = tableSize;
        tableDesc.Height = 1;
        tableDesc.DepthOrArraySize = 1;
        tableDesc.MipLevels = 1;
        tableDesc.SampleDesc = { 1, 0 };
        tableDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        DX_CHECK(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &tableDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_shaderTable)));
        m_shaderTable->SetName(L"RtShaderTable");

        uint8_t* mapped = nullptr;
        m_shaderTable->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

        std::memcpy(mapped + m_raygenRecordOffset, m_stateObjectProps->GetShaderIdentifier(L"RayGen"), kShaderIdentifierSize);
        std::memcpy(mapped + m_missRecordOffset, m_stateObjectProps->GetShaderIdentifier(L"Miss"), kShaderIdentifierSize);

        void* hitGroupId = m_stateObjectProps->GetShaderIdentifier(L"HitGroup");
        for (size_t i = 0; i < m_blasEntries.size(); ++i)
        {
            uint8_t* record = mapped + m_hitGroupTableOffset + m_hitGroupRecordStride * i;
            std::memcpy(record, hitGroupId, kShaderIdentifierSize);

            // Local root argument 0: the 2-descriptor table (vertex, index) -
            // valid because vertexSrvIndex/indexSrvIndex were allocated
            // back-to-back for this instance above.
            D3D12_GPU_DESCRIPTOR_HANDLE tableHandle = srvHeap.GetGpuHandle(m_blasEntries[i].vertexSrvIndex);
            std::memcpy(record + kShaderIdentifierSize, &tableHandle, sizeof(tableHandle));

            // Local root argument 1: the 8 root-constant DWORDs (albedo.xyz, metallic, roughness, pad.xyz).
            float materialConstants[8] = {
                objects[i].material->factors.baseColorFactor.x,
                objects[i].material->factors.baseColorFactor.y,
                objects[i].material->factors.baseColorFactor.z,
                objects[i].material->factors.metallicFactor,
                objects[i].material->factors.roughnessFactor,
                0.0f, 0.0f, 0.0f
            };
            std::memcpy(record + kShaderIdentifierSize + sizeof(tableHandle), materialConstants, sizeof(materialConstants));
        }

        D3D12_RANGE noRead{ 0, 0 };
        m_shaderTable->Unmap(0, &noRead);
    }

    void RaytracingContext::UpdateTopLevelStructure(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const std::vector<RenderObject>& objects, double totalTimeSeconds)
    {
        if (!m_usable) return;

        ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        DX_CHECK(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4)));

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances(objects.size());
        for (size_t i = 0; i < objects.size(); ++i)
        {
            DirectX::XMMATRIX world = objects[i].GetWorldMatrix(totalTimeSeconds);
            // D3D12_RAYTRACING_INSTANCE_DESC::Transform is a row-major 3x4
            // matrix (object-to-world) - NOT transposed like the shader
            // matrices elsewhere in this engine, which use the mul(vec,
            // matrix) row-vector convention with a pre-transpose for HLSL's
            // column-major default. DXR always wants row-major here.
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

    void RaytracingContext::DispatchReflections(
        ID3D12GraphicsCommandList* cmdList, DescriptorHeap& srvHeap,
        D3D12_GPU_VIRTUAL_ADDRESS screenCbAddress, D3D12_GPU_VIRTUAL_ADDRESS lightCbAddress,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferTable, D3D12_GPU_DESCRIPTOR_HANDLE envTable)
    {
        if (!m_usable) return;

        ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        DX_CHECK(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4)));

        cmdList4->SetComputeRootSignature(m_globalRootSig.Get());
        cmdList4->SetComputeRootShaderResourceView(0, m_tlas->GetGPUVirtualAddress());
        cmdList4->SetComputeRootConstantBufferView(1, screenCbAddress);
        cmdList4->SetComputeRootConstantBufferView(2, lightCbAddress);
        cmdList4->SetComputeRootDescriptorTable(3, gbufferTable);
        cmdList4->SetComputeRootDescriptorTable(4, envTable);
        cmdList4->SetComputeRootDescriptorTable(5, srvHeap.GetGpuHandle(m_outputUavIndex));

        D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
        dispatchDesc.RayGenerationShaderRecord.StartAddress = m_shaderTable->GetGPUVirtualAddress() + m_raygenRecordOffset;
        dispatchDesc.RayGenerationShaderRecord.SizeInBytes = kShaderIdentifierSize;

        dispatchDesc.MissShaderTable.StartAddress = m_shaderTable->GetGPUVirtualAddress() + m_missRecordOffset;
        dispatchDesc.MissShaderTable.SizeInBytes = kShaderIdentifierSize;
        dispatchDesc.MissShaderTable.StrideInBytes = kShaderIdentifierSize;

        dispatchDesc.HitGroupTable.StartAddress = m_shaderTable->GetGPUVirtualAddress() + m_hitGroupTableOffset;
        dispatchDesc.HitGroupTable.SizeInBytes = static_cast<UINT64>(m_hitGroupRecordStride) * m_hitGroupCount;
        dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupRecordStride;

        dispatchDesc.Width = m_width;
        dispatchDesc.Height = m_height;
        dispatchDesc.Depth = 1;

        cmdList4->SetPipelineState1(m_stateObject.Get());
        cmdList4->DispatchRays(&dispatchDesc);
    }

    void RaytracingContext::CreateOutputShaderResourceView(ID3D12Device* device, DescriptorHeap& srvHeap)
    {
        m_outputUavIndex = srvHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = kOutputFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uavDesc, srvHeap.GetCpuHandle(m_outputUavIndex));

        if (!m_outputSrvAllocated)
        {
            m_outputSrvIndex = srvHeap.Allocate();
            m_outputSrvAllocated = true;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = kOutputFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_output.Get(), &srvDesc, srvHeap.GetCpuHandle(m_outputSrvIndex));
    }

    D3D12_GPU_DESCRIPTOR_HANDLE RaytracingContext::GetOutputSrvGpuHandle(DescriptorHeap& srvHeap) const
    {
        return srvHeap.GetGpuHandle(m_outputSrvIndex);
    }

    void RaytracingContext::Resize(ID3D12Device* device, uint32_t width, uint32_t height, DescriptorHeap& srvHeap)
    {
        if (width == 0 || height == 0 || (width == m_width && height == m_height)) return;
        m_width = width;
        m_height = height;
        m_output.Reset();
        CreateOutputResource(device, width, height);
        CreateOutputShaderResourceView(device, srvHeap);
    }
}
