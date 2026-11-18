#pragma once

#include <cstdint>
#include <d3d12.h>
#include "native_input.hpp"

// GPU1-local native inputs for the experimental DLSSG path. Raw GPU0 texture
// pointers never leave the provider. The shared allocation contains LINEAR
// copies; this module waits for its producer fence before copying them into
// textures owned by the neural adapter. No function here enables interpolation.
namespace mgpu::native_gpu1
{
struct frame
{
    native_input::lease input{};
    ID3D12Resource *mvec = nullptr;   // Borrowed from this module's slot.
    ID3D12Resource *depth = nullptr;
    bool copied = false;

    frame() = default;
    frame(const frame &) = delete;
    frame &operator=(const frame &) = delete;
    frame(frame &&) noexcept;
    frame &operator=(frame &&) noexcept;
    ~frame();
    void reset();
};

// Call on the bridge thread, recording into the neural queue's open list.
// A false result always leaves a released/empty frame: the caller must show
// its real final neural colour and must not run interpolation for this pair.
bool prepare(ID3D12Device *device, ID3D12CommandQueue *queue,
             ID3D12GraphicsCommandList *list, uint64_t game_luid,
             uint64_t color_frame, unsigned slot, bool import_for_fg, frame &out);

// The FG set releases its lease only after its presents and the neural queue
// have retired. release_v1 is the provider's consumed acknowledgement: until
// then it cannot reuse that shared slot or replace the heap. If retirement is
// unproven, the lease is retained rather than risking a cross-adapter UAF.
void retire(unsigned slot, frame &out);
// A queued evaluation can outlive the provider's camera/resource generation.
// Recheck before handing an interpolated frame to the pacing thread.
bool current(const frame &out);

// Drains the module's own neural-queue completion fence before releasing any
// imported heap or GPU1 texture. A timeout keeps allocations alive and fails
// closed; it never frees memory beneath pending GPU work.
void reset(ID3D12CommandQueue *queue, bool final_release);
const char *reason();
const char *pixel_reason();
uint64_t verified_frames();
uint64_t imported_frames();
}
