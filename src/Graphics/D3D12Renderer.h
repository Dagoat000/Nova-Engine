// File: src/Graphics/D3D12Renderer.h
//
// Top-level renderer. Owns the Device, SwapChain, G-buffer, scene, and the
// pipelines for the deferred renderer's passes, and exposes the
// RenderFrame call Application drives every tick.
//
// Frame synchronization model:
//   - One ID3D12Fence, one monotonically increasing counter.
//   - Each FrameContext remembers the fence value it must wait for before
//     its command allocator can be reset.
//   - RenderFrame blocks (event-based, not a spin loop) only if that
//     frame's previous submission hasn't finished yet - with 3 frames in
//     flight this normally does not block at all.
//   - EndFrame submits the command list, signals the fence, and presents.
//
// Rendering model: deferred shading + optional ray-traced reflections.
// Per frame: (1) a depth-only shadow pass renders the scene from the
// light's point of view; (2) the geometry (G-buffer) pass loops over every
// scene object, writing material data - no lighting - into 3 render
// targets + its own depth buffer; (3) if DXR is available
// (Graphics/Raytracing), one reflection ray per eligible (smooth-enough)
// pixel is traced against the whole scene; (4) a full-screen lighting pass
// reads the G-buffer + shadow map + environment map (+ ray-traced
// reflections where applicable) and evaluates the actual PBR lighting once
// per covered pixel, straight to the back buffer.
#pragma once

#include "Graphics/Device.h"
#include "Graphics/SwapChain.h"
#include "Graphics/FrameContext.h"
#include "Graphics/DescriptorHeap.h"
#include "Graphics/Buffer.h"
#include "Graphics/Pipeline.h"
#include "Graphics/Material.h"
#include "Graphics/ShadowMap.h"
#include "Graphics/GBuffer.h"
#include "Graphics/EnvironmentMap.h"
#include "Graphics/Lighting.h"
#include "Graphics/Scene.h"
#include "Graphics/Raytracing.h"
#include "Graphics/LensFlare.h"
#include "Rendering/Camera.h"
#include <DirectXMath.h>
#include <array>
#include <memory>
#include <cstdint>

namespace gfx
{
    // Mirrors shaders/GBufferVS.hlsl's PerObject cbuffer (b0, vertex-only -
    // the geometry pass does no lighting, so it has no use for the camera
    // position the lighting pass needs).
    struct GBufferPerObjectConstants
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 viewProj;
    };

    // Mirrors shaders/DeferredLightingPS.hlsl's (and
    // shaders/RaytracingReflections.hlsl's) ScreenConstants cbuffer. The
    // inverse view-projection is what lets both shaders reconstruct world
    // position from the G-buffer's depth instead of the G-buffer needing
    // its own world-position render target.
    struct ScreenConstants
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT3 cameraPosWS;
        float rtReflectionsThreshold = 0.0f;

        DirectX::XMFLOAT2 sunScreenPos;
        float sunVisible = 0.0f;
        float padding = 0.0f;
    };
    // Mirrors shaders/ShadowVS.hlsl's ShadowConstants cbuffer (b0 in the
    // shadow pass's own, separate root signature). One per (frame, object)
    // pair - see kMaxSceneObjects below.
    struct ShadowConstants
    {
        DirectX::XMFLOAT4X4 worldViewProjLight;
    };

    struct GpuFrameStats
    {
        double cpuFrameMs = 0.0;
        double gpuFrameMs = 0.0;
        double fps = 0.0;
    };

    // The engine has no dynamic object list yet - Scene::Load builds a
    // fixed handful of objects. This just needs to be >= that count; sized
    // with headroom so growing the default scene slightly doesn't require
    // touching this constant.
    constexpr uint32_t kMaxSceneObjects = 8;

    class D3D12Renderer
    {
    public:
        D3D12Renderer(HWND hwnd, uint32_t width, uint32_t height, bool debugLayer);
        ~D3D12Renderer();

        D3D12Renderer(const D3D12Renderer&) = delete;
        D3D12Renderer& operator=(const D3D12Renderer&) = delete;

        void Resize(uint32_t width, uint32_t height);
        void ToggleFullscreen() { m_swapChain->ToggleFullscreen(); }
        void SetVSync(bool enabled) { m_vsyncEnabled = enabled; }
        bool IsVSyncEnabled() const { return m_vsyncEnabled; }

        // Advances scene state and records + submits the frame's command
        // list (shadow pass, G-buffer pass, optional RT reflections,
        // lighting pass), then presents. Called once per Application::Render().
        void RenderFrame(double totalTimeSeconds);

        // Blocks until the GPU has finished all work submitted so far.
        // Only used for resize/shutdown - never in the steady-state loop.
        void Flush();

        const GpuFrameStats& GetStats() const { return m_stats; }
        const GpuCapabilities& GetCapabilities() const { return m_device->GetCapabilities(); }
        bool IsRaytracingEnabled() const { return m_raytracing && m_raytracing->IsUsable(); }

        // Application owns input handling and drives this every frame -
        // the renderer just reads the result when it builds the view matrix.
        rendering::Camera& GetCamera() { return m_camera; }

    private:
        void CreateFrameContexts();
        void LoadAssets();
        void WaitForFrame(FrameContext& frame);
        uint64_t Signal();

        std::unique_ptr<Device> m_device;
        std::unique_ptr<SwapChain> m_swapChain;

        std::array<FrameContext, kBackBufferCount> m_frames;
        ComPtr<ID3D12GraphicsCommandList> m_commandList;

        ComPtr<ID3D12Fence> m_fence;
        HANDLE m_fenceEvent = nullptr;
        uint64_t m_fenceValue = 0;

        PipelineBundle m_shadowPipeline;
        PipelineBundle m_gbufferPipeline;
        PipelineBundle m_lightingPipeline;

        // Sized kBackBufferCount * kMaxSceneObjects and indexed via
        // `frameIndex * kMaxSceneObjects + objectIndex` - every object in
        // the G-buffer/shadow passes needs its own slot within a frame, not
        // just its own slot per frame-in-flight, now that there's more
        // than one object.
        ConstantBuffer<GBufferPerObjectConstants> m_gbufferConstantBuffer;
        ConstantBuffer<MaterialConstants> m_materialConstantBuffer;
        ConstantBuffer<ShadowConstants> m_shadowConstantBuffer;

        // Scene-wide (not per-object): one slot per frame-in-flight.
        ConstantBuffer<ScreenConstants> m_screenConstantBuffer;
        ConstantBuffer<LightConstants> m_lightConstantBuffer;

        // Shader-visible CBV_SRV_UAV heap. Holds every material's 4 SRVs,
        // the G-buffer's 4 SRVs, the shadow map's SRV, the environment
        // map's 4 SRVs, the RT reflections output's UAV+SRV, and (if DXR is
        // usable) 2 raw-buffer SRVs per scene object.
        std::unique_ptr<DescriptorHeap> m_srvHeap;
        Scene m_scene;
        std::unique_ptr<ShadowMap> m_shadowMap;
        std::unique_ptr<GBuffer> m_gbuffer;
        std::unique_ptr<EnvironmentMap> m_environment;
        std::unique_ptr<RaytracingContext> m_raytracing;
        LensFlare m_lensFlare;
        SceneLighting m_lighting;

        rendering::Camera m_camera;

        // GPU timing via timestamp queries: one query pair (begin/end) per
        // frame in flight, resolved into a small readback buffer.
        ComPtr<ID3D12QueryHeap> m_timestampHeap;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint64_t m_gpuTimestampFrequency = 1;

        GpuFrameStats m_stats;

        bool m_vsyncEnabled = true;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
    };
}
