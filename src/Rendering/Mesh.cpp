// File: src/Rendering/Mesh.cpp
#include "Rendering/Mesh.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <functional>
#include <cmath>
#include <string>
#include <vector>

using namespace DirectX;

namespace rendering
{
    MeshData CreateTexturedCube(float h)
    {
        MeshData mesh;

        // 24 vertices (4 per face, not shared) so every face gets correct
        // per-face normals and its own 0..1 UV range - sharing corner
        // vertices between faces would average normals and smear UVs.
        struct Face { XMFLOAT3 normal; XMFLOAT3 a, b, c, d; };
        Face faces[6] = {
            { {  0,  0,  1 }, { -h,-h, h }, {  h,-h, h }, {  h, h, h }, { -h, h, h } }, // +Z
            { {  0,  0, -1 }, {  h,-h,-h }, { -h,-h,-h }, { -h, h,-h }, {  h, h,-h } }, // -Z
            { {  1,  0,  0 }, {  h,-h, h }, {  h,-h,-h }, {  h, h,-h }, {  h, h, h } }, // +X
            { { -1,  0,  0 }, { -h,-h,-h }, { -h,-h, h }, { -h, h, h }, { -h, h,-h } }, // -X
            { {  0,  1,  0 }, { -h, h, h }, {  h, h, h }, {  h, h,-h }, { -h, h,-h } }, // +Y
            { {  0, -1,  0 }, { -h,-h,-h }, {  h,-h,-h }, {  h,-h, h }, { -h,-h, h } }, // -Y
        };

        for (const Face& f : faces)
        {
            uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back({ f.a, f.normal, {0,0,0}, { 0.0f, 1.0f } });
            mesh.vertices.push_back({ f.b, f.normal, {0,0,0}, { 1.0f, 1.0f } });
            mesh.vertices.push_back({ f.c, f.normal, {0,0,0}, { 1.0f, 0.0f } });
            mesh.vertices.push_back({ f.d, f.normal, {0,0,0}, { 0.0f, 0.0f } });

            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 1);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 3);
        }

        return mesh;
    }

    MeshData CreateUVSphere(float radius, uint32_t stacks, uint32_t slices)
    {
        MeshData mesh;
        mesh.vertices.reserve(static_cast<size_t>(stacks + 1) * (slices + 1));

        for (uint32_t stack = 0; stack <= stacks; ++stack)
        {
            // theta: 0 at the north pole, kPi at the south pole.
            float theta = static_cast<float>(stack) / static_cast<float>(stacks) * XM_PI;
            float sinTheta = sinf(theta);
            float cosTheta = cosf(theta);

            for (uint32_t slice = 0; slice <= slices; ++slice)
            {
                float phi = static_cast<float>(slice) / static_cast<float>(slices) * XM_2PI;
                float sinPhi = sinf(phi);
                float cosPhi = cosf(phi);

                XMFLOAT3 dir(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);
                Vertex v{};
                v.position = XMFLOAT3(dir.x * radius, dir.y * radius, dir.z * radius);
                v.normal = dir; // already unit-length for a sphere centered at the origin
                v.tangent = XMFLOAT3(0, 0, 0); // filled in by ComputeTangents
                v.uv = XMFLOAT2(static_cast<float>(slice) / slices, static_cast<float>(stack) / stacks);
                mesh.vertices.push_back(v);
            }
        }

        uint32_t ring = slices + 1;
        for (uint32_t stack = 0; stack < stacks; ++stack)
        {
            for (uint32_t slice = 0; slice < slices; ++slice)
            {
                uint32_t a = stack * ring + slice;
                uint32_t b = a + ring;

                // Degenerate triangles at the exact poles (a or b's row
                // collapses to a single point) are harmless - zero area,
                // rasterized as nothing - so no special-casing is needed.
                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(a + 1);

                mesh.indices.push_back(a + 1);
                mesh.indices.push_back(b);
                mesh.indices.push_back(b + 1);
            }
        }

        return mesh;
    }

    MeshData CreatePlane(float width, float depth, float uvTiling)
    {
        MeshData mesh;
        float hw = width * 0.5f;
        float hd = depth * 0.5f;

        XMFLOAT3 up(0, 1, 0);
        mesh.vertices = {
            { { -hw, 0,  hd }, up, {0,0,0}, { 0.0f, 0.0f } },
            { {  hw, 0,  hd }, up, {0,0,0}, { uvTiling, 0.0f } },
            { {  hw, 0, -hd }, up, {0,0,0}, { uvTiling, uvTiling } },
            { { -hw, 0, -hd }, up, {0,0,0}, { 0.0f, uvTiling } },
        };
        mesh.indices = { 0, 1, 2, 0, 2, 3 };
        return mesh;
    }
        // OBJ face indices are 1-based and separately index position/uv/normal;
        // this key lets us deduplicate (pos,uv,normal) triples into a single
        // interleaved vertex the way a GPU vertex buffer needs.
        struct IndexKey
        {
            int p, t, n;
            bool operator==(const IndexKey& o) const { return p == o.p && t == o.t && n == o.n; }
        };
        struct IndexKeyHash
        {
            size_t operator()(const IndexKey& k) const
            {
                return (std::hash<int>()(k.p) * 73856093) ^ (std::hash<int>()(k.t) * 19349663) ^ (std::hash<int>()(k.n) * 83492791);
            }
        };

    bool LoadObj(const std::string& path, MeshData& outMesh)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return false;
        }

        std::vector<XMFLOAT3> positions;
        std::vector<XMFLOAT3> normals;
        std::vector<XMFLOAT2> uvs;

        std::unordered_map<IndexKey, uint32_t, IndexKeyHash> cache;
        outMesh.vertices.clear();
        outMesh.indices.clear();

        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream ss(line);
            std::string tag;
            ss >> tag;

            if (tag == "v")
            {
                XMFLOAT3 p;
                ss >> p.x >> p.y >> p.z;
                positions.push_back(p);
            }
            else if (tag == "vn")
            {
                XMFLOAT3 n;
                ss >> n.x >> n.y >> n.z;
                normals.push_back(n);
            }
            else if (tag == "vt")
            {
                XMFLOAT2 uv;
                ss >> uv.x >> uv.y;
                uvs.push_back(uv);
            }
            else if (tag == "f")
            {
                // Parse every "p/t/n", "p/t", "p//n" or "p" token on the line
                // and fan-triangulate if there are more than 3 (convex N-gon).
                std::vector<uint32_t> faceIndices;
                std::string token;
                while (ss >> token)
                {
                    int p = 0, t = 0, n = 0;
                    size_t firstSlash = token.find('/');
                    if (firstSlash == std::string::npos)
                    {
                        p = std::stoi(token);
                    }
                    else
                    {
                        p = std::stoi(token.substr(0, firstSlash));
                        size_t secondSlash = token.find('/', firstSlash + 1);
                        if (secondSlash == std::string::npos)
                        {
                            t = std::stoi(token.substr(firstSlash + 1));
                        }
                        else
                        {
                            std::string tStr = token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
                            if (!tStr.empty()) t = std::stoi(tStr);
                            std::string nStr = token.substr(secondSlash + 1);
                            if (!nStr.empty()) n = std::stoi(nStr);
                        }
                    }

                    // OBJ indices are 1-based; negative indices (relative to
                    // the end of the list so far) are also legal.
                    auto resolve = [](int idx, size_t count) -> int
                    {
                        if (idx > 0) return idx;
                        if (idx < 0) return static_cast<int>(count) + idx + 1;
                        return 0;
                    };
                    p = resolve(p, positions.size());
                    t = resolve(t, uvs.size());
                    n = resolve(n, normals.size());

                    IndexKey key{ p, t, n };
                    auto it = cache.find(key);
                    if (it != cache.end())
                    {
                        faceIndices.push_back(it->second);
                        continue;
                    }

                    Vertex v{};
                    v.position = (p > 0 && p <= static_cast<int>(positions.size())) ? positions[p - 1] : XMFLOAT3{ 0, 0, 0 };
                    v.uv = (t > 0 && t <= static_cast<int>(uvs.size())) ? uvs[t - 1] : XMFLOAT2{ 0, 0 };
                    v.normal = (n > 0 && n <= static_cast<int>(normals.size())) ? normals[n - 1] : XMFLOAT3{ 0, 1, 0 };

                    uint32_t newIndex = static_cast<uint32_t>(outMesh.vertices.size());
                    outMesh.vertices.push_back(v);
                    cache.emplace(key, newIndex);
                    faceIndices.push_back(newIndex);
                }

                // Fan triangulation around the first vertex of the face.
                for (size_t i = 1; i + 1 < faceIndices.size(); ++i)
                {
                    outMesh.indices.push_back(faceIndices[0]);
                    outMesh.indices.push_back(faceIndices[i]);
                    outMesh.indices.push_back(faceIndices[i + 1]);
                }
            }
        }

        // If the file had no vn lines, every vertex above got a placeholder
        // (0,1,0) normal - patch in real flat-shaded per-face normals so
        // lighting still looks correct.
        bool hadNormals = !normals.empty();
        if (!hadNormals)
        {
            std::vector<XMFLOAT3> accum(outMesh.vertices.size(), XMFLOAT3{ 0, 0, 0 });
            for (size_t i = 0; i + 2 < outMesh.indices.size(); i += 3)
            {
                uint32_t ia = outMesh.indices[i], ib = outMesh.indices[i + 1], ic = outMesh.indices[i + 2];
                XMVECTOR a = XMLoadFloat3(&outMesh.vertices[ia].position);
                XMVECTOR b = XMLoadFloat3(&outMesh.vertices[ib].position);
                XMVECTOR c = XMLoadFloat3(&outMesh.vertices[ic].position);
                XMVECTOR faceN = XMVector3Cross(XMVectorSubtract(b, a), XMVectorSubtract(c, a));

                XMFLOAT3 fn;
                XMStoreFloat3(&fn, faceN);
                accum[ia].x += fn.x; accum[ia].y += fn.y; accum[ia].z += fn.z;
                accum[ib].x += fn.x; accum[ib].y += fn.y; accum[ib].z += fn.z;
                accum[ic].x += fn.x; accum[ic].y += fn.y; accum[ic].z += fn.z;
            }
            for (size_t i = 0; i < outMesh.vertices.size(); ++i)
            {
                XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&accum[i]));
                XMStoreFloat3(&outMesh.vertices[i].normal, n);
            }
        }

        return !outMesh.vertices.empty();
    }

    void ComputeTangents(MeshData& mesh)
    {
        std::vector<XMFLOAT3> accum(mesh.vertices.size(), XMFLOAT3{ 0.0f, 0.0f, 0.0f });

        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
            const Vertex& v0 = mesh.vertices[i0];
            const Vertex& v1 = mesh.vertices[i1];
            const Vertex& v2 = mesh.vertices[i2];

            XMVECTOR p0 = XMLoadFloat3(&v0.position);
            XMVECTOR p1 = XMLoadFloat3(&v1.position);
            XMVECTOR p2 = XMLoadFloat3(&v2.position);
            XMVECTOR edge1 = XMVectorSubtract(p1, p0);
            XMVECTOR edge2 = XMVectorSubtract(p2, p0);

            float du1 = v1.uv.x - v0.uv.x, dv1 = v1.uv.y - v0.uv.y;
            float du2 = v2.uv.x - v0.uv.x, dv2 = v2.uv.y - v0.uv.y;

            float denom = du1 * dv2 - du2 * dv1;
            // Degenerate UVs (e.g. all-zero, or a seam triangle): fall back to
            // the edge direction itself rather than dividing by ~0, which
            // would otherwise inject NaNs into the tangent basis.
            float f = (fabsf(denom) > 1e-8f) ? (1.0f / denom) : 0.0f;

            XMVECTOR tangent = (f != 0.0f)
                ? XMVectorScale(XMVectorSubtract(XMVectorScale(edge1, dv2), XMVectorScale(edge2, dv1)), f)
                : XMVector3Normalize(edge1);

            XMFLOAT3 t;
            XMStoreFloat3(&t, tangent);
            accum[i0].x += t.x; accum[i0].y += t.y; accum[i0].z += t.z;
            accum[i1].x += t.x; accum[i1].y += t.y; accum[i1].z += t.z;
            accum[i2].x += t.x; accum[i2].y += t.y; accum[i2].z += t.z;
        }

        for (size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            XMVECTOR n = XMLoadFloat3(&mesh.vertices[i].normal);
            XMVECTOR t = XMLoadFloat3(&accum[i]);

            // Gram-Schmidt orthogonalize against the normal so the tangent
            // stays perpendicular to it even after averaging across faces.
            t = XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t))));
            float lenSq = XMVectorGetX(XMVector3LengthSq(t));

            XMFLOAT3 outTangent;
            if (lenSq > 1e-12f)
            {
                XMStoreFloat3(&outTangent, XMVector3Normalize(t));
            }
            else
            {
                // Degenerate (e.g. a UV island collapsed to a point) - any
                // vector perpendicular to the normal is an acceptable,
                // non-NaN fallback.
                XMVECTOR arbitrary = (fabsf(XMVectorGetY(n)) < 0.99f) ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
                XMStoreFloat3(&outTangent, XMVector3Normalize(XMVector3Cross(arbitrary, n)));
            }
            mesh.vertices[i].tangent = outTangent;
        }
    }
}
