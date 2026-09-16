// File: shaders/IrradianceCS.hlsl
//
// Diffuse irradiance convolution: for each texel of a small output
// cubemap, integrates the sky cubemap over the cosine-weighted hemisphere
// around that texel's direction. This is the "diffuse IBL" precompute -
// at runtime, a single texture sample of this map (by surface normal)
// replaces the flat ambient constant with a real, sky-colored diffuse term.
//
// Done once at startup on a small (32x32 per face) target - the result is
// so low-frequency (cosine-weighted integration of a whole hemisphere)
// that a small resolution loses nothing visible.

#include "IBLCommon.hlsli"

cbuffer FaceConstants : register(b0)
{
    uint gFaceIndex;
    uint gResolution;
};

TextureCube gSkyMap : register(t0);
SamplerState gSampler : register(s0);
RWTexture2DArray<float4> gOutIrradiance : register(u0);

static const uint kPhiSamples = 32;
static const uint kThetaSamples = 8;

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gResolution || dtid.y >= gResolution) return;

    float2 uv = (float2(dtid.xy) + 0.5f) / float(gResolution);
    float3 N = FaceUvToDirection(gFaceIndex, uv);

    float3 tangent, bitangent;
    BuildBasis(N, tangent, bitangent);

    float3 irradiance = 0.0f.xxx;
    uint sampleCount = 0;

    for (uint p = 0; p < kPhiSamples; ++p)
    {
        float phi = (float(p) / float(kPhiSamples)) * 2.0f * kPi;
        for (uint t = 0; t < kThetaSamples; ++t)
        {
            float theta = (float(t) / float(kThetaSamples)) * 0.5f * kPi;

            float3 tangentSpaceDir = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleDir = tangentSpaceDir.x * tangent + tangentSpaceDir.y * bitangent + tangentSpaceDir.z * N;

            // cos(theta) weight for the cosine-weighted hemisphere integral,
            // sin(theta) weight from the solid-angle element in spherical coords.
            irradiance += gSkyMap.SampleLevel(gSampler, sampleDir, 0).rgb * cos(theta) * sin(theta);
            sampleCount++;
        }
    }

    irradiance = kPi * irradiance / float(sampleCount);
    gOutIrradiance[uint3(dtid.xy, gFaceIndex)] = float4(irradiance, 1.0f);
}
