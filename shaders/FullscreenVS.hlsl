// File: shaders/FullscreenVS.hlsl
//
// Generates a single full-screen triangle from just the vertex ID - no
// vertex buffer, no index buffer, no input layout. The classic trick:
// 3 vertices placed so the triangle's edge crosses the far corners of the
// screen, covering the full viewport with no wasted overdraw from a
// diagonal seam (which a screen-space quad made of 2 triangles would have).

struct VSOutput
{
    float4 clipPosition : SV_POSITION;
    float2 uv           : TEXCOORD;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    VSOutput output;

    // id=0 -> (0,0), id=1 -> (2,0), id=2 -> (0,2) in UV space [0,2] -
    // covers [0,1]x[0,1] plus the overhang needed for a single covering
    // triangle instead of two.
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.clipPosition = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);

    return output;
}
