// File: shaders/SkyCS.hlsl
//
// Procedural sky, written directly into one face of the sky cubemap.
// Purely analytic (a horizon/zenith gradient plus a bright sun disk) - no
// samples needed, no HDR asset required. Dispatched once per face (6 total)
// during startup; the sky is currently static (regenerating it per-frame
// to follow a moving sun is a natural but not-yet-done extension).

#include "IBLCommon.hlsli"

cbuffer SkyConstants : register(b0)
{
    float3 gSunDirWS;     // direction the light travels (surface-to-sun is -gSunDirWS)
    float  gSunIntensity;
    float3 gSunColor;
    float  gSunSharpness; // higher = smaller, crisper sun disk
    float3 gHorizonColor;
    float  _pad0;
    float3 gZenithColor;
    float  _pad1;
};

cbuffer FaceConstants : register(b1)
{
    uint gFaceIndex;
    uint gResolution;
};

RWTexture2DArray<float4> gOutCubemap : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gResolution || dtid.y >= gResolution) return;

    float2 uv = (float2(dtid.xy) + 0.5f) / float(gResolution);
    float3 dir = FaceUvToDirection(gFaceIndex, uv);

    float horizonBlend = saturate(dir.y * 0.5f + 0.5f);
    float3 sky = lerp(gHorizonColor, gZenithColor, pow(horizonBlend, 0.6f));

    float3 toSun = normalize(-gSunDirWS);
    float sunDot = saturate(dot(dir, toSun));
    float sunDisk = pow(sunDot, gSunSharpness);
    // A softer, wider halo around the disk itself - without this the sun
    // reads as a single hard, unnaturally isolated dot against the gradient.
    float sunHalo = pow(sunDot, gSunSharpness * 0.05f) * 0.3f;

    float3 color = sky + gSunColor * gSunIntensity * (sunDisk + sunHalo);

    gOutCubemap[uint3(dtid.xy, gFaceIndex)] = float4(color, 1.0f);
}
