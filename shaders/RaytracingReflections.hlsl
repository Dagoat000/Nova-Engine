// File: shaders/RaytracingReflections.hlsl
//
// A single reflection ray per eligible pixel (the ray generation shader
// itself skips pixels above the roughness threshold - see ScreenConstants'
// gRtReflectionsThreshold, shared with DeferredLightingPS.hlsl so both
// agree on which surfaces count as "mirror-like"). Reflections are
// single-bounce only: the closest-hit shader shades directly (one
// directional light, no shadow test, plus a cheap diffuse-IBL ambient
// term) rather than recursing into a second TraceRay - a second bounce's
// contribution to a reflection of a reflection is a small, usually
// unnoticeable visual gain for double the ray budget, so it's a deliberate
// scope cut rather than an oversight.
//
// Compiled with DXC (lib_6_3), not the legacy FXC-based D3DCompiler every
// other shader in this engine uses - DXR shader libraries require DXIL,
// which FXC cannot produce. See Graphics/Raytracing.cpp.

#include "Common.hlsli"

// --- Global root signature (shared by all shader stages here) ---
RaytracingAccelerationStructure gScene : register(t0);

cbuffer ScreenConstants : register(b0)
{
    float4x4 gInvViewProj;
    float3   gCameraPosWS;
    float    gRtReflectionsThreshold; // roughness must be BELOW this to get a traced reflection
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

Texture2D gGBufferAlbedo  : register(t1);
Texture2D gGBufferNormal  : register(t2);
Texture2D gGBufferMRAO    : register(t3);
Texture2D gGBufferDepth   : register(t4);

TextureCube gSkyMap         : register(t5);
TextureCube gIrradianceMap  : register(t6);
TextureCube gPrefilteredMap : register(t7);
Texture2D   gBrdfLut        : register(t8);

RWTexture2D<float4> gOutput : register(u0);

SamplerState gPointSampler  : register(s0);
SamplerState gLinearSampler : register(s1);

// --- Local root signature (space1, one shader-table record per instance) ---
ByteAddressBuffer gVertexBuffer : register(t0, space1);
ByteAddressBuffer gIndexBuffer  : register(t1, space1);
cbuffer InstanceMaterial : register(b0, space1)
{
    float3 gAlbedo;
    float  gMetallic;
    float  gRoughness;
    float3 _instPad;
};

struct ReflectionPayload
{
    float3 color;
    float  _pad;
};

static const uint kVertexStrideBytes = 44; // position(12) + normal(12) + tangent(12) + uv(8) - see rendering::Vertex
static const float kAmbientIntensity = 0.06f; // matches DeferredLightingPS.hlsl's IBL weighting

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dims = DispatchRaysDimensions().xy;
    float2 uv = (float2(pixel) + 0.5f) / float2(dims);

    float depth = gGBufferDepth.SampleLevel(gPointSampler, uv, 0).r;
    float roughness = gGBufferMRAO.SampleLevel(gPointSampler, uv, 0).g;

    // Background pixels, or surfaces too rough to show a sharp reflection
    // anyway, are left at (0,0,0,0) - DeferredLightingPS.hlsl only ever
    // reads this texture when its own roughness check agrees a traced
    // reflection should exist, so an untraced texel here is never sampled.
    if (depth >= 0.99999f || roughness >= gRtReflectionsThreshold)
    {
        gOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float2 ndcXY = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 clipPos = float4(ndcXY, depth, 1.0f);
    float4 worldPosH = mul(clipPos, gInvViewProj);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    float3 N = normalize(gGBufferNormal.SampleLevel(gPointSampler, uv, 0).xyz);
    float3 V = normalize(gCameraPosWS - worldPos);
    float3 R = reflect(-V, N);

    RayDesc ray;
    ray.Origin = worldPos + N * 0.01f; // small bias along the normal to avoid self-intersection
    ray.Direction = R;
    ray.TMin = 0.01f;
    ray.TMax = 100.0f;

    ReflectionPayload payload;
    payload.color = float3(0.0f, 0.0f, 0.0f);
    payload._pad = 0.0f;

    TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    gOutput[pixel] = float4(payload.color, 1.0f);
}

[shader("closesthit")]
void ClosestHit(inout ReflectionPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    uint primitiveIndex = PrimitiveIndex();
    uint i0 = gIndexBuffer.Load(primitiveIndex * 12 + 0);
    uint i1 = gIndexBuffer.Load(primitiveIndex * 12 + 4);
    uint i2 = gIndexBuffer.Load(primitiveIndex * 12 + 8);

    // Normal lives at byte offset 12 within each 44-byte vertex.
    float3 n0 = asfloat(gVertexBuffer.Load3(i0 * kVertexStrideBytes + 12));
    float3 n1 = asfloat(gVertexBuffer.Load3(i1 * kVertexStrideBytes + 12));
    float3 n2 = asfloat(gVertexBuffer.Load3(i2 * kVertexStrideBytes + 12));

    float3 bary = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    float3 localNormal = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    float3 N = normalize(mul((float3x3)ObjectToWorld3x4(), localNormal));

    float3 V = normalize(-WorldRayDirection());
    float3 L = normalize(-gDirLightDirWS);
    float NdotL = max(dot(N, L), 0.0f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), gAlbedo, gMetallic);
    float3 brdf = EvaluateBRDF(N, V, L, gAlbedo, gMetallic, gRoughness, F0);

    // No shadow test at this bounce (see the file header comment) - a
    // reflected object sitting in the directional light's shadow will
    // read slightly too bright. Acceptable for a single reflection bounce.
    float3 direct = brdf * gDirLightColor * gDirLightIntensity * NdotL;

    // Diffuse-only IBL ambient (no specular IBL at this bounce depth -
    // reflecting a reflection's specular highlight is not worth a second
    // texture fetch's cost for how little it would be seen).
    float3 irradiance = gIrradianceMap.SampleLevel(gLinearSampler, N, 0).rgb;
    float3 ambient = gAlbedo * irradiance * kAmbientIntensity * (1.0f - gMetallic);

    payload.color = direct + ambient;
}

[shader("miss")]
void Miss(inout ReflectionPayload payload)
{
    payload.color = gSkyMap.SampleLevel(gLinearSampler, WorldRayDirection(), 0).rgb;
}
