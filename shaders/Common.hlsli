// File: shaders/Common.hlsli
//
// Shared PBR math: Cook-Torrance BRDF (GGX + Smith + Fresnel-Schlick) and
// the PCF shadow lookup. Pulled into its own include so the deferred
// lighting pass is the only place this logic lives - duplicating it across
// shader files was fine back when there was only one shading pass
// (Phase 3/4's forward BasicPS.hlsl); with a separate G-buffer pass now
// producing the inputs, keeping the actual shading math in one place is
// worth the #include.

static const float kPi = 3.14159265f;

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * d * d, 1e-6f);
}

float GeometrySmithGGX(float NdotV, float NdotL, float roughness)
{
    // Direct-lighting remapping of k (as opposed to the IBL remapping).
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float gV = NdotV / (NdotV * (1.0f - k) + k);
    float gL = NdotL / (NdotL * (1.0f - k) + k);
    return gV * gL;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// Cook-Torrance BRDF, evaluated for one light direction. Returns the
// (diffuse + specular) reflectance to be multiplied by that light's
// incoming radiance and NdotL by the caller.
float3 EvaluateBRDF(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness, float3 F0)
{
    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4f);
    float NdotL = max(dot(N, L), 0.0f);
    float NdotH = max(dot(N, H), 0.0f);
    float HdotV = max(dot(H, V), 0.0f);

    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmithGGX(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(HdotV, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);

    // Energy conservation: whatever fraction of light is reflected
    // specularly (F) cannot also be diffusely reflected. Metals have no
    // diffuse term at all.
    float3 kD = (1.0f.xxx - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo / kPi;

    return diffuse + specular;
}

// 3x3 hardware-PCF kernel: each tap is itself a bilinearly-filtered
// hardware comparison (SampleCmpLevelZero with a COMPARISON_..._LINEAR
// sampler), so 9 taps here behaves like a much larger software PCF kernel
// would. A small slope-scaled bias (on top of the shadow PSO's constant
// rasterizer depth bias) fights shadow acne on angled surfaces.
float SampleShadow(Texture2D shadowMap, SamplerComparisonState shadowSampler, float shadowMapSize, float4 lightClipPos, float NdotL)
{
    float3 ndc = lightClipPos.xyz / lightClipPos.w;
    float2 uv = ndc.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y; // NDC +Y is up; texture V is down
    float currentDepth = ndc.z;

    float bias = max(0.0015f * (1.0f - NdotL), 0.0003f);
    float texelSize = 1.0f / shadowMapSize;

    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texelSize;
            shadow += shadowMap.SampleCmpLevelZero(shadowSampler, uv + offset, currentDepth - bias);
        }
    }
    return shadow / 9.0f;
}
