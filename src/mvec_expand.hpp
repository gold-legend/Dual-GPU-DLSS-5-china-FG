#pragma once

// Optional lossless transport reduction for Lumenite's low-resolution flow.
// The game card transports the small RG16F field; GPU 1 reproduces the same
// point-sampled full-resolution field immediately before DLSS-NR reads it.

#include <windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

namespace mgpu::mvecex
{
bool arm(ID3D12Device *dev, bool enabled,
         unsigned src_w, unsigned src_h, unsigned dst_w, unsigned dst_h,
         DXGI_FORMAT format, ID3D12Resource *const src[2]);
bool active();
ID3D12Resource *output(unsigned parity);
void expand(ID3D12GraphicsCommandList *cl, unsigned parity, ID3D12Resource *src);
void release();
}
