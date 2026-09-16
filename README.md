# DX12 Renderer

A C++ / DirectX 12 rendering engine that I'm building from scratch to learn how modern game rendering works at a lower level.

The project started as a simple D3D12 triangle and has gradually grown into a small real-time renderer with PBR materials, deferred rendering, image-based lighting and DirectX Raytracing.

The main goal isn't to build another game engine. It's to understand what actually happens underneath engines like Unreal Engine and to experiment with modern rendering techniques myself.

## Current Features

* DirectX 12 renderer
* Win32 window and input handling
* Automatic GPU adapter selection
* D3D12 debug layer support
* Triple-buffered swap chain
* GPU/CPU synchronization with fences
* GPU timestamp queries
* OBJ model loading
* Texture loading through WIC
* FPS-style camera
* PBR materials
* Cook-Torrance BRDF
* GGX distribution
* Smith visibility function
* Fresnel-Schlick
* Normal mapping
* Metallic/roughness materials
* Directional and point lights
* Shadow mapping with hardware PCF
* Deferred rendering
* G-buffer
* Procedural sky
* Image-based lighting
* Diffuse irradiance
* Prefiltered specular environment maps
* BRDF integration LUT
* Compute shader support
* DirectX Raytracing reflections

---

## Ray-Traced Reflections

The latest version of the renderer adds DXR-based reflections.

The current test scene contains:

* A large floor
* A rotating showcase mesh
* A mirror sphere
* A rough gold sphere
* Multiple lights
* Image-based lighting

The renderer builds a bottom-level acceleration structure for each object and a top-level acceleration structure for the scene.

Reflection rays are fired for sufficiently smooth surfaces. The mirror sphere uses ray-traced reflections, while rougher materials fall back to the prefiltered IBL environment map.

The current implementation intentionally uses **single-bounce reflections**. I wanted to get a working and understandable DXR pipeline in place before adding more complicated recursion and denoising.

### DXR pipeline

```text
Scene
  ↓
BLAS
  ↓
TLAS
  ↓
Ray Generation Shader
  ↓
Reflection Ray
  ↓
Closest Hit Shader
  ↓
Reflected Lighting
  ↓
Reflection Texture
  ↓
Final Lighting
```

The closest-hit shader also fetches the hit triangle's vertex and index data so that reflections use the actual geometry and surface normals instead of simply treating objects as flat silhouettes.

---

## Real Scene

The renderer is no longer limited to a single hardcoded object.

The current scene is built from separate meshes, materials and transforms:

```text
Scene
├── Floor
├── Showcase Mesh
├── Mirror Sphere
└── Gold Sphere
```

Meshes can be loaded from `assets/`, and procedural geometry is also available for basic shapes such as planes and spheres.

---

## Rendering Pipeline

The current rendering pipeline roughly looks like this:

```text
Geometry
    ↓
G-Buffer
    ↓
Deferred Lighting
    ↓
Image-Based Lighting
    ↓
Ray-Traced Reflections
    ↓
Back Buffer
```

The G-buffer stores the information needed by the lighting pass, allowing lighting to be calculated once for each covered pixel instead of once per object.

---

## Image-Based Lighting

Before adding DXR, I implemented image-based lighting using a procedural sky.

The renderer generates:

* Diffuse irradiance
* Prefiltered specular environment maps
* BRDF integration LUT

These are generated using compute shaders and provide ambient lighting based on the environment.

This also gives the renderer a fallback for surfaces where ray-traced reflections aren't being used.

---

## PBR

Materials use a Cook-Torrance physically based BRDF.

The renderer currently supports:

* Albedo
* Metallic
* Roughness
* Normal maps
* Ambient occlusion

The lighting implementation uses GGX, Smith visibility and Fresnel-Schlick.

---

## Shadows

Directional shadows are implemented using a depth-only shadow pass.

The lighting pass uses the shadow map with hardware PCF to soften the shadow edges.

---

## Camera Controls

| Input     | Action |
| --------- | ------ |
| `W A S D` | Move   |
| `R        |        |
