// File: src/Graphics/Lighting.h
//
// The scene's light list (one directional "sun" + a small fixed number of
// point lights) and the exact GPU-mirrored constant buffer layout for it
// (see the LightConstants cbuffer in shaders/BasicVS.hlsl /
// shaders/BasicPS.hlsl - the two must stay byte-for-byte in sync).
//
// A fixed-size point light array (rather than a real light list with a
// count-driven structured buffer) is a deliberate, named scope limit: it
// covers "a lit scene looks more alive than one directional light" without
// building the per-light culling/indexing infrastructure a large dynamic
// light count would need - that belongs with Phase 5/6 (Forward+ / GPU
// culling), not with introducing lighting itself.
#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <cmath>

namespace gfx
{
    constexpr uint32_t kMaxPointLights = 2;

    // 32 bytes: two float4-sized chunks, so array indexing in HLSL has no
    // surprise padding between elements.
    struct PointLightGpu
    {
        DirectX::XMFLOAT3 positionWS{ 0, 0, 0 };
        float range = 5.0f;
        DirectX::XMFLOAT3 color{ 1, 1, 1 };
        float intensity = 1.0f;
    };

    // Mirrors the HLSL cbuffer LightConstants (register b2) exactly.
    struct LightConstants
    {
        DirectX::XMFLOAT3 dirLightDirWS{ -0.4f, -0.7f, 0.5f }; // direction the light travels
        float dirLightIntensity = 3.0f;
        DirectX::XMFLOAT3 dirLightColor{ 1.0f, 0.97f, 0.92f };
        float _pad0 = 0.0f;
        DirectX::XMFLOAT4X4 lightViewProj{}; // for the shadow map, filled in per-frame
        PointLightGpu pointLights[kMaxPointLights];
        uint32_t numPointLights = 0;
        DirectX::XMFLOAT3 _pad1{};
    };

    // CPU-side scene light description; converted into LightConstants (plus
    // the computed shadow matrix) once per frame by D3D12Renderer.
    struct SceneLighting
    {
        DirectX::XMFLOAT3 dirLightDirection{ -0.4f, -0.7f, 0.5f };
        DirectX::XMFLOAT3 dirLightColor{ 1.0f, 0.97f, 0.92f };
        float dirLightIntensity = 3.0f;

        PointLightGpu pointLights[kMaxPointLights] = {
            // A warm fill light and a cool rim light on either side of the
            // demo object - cheap way to show the BRDF responds to more
            // than one light direction without needing real scene content.
            PointLightGpu{ { 2.2f, 1.2f, -1.5f }, 6.0f, { 1.0f, 0.55f, 0.3f }, 2.0f },
            PointLightGpu{ { -2.0f, 0.6f, 1.8f }, 6.0f, { 0.35f, 0.55f, 1.0f }, 1.5f },
        };
        uint32_t numPointLights = kMaxPointLights;

        // Orthographic projection sized to just cover a sphere of the given
        // radius around the origin - fine for a single centered demo object;
        // a real scene needs this fit to the camera frustum's visible
        // bounds instead (cascaded shadow maps are exactly that, generalized
        // across multiple distance ranges).
        DirectX::XMMATRIX ComputeLightViewProj(float sceneRadius) const
        {
            using namespace DirectX;
            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&dirLightDirection));
            XMVECTOR lightPos = XMVectorScale(dir, -sceneRadius * 4.0f);
            XMVECTOR target = XMVectorZero();
            XMVECTOR up = (fabsf(XMVectorGetY(dir)) > 0.99f) ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);

            XMMATRIX view = XMMatrixLookAtLH(lightPos, target, up);
            XMMATRIX proj = XMMatrixOrthographicLH(sceneRadius * 2.5f, sceneRadius * 2.5f, 0.1f, sceneRadius * 8.0f);
            return view * proj;
        }
    };
}
