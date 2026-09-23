cbuffer LensConstants : register(b0)
{
    float4 gLightDirCamera;
    float2 gSunScreenPos;
    float gSunVisible;
    float gTime;
    float gSpread;
    float gPlateSize;
    float gNumInterfaces;
    float gCoatingQuality;
    float gApertureOpening;
    float gNumberOfBlades;
    float gFlareScale;
    float gStrength;
};

struct FlareVertex
{
    float4 pos;
    float4 color;
    float4 coordinates;
    float4 reflectance;
};

StructuredBuffer<FlareVertex> gVertices : register(t0);
Texture2D gDepth : register(t1);
SamplerState gLinearSampler : register(s0);

struct VSOut
{
    float4 position : SV_POSITION;
    float4 color : TEXCOORD0;
    float4 coordinates : TEXCOORD1;
    float4 reflectance : TEXCOORD2;
};

VSOut VS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint patchSize = 32;
    uint index = instanceId * patchSize * patchSize + vertexId;
    FlareVertex v = gVertices[index];

    float2 sunNdc = float2(gSunScreenPos.x * 2.0f - 1.0f, 1.0f - gSunScreenPos.y * 2.0f);
    float2 offset = v.pos.xy / max(gPlateSize, 0.001f) * gFlareScale;

    VSOut o;
    o.position = float4(sunNdc + offset, 0.0f, 1.0f);
    o.color = v.color;
    o.coordinates = v.coordinates;
    o.reflectance = v.reflectance;
    return o;
}

float ApertureMask(float2 uv)
{
    float2 p = uv * 2.0f - 1.0f;
    float radius = length(p);
    float angle = atan2(p.y, p.x);
    float blades = max(gNumberOfBlades, 3.0f);
    float sector = cos(fmod(angle + gApertureOpening, 6.28318530718f) * blades);
    float edge = 0.70f + 0.08f * sector;
    return 1.0f - smoothstep(edge, edge + 0.10f, radius);
}

float Starburst(float2 uv)
{
    float2 p = uv * 2.0f - 1.0f;
    float r = length(p);
    float a = atan2(p.y, p.x);
    float blades = max(gNumberOfBlades, 3.0f);
    float ray = pow(saturate(abs(cos(a * blades))), 48.0f);
    float radial = exp(-r * 2.5f);
    return ray * radial;
}

float4 PS(VSOut input) : SV_Target
{
    if (gSunVisible <= 0.0f)
        discard;

    float2 apertureUv = input.coordinates.zw * 0.5f + 0.5f;
    float aperture = ApertureMask(apertureUv);
    float lensDistance = length(input.coordinates.xy);
    float sunDisk = 1.0f - saturate((lensDistance - 0.80f) / 0.20f);
    sunDisk = smoothstep(0.0f, 1.0f, sunDisk);
    sunDisk *= lerp(0.5f, 1.0f, saturate(lensDistance));

    float area = saturate(input.color.w);
    float3 flare = input.reflectance.rgb * area * aperture * sunDisk;
    flare += input.reflectance.rgb * Starburst(apertureUv) * 0.045f;
    flare *= gStrength;

    if (max(flare.r, max(flare.g, flare.b)) < 1e-5f)
        discard;

    return float4(flare, 1.0f);
}
