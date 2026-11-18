#pragma once

// Three same-frame DLSS-NR answers, one current-frame robust composite.
// The feature handles retain their own temporal histories; this module adds
// no cross-frame accumulation of its own.

#include <windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

namespace mgpu::ensemble
{
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt);
bool active();
void begin_frame();
bool capture_first(ID3D12GraphicsCommandList *cl, ID3D12Resource *first);
ID3D12Resource *compose(ID3D12GraphicsCommandList *cl, unsigned parity,
                        ID3D12Resource *original, ID3D12Resource *second,
                        ID3D12Resource *third);
void release();
}
