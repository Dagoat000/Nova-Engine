// File: shaders/GBufferVS.hlsl
//
// Geometry pass of the deferred pipeline: transforms vertices and passes
// through everything the G-buffer pixel shader needs to write world-space
// normal + material data. No lighting, no shadow-space position - those
// belong to the lighting pass now, which reconstructs world position from
// depth instead of carrying it through the G-buffer.

cbuffer PerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 clipPosition : SV_POSITION;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float2 uv           : TEXCOORD;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4 worldPos = mul(float4(input.position, 1.0f), gWorld);
    output.clipPosition = mul(worldPos, gViewProj);

    // Uniform scale only, so the world matrix itself is a valid normal/
    // tangent transform - a real inverse-transpose normal matrix is added
    // once non-uniform scaling shows up.
    output.worldNormal = normalize(mul(input.normal, (float3x3)gWorld));
    output.worldTangent = normalize(mul(input.tangent, (float3x3)gWorld));
    output.uv = input.uv;

    return output;
}
