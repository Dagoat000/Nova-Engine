// File: shaders/ShadowVS.hlsl
//
// Depth-only pass: transforms geometry straight into the light's clip
// space. No pixel shader runs at all (see Pipeline.cpp's shadow PSO) - the
// rasterizer's depth output is the only thing this pass produces.

cbuffer ShadowConstants : register(b0)
{
    float4x4 gWorldViewProjLight;
};

struct VSInput
{
    float3 position : POSITION;
};

float4 main(VSInput input) : SV_POSITION
{
    return mul(float4(input.position, 1.0f), gWorldViewProjLight);
}
