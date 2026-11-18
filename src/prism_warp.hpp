#pragma once

// Independent late-frame neural-edit experiment. OFF unless PrismWarp=1.
// The previous evaluation supplies only a log-ratio edit; the current game
// colour remains the picture. No reprojected pixels are fed back to DLSS-NR.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::pwarp
{
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt);
bool active();

// base, motion and depth are NON_PIXEL_SHADER_RESOURCE on entry and stay so.
// neural is UNORDERED_ACCESS on entry and is restored before return.
// The returned texture is owned by this module and rests UNORDERED_ACCESS.
// nullptr means the ordinary current neural output must be presented instead.
ID3D12Resource *apply(ID3D12GraphicsCommandList *cl, unsigned parity,
                      ID3D12Resource *base, ID3D12Resource *neural,
                      ID3D12Resource *motion, float motion_scale_x, float motion_scale_y,
                      ID3D12Resource *depth, bool depth_inverted);

void invalidate();
void release();
}
