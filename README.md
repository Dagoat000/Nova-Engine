# Engine — Ray-Traced Reflections + a Real Scene

A from-scratch C++ / DirectX 12 rendering engine foundation for Windows.

**⚠️ Honest note on this step:** DXR is the most intricate API surface in this whole engine (acceleration structures, a raytracing pipeline state object, shader tables with per-instance local root arguments), and it was written without being able to compile or run it on real hardware first. Everything here follows the standard, well-documented DXR patterns closely, and one real mistake (a UAV resource created in the wrong initial state) was caught and fixed during writing - but if `Engine: DXR` shows **Off** in the title bar on a card that should support it, or the mirror sphere looks wrong, that's the first place to look. The engine is designed to degrade gracefully: if DXR setup fails for any reason (old driver, missing `dxcompiler.dll`/`dxil.dll`, hardware below Tier 1.0), it logs a warning and falls back to the IBL-only reflections from the previous step - it does not crash or refuse to run.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

or open the generated `build/Engine.sln` in Visual Studio.

**New build dependency:** ray-traced reflections are compiled with DXC (not the legacy FXC-based D3DCompiler every other shader here uses), which needs `dxcompiler.dll` + `dxil.dll` next to `Engine.exe` at runtime. CMake tries to copy both from your Windows SDK install automatically; if it can't find them, it prints a `WARNING` at configure time and copies neither, in which case DXR safely fails to initialize at startup (see above) rather than crashing. To fix that: manually copy `dxcompiler.dll` and `dxil.dll` from `<Windows SDK>\Redist\D3D\x64\` (or download them from the [DirectXShaderCompiler releases](https://github.com/microsoft/DirectXShaderCompiler/releases)) into `build/bin/Release/` (or `Debug/`) next to `Engine.exe`.

Controls:
- **Hold Right Mouse Button** — enables mouse-look; while held: **WASD** to move, **Shift** to move faster, **E/Space** up, **Q/Ctrl** down.
- **F1** — toggle VSync.
- **F11 / Alt+Enter** — toggle fullscreen.
- **Esc** — quit.

Drop any of these into `assets/` and the engine picks them up automatically (any missing one falls back to a procedural default): `model.obj`, `albedo.png`/`.jpg`/`.bmp`, `normal.png`, `metallicRoughness.png` (G=roughness, B=metallic), `ao.png`. Per-object subfolders (`assets/floor/`, `assets/mirror/`, `assets/gold/`) let you override individual scene objects the same way.

## What's in Phase 1 (D3D12 foundation)

Win32 window with a title-bar overlay (FPS, CPU/GPU frame time, resolution, VSync, DXR status); D3D12 device with automatic adapter selection and debug-layer validation; triple-buffered swap chain; fence-based GPU/CPU sync; default-heap vertex/index buffers; GPU timestamp queries.

## What Phase 2 added

Wavefront OBJ loading; an FPS camera controller (WASD + mouse-look while RMB held); WIC-based texture loading with procedural fallbacks.

## What Phase 3 added

Cook-Torrance PBR shading (GGX, Smith, Fresnel-Schlick); normal mapping via computed tangents; a metallic-roughness material system.

## What Phase 4 added

A real light list (one directional sun + point lights); shadow mapping for the directional light via a depth-only pass + hardware PCF.

## What Phase 5 added

Deferred shading: a G-buffer geometry pass plus a full-screen lighting pass that shades once per covered pixel, reconstructing world position from depth.

## What the Image-Based Lighting step added

A procedural sky, diffuse irradiance convolution, prefiltered specular environment map, and a BRDF integration LUT - all precomputed once via **compute shaders** (the engine's first use of the compute pipeline) - replacing the flat ambient constant with real sky-colored ambient lighting.

## What this step adds: DXR reflections + a real scene

- **A real scene** (`Graphics/Scene`): a 14×14 floor, the original rotating showcase mesh, a mirror-smooth sphere, and a rougher gold sphere - replacing the single hardcoded object every earlier phase rendered. Each object gets its own mesh, material, and transform; the G-buffer and shadow passes now loop over all of them (`Graphics/D3D12Renderer`'s per-object constant-buffer slots: `frameIndex * kMaxSceneObjects + objectIndex`).
- **New procedural mesh generators** (`Rendering/Mesh`): `CreateUVSphere` and `CreatePlane`, alongside the existing cube and OBJ loader.
- **Ray-traced reflections** (`Graphics/Raytracing`, `shaders/RaytracingReflections.hlsl`): one bottom-level acceleration structure per object (built once), one top-level acceleration structure rebuilt every frame (objects move/rotate), and a small raytracing pipeline that fires one reflection ray per pixel whose roughness is below a threshold (0.35) - the mirror sphere gets sharp, genuinely traced reflections of the rest of the scene; the gold sphere and everything rougher keeps using the prefiltered IBL cubemap from the previous step. Reflections are single-bounce: the closest-hit shader shades directly (one light, no shadow, plus a cheap diffuse-IBL ambient term) rather than recursing into a second `TraceRay` - a deliberate, named scope cut, not an oversight.
- **Per-instance shading in the closest-hit shader**: rather than flat-coloring hit objects, the hit shader fetches the actual triangle's vertices out of a **local root signature**-bound raw vertex/index buffer pair (one per instance) and interpolates the real surface normal via barycentrics - so reflections show correctly-shaded geometry, not silhouettes.
- **DXC as a second shader compiler**: DXR shader libraries require DXIL (shader model 6.3+), which the legacy D3DCompiler used everywhere else in this engine cannot produce. `Graphics/Raytracing.cpp` compiles `RaytracingReflections.hlsl` with DXC directly.
- **Graceful degradation**: `RaytracingContext::IsUsable()` gates every DXR code path; hardware/driver/DLL failures fall back to the IBL-only rendering from the previous step, logged clearly, without crashing.

## Roadmap

| Phase | Focus |
|---|---|
| 1 | D3D12 foundation + triangle |
| 2 | 3D meshes, camera controller, textures |
| 3 | PBR + materials |
| 4 | Lighting + shadows |
| 5 | Deferred / Forward+ rendering |
| — | Image-Based Lighting |
| — | **Ray-traced reflections + real scene** *(this)* |
| 6 | GPU culling + indirect drawing |
| 7 | Render graph |
| 8 | (folded into this step) |
| 9 | (folded into this step) |
| 10 | Temporal accumulation + denoising |
| 11 | Global illumination |
| 12 | Game engine integration |

Forward-looking decisions already baked in so these don't require a rewrite:
- `MeshData`'s plain interleaved-vertex/32-bit-index layout is exactly what `Graphics/Raytracing.cpp` feeds straight into a BLAS geometry desc, with no conversion step - the format was chosen with this in mind back in Phase 2.
- The closest-hit shader's raw-buffer vertex fetch generalizes directly to more materials or per-vertex data later (e.g. vertex colors) - it's already reading arbitrary byte offsets, not a fixed struct binding.
- `RaytracingContext`'s output texture always exists and is always bound, regardless of hardware support - adding ray-traced shadows or GI later means adding another such texture + a `roughness`-style gating value, not restructuring the binding model.
- Recursion depth is capped at 1 (single-bounce) on purpose; a future multi-bounce mode is a `MaxTraceRecursionDepth` and shader-side recursion change, not an architecture change.






