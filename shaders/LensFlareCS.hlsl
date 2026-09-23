#define PI 3.14159265359f
#define NANO_METER 0.0000001f
#define PATCH_SIZE 32
#define NUM_THREADS 32
#define AP_IDX 14

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

struct LensInterface
{
    float3 center;
    float radius;
    float3 n;
    float sa;
    float d1;
    float flat;
    float pos;
    float w;
};

struct GhostData
{
    float bounce1;
    float bounce2;
    float2 padding;
};

struct FlareVertex
{
    float4 pos;
    float4 color;
    float4 coordinates;
    float4 reflectance;
};

StructuredBuffer<LensInterface> gLens : register(t0);
StructuredBuffer<GhostData> gGhosts : register(t1);
RWStructuredBuffer<FlareVertex> gOutput : register(u0);

struct Intersection
{
    float3 pos;
    float3 norm;
    float theta;
    uint hit;
    uint inverted;
};

struct Ray
{
    float3 pos;
    float3 dir;
    float4 tex;
};

Intersection TestSphere(Ray r, LensInterface F)
{
    Intersection i;
    float3 D = r.pos - F.center;
    float B = dot(D, r.dir);
    float C = dot(D, D) - F.radius * F.radius;
    float B2_C = B * B - C;
    if (B2_C < 0.0f)
    {
        i.hit = 0;
        i.inverted = 0;
        i.pos = 0;
        i.norm = 0;
        i.theta = 0;
        return i;
    }
    float sgn = (F.radius * r.dir.z) > 0.0f ? 1.0f : -1.0f;
    float t = sqrt(B2_C) * sgn - B;
    i.pos = r.dir * t + r.pos;
    i.norm = normalize(i.pos - F.center);
    if (dot(i.norm, r.dir) > 0.0f)
        i.norm = -i.norm;
    float d = clamp(dot(-r.dir, i.norm), -1.0f, 1.0f);
    i.theta = acos(d);
    i.hit = 1;
    i.inverted = t < 0.0f ? 1u : 0u;
    return i;
}

Intersection TestFlat(Ray r, LensInterface F)
{
    Intersection i;
    float dz = abs(r.dir.z) > 1e-6f ? r.dir.z : (r.dir.z < 0.0f ? -1e-6f : 1e-6f);
    i.pos = r.pos + r.dir * ((F.center.z - r.pos.z) / dz);
    i.norm = r.dir.z > 0.0f ? float3(0, 0, -1) : float3(0, 0, 1);
    i.theta = 0.0f;
    i.hit = 1;
    i.inverted = 0;
    return i;
}

float FresnelAR(float theta0, float lambda, float d1, float n0, float n1, float n2)
{
    float s0 = sin(theta0);
    float theta1 = asin(clamp(s0 * n0 / n1, -0.99999f, 0.99999f));
    float theta2 = asin(clamp(s0 * n0 / n2, -0.99999f, 0.99999f));

    float rs01 = -sin(theta0 - theta1) / max(abs(sin(theta0 + theta1)), 1e-5f);
    float rp01 = tan(theta0 - theta1) / max(abs(tan(theta0 + theta1)), 1e-5f);
    float ts01 = 2.0f * sin(theta1) * cos(theta0) / max(abs(sin(theta0 + theta1)), 1e-5f);
    float tp01 = ts01 * cos(theta0 - theta1);
    float rs12 = -sin(theta1 - theta2) / max(abs(sin(theta1 + theta2)), 1e-5f);
    float rp12 = tan(theta1 - theta2) / max(abs(tan(theta1 + theta2)), 1e-5f);

    float ris = ts01 * ts01 * rs12;
    float rip = tp01 * tp01 * rp12;
    float dy = d1 * n1;
    float dx = tan(theta1) * dy;
    float delay = sqrt(dx * dx + dy * dy);
    float relPhase = 4.0f * PI / max(lambda, 1e-10f) * (delay - dx * sin(theta0));
    float outS2 = rs01 * rs01 + ris * ris + 2.0f * rs01 * ris * cos(relPhase);
    float outP2 = rp01 * rp01 + rip * rip + 2.0f * rp01 * rip * cos(relPhase);
    return saturate((outS2 + outP2) * 0.5f);
}

Ray Trace(Ray r, float lambda, int2 bouncePair)
{
    int interfaceCount = (int)gNumInterfaces;
    int len = bouncePair.x + (bouncePair.x - bouncePair.y) + (interfaceCount - bouncePair.y) - 1;
    int phase = 0;
    int delta = 1;
    int T = 1;
    int k = 0;

    [loop]
    for (; k < len; ++k, T += delta)
    {
        LensInterface F = gLens[T];
        bool bReflect = (phase < 2 && T == bouncePair[phase]);
        if (bReflect)
        {
            delta = -delta;
            ++phase;
        }

        Intersection i;

        if (F.flat > 0.5f)
        {
            i = TestFlat(r, F);
        }
        else
        {
            i = TestSphere(r, F);
        }
        if (i.hit == 0)
        {
            r.pos = 0;
            r.tex.a = 0;
            break;
        }

        if (F.flat <= 0.5f)
            r.tex.z = max(r.tex.z, length(i.pos.xy) / max(F.sa, 1e-4f));
        else if (T == AP_IDX)
            r.tex.xy = i.pos.xy / max(F.sa, 1e-4f);

        r.dir = normalize(i.pos - r.pos);
        if (i.inverted != 0)
            r.dir *= -1.0f;
        r.pos = i.pos;

        if (F.flat > 0.5f)
            continue;

        float n0 = r.dir.z < 0.0f ? F.n.x : F.n.z;
        float n2 = r.dir.z < 0.0f ? F.n.z : F.n.x;
        if (!bReflect)
        {
            r.dir = refract(r.dir, i.norm, n0 / max(n2, 1e-4f));
            if (length(r.dir) < 1e-5f)
            {
                r.pos = 0;
                r.tex.a = 0;
                break;
            }
        }
        else
        {
            r.dir = reflect(r.dir, i.norm);
            float n1 = max(sqrt(max(n0 * n2, 1e-4f)), 1.38f + gCoatingQuality);
            float R = FresnelAR(i.theta + 0.001f, lambda, F.d1 * NANO_METER, n0, n1, n2);
            r.tex.a *= R;
        }
    }

    if (k < len)
    {
        r.pos = 0;
        r.tex.a = 0;
    }
    return r;
}

float3 TemperatureToColor(float t)
{
    static const float4 map[25] = {
        float4(0,0,0,0), float4(1000,1,.007,0), float4(1500,1,.126,0),
        float4(2000,1,.234,.01), float4(2500,1,.349,.067), float4(3000,1,.454,.151),
        float4(3500,1,.549,.254), float4(4000,1,.635,.370), float4(4500,1,.710,.493),
        float4(5000,1,.778,.620), float4(5500,1,.837,.746), float4(6000,1,.890,.869),
        float4(6500,1,.937,.988), float4(7000,.907,.888,1), float4(7500,.827,.839,1),
        float4(8000,.762,.800,1), float4(8500,.711,.766,1), float4(9000,.668,.738,1),
        float4(9500,.632,.714,1), float4(10000,.602,.693,1), float4(12000,.518,.632,1),
        float4(14000,.468,.593,1), float4(16000,.435,.567,1), float4(18000,.411,.547,1),
        float4(20000,.394,.533,1)
    };
    if (t <= 1000.0f) return map[1].yzw;
    [loop]
    for (int i = 2; i < 25; ++i)
    {
        if (t < map[i].x)
        {
            float l = (t - map[i-1].x) / max(map[i].x - map[i-1].x, 1.0f);
            return lerp(map[i-1].yzw, map[i].yzw, l);
        }
    }
    return map[24].yzw;
}

float2 Rotate(float2 p, float a)
{
    float s = sin(a);
    float c = cos(a);
    return float2(p.x * c - p.y * s, p.y * c + p.x * s);
}

float GetAreaScale(float2 ndc)
{
    float d = length(ndc);
    return 1.0f / max(0.25f + d * d * 2.0f, 0.25f);
}

[numthreads(NUM_THREADS, NUM_THREADS, 1)]
void CS(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    uint ghostId = groupId.x;
    if (ghostId >= gNumInterfaces * 1000 || ghostId >= 352)
        return;

    uint x = groupThreadId.x;
    uint y = groupThreadId.y;
    uint patchIndex = y * PATCH_SIZE + x;
    uint outputIndex = ghostId * PATCH_SIZE * PATCH_SIZE + patchIndex;

    float2 uv = float2(x, y) / float(PATCH_SIZE - 1);
    float2 ndc = (uv - 0.5f) * 2.0f;
    float3 startingPos = float3(ndc * gSpread, 1000.0f);
    startingPos.xy = Rotate(startingPos.xy, 2.0f);

    Ray cameraRay;
    cameraRay.pos = startingPos;
    cameraRay.dir = float3(0, 0, -1);
    cameraRay.tex = 0;

    Intersection entry = TestSphere(cameraRay, gLens[0]);
    FlareVertex result;
    result.pos = 0;
    result.color = 0;
    result.coordinates = float4(ndc, 0, 0);
    result.reflectance = 0;

    if (entry.hit == 0)
    {
        gOutput[outputIndex] = result;
        return;
    }

    startingPos = entry.pos - gLightDirCamera.xyz;
    GhostData ghost = gGhosts[ghostId];
    int2 bouncePair = int2(ghost.bounce1, ghost.bounce2);

    float wavelengths[3] = { 650.0f, 510.0f, 475.0f };
    float3 energy = 0;
    float2 apertureUv = 0;
    float3 imagePos = 0;

    [unroll]
    for (int c = 0; c < 3; ++c)
    {
        Ray r;
        r.pos = startingPos;
        r.dir = gLightDirCamera.xyz;
        r.tex = float4(0, 0, 0, 1);
        Ray traced = Trace(r, wavelengths[c] * NANO_METER, bouncePair);
        energy[c] = traced.tex.a;
        apertureUv = traced.tex.xy;
        imagePos = traced.pos;
    }

    result.pos = float4(imagePos, 1);
    result.coordinates = float4(ndc, apertureUv);
    result.reflectance = float4(energy * TemperatureToColor(6000.0f), 1.0f);
    result.color = float4(energy, GetAreaScale(ndc));
    gOutput[outputIndex] = result;
}
