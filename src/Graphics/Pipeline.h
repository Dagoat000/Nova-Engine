// File: src/Graphics/Pipeline.h
//
// Root signature + PSO creation. Three pipelines live here, one per pass
// of the deferred renderer:
//
//   - CreateShadowPipeline: depth-only pass rendering the scene from the
//     light's point of view into the shadow map. Root signature is
//     deliberately just one root CBV (the light-space world-view-
//     projection matrix) - it draws no pixels, so it needs nothing else.
//
//   - CreateGBufferPipeline: the geometry pass. Writes material data
//     (albedo, world-space normal, metallic/roughness/AO) into the
//     G-buffer instead of shading directly. Root signature is b0 per-
//     object transform (vertex-only - this pass has no use for the camera
//     position), b1 material factors (pixel), a t0..t3 material SRV table
//     (pixel), and a linear-wrap sampler (s0).
//
//   - CreateLightingPipeline: a full-screen pass with NO input layout and
//     NO vertex/index buffer (see shaders/FullscreenVS.hlsl) that reads
//     the G-buffer + shadow map and evaluates the actual PBR lighting once
//     per covered pixel. Root signature is b0 screen constants (inverse
//     view-projection + camera position, pixel-only), b1 light constants
//     incl. the shadow light-space matrix, a t0..t3 G-buffer SRV table,
//     a t4 shadow-map SRV table, a point-clamp sampler (s0, exact texel
//     reads of the G-buffer) and a comparison sampler for hardware PCF (s1).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace gfx
{
    using Microsoft::WRL::ComPtr;

    // Compiles an HLSL file at runtime via the D3DCompiler. Runtime
    // compilation keeps the CMake setup simple; switching to offline DXC
    // compilation with a shader cache is a later task once the shader count
    // grows enough that compile time becomes noticeable at startup.
    ComPtr<class ID3D10Blob> CompileShader(const wchar_t* path, const char* entryPoint, const char* target);

    struct PipelineBundle
    {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> pso;
    };

    PipelineBundle CreateShadowPipeline(ID3D12Device* device, DXGI_FORMAT shadowDsvFormat);
    PipelineBundle CreateGBufferPipeline(ID3D12Device* device, DXGI_FORMAT albedoFormat, DXGI_FORMAT normalFormat, DXGI_FORMAT mraoFormat, DXGI_FORMAT dsvFormat);
    PipelineBundle CreateLightingPipeline(ID3D12Device* device, DXGI_FORMAT rtvFormat);
}
