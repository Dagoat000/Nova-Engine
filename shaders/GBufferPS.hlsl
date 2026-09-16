// File: shaders/GBufferPS.hlsl
//
// Writes material data into the G-buffer instead of shading directly -
// the deferred lighting pass (DeferredLightingPS.hlsl) does the actual
// Cook-Torrance evaluation once per pixel, regardless of how many objects'
// geometry overlapped it here.
//
// G-buffer layout (see Graphics/GBuffer.h for the exact resource formats):
//   RT0 (albedo, RGBA8_UNORM):              albedo.rgb, alpha unused
//   RT1 (normal, RGBA16_FLOAT):             world-space normal.xyz (post normal-map), w unused
//   RT2 (metallicRoughnessAO, RGBA8_UNORM): r=AO, g=roughness, b=metallic, a unused

cbuffer MaterialConstants : register(b1)
{
    float4 gBaseColorFactor;
    float  gMetallicFactor;
    float  gRoughnessFactor;
    float  gAoStrength;
    float  _matPad;
};

Texture2D gAlbedoTex             : register(t0);
Texture2D gNormalTex             : register(t1);
Texture2D gMetallicRoughnessTex  : register(t2);
Texture2D gAoTex                 : register(t3);
SamplerState gSampler            : register(s0);

struct PSInput
{
    float4 clipPosition : SV_POSITION;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float2 uv           : TEXCOORD;
};

struct PSOutput
{
    float4 albedo               : SV_TARGET0;
    float4 normal               : SV_TARGET1;
    float4 metallicRoughnessAO  : SV_TARGET2;
};

PSOutput main(PSInput input)
{
    PSOutput output;

    float4 albedoSample = gAlbedoTex.Sample(gSampler, input.uv);
    output.albedo = float4(albedoSample.rgb * gBaseColorFactor.rgb, 1.0f);

    float3 mr = gMetallicRoughnessTex.Sample(gSampler, input.uv).rgb;
    float roughness = saturate(mr.g * gRoughnessFactor);
    float metallic = saturate(mr.b * gMetallicFactor);
    roughness = max(roughness, 0.045f); // avoid a near-zero-roughness singularity in the lighting pass's GGX denominator
    float ao = lerp(1.0f, gAoTex.Sample(gSampler, input.uv).r, gAoStrength);
    output.metallicRoughnessAO = float4(ao, roughness, metallic, 1.0f);

    // Tangent-space normal map -> world space via the per-vertex TBN basis.
    // This is the ONLY place normal mapping is resolved - the G-buffer
    // stores the final world-space normal, so the lighting pass never
    // needs the tangent basis at all.
    float3 N = normalize(input.worldNormal);
    float3 T = normalize(input.worldTangent - N * dot(N, input.worldTangent));
    float3 B = cross(N, T);
    float3 sampledNormal = gNormalTex.Sample(gSampler, input.uv).xyz * 2.0f - 1.0f;
    N = normalize(mul(sampledNormal, float3x3(T, B, N)));
    output.normal = float4(N, 0.0f);

    return output;
}
