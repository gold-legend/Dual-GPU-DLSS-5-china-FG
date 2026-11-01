#pragma once

// MGPU Bridge - experimental per-frame edit-field alignment for cadence holds.
// OFF unless PrismCarry=1 and the PX1 reduced-network path is armed.
// Evaluated frames keep their existing NGX output. Held frames follow the last
// edit field with CURRENT depth + motion vectors, sample the landed edit, fill
// disocclusions, and compose that edit onto the CURRENT native colour.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::prism_carry
{
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT colour_fmt,
         unsigned depth_width, unsigned depth_height, DXGI_FORMAT depth_fmt,
         unsigned motion_width, unsigned motion_height, DXGI_FORMAT motion_fmt);
bool active();
bool ready();

// base/depth are NON_PIXEL_SHADER_RESOURCE; evaluated is UNORDERED_ACCESS.
// The evaluated picture is only used to recover the exact edit that was shown.
bool bake(ID3D12GraphicsCommandList *cl, unsigned parity,
          ID3D12Resource *base, ID3D12Resource *evaluated, ID3D12Resource *depth);

// All inputs are NON_PIXEL_SHADER_RESOURCE except out, which is UAV on entry
// and remains UAV on return. out must not alias any input. No NGX evaluation.
bool align(ID3D12GraphicsCommandList *cl, unsigned parity,
           ID3D12Resource *base, ID3D12Resource *depth, ID3D12Resource *motion,
           ID3D12Resource *out, float motion_scale_x, float motion_scale_y);

// Invalidates history without releasing GPU resources. Safe from the UI thread;
// actual resource destruction remains release(), after the stream fence drain.
void reset();
void note_fallback();
void release();
}
