// File: src/Rendering/Mesh.h
//
// CPU-side mesh representation shared by the procedural fallback geometry
// and the OBJ loader. Kept as plain interleaved vertex + 32-bit index data
// because that is exactly what both a D3D12 vertex/index buffer AND a
// DXR bottom-level acceleration structure geometry desc want - designing
// this once meant DXR (Graphics/Raytracing) consumes MeshData directly
// instead of needing a converter.
#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace rendering
{
    struct Vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT3 tangent;
        DirectX::XMFLOAT2 uv;
    };

    struct MeshData
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices; // 32-bit: OBJ meshes can easily exceed 65535 verts.
    };

    // Simple built-in geometry - no file dependency, always available so the
    // demo runs even with an empty assets/ folder.
    MeshData CreateTexturedCube(float halfExtent = 1.0f);

    // UV sphere (latitude/longitude), UVs and smooth per-vertex normals
    // included - the natural showcase shape for reflections since every
    // point on it faces a different direction.
    MeshData CreateUVSphere(float radius = 1.0f, uint32_t stacks = 24, uint32_t slices = 32);

    // A flat XZ-plane centered at the origin, facing +Y. uvTiling repeats
    // the albedo/checkerboard texture across the plane instead of
    // stretching one texel per meter.
    MeshData CreatePlane(float width = 10.0f, float depth = 10.0f, float uvTiling = 4.0f);

    // Minimal Wavefront OBJ loader. Supports v / vn / vt / f (triangles and
    // convex N-gons via fan triangulation). Does NOT parse materials
    // (mtllib/usemtl) - a real material comes from Graphics/Material,
    // driven by files under assets/ rather than the OBJ's own mtllib.
    // Missing normals are filled in as flat per-face normals; missing UVs
    // default to (0,0). Returns false if the file could not be opened.
    bool LoadObj(const std::string& path, MeshData& outMesh);

    // Derives per-vertex tangents from positions + UVs (standard
    // triangle-edge/UV-delta method), needed for normal mapping. Call once
    // after the mesh's normals and UVs are final - works for both the OBJ
    // loader's output and the procedural cube, so it's a separate pass
    // rather than being duplicated in both mesh sources.
    void ComputeTangents(MeshData& mesh);
}
