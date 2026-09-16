// File: src/Graphics/FrameContext.h
//
// One instance per swap-chain buffer. Each frame in flight needs its own
// command allocator (allocators cannot be reset while the GPU is still
// executing commands recorded from them) and its own upload resources for
// per-frame constant data - reusing one allocator/buffer across frames in
// flight is the single most common source of D3D12 corruption bugs.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace gfx
{
    using Microsoft::WRL::ComPtr;

    struct FrameContext
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;

        // The fence value that, once reached by the GPU, guarantees this
        // frame's commands have finished executing and its allocator/buffers
        // are safe to reuse.
        uint64_t fenceValue = 0;

        void Reset()
        {
            // ResetAllocator is only valid to call once fenceValue has been
            // reached - the renderer checks this before recording.
            commandAllocator->Reset();
        }
    };
}
