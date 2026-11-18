// Intentionally no #pragma once: prism_x1_multi.cpp includes the implementation
// three times with a different PRISM_X1_NAMESPACE, and each namespace needs its
// own declarations (including mark_callback).

// MGPU Bridge - per-pass, current-frame Prism reconstruction.
// The implementation can be included more than once with a different
// PRISM_X1_NAMESPACE so every NR pass owns independent history and coefficients.

#include <windows.h>
#include <d3d12.h>
#include "prism_x1_motion.hpp"

#ifndef PRISM_X1_NAMESPACE
#define PRISM_X1_NAMESPACE prism_x1
#endif

namespace mgpu::PRISM_X1_NAMESPACE
{
// Call after the NRS scratch pair has been allocated. low_width/low_height are
// this pass's current work extent, not the shared scratch allocation's extent.
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt,
         unsigned low_width, unsigned low_height);
bool active();
bool ab_enabled();
bool toggle_ab();

// The caller owns low_proxy/low_neural states. down expects base in
// NON_PIXEL_SHADER_RESOURCE and low_proxy in UNORDERED_ACCESS.
bool down(ID3D12GraphicsCommandList *cl, unsigned parity,
          ID3D12Resource *base, ID3D12Resource *low_proxy,
          unsigned low_width, unsigned low_height);

// Both low inputs are NON_PIXEL_SHADER_RESOURCE; out is UNORDERED_ACCESS.
// Returns true only after GuidedFit + Compose have both been recorded.
using mark_callback = void (*)(void *context, unsigned role);
bool compose(ID3D12GraphicsCommandList *cl, unsigned parity,
             ID3D12Resource *base, ID3D12Resource *mask_base,
             ID3D12Resource *low_proxy, ID3D12Resource *low_neural, ID3D12Resource *out,
             unsigned low_width, unsigned low_height,
             ID3D12Resource *depth, bool limit_residual,
             const MgpuPrismMotion &motion,
             mark_callback mark = nullptr, void *mark_context = nullptr);

void set_intensity(float value);
float intensity();
void set_guard(float value);
float guard();
// Invalidate only this instance's lightweight confidence history. Resource and
// PSO lifetime stay unchanged; the next Compose captures a fresh low input.
void reset_history();
void release();
}
