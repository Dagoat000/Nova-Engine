#include "IBLCommon.hlsli"

cbuffer SkyConstants : register(b0)
{
    float3 sunDir;
    float sunIntensity;

    float3 sunColor;
    float sunSharpness;

    float3 horizon;
    float _p0;

    float3 zenith;
    float _p1;
};

cbuffer FaceConstants : register(b1)
{
    uint gFaceIndex;
    uint gResolution;
};

RWTexture2DArray<float4> gOutput : register(u0);

static const float PI = 3.14159265359f;

float Hash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float Hash31(float3 p)
{
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float ValueNoise2D(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);

    f = f * f * (3.0f - 2.0f * f);

    float a = Hash21(i);
    float b = Hash21(i + float2(1.0f, 0.0f));
    float c = Hash21(i + float2(0.0f, 1.0f));
    float d = Hash21(i + float2(1.0f, 1.0f));

    return lerp(
        lerp(a, b, f.x),
        lerp(c, d, f.x),
        f.y
    );
}

float FBM2D(float2 p)
{
    float value = 0.0f;
    float amplitude = 0.5f;

    value += ValueNoise2D(p) * amplitude;
    p *= 2.0f;
    amplitude *= 0.5f;

    value += ValueNoise2D(p) * amplitude;
    p *= 2.03f;
    amplitude *= 0.5f;

    value += ValueNoise2D(p) * amplitude;
    p *= 2.01f;
    amplitude *= 0.5f;

    value += ValueNoise2D(p) * amplitude;

    return value;
}

float PhaseRayleigh(float cosTheta)
{
    return 0.75f *
        (1.0f + cosTheta * cosTheta);
}

float PhaseMie(float cosTheta, float g)
{
    float g2 = g * g;

    float denom =
        1.0f +
        g2 -
        2.0f * g * cosTheta;

    return
        (1.0f - g2) /
        (4.0f * PI *
         pow(max(denom, 1e-4f), 1.5f));
}

float3 EvaluateAtmosphere(float3 dir)
{
    dir = normalize(dir);

    float3 L =
        normalize(sunDir);

    float sunDot =
        dot(dir, L);

    float height =
        saturate(dir.y * 0.5f + 0.5f);

    float rayleigh =
        PhaseRayleigh(sunDot);

    float mie =
        PhaseMie(sunDot, 0.76f);

    float zenithFade =
        pow(height, 0.45f);

    float3 rayleighColor =
        float3(
            0.18f,
            0.40f,
            1.0f
        ) *
        rayleigh *
        (0.45f + 0.55f * zenithFade);

    float3 mieColor =
        sunColor *
        mie *
        sunIntensity *
        0.008f;

    float sunset =
        pow(
            1.0f -
            saturate(dir.y * 0.5f + 0.5f),
            4.0f
        );

    float3 warmHorizon =
        float3(
            1.0f,
            0.32f,
            0.08f
        ) *
        sunset *
        max(sunDot, 0.0f) *
        0.16f;

    float3 baseGradient =
        lerp(
            horizon,
            zenith,
            zenithFade
        );

    float3 atmosphere =
        baseGradient +
        rayleighColor * 0.24f +
        mieColor +
        warmHorizon;

    float horizonHaze =
        exp(
            -abs(dir.y) * 8.0f
        );

    atmosphere +=
        float3(
            0.08f,
            0.11f,
            0.15f
        ) *
        horizonHaze *
        0.35f;

    float sunGlow =
        pow(
            saturate(sunDot),
            1.0f
        );

    atmosphere +=
        sunColor *
        sunGlow *
        0.002f *
        sunIntensity;

    return max(
        atmosphere,
        0.0f
    );
}

float CloudLargeShape(float2 p)
{
    float n0 =
        FBM2D(
            p * 0.045f
        );

    float n1 =
        FBM2D(
            p * 0.085f +
            float2(31.7f, 17.2f)
        );

    float n2 =
        ValueNoise2D(
            p * 0.18f +
            float2(12.4f, 52.8f)
        );

    return
        n0 * 0.58f +
        n1 * 0.30f +
        n2 * 0.12f;
}

float CloudCoverage(float2 p)
{
    float shape =
        CloudLargeShape(p);

    return smoothstep(
        0.47f,
        0.61f,
        shape
    );
}

float CloudDetail(float2 p)
{
    float large =
        FBM2D(
            p * 0.20f
        );

    float medium =
        FBM2D(
            p * 0.55f +
            float2(41.3f, 9.7f)
        );

    return
        large * 0.68f +
        medium * 0.32f;
}

float CloudVerticalShape(float h)
{
    float base =
        smoothstep(
            0.0f,
            0.08f,
            h
        );

    float lowerBody =
        smoothstep(
            0.05f,
            0.20f,
            h
        );

    float upperBody =
        1.0f -
        smoothstep(
            0.68f,
            1.0f,
            h
        );

    float dome =
        1.0f -
        smoothstep(
            0.72f,
            1.0f,
            h
        );

    return
        base *
        lowerBody *
        upperBody *
        (0.82f + dome * 0.18f);
}

float CloudDensityAt(
    float3 position)
{
    const float cloudBase = 1.35f;
    const float cloudTop = 2.55f;

    if (position.y <= cloudBase ||
        position.y >= cloudTop)
    {
        return 0.0f;
    }

    float height =
        saturate(
            (position.y - cloudBase) /
            (cloudTop - cloudBase)
        );

    float vertical =
        CloudVerticalShape(
            height
        );

    if (vertical <= 0.001f)
        return 0.0f;

    float2 p =
        position.xz *
        0.11f;

    float coverage =
        CloudCoverage(p);

    float detail =
        CloudDetail(p);

    float density =
        coverage *
        vertical;

    float erosion =
        smoothstep(
            0.28f,
            0.70f,
            detail
        );

    density *=
        lerp(
            0.62f,
            1.0f,
            erosion
        );

    float edgeNoise =
        FBM2D(
            p * 1.7f
        );

    float edge =
        smoothstep(
            0.25f,
            0.68f,
            edgeNoise
        );

    density *=
        lerp(
            0.72f,
            1.0f,
            edge
        );

    return saturate(
        density
    );
}

float CloudLightSample(
    float3 position)
{
    float3 L =
        normalize(sunDir);

    float opticalDepth = 0.0f;

    const float stepSize = 0.18f;

    [unroll]
    for (int i = 1; i <= 5; ++i)
    {
        float3 p =
            position +
            L *
            (
                stepSize *
                (float) i
            );

        float density =
            CloudDensityAt(
                p
            );

        opticalDepth +=
            density *
            0.42f;
    }

    return exp(
        -opticalDepth *
        2.8f
    );
}

float3 EvaluateCloudLighting(
    float3 position,
    float3 viewDir,
    float density)
{
    float3 L =
        normalize(sunDir);

    float sunDot =
        saturate(
            dot(
                viewDir,
                L
            )
        );

    float light =
        CloudLightSample(
            position
        );

    float height =
        saturate(
            (
                position.y -
                1.35f
            ) /
            1.2f
        );

    float3 ambient =
        lerp(
            float3(
                0.10f,
                0.12f,
                0.16f
            ),
            float3(
                0.72f,
                0.80f,
                0.90f
            ),
            height
        );

    float forward =
        PhaseMie(
            dot(viewDir, L),
            0.70f
        );

    float silver =
        pow(
            sunDot,
            22.0f
        );

    float3 direct =
        sunColor *
        sunIntensity *
        light *
        0.52f;

    float3 scattering =
        sunColor *
        sunIntensity *
        forward *
        0.12f;

    float3 silverLining =
        sunColor *
        sunIntensity *
        silver *
        0.85f;

    float underside =
        1.0f -
        smoothstep(
            0.10f,
            0.55f,
            height
        );

    float3 undersideColor =
        float3(
            0.055f,
            0.065f,
            0.085f
        );

    float3 lighting =
        ambient +
        direct +
        scattering +
        silverLining;

    lighting =
        lerp(
            lighting,
            undersideColor,
            underside * 0.55f
        );

    float densityShade =
        lerp(
            1.0f,
            0.76f,
            density
        );

    lighting *=
        densityShade;

    return max(
        lighting,
        0.0f
    );
}
float4 EvaluateCloudLayer(float3 dir)
{
    if (dir.y <= 0.025f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const float cloudBase = 1.35f;
    const float cloudTop = 2.55f;

    float tBase = cloudBase / dir.y;
    float tTop = cloudTop / dir.y;

    if (tBase > 70.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float distance = tTop - tBase;

    if (distance <= 0.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const int steps = 16;

    float stepSize =
        distance / (float) steps;

    float transmittance = 1.0f;
    float3 cloudLight = 0.0f.xxx;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        float t =
            tBase +
            stepSize *
            ((float) i + 0.5f);

        float3 position =
            dir * t;

        float density =
            CloudDensityAt(position);

        if (density > 0.001f)
        {
            float3 lighting =
                EvaluateCloudLighting(
                    position,
                    dir,
                    density
                );

            float extinction =
                density *
                stepSize *
                0.85f;

            float alpha =
                1.0f -
                exp(-extinction);

            cloudLight +=
                lighting *
                alpha *
                transmittance;

            transmittance *=
                exp(-extinction);
        }
    }

    return float4(
        cloudLight,
        transmittance
    );
}

float3 EvaluateSun(float3 dir)
{
    dir = normalize(dir);

    float3 L = normalize(sunDir);

    float sunDot = saturate(dot(dir, L));

    // Real solar angular diameter ≈ 0.53 degrees.
    // Therefore the angular radius is ≈ 0.265 degrees.
    float sunRadius = radians(0.265f);

    // Extremely small transition at the solar edge.
    float sunSoftness = radians(0.0025f);

    float sunCos = cos(sunRadius);
    float sunEdge = cos(sunRadius + sunSoftness);

    float disc = smoothstep(
        sunEdge,
        sunCos,
        sunDot
    );

    // Actual solar disk.
    float3 sunDisc =
        sunColor *
        disc *
        8.0f;

    // Almost no visible halo.
    float tightGlow =
        pow(sunDot, 8192.0f);

    float3 tightGlowColor =
        sunColor *
        tightGlow *
        0.25f;

    return sunDisc + tightGlowColor;
}

float3 ApplySkyContrast(float3 color)
{
    color =
        max(
            color,
            0.0f
        );

    float luminance =
        dot(
            color,
            float3(
                0.2126f,
                0.7152f,
                0.0722f
            )
        );

    return lerp(
        luminance.xxx,
        color,
        1.05f
    );
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gResolution ||
        id.y >= gResolution)
    {
        return;
    }

    float2 uv =
        (
            float2(id.xy) +
            0.5f
        ) /
        float2(
            gResolution,
            gResolution
        );

    float3 dir =
        FaceUvToDirection(
            gFaceIndex,
            uv
        );

    float3 sky =
        EvaluateAtmosphere(
            dir
        );

    float4 clouds =
        EvaluateCloudLayer(
            dir
        );

    sky =
        sky *
        clouds.a +
        clouds.rgb;

    sky +=
        EvaluateSun(
            dir
        );

    sky =
        ApplySkyContrast(
            sky
        );

    sky =
        max(
            sky,
            0.0f
        );

    gOutput[
        uint3(
            id.xy,
            0
        )
    ] =
        float4(
            sky,
            1.0f
        );
}