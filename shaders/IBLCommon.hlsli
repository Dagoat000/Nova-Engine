// File: shaders/IBLCommon.hlsli
//
// Shared helpers for the IBL precompute compute shaders: a low-discrepancy
// Hammersley sequence (cheap, deterministic quasi-random 2D points - no
// RNG state needed) and GGX importance sampling built on top of it. This
// is the standard split-sum-approximation toolkit (Karis, "Real Shading
// in Unreal Engine 4", 2013) used by SkyCS/IrradianceCS/PrefilterCS/BrdfLutCS.

static const float kPi = 3.14159265f;

// Van der Corput radical inverse in base 2 - the bit-reversal trick makes
// this a handful of instructions instead of a loop.
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f; // / 2^32
}

float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC(i));
}

// Builds an orthonormal basis around N (Duff et al.'s branchless method),
// used to rotate hemisphere/lobe samples (defined in a local +Z-up frame)
// into world space.
void BuildBasis(float3 N, out float3 tangent, out float3 bitangent)
{
    float sign = N.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + N.z);
    float b = N.x * N.y * a;
    tangent = float3(1.0f + sign * N.x * N.x * a, sign * b, -sign * N.x);
    bitangent = float3(b, sign + N.y * N.y * a, -N.y);
}

// Importance-samples the GGX normal distribution for the given roughness,
// returning a half-vector in world space. Concentrates samples where the
// specular lobe actually has energy, so a handful of samples approximates
// the full integral well.
float3 ImportanceSampleGGX(float2 xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0f * kPi * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 h;
    h.x = cos(phi) * sinTheta;
    h.y = sin(phi) * sinTheta;
    h.z = cosTheta;

    float3 tangent, bitangent;
    BuildBasis(N, tangent, bitangent);
    return normalize(tangent * h.x + bitangent * h.y + N * h.z);
}

// Same GGX geometry term as Common.hlsli, but with the IBL remapping of k
// (as opposed to the direct-lighting remapping) - the two are genuinely
// different constants in the split-sum approximation.
float GeometrySchlickGGX_IBL(float NdotX, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0f;
    return NdotX / (NdotX * (1.0f - k) + k);
}

float GeometrySmith_IBL(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX_IBL(NdotV, roughness) * GeometrySchlickGGX_IBL(NdotL, roughness);
}

// Maps a compute thread's (face, uv) to a world-space direction on the unit
// cube, matching D3D's cubemap face convention (+X,-X,+Y,-Y,+Z,-Z).
float3 FaceUvToDirection(uint face, float2 uv)
{
    float2 a = uv * 2.0f - 1.0f;

    switch (face)
    {
        // +X
        case 0:
            return normalize(float3(
                1.0f,
                -a.y,
                -a.x
            ));

        // -X
        case 1:
            return normalize(float3(
                -1.0f,
                -a.y,
                a.x
            ));

        // +Y
        case 2:
            return normalize(float3(
                a.x,
                1.0f,
                a.y
            ));

        // -Y
        case 3:
            return normalize(float3(
                a.x,
                -1.0f,
                -a.y
            ));

        // +Z
        case 4:
            return normalize(float3(
                a.x,
                -a.y,
                1.0f
            ));

        // -Z
        case 5:
        default:
            return normalize(float3(
                -a.x,
                -a.y,
                -1.0f
            ));
    }
}