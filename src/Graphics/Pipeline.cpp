// File: src/Graphics/Pipeline.cpp
#include "Graphics/Pipeline.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"
#include <d3dcompiler.h>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace gfx
{
    ComPtr<ID3D10Blob> CompileShader(const wchar_t* path, const char* entryPoint, const char* target)
    {
        UINT flags = 0;
#if defined(_DEBUG)
        // Skip optimization + embed debug info so PIX/RenderDoc can show
        // readable HLSL instead of disassembly.
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3D10Blob> shaderBlob;
        ComPtr<ID3D10Blob> errorBlob;

        HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint, target, flags, 0, &shaderBlob, &errorBlob);

        if (FAILED(hr))
        {
            if (errorBlob)
            {
                LOG_ERROR(std::string("Shader compile error: ") + static_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            DX_CHECK(hr);
        }

        return shaderBlob;
    }

    static ComPtr<ID3D12RootSignature> CreateGBufferRootSignature(ID3D12Device* device)
    {
        // t0..t3: albedo, normal, metallic-roughness, ao - one contiguous
        // range so Material can bind all 4 with a single base GPU handle.
        D3D12_DESCRIPTOR_RANGE1 materialSrvRange{};
        materialSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        materialSrvRange.NumDescriptors = 4;
        materialSrvRange.BaseShaderRegister = 0; // t0..t3
        materialSrvRange.RegisterSpace = 0;
        materialSrvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
        materialSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParams[3]{};

        // b0: world/viewProj only - the geometry pass does no lighting, so
        // it has no use for the camera position (unlike the old forward
        // pass's per-frame CBV, which needed it for the view vector).
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0; // b0
        rootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        // b1: material factors (base color / metallic / roughness / AO
        // strength), pixel-only.
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[1].Descriptor.ShaderRegister = 1; // b1
        rootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[2].DescriptorTable.pDescriptorRanges = &materialSrvRange;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0; // s0
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = _countof(rootParams);
        desc.Desc_1_1.pParameters = rootParams;
        desc.Desc_1_1.NumStaticSamplers = 1;
        desc.Desc_1_1.pStaticSamplers = &sampler;
        desc.Desc_1_1.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3D10Blob> signatureBlob;
        ComPtr<ID3D10Blob> errorBlob;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            if (errorBlob)
            {
                LOG_ERROR(std::string("GBuffer root signature serialize error: ") + static_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            DX_CHECK(hr);
        }

        ComPtr<ID3D12RootSignature> rootSignature;
        DX_CHECK(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
        rootSignature->SetName(L"GBufferRootSignature");
        return rootSignature;
    }

    PipelineBundle CreateGBufferPipeline(ID3D12Device* device, DXGI_FORMAT albedoFormat, DXGI_FORMAT normalFormat, DXGI_FORMAT mraoFormat, DXGI_FORMAT dsvFormat)
    {
        PipelineBundle bundle;
        bundle.rootSignature = CreateGBufferRootSignature(device);

        ComPtr<ID3D10Blob> vs = CompileShader(L"shaders/GBufferVS.hlsl", "main", "vs_5_1");
        ComPtr<ID3D10Blob> ps = CompileShader(L"shaders/GBufferPS.hlsl", "main", "ps_5_1");

        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = bundle.rootSignature.Get();
        psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        // 3 render targets, all fully opaque writes - the G-buffer never
        // blends (that would corrupt the material data the lighting pass
        // depends on being exact).
        for (UINT i = 0; i < 3; ++i)
        {
            psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }

        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 3;
        psoDesc.RTVFormats[0] = albedoFormat;
        psoDesc.RTVFormats[1] = normalFormat;
        psoDesc.RTVFormats[2] = mraoFormat;
        psoDesc.DSVFormat = dsvFormat;
        psoDesc.SampleDesc = { 1, 0 };

        DX_CHECK(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&bundle.pso)));
        bundle.pso->SetName(L"GBufferPSO");

        return bundle;
    }

    static ComPtr<ID3D12RootSignature> CreateLightingRootSignature(ID3D12Device* device)
    {
        // t0..t3: the G-buffer (albedo, normal, metallicRoughnessAO, depth) -
        // one contiguous range, allocated together by GBuffer::CreateShaderResourceViews.
        D3D12_DESCRIPTOR_RANGE1 gbufferRange{};
        gbufferRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        gbufferRange.NumDescriptors = 4;
        gbufferRange.BaseShaderRegister = 0; // t0..t3
        gbufferRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE; // GBuffer's resources are recreated on resize, unlike a material's static textures
        gbufferRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t4: the shadow map - its own table for the same reason as the
        // forward pass's equivalent binding: it isn't allocated contiguously
        // with the G-buffer's 4 descriptors.
        D3D12_DESCRIPTOR_RANGE1 shadowRange{};
        shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadowRange.NumDescriptors = 1;
        shadowRange.BaseShaderRegister = 4; // t4
        shadowRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
        shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t5..t8: the environment map (sky, irradiance, prefiltered
        // specular, BRDF LUT) - one contiguous range, allocated together by
        // EnvironmentMap::CreateShaderResourceViews.
        D3D12_DESCRIPTOR_RANGE1 envRange{};
        envRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        envRange.NumDescriptors = 4;
        envRange.BaseShaderRegister = 5; // t5..t8
        envRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
        envRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t9: ray-traced reflections output (see Graphics/Raytracing) -
        // always bound, even on hardware without DXR support, since the
        // texture is always created (see RaytracingContext's constructor
        // comment); DeferredLightingPS.hlsl's roughness-threshold gate is
        // what actually prevents ever sampling it when unusable.
        D3D12_DESCRIPTOR_RANGE1 rtReflectionsRange{};
        rtReflectionsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rtReflectionsRange.NumDescriptors = 1;
        rtReflectionsRange.BaseShaderRegister = 9; // t9
        rtReflectionsRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE; // recreated on resize
        rtReflectionsRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParams[6]{};

        // b0: inverse view-projection (to reconstruct world position from
        // depth) + camera position. Pixel-only - the full-screen vertex
        // shader needs no per-frame data at all.
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0; // b0
        rootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // b1: light list + shadow light-space matrix.
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[1].Descriptor.ShaderRegister = 1; // b1
        rootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[2].DescriptorTable.pDescriptorRanges = &gbufferRange;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[3].DescriptorTable.pDescriptorRanges = &shadowRange;
        rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[4].DescriptorTable.pDescriptorRanges = &envRange;
        rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[5].DescriptorTable.pDescriptorRanges = &rtReflectionsRange;
        rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // s0: point-clamp - the lighting pass reads the G-buffer at exact,
        // 1:1 texel coordinates, so no filtering or wrap addressing is ever
        // wanted here (unlike material sampling, which is bilinear + wrap).
        // s1: the same comparison sampler as the (now-removed) forward pass
        // used for shadow PCF.
        // s2: linear-clamp for the environment maps (cubemap sampling +
        // trilinear across the prefiltered map's roughness mips).
        D3D12_STATIC_SAMPLER_DESC samplers[3]{};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister = 0; // s0
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1; // s1
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[2].ShaderRegister = 2; // s2
        samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = _countof(rootParams);
        desc.Desc_1_1.pParameters = rootParams;
        desc.Desc_1_1.NumStaticSamplers = _countof(samplers);
        desc.Desc_1_1.pStaticSamplers = samplers;
        // No input-assembler flag: the lighting pass has no input layout at
        // all (see shaders/FullscreenVS.hlsl), and no vertex-stage root
        // access either.
        desc.Desc_1_1.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3D10Blob> signatureBlob;
        ComPtr<ID3D10Blob> errorBlob;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            if (errorBlob)
            {
                LOG_ERROR(std::string("Lighting root signature serialize error: ") + static_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            DX_CHECK(hr);
        }

        ComPtr<ID3D12RootSignature> rootSignature;
        DX_CHECK(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
        rootSignature->SetName(L"LightingRootSignature");
        return rootSignature;
    }

    PipelineBundle CreateLightingPipeline(ID3D12Device* device, DXGI_FORMAT rtvFormat)
    {
        PipelineBundle bundle;
        bundle.rootSignature = CreateLightingRootSignature(device);

        ComPtr<ID3D10Blob> vs = CompileShader(L"shaders/FullscreenVS.hlsl", "main", "vs_5_1");
        ComPtr<ID3D10Blob> ps = CompileShader(L"shaders/DeferredLightingPS.hlsl", "main", "ps_5_1");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = bundle.rootSignature.Get();
        psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        psoDesc.InputLayout = { nullptr, 0 }; // no vertex buffer at all - see shaders/FullscreenVS.hlsl

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // a full-screen triangle's winding doesn't matter
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // No depth test/write: this pass draws exactly one full-screen
        // triangle straight to the back buffer and does its own depth-based
        // logic (discarding background pixels) inside the shader instead.
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtvFormat;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc = { 1, 0 };

        DX_CHECK(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&bundle.pso)));
        bundle.pso->SetName(L"LightingPSO");

        return bundle;
    }

    static ComPtr<ID3D12RootSignature> CreateShadowRootSignature(ID3D12Device* device)
    {
        // A single root CBV: the light-space world-view-projection matrix.
        // The shadow pass draws no pixels and touches no textures, so this
        // is deliberately the smallest root signature in the engine.
        D3D12_ROOT_PARAMETER1 rootParam{};
        rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParam.Descriptor.ShaderRegister = 0; // b0
        rootParam.Descriptor.RegisterSpace = 0;
        rootParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = 1;
        desc.Desc_1_1.pParameters = &rootParam;
        desc.Desc_1_1.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3D10Blob> signatureBlob;
        ComPtr<ID3D10Blob> errorBlob;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            if (errorBlob)
            {
                LOG_ERROR(std::string("Shadow root signature serialize error: ") + static_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            DX_CHECK(hr);
        }

        ComPtr<ID3D12RootSignature> rootSignature;
        DX_CHECK(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
        rootSignature->SetName(L"ShadowRootSignature");
        return rootSignature;
    }

    PipelineBundle CreateShadowPipeline(ID3D12Device* device, DXGI_FORMAT shadowDsvFormat)
    {
        PipelineBundle bundle;
        bundle.rootSignature = CreateShadowRootSignature(device);

        ComPtr<ID3D10Blob> vs = CompileShader(L"shaders/ShadowVS.hlsl", "main", "vs_5_1");

        // Only POSITION is read - the shadow pass doesn't need normals,
        // tangents or UVs, even though the vertex buffer bound during this
        // pass contains them (same buffer as the main pass; the extra
        // attributes are simply skipped by this input layout).
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = bundle.rootSignature.Get();
        psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        psoDesc.PS = { nullptr, 0 }; // depth-only: no pixel shader at all
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        // A depth bias baked into the PSO, on top of the shader-side
        // slope-scaled bias applied in Common.hlsli's SampleShadow - two
        // complementary defenses against shadow acne. Values are tuned for
        // a D32_FLOAT shadow map at typical demo-scene scale; a much larger
        // scene will need to revisit these.
        psoDesc.RasterizerState.DepthBias = 50;
        psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;

        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0; // no color output at all

        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 0; // depth-only pass: no render targets
        psoDesc.DSVFormat = shadowDsvFormat;
        psoDesc.SampleDesc = { 1, 0 };

        DX_CHECK(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&bundle.pso)));
        bundle.pso->SetName(L"ShadowPSO");

        return bundle;
    }
}
