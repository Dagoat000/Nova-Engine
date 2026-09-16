// File: shaders/DeferredLightingPS.hlsl
//
// The deferred pipeline's single lighting pass: reconstructs world
// position from the G-buffer's depth, reads the rest of the G-buffer, and
// evaluates the same Cook-Torrance BRDF (shared/Common.hlsli) once per
// pixel for the shadowed directional light plus every point light. Ambient
// lighting comes from real image-based lighting (Graphics/EnvironmentMap)
// instead of a flat constant: diffuse irradiance + a prefiltered specular
// reflection combined via the split-sum approximation's BRDF LUT.
// Background pixels (no G-buffer geometry) show the procedural sky
// directly instead of a flat clear color.

#include "Common.hlsli"

cbuffer ScreenConstants : register(b0)
{
    float4x4 gInvViewProj;
    float3   gCameraPosWS;
    float    gRtReflectionsThreshold; // roughness must be BELOW this to use the ray-traced reflection texture; see Graphics/Raytracing
};

struct PointLight
{
    float3 positionWS;
    float  range;
    float3 color;
    float  intensity;
};

cbuffer LightConstants : register(b1)
{
    float3     gDirLightDirWS;
    float      gDirLightIntensity;
    float3     gDirLightColor;
    float      _lightPad0;
    float4x4   gLightViewProj;
    PointLight gPointLights[2];
    uint       gNumPointLights;
    float3     _lightPad1;
};

Texture2D gGBufferAlbedo   : register(t0);
Texture2D gGBufferNormal   : register(t1);
Texture2D gGBufferMRAO     : register(t2);
Texture2D gGBufferDepth    : register(t3);
Texture2D gShadowMap       : register(t4);
TextureCube gSkyMap          : register(t5);
TextureCube gIrradianceMap   : register(t6);
TextureCube gPrefilteredMap  : register(t7);
Texture2D   gBrdfLut         : register(t8);
Texture2D   gRtReflections   : register(t9);

SamplerState gPointSampler       : register(s0);
SamplerComparisonState gShadowSampler : register(s1);
SamplerState gLinearSampler      : register(s2);

static const float kShadowMapSize = 2048.0f;
static const float kPrefilteredMaxMip = 5.0f; // kPrefilterMipCount - 1, see Graphics/EnvironmentMap.h

struct PSInput
{
    float4 clipPosition : SV_POSITION;
    float2 uv           : TEXCOORD;
};

// Fresnel with a roughness term (Sebastien Lagarde's variant): at grazing
// angles on rough surfaces, ordinary Fresnel-Schlick over-brightens the
// ambient specular rim - clamping F0's upper bound by (1-roughness) fixes
// that without needing a full multi-scatter model.
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 upperBound = max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0);
    return F0 + (upperBound - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float4 main(PSInput input) : SV_TARGET
{
    float depth = gGBufferDepth.Sample(gPointSampler, input.uv).r;

    // Reconstruct the view ray direction regardless of depth - needed both
    // for the background sky sample below and for world-position
    // reconstruction when there IS geometry.
    float2 ndcXY = input.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);

    // Far plane / cleared depth = no geometry at this pixel: show the sky
    // itself instead of shading anything.
    if (depth >= 0.99999f)
    {
        float4 nearPointH = mul(float4(ndcXY, 0.0f, 1.0f), gInvViewProj);
        float3 nearPoint = nearPointH.xyz / nearPointH.w;
        float3 viewRay = normalize(nearPoint - gCameraPosWS);
        float3 sky = gSkyMap.SampleLevel(gLinearSampler, viewRay, 0).rgb;
        sky = sky / (sky + 1.0f);
        sky = pow(sky, 1.0f / 2.2f);
        return float4(sky, 1.0f);
    }

    // Reconstruct world-space position from the depth buffer instead of
    // carrying it through the G-buffer - one less RGBA32_FLOAT render
    // target, at the cost of one matrix multiply per pixel here.
    float4 clipPos = float4(ndcXY, depth, 1.0f);
    float4 worldPosH = mul(clipPos, gInvViewProj);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    float3 albedo = gGBufferAlbedo.Sample(gPointSampler, input.uv).rgb;
    float3 mrao = gGBufferMRAO.Sample(gPointSampler, input.uv).rgb;
    float ao = mrao.r;
    float roughness = mrao.g;
    float metallic = mrao.b;
    float3 N = normalize(gGBufferNormal.Sample(gPointSampler, input.uv).xyz);

    float3 V = normalize(gCameraPosWS - worldPos);
    float NdotV = max(dot(N, V), 1e-4f);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float3 directLight = float3(0.0f, 0.0f, 0.0f);

    // --- Directional light (shadowed) ---
    {
        float3 L = normalize(-gDirLightDirWS);
        float NdotL = max(dot(N, L), 0.0f);
        float4 lightClipPos = mul(float4(worldPos, 1.0f), gLightViewProj);
        float shadow = SampleShadow(gShadowMap, gShadowSampler, kShadowMapSize, lightClipPos, NdotL);
        float3 brdf = EvaluateBRDF(N, V, L, albedo, metallic, roughness, F0);
        directLight += brdf * gDirLightColor * gDirLightIntensity * NdotL * shadow;
    }

    // --- Point lights (unshadowed) ---
    for (uint i = 0; i < gNumPointLights; ++i)
    {
        float3 toLight = gPointLights[i].positionWS - worldPos;
        float dist = length(toLight);
        float3 L = toLight / max(dist, 1e-4f);
        float NdotL = max(dot(N, L), 0.0f);

        float falloff = saturate(1.0f - pow(dist / gPointLights[i].range, 4.0f));
        float attenuation = (falloff * falloff) / max(dist * dist, 1e-2f);

        float3 brdf = EvaluateBRDF(N, V, L, albedo, metallic, roughness, F0);
        directLight += brdf * gPointLights[i].color * gPointLights[i].intensity * attenuation * NdotL;
    }

    // --- Image-based (ambient) lighting ---
    // Split-sum approximation: diffuse from the irradiance map, specular
    // from the prefiltered environment map + BRDF LUT. This is what
    // replaces the old flat ambient constant with something that actually
    // reflects the sky and responds correctly to roughness/metalness.
    float3 kS = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD = (1.0f.xxx - kS) * (1.0f - metallic);

    float3 irradiance = gIrradianceMap.SampleLevel(gLinearSampler, N, 0).rgb;
    float3 diffuseIBL = kD * irradiance * albedo;

    float3 R = reflect(-V, N);
    float3 prefilteredColor;
    if (roughness < gRtReflectionsThreshold)
    {
        // RaytracingReflections.hlsl's ray generation shader applies the
        // exact same threshold against the same G-buffer data, so this
        // texel is guaranteed to hold a real traced result here (or a sky
        // sample from the miss shader) rather than an untouched texel.
        prefilteredColor = gRtReflections.Sample(gPointSampler, input.uv).rgb;
    }
    else
    {
        prefilteredColor = gPrefilteredMap.SampleLevel(gLinearSampler, R, roughness * kPrefilteredMaxMip).rgb;
    }
    float2 brdf = gBrdfLut.SampleLevel(gLinearSampler, float2(NdotV, roughness), 0).rg;
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

    float3 ambient = (diffuseIBL + specularIBL) * ao;

    float3 color = ambient + directLight;

    // Reinhard tonemap + gamma correction - the swap chain is a UNORM
    // (not sRGB) target, so gamma must be applied manually here.
    color = color / (color + 1.0f);
    color = pow(color, 1.0f / 2.2f);

    return float4(color, 1.0f);
}
