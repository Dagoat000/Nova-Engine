// File: shaders/BrdfLutCS.hlsl
//
// The other half of the split-sum approximation: a 2D lookup table over
// (NdotV, roughness) storing the environment BRDF's (scale, bias) pair -
// i.e. the answer to "integrate the GGX BRDF over the hemisphere, assuming
// a perfectly white uniform environment". At runtime,
// specular = prefilteredColor * (F0 * lut.r + lut.g) reconstructs the full
// specular IBL term without any per-pixel integration. Independent of the
// scene entirely (pure function of NdotV/roughness), so this is a single
// dispatch producing a small, reusable texture.

#include "IBLCommon.hlsli"

cbuffer LutConstants : register(b0)
{
    uint gResolution;
};

RWTexture2D<float2> gOutLut : register(u0);

static const uint kSampleCount = 64;

float2 IntegrateBRDF(float NdotV, float roughness)
{
    float3 V;
    V.x = sqrt(1.0f - NdotV * NdotV);
    V.y = 0.0f;
    V.z = NdotV;

    float A = 0.0f;
    float B = 0.0f;
    float3 N = float3(0.0f, 0.0f, 1.0f);

    for (uint i = 0; i < kSampleCount; ++i)
    {
        float2 xi = Hammersley(i, kSampleCount);
        float3 H = ImportanceSampleGGX(xi, N, roughness);
        float3 L = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = saturate(L.z);
        float NdotH = saturate(H.z);
        float VdotH = saturate(dot(V, H));

        if (NdotL > 0.0f)
        {
            float G = GeometrySmith_IBL(NdotV, NdotL, roughness);
            float Gvis = (G * VdotH) / max(NdotH * NdotV, 1e-4f);
            float Fc = pow(1.0f - VdotH, 5.0f);

            A += (1.0f - Fc) * Gvis;
            B += Fc * Gvis;
        }
    }

    return float2(A, B) / float(kSampleCount);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gResolution || dtid.y >= gResolution) return;

    float2 uv = (float2(dtid.xy) + 0.5f) / float(gResolution);
    gOutLut[dtid.xy] = IntegrateBRDF(uv.x, uv.y);
}
