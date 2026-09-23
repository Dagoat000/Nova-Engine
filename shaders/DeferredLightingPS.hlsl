#include "Common.hlsli"

cbuffer ScreenConstants : register(b0)
{
    float4x4 gInvViewProj;
    float4x4 gViewProj;
    float3 gCameraPosWS;
    float gRtReflectionsThreshold;

    float2 gSunScreenPos;
    float gSunVisible;
    float gScreenPad0;
};

struct PointLight
{
    float3 positionWS;
    float range;
    float3 color;
    float intensity;
};

cbuffer LightConstants : register(b1)
{
    float3 gDirLightDirWS;
    float gDirLightIntensity;
    float3 gDirLightColor;
    float _lightPad0;
    float4x4 gLightViewProj;
    PointLight gPointLights[2];
    uint gNumPointLights;
    float3 _lightPad1;
};

Texture2D gGBufferAlbedo : register(t0);
Texture2D gGBufferNormal : register(t1);
Texture2D gGBufferMRAO : register(t2);
Texture2D gGBufferDepth : register(t3);
Texture2D gShadowMap : register(t4);
TextureCube gSkyMap : register(t5);
TextureCube gIrradianceMap : register(t6);
TextureCube gPrefilteredMap : register(t7);
Texture2D gBrdfLut : register(t8);
Texture2D gRtReflections : register(t9);

SamplerState gPointSampler : register(s0);
SamplerComparisonState gShadowSampler : register(s1);
SamplerState gLinearSampler : register(s2);

static const float kShadowMapSize = 2048.0f;
static const float kPrefilteredMaxMip = 5.0f;

struct PSInput
{
    float4 clipPosition : SV_POSITION;
    float2 uv : TEXCOORD;
};

float3 FresnelSchlickRoughness(
float cosTheta,
float3 F0,
float roughness)
{
    float3 upperBound =
max(
float3(
1.0f - roughness,
1.0f - roughness,
1.0f - roughness
),
F0
);
    return
    F0 +
    (upperBound - F0) *
    pow(
        saturate(1.0f - cosTheta),
        5.0f
    );

}
float3 EvaluateLensFlare(float3 viewRay, float2 uv)
{
    float3 sunDirection = normalize(-gDirLightDirWS);

    float sunDot = dot(viewRay, sunDirection);

    float2 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = 1.0f - uv.y * 2.0f;

    float4 rayH =
        mul(
            float4(ndc, 0.0f, 1.0f),
            gInvViewProj
        );

    float3 pixelRay =
        normalize(
            rayH.xyz / rayH.w -
            gCameraPosWS
        );

    float alignment =
        saturate(
            dot(
                pixelRay,
                sunDirection
            )
        );

    float sunVisibility =
        smoothstep(
            0.90f,
            0.999f,
            alignment
        );

    float sunAngular =
        acos(
            saturate(alignment)
        );

    float sunSize =
        radians(0.35f);

    float sunDisc =
        1.0f -
        smoothstep(
            sunSize * 0.35f,
            sunSize,
            sunAngular
        );

    float2 center =
        uv -
        0.5f.xx;

    float centerDistance =
        length(center);

    float halo =
        exp(
            -centerDistance *
            centerDistance *
            18.0f
        );

    float wideHalo =
        exp(
            -centerDistance *
            centerDistance *
            3.0f
        );

    float2 flareDirection =
        normalize(
            center +
            float2(1e-5f, 1e-5f)
        );

    float2 perpendicular =
        float2(
            -flareDirection.y,
            flareDirection.x
        );

    float along =
        dot(
            center,
            flareDirection
        );

    float across =
        abs(
            dot(
                center,
                perpendicular
            )
        );

    float streak =
        exp(
            -across *
            across /
            0.00015f
        ) *
        exp(
            -along *
            along /
            0.20f
        );

    float ghost1 =
        exp(
            -pow(
                centerDistance - 0.20f,
                2.0f
            ) *
            300.0f
        );

    float ghost2 =
        exp(
            -pow(
                centerDistance - 0.38f,
                2.0f
            ) *
            180.0f
        );

    float ghost3 =
        exp(
            -pow(
                centerDistance - 0.58f,
                2.0f
            ) *
            120.0f
        );

    float3 sunColor =
        gDirLightColor *
        gDirLightIntensity;

    float3 flare =
        sunColor *
        sunDisc *
        15.0f;

    flare +=
        sunColor *
        halo *
        sunVisibility *
        2.5f;

    flare +=
        sunColor *
        wideHalo *
        sunVisibility *
        0.25f;

    flare +=
        sunColor *
        streak *
        sunVisibility *
        0.35f;

    flare +=
        float3(1.0f, 0.45f, 0.20f) *
        ghost1 *
        sunVisibility *
        0.30f;

    flare +=
        float3(0.25f, 0.55f, 1.0f) *
        ghost2 *
        sunVisibility *
        0.20f;

    flare +=
        float3(1.0f, 0.25f, 0.10f) *
        ghost3 *
        sunVisibility *
        0.12f;

    return flare;
}

float4 main(PSInput input) : SV_TARGET
{
    float depth =
gGBufferDepth.Sample(
gPointSampler,
input.uv
).r;

    float2 ndcXY =
    input.uv *
    float2(2.0f, -2.0f) +
    float2(-1.0f, 1.0f);

    if (depth >= 0.99999f)
    {
        float2 ndc;

        ndc.x =
        input.uv.x *
        2.0f -
        1.0f;

        ndc.y =
        1.0f -
        input.uv.y *
        2.0f;

        float4 nearPointH =
        mul(
            float4(
                ndc,
                0.0f,
                1.0f
            ),
            gInvViewProj
        );

        float3 nearPointWS =
        nearPointH.xyz /
        nearPointH.w;

        float3 viewRay =
        normalize(
            nearPointWS -
            gCameraPosWS
        );

        float3 sky =
        gSkyMap.SampleLevel(
            gLinearSampler,
            viewRay,
            0
        ).rgb;
        sky += EvaluateLensFlare(viewRay, input.uv);
        
        sky =
        sky /
        (sky + 1.0f);

        sky =
        pow(
            sky,
            1.0f / 2.2f
        );

        return float4(
        sky,
        1.0f
    );
    }

    float4 clipPos =
    float4(
        ndcXY,
        depth,
        1.0f
    );

    float4 worldPosH =
    mul(
        clipPos,
        gInvViewProj
    );

    float3 worldPos =
    worldPosH.xyz /
    worldPosH.w;

    float3 albedo =
    gGBufferAlbedo.Sample(
        gPointSampler,
        input.uv
    ).rgb;

    float3 mrao =
    gGBufferMRAO.Sample(
        gPointSampler,
        input.uv
    ).rgb;

    float ao =
    mrao.r;

    float roughness =
    mrao.g;

    float metallic =
    mrao.b;

    float3 N =
    normalize(
        gGBufferNormal.Sample(
            gPointSampler,
            input.uv
        ).xyz
    );

    float3 V =
    normalize(
        gCameraPosWS -
        worldPos
    );

    float NdotV =
    max(
        dot(N, V),
        1e-4f
    );

    float3 F0 =
    lerp(
        float3(
            0.04f,
            0.04f,
            0.04f
        ),
        albedo,
        metallic
    );

    float3 directLight =
    float3(
        0.0f,
        0.0f,
        0.0f
    );

{
        float3 L =
        normalize(
            -gDirLightDirWS
        );

        float NdotL =
        max(
            dot(N, L),
            0.0f
        );

        float4 lightClipPos =
        mul(
            float4(
                worldPos,
                1.0f
            ),
            gLightViewProj
        );

        float shadow =
        SampleShadow(
            gShadowMap,
            gShadowSampler,
            kShadowMapSize,
            lightClipPos,
            NdotL
        );

        float3 brdf =
        EvaluateBRDF(
            N,
            V,
            L,
            albedo,
            metallic,
            roughness,
            F0
        );

        directLight +=
        brdf *
        gDirLightColor *
        gDirLightIntensity *
        NdotL *
        shadow;
    }

    for (uint i = 0; i < gNumPointLights; ++i)
    {
        float3 toLight =
        gPointLights[i].positionWS -
        worldPos;

        float dist =
        length(toLight);

        float3 L =
        toLight /
        max(
            dist,
            1e-4f
        );

        float NdotL =
        max(
            dot(N, L),
            0.0f
        );

        float falloff =
        saturate(
            1.0f -
            pow(
                dist /
                gPointLights[i].range,
                4.0f
            )
        );

        float attenuation =
        (falloff * falloff) /
        max(
            dist * dist,
            1e-2f
        );

        float3 brdf =
        EvaluateBRDF(
            N,
            V,
            L,
            albedo,
            metallic,
            roughness,
            F0
        );

        directLight +=
        brdf *
        gPointLights[i].color *
        gPointLights[i].intensity *
        attenuation *
        NdotL;
    }

    float3 kS =
    FresnelSchlickRoughness(
        NdotV,
        F0,
        roughness
    );

    float3 kD =
    (1.0f.xxx - kS) *
    (1.0f - metallic);

    float3 irradiance =
    gIrradianceMap.SampleLevel(
        gLinearSampler,
        N,
        0
    ).rgb;

    float3 diffuseIBL =
    kD *
    irradiance *
    albedo;

    float3 R =
    reflect(
        -V,
        N
    );

    float3 prefilteredColor;

    if (roughness < gRtReflectionsThreshold)
    {
        prefilteredColor =
        gRtReflections.Sample(
            gPointSampler,
            input.uv
        ).rgb;
    }
    else
    {
        prefilteredColor =
        gPrefilteredMap.SampleLevel(
            gLinearSampler,
            R,
            roughness *
            kPrefilteredMaxMip
        ).rgb;
    }

    float2 brdf =
    gBrdfLut.SampleLevel(
        gLinearSampler,
        float2(
            NdotV,
            roughness
        ),
        0
    ).rg;

    float3 specularIBL =
    prefilteredColor *
    (
        F0 * brdf.x +
        brdf.y
    );

    float3 ambient =
    (
        diffuseIBL +
        specularIBL
    ) *
    ao;

    float3 color =
    ambient +
    directLight;

    color =
    color /
    (color + 1.0f);

    color =
    pow(
        color,
        1.0f / 2.2f
    );

    return float4(
    color,
    1.0f
);

}