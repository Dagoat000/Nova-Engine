// File: src/Graphics/D3D12Renderer.cpp
#include "Graphics/D3D12Renderer.h"
#include "Graphics/DXHelper.h"
#include "Core/Logger.h"
#include <vector>
#include <stdexcept>
#include <string>
#include <utility>
#include <memory>

using namespace DirectX;

namespace gfx
{
    D3D12Renderer::D3D12Renderer(HWND hwnd, uint32_t width, uint32_t height, bool debugLayer)
        : m_width(width)
        , m_height(height)
    {
        m_device = std::make_unique<Device>(debugLayer);
        m_swapChain = std::make_unique<SwapChain>(*m_device, hwnd, width, height);

        DX_CHECK(m_device->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent)
        {
            throw std::runtime_error("Failed to create fence event");
        }

        CreateFrameContexts();

        // One shared command list, reset against a different allocator each
        // frame. This is the standard pattern - the list itself is cheap to
        // reuse, the allocator is the thing that must not be touched while
        // the GPU may still be reading from it.
        DX_CHECK(m_device->GetDevice()->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].commandAllocator.Get(), nullptr,
            IID_PPV_ARGS(&m_commandList)));
        DX_CHECK(m_commandList->Close()); // Created in recording state; BeginFrame expects it closed.

        m_shadowPipeline = CreateShadowPipeline(m_device->GetDevice(), kGBufferDepthDsvFormat);
        m_gbufferPipeline = CreateGBufferPipeline(
            m_device->GetDevice(), kGBufferAlbedoFormat, kGBufferNormalFormat, kGBufferMRAOFormat, kGBufferDepthDsvFormat);
        m_lightingPipeline = CreateLightingPipeline(m_device->GetDevice(), kBackBufferFormat);

        // Generous capacity: per-material 4 SRVs x a handful of objects,
        // the G-buffer's 4, the shadow map's 1, the environment map's 4,
        // the RT output's UAV+SRV, and 2 raw-buffer SRVs per object for
        // DXR's closest-hit shader.
        m_srvHeap = std::make_unique<DescriptorHeap>(
            m_device->GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 128, /*shaderVisible*/ true);

        m_shadowMap = std::make_unique<ShadowMap>(m_device->GetDevice(), 2048);
        m_shadowMap->CreateShaderResourceView(m_device->GetDevice(), *m_srvHeap);

        m_gbuffer = std::make_unique<GBuffer>(m_device->GetDevice(), width, height);
        m_gbuffer->CreateShaderResourceViews(m_device->GetDevice(), *m_srvHeap);

        m_environment = std::make_unique<EnvironmentMap>(m_device->GetDevice());

        // Created regardless of hardware DXR support - see Raytracing.h's
        // header comment on why the output texture always exists.
        m_raytracing = std::make_unique<RaytracingContext>(m_device->GetDevice(), width, height);
        m_raytracing->CreateOutputShaderResourceView(m_device->GetDevice(), *m_srvHeap);

        LoadAssets();

        m_gbufferConstantBuffer.Create(m_device->GetDevice(), kBackBufferCount * kMaxSceneObjects, L"GBufferPerObjectCB");
        m_materialConstantBuffer.Create(m_device->GetDevice(), kBackBufferCount * kMaxSceneObjects, L"MaterialCB");
        m_shadowConstantBuffer.Create(m_device->GetDevice(), kBackBufferCount * kMaxSceneObjects, L"ShadowCB");
        m_screenConstantBuffer.Create(m_device->GetDevice(), kBackBufferCount, L"ScreenCB");
        m_lightConstantBuffer.Create(m_device->GetDevice(), kBackBufferCount, L"LightCB");

        m_camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
        // Pulled back and up a little from the Phase-2 default so the whole
        // 4-object scene (floor + 3 objects spread ~4.4m apart) is framed
        // on startup instead of starting inside the showcase mesh.
        m_camera.SetPosition({ 0.0f, 1.6f, -6.0f });

        // GPU timing: 2 timestamps (begin/end) per frame-in-flight.
        D3D12_QUERY_HEAP_DESC queryDesc{};
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryDesc.Count = kBackBufferCount * 2;
        DX_CHECK(m_device->GetDevice()->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestampHeap)));

        D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC readbackDesc{};
        readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDesc.Width = sizeof(uint64_t) * kBackBufferCount * 2;
        readbackDesc.Height = 1;
        readbackDesc.DepthOrArraySize = 1;
        readbackDesc.MipLevels = 1;
        readbackDesc.SampleDesc = { 1, 0 };
        readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        DX_CHECK(m_device->GetDevice()->CreateCommittedResource(
            &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_timestampReadback)));

        m_device->GetGraphicsQueue()->GetTimestampFrequency(&m_gpuTimestampFrequency);

        LOG_INFO(std::string("D3D12Renderer initialized (deferred pipeline, DXR ") +
            (m_raytracing->IsUsable() ? "enabled" : "unavailable") + ").");
    }

    D3D12Renderer::~D3D12Renderer()
    {
        Flush();
        if (m_fenceEvent) CloseHandle(m_fenceEvent);
    }

    void D3D12Renderer::CreateFrameContexts()
    {
        for (auto& frame : m_frames)
        {
            DX_CHECK(m_device->GetDevice()->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.commandAllocator)));
        }
    }

    void D3D12Renderer::LoadAssets()
    {
        DX_CHECK(m_frames[0].commandAllocator->Reset());
        DX_CHECK(m_commandList->Reset(m_frames[0].commandAllocator.Get(), nullptr));

        std::vector<ComPtr<ID3D12Resource>> uploadsKeepAlive;

        m_scene.Load(m_device->GetDevice(), m_commandList.Get(), uploadsKeepAlive, *m_srvHeap);
        if (m_scene.GetObjects().size() > kMaxSceneObjects)
        {
            throw std::runtime_error("Scene object count exceeds kMaxSceneObjects - raise the constant in D3D12Renderer.h");
        }

        // The environment map's sun matches the scene's directional light -
        // this is a one-time snapshot (see EnvironmentMap.h's header
        // comment on why a moving sun isn't handled here yet).
        m_environment->Generate(
            m_device->GetDevice(), m_commandList.Get(),
            m_lighting.dirLightDirection, m_lighting.dirLightColor, m_lighting.dirLightIntensity);

        if (m_raytracing->IsUsable())
        {
            m_raytracing->BuildBottomLevelStructures(m_device->GetDevice(), m_commandList.Get(), m_scene.GetObjects(), *m_srvHeap);
        }

        DX_CHECK(m_commandList->Close());
        ID3D12CommandList* lists[] = { m_commandList.Get() };
        m_device->GetGraphicsQueue()->ExecuteCommandLists(1, lists);

        Flush(); // One-time startup upload; not on the hot path.

        m_environment->CreateShaderResourceViews(m_device->GetDevice(), *m_srvHeap);
    }

    uint64_t D3D12Renderer::Signal()
    {
        const uint64_t value = ++m_fenceValue;
        DX_CHECK(m_device->GetGraphicsQueue()->Signal(m_fence.Get(), value));
        return value;
    }

    void D3D12Renderer::WaitForFrame(FrameContext& frame)
    {
        if (frame.fenceValue != 0 && m_fence->GetCompletedValue() < frame.fenceValue)
        {
            DX_CHECK(m_fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent));
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    void D3D12Renderer::Flush()
    {
        const uint64_t value = Signal();
        if (m_fence->GetCompletedValue() < value)
        {
            DX_CHECK(m_fence->SetEventOnCompletion(value, m_fenceEvent));
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    void D3D12Renderer::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0 || (width == m_width && height == m_height)) return;

        Flush();

        m_width = width;
        m_height = height;
        m_swapChain->Resize(width, height);
        m_gbuffer->Resize(m_device->GetDevice(), width, height, *m_srvHeap);
        m_raytracing->Resize(m_device->GetDevice(), width, height, *m_srvHeap);
        m_camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
    }

    void D3D12Renderer::RenderFrame(double totalTimeSeconds)
    {
        const uint32_t frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        FrameContext& frame = m_frames[frameIndex];
        const auto& objects = m_scene.GetObjects();

        // --- BeginFrame ---
        WaitForFrame(frame);
        DX_CHECK(frame.commandAllocator->Reset());
        DX_CHECK(m_commandList->Reset(frame.commandAllocator.Get(), nullptr));

        m_commandList->EndQuery(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2);

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap->Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);

        // --- Update: scene-wide constants ---
        XMMATRIX viewProj = m_camera.GetViewMatrix() * m_camera.GetProjectionMatrix();
        XMVECTOR viewProjDet;
        XMMATRIX invViewProj = XMMatrixInverse(&viewProjDet, viewProj);

        // Orthographic light frustum sized to cover the whole demo scene
        // (a 14x14 floor plus 3 objects spread ~4.4m apart) - see
        // Lighting.h's ComputeLightViewProj for how this radius maps to
        // the ortho frustum size and light distance.
        constexpr float kSceneRadius = 8.0f;
        XMMATRIX lightViewProj = m_lighting.ComputeLightViewProj(kSceneRadius);

        // 0.35 keeps the mirror sphere (roughness 0.03) ray-traced while the
        // gold sphere (0.35, right at the edge) and everything rougher stays
        // on the prefiltered-IBL path - a deliberate visual contrast between
        // the two techniques. -1 makes the check impossible to satisfy,
        // which is how DXR-unavailable hardware safely never reads the
        // (never-dispatched-into) RT output texture.
        const bool rtUsable = m_raytracing->IsUsable();
        ScreenConstants screenConstants;
        XMStoreFloat4x4(&screenConstants.invViewProj, XMMatrixTranspose(invViewProj));
        screenConstants.cameraPosWS = m_camera.GetPosition();
        screenConstants.rtReflectionsThreshold = rtUsable ? 0.35f : -1.0f;
        m_screenConstantBuffer.Update(frameIndex, screenConstants);

        LightConstants lightConstants;
        lightConstants.dirLightDirWS = m_lighting.dirLightDirection;
        lightConstants.dirLightIntensity = m_lighting.dirLightIntensity;
        lightConstants.dirLightColor = m_lighting.dirLightColor;
        XMStoreFloat4x4(&lightConstants.lightViewProj, XMMatrixTranspose(lightViewProj));
        for (uint32_t i = 0; i < kMaxPointLights; ++i) lightConstants.pointLights[i] = m_lighting.pointLights[i];
        lightConstants.numPointLights = m_lighting.numPointLights;
        m_lightConstantBuffer.Update(frameIndex, lightConstants);

        // --- Pass 1: shadow - render depth from the light's point of view, once per object ---
        {
            D3D12_RESOURCE_BARRIER toDepthWrite{};
            toDepthWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toDepthWrite.Transition.pResource = m_shadowMap->GetResource();
            toDepthWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toDepthWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            toDepthWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &toDepthWrite);

            D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = m_shadowMap->GetDsv();
            m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
            m_commandList->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            D3D12_VIEWPORT shadowViewport = m_shadowMap->GetViewport();
            D3D12_RECT shadowScissor = m_shadowMap->GetScissorRect();
            m_commandList->RSSetViewports(1, &shadowViewport);
            m_commandList->RSSetScissorRects(1, &shadowScissor);

            m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_commandList->SetPipelineState(m_shadowPipeline.pso.Get());
            m_commandList->SetGraphicsRootSignature(m_shadowPipeline.rootSignature.Get());

            for (size_t i = 0; i < objects.size(); ++i)
            {
                const RenderObject& obj = objects[i];
                const uint32_t slot = frameIndex * kMaxSceneObjects + static_cast<uint32_t>(i);

                XMMATRIX world = obj.GetWorldMatrix(totalTimeSeconds);
                ShadowConstants shadowConstants;
                XMStoreFloat4x4(&shadowConstants.worldViewProjLight, XMMatrixTranspose(world * lightViewProj));
                m_shadowConstantBuffer.Update(slot, shadowConstants);

                m_commandList->SetGraphicsRootConstantBufferView(0, m_shadowConstantBuffer.GetGpuAddress(slot));
                m_commandList->IASetVertexBuffers(0, 1, &obj.vbv);
                m_commandList->IASetIndexBuffer(&obj.ibv);
                m_commandList->DrawIndexedInstanced(obj.indexCount, 1, 0, 0, 0);
            }

            D3D12_RESOURCE_BARRIER toShaderResource{};
            toShaderResource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toShaderResource.Transition.pResource = m_shadowMap->GetResource();
            toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toShaderResource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &toShaderResource);
        }

        // --- Pass 2: G-buffer - write material data, no lighting, once per object ---
        {
            D3D12_RESOURCE_BARRIER toRenderTarget[3]{};
            ID3D12Resource* gbufferColorResources[3] = {
                m_gbuffer->GetAlbedoResource(), m_gbuffer->GetNormalResource(), m_gbuffer->GetMRAOResource()
            };
            for (int i = 0; i < 3; ++i)
            {
                toRenderTarget[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toRenderTarget[i].Transition.pResource = gbufferColorResources[i];
                toRenderTarget[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                toRenderTarget[i].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                toRenderTarget[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            D3D12_RESOURCE_BARRIER gbufferDepthToWrite{};
            gbufferDepthToWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            gbufferDepthToWrite.Transition.pResource = m_gbuffer->GetDepthResource();
            gbufferDepthToWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            gbufferDepthToWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            gbufferDepthToWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            D3D12_RESOURCE_BARRIER gbufferBarriersIn[4] = { toRenderTarget[0], toRenderTarget[1], toRenderTarget[2], gbufferDepthToWrite };
            m_commandList->ResourceBarrier(4, gbufferBarriersIn);

            D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtvs[3];
            m_gbuffer->GetRtvHandles(gbufferRtvs);
            D3D12_CPU_DESCRIPTOR_HANDLE gbufferDsv = m_gbuffer->GetDsv();
            m_commandList->OMSetRenderTargets(3, gbufferRtvs, FALSE, &gbufferDsv);

            const float albedoClear[4] = { 0.04f, 0.045f, 0.06f, 1.0f };
            const float zeroClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            m_commandList->ClearRenderTargetView(gbufferRtvs[0], albedoClear, 0, nullptr);
            m_commandList->ClearRenderTargetView(gbufferRtvs[1], zeroClear, 0, nullptr);
            m_commandList->ClearRenderTargetView(gbufferRtvs[2], zeroClear, 0, nullptr);
            m_commandList->ClearDepthStencilView(gbufferDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            D3D12_VIEWPORT gbufferViewport = m_gbuffer->GetViewport();
            D3D12_RECT gbufferScissor = m_gbuffer->GetScissorRect();
            m_commandList->RSSetViewports(1, &gbufferViewport);
            m_commandList->RSSetScissorRects(1, &gbufferScissor);

            m_commandList->SetPipelineState(m_gbufferPipeline.pso.Get());
            m_commandList->SetGraphicsRootSignature(m_gbufferPipeline.rootSignature.Get());

            for (size_t i = 0; i < objects.size(); ++i)
            {
                const RenderObject& obj = objects[i];
                const uint32_t slot = frameIndex * kMaxSceneObjects + static_cast<uint32_t>(i);

                XMMATRIX world = obj.GetWorldMatrix(totalTimeSeconds);
                GBufferPerObjectConstants gbufferConstants;
                XMStoreFloat4x4(&gbufferConstants.world, XMMatrixTranspose(world));
                XMStoreFloat4x4(&gbufferConstants.viewProj, XMMatrixTranspose(viewProj));
                m_gbufferConstantBuffer.Update(slot, gbufferConstants);
                m_materialConstantBuffer.Update(slot, obj.material->factors);

                m_commandList->SetGraphicsRootConstantBufferView(0, m_gbufferConstantBuffer.GetGpuAddress(slot));
                m_commandList->SetGraphicsRootConstantBufferView(1, m_materialConstantBuffer.GetGpuAddress(slot));
                m_commandList->SetGraphicsRootDescriptorTable(2, obj.material->GetTableGpuHandle(*m_srvHeap));

                m_commandList->IASetVertexBuffers(0, 1, &obj.vbv);
                m_commandList->IASetIndexBuffer(&obj.ibv);
                m_commandList->DrawIndexedInstanced(obj.indexCount, 1, 0, 0, 0);
            }

            D3D12_RESOURCE_BARRIER gbufferBarriersOut[4]{};
            for (int i = 0; i < 3; ++i)
            {
                gbufferBarriersOut[i] = toRenderTarget[i];
                std::swap(gbufferBarriersOut[i].Transition.StateBefore, gbufferBarriersOut[i].Transition.StateAfter);
            }
            gbufferBarriersOut[3] = gbufferDepthToWrite;
            std::swap(gbufferBarriersOut[3].Transition.StateBefore, gbufferBarriersOut[3].Transition.StateAfter);
            m_commandList->ResourceBarrier(4, gbufferBarriersOut);
        }

        // --- Pass 3: ray-traced reflections (skipped entirely if DXR is unavailable) ---
        if (rtUsable)
        {
            m_raytracing->UpdateTopLevelStructure(m_device->GetDevice(), m_commandList.Get(), objects, totalTimeSeconds);

            D3D12_RESOURCE_BARRIER toUav{};
            toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toUav.Transition.pResource = m_raytracing->GetOutputResource();
            toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &toUav);

            m_raytracing->DispatchReflections(
                m_commandList.Get(), *m_srvHeap,
                m_screenConstantBuffer.GetGpuAddress(frameIndex), m_lightConstantBuffer.GetGpuAddress(frameIndex),
                m_gbuffer->GetSrvTableGpuHandle(*m_srvHeap), m_environment->GetSrvTableGpuHandle(*m_srvHeap));

            D3D12_RESOURCE_BARRIER toShaderRead{};
            toShaderRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toShaderRead.Transition.pResource = m_raytracing->GetOutputResource();
            toShaderRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            toShaderRead.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toShaderRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &toShaderRead);
        }

        // --- Pass 4: lighting - full-screen PBR shading, straight to the back buffer ---
        D3D12_RESOURCE_BARRIER toRenderTargetBB{};
        toRenderTargetBB.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTargetBB.Transition.pResource = m_swapChain->GetCurrentBackBuffer();
        toRenderTargetBB.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRenderTargetBB.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRenderTargetBB.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &toRenderTargetBB);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain->GetCurrentRtv();
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT viewport = m_swapChain->GetViewport();
        D3D12_RECT scissor = m_swapChain->GetScissorRect();
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);

        m_commandList->SetPipelineState(m_lightingPipeline.pso.Get());
        m_commandList->SetGraphicsRootSignature(m_lightingPipeline.rootSignature.Get());
        m_commandList->SetGraphicsRootConstantBufferView(0, m_screenConstantBuffer.GetGpuAddress(frameIndex));
        m_commandList->SetGraphicsRootConstantBufferView(1, m_lightConstantBuffer.GetGpuAddress(frameIndex));
        m_commandList->SetGraphicsRootDescriptorTable(2, m_gbuffer->GetSrvTableGpuHandle(*m_srvHeap));
        m_commandList->SetGraphicsRootDescriptorTable(3, m_shadowMap->GetSrvGpuHandle(*m_srvHeap));
        m_commandList->SetGraphicsRootDescriptorTable(4, m_environment->GetSrvTableGpuHandle(*m_srvHeap));
        m_commandList->SetGraphicsRootDescriptorTable(5, m_raytracing->GetOutputSrvGpuHandle(*m_srvHeap));

        // No vertex/index buffer needed - the full-screen triangle is
        // generated entirely from SV_VertexID (see shaders/FullscreenVS.hlsl).
        m_commandList->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER toPresent{};
        toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toPresent.Transition.pResource = m_swapChain->GetCurrentBackBuffer();
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &toPresent);

        m_commandList->EndQuery(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2 + 1);
        m_commandList->ResolveQueryData(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            frameIndex * 2, 2, m_timestampReadback.Get(), frameIndex * 2 * sizeof(uint64_t));

        DX_CHECK(m_commandList->Close());

        ID3D12CommandList* lists[] = { m_commandList.Get() };
        m_device->GetGraphicsQueue()->ExecuteCommandLists(1, lists);

        // --- EndFrame ---
        frame.fenceValue = Signal();
        m_swapChain->Present(m_vsyncEnabled);

        D3D12_RANGE readRange{ frameIndex * 2 * sizeof(uint64_t), (frameIndex * 2 + 2) * sizeof(uint64_t) };
        uint64_t* timestamps = nullptr;
        if (SUCCEEDED(m_timestampReadback->Map(0, &readRange, reinterpret_cast<void**>(&timestamps))))
        {
            const uint64_t delta = timestamps[1] > timestamps[0] ? timestamps[1] - timestamps[0] : 0;
            m_stats.gpuFrameMs = (static_cast<double>(delta) / static_cast<double>(m_gpuTimestampFrequency)) * 1000.0;
            D3D12_RANGE noWrite{ 0, 0 };
            m_timestampReadback->Unmap(0, &noWrite);
        }
    }
}
