#pragma once

#include <d3d12.h>

// The exact motion-vector contract already bound to this pass's DLSSNR evaluate.
// Scale converts stored render-pixel displacement to PX1's low-resolution texels;
// base/extent describe the valid corner inside the vector resource.
struct MgpuPrismMotion
{
    ID3D12Resource *resource = nullptr;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    unsigned base_x = 0u;
    unsigned base_y = 0u;
    unsigned width = 0u;
    unsigned height = 0u;
};
