#pragma once

// Experimental secondary-adapter DLSS Frame Generation. OFF unless FrameGen=4.
// FGMulti=2|4 selects one or three distinct generated frames per real frame.
// DLSSG uses the official 0.6.0 path: the motion vectors and depth already
// transported to GPU1, with the camera estimated from FGFov/FGNear/FGFar.
// NativeInput=1 remains a separate GPU0 readback diagnostic, not an FG input.
#include <cstdint>
#include <windows.h>
#include <d3d12.h>

namespace mgpu::fg
{
struct arm_info
{
    ID3D12Device *ndev = nullptr;         // Neural device, borrowed from the stream.
    ID3D12CommandQueue *nqueue = nullptr; // Neural queue, borrowed, not a present queue.
    unsigned w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    unsigned mvec_w = 0, mvec_h = 0;
    unsigned depth_w = 0, depth_h = 0;
    bool depth_inverted = true;
    float mvec_scale_x = 1.0f, mvec_scale_y = 1.0f;
    uint64_t game_luid = 0;               // Used only by NativeInput diagnostics.
};

bool arm(const arm_info &i);
bool on();
// One inexpensive check: FrameGen=0 and NativeInput=0 do no capture work.
bool capture_requested();

// Diagnostic capture only. Frame generation does not enable the second
// cross-adapter input path; it consumes the stream's GPU1-local textures.
uint64_t bind_color(uint64_t current, uint64_t previous, uint64_t seal_qpc);

// Official 0.6.0 call site: final colour is UNORDERED_ACCESS; MV/depth are
// still NON_PIXEL_SHADER_RESOURCE and belong to this same neural frame.
void record(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_final,
            ID3D12Resource *mvec, ID3D12Resource *depth, float mv_el,
            uint64_t game_frame);

// Bridge thread only. Neural OFF drains and joins the pacing thread before
// the worker takes presentation back; neural ON resumes FG after that handover.
// Returns whether handoff() may be used for the current worker iteration.
bool sync_neural(bool neural);

// True only when this module's single pacing thread presents this output.
// A false return always means the worker presents the current real nr_final.
bool handoff(float r, float g, float b);

// Join outside the stream lock; its present path takes that lock.
void stop();

// Called inside stream_release before any neural queue/texture is freed.
void release();
}
