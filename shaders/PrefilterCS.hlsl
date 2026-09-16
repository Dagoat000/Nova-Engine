// File: shaders/PrefilterCS.hlsl
//
// Specular IBL precompute (the "prefiltered environment map" half of the
// split-sum approximation): for each mip level of the output cubemap,
// GGX-importance-samples the sky cubemap at that mip's roughness and
// stores the weighted average. At runtime, sampling this map at
// `roughness * (mipCount-1)` gives an approximate pre-convolved specular
// reflection with no per-frame integration cost.

#include "IBLCommon.hlsli"

cbuffer FaceConstants : register(b0)
{
    uint gFaceIndex;
    uint gMipResolution;
    float gRoughness;
};

TextureCube gSkyMap : register(t0);
SamplerState gSampler : register(s0);
RWTexture2DArray<float4> gOutMip : register(u0);

static const uint kSampleCount = 48;

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gMipResolution || dtid.y >= gMipResolution) return;

    float2 uv = (float2(dtid.xy) + 0.5f) / float(gMipResolution);
    float3 N = FaceUvToDirection(gFaceIndex, uv);
    float3 V = N; // the standard simplifying assumption: view direction == normal == reflection direction

    float3 result = 0.0f.xxx;
    float totalWeight = 0.0f;

    for (uint i = 0; i < kSampleCount; ++i)
    {
        float2 xi = Hammersley(i, kSampleCount);
        float3 H = ImportanceSampleGGX(xi, N, gRoughness);
        float3 L = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = dot(N, L);
        if (NdotL > 0.0f)
        {
            result += gSkyMap.SampleLevel(gSampler, L, 0).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    result = totalWeight > 0.0f ? result / totalWeight : gSkyMap.SampleLevel(gSampler, N, 0).rgb;
    gOutMip[uint3(dtid.xy, gFaceIndex)] = float4(result, 1.0f);
}
