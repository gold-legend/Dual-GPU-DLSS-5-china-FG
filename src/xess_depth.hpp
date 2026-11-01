#pragma once

#include <d3d12.h>

namespace mgpu::xess_depth
{
// source is NON_PIXEL_SHADER_RESOURCE. The returned texture has the motion
// vector extent, R32_FLOAT format, and rests in NON_PIXEL_SHADER_RESOURCE.
// Two independent slots match the bridge's input lifetime. No CPU readback.
bool prepare(ID3D12Device *device, ID3D12GraphicsCommandList *cl,
             ID3D12Resource *source, unsigned slot,
             unsigned target_width, unsigned target_height,
             ID3D12Resource **out);

// Only after all direct/copy/present queue work has drained.
void release();
}
