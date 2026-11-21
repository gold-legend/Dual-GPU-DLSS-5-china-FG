#pragma once

// Independent FG-only frame-pair motion estimation. No game MV, no CPU readback.
#include <d3d12.h>

namespace mgpu::fgpair
{
bool arm(ID3D12Device *dev, bool enabled, unsigned w, unsigned h, DXGI_FORMAT fmt);
bool active();

// current must be in NON_PIXEL_SHADER_RESOURCE. slot is the FG set index (0..3).
// The returned RG16F texture is in NON_PIXEL_SHADER_RESOURCE and contains
// current -> previous displacement in output pixels. The first pair is zero.
ID3D12Resource *record(ID3D12GraphicsCommandList *cl, ID3D12Resource *current,
                       unsigned slot, bool *estimated = nullptr);

void stats(unsigned long long &pairs, unsigned long long &fallbacks);
void release();
}
