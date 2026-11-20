#pragma once

// Golden Prism 1.1 same-size colour/history stabiliser. This module deliberately
// contains no upscaler, no cadence change and no motion-vector production.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::temporal_stab
{
// Reads TemporalStrength (0..100) and TemporalMotionAdaptive (0/1) at arm.
// Zero or an absent strength leaves the published render path untouched.
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt);
bool active();

// Stabilises nr_final in place and retains the stabilised result as the next
// evaluation's history. nr_final enters and leaves in UNORDERED_ACCESS.
void apply(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_final);

// Called only after the stream's GPU work has drained.
void release();
}
