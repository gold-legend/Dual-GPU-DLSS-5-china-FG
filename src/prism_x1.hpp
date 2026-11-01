#pragma once

// MGPU Bridge - experimental reduced-network, same-frame Prism reconstruction.
// OFF unless PrismX1=1 and the existing NRScale path has a reduced extent.
// Cadence 1, 2 and diagnostic 4 are accepted: PX1 runs on evaluated frames;
// held frames reuse the previous neural output. No temporal edit transport,
// asynchronous landing or FG. Cadence 4 is not original-Prism feature parity.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::prism_x1
{
// Call after the existing NRS scratch pair has been allocated. The coefficient
// textures use the native extent; only their current low-size top-left is used.
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt,
         unsigned low_width, unsigned low_height);
bool active();
// Runtime A/B switch. When bypassed, the existing NRS path runs unchanged.
bool ab_enabled();
bool toggle_ab();

// The caller owns low_proxy/low_neural states. down expects base in
// NON_PIXEL_SHADER_RESOURCE and low_proxy in UNORDERED_ACCESS.
bool down(ID3D12GraphicsCommandList *cl, unsigned parity,
          ID3D12Resource *base, ID3D12Resource *low_proxy);

// Both low inputs are NON_PIXEL_SHADER_RESOURCE; out is UNORDERED_ACCESS.
// Returns true only after GuidedFit + Compose have both been recorded.
// The optional mark callback uses the caller's existing EvalTimes query heap.
using mark_callback = void (*)(void *context, unsigned role);
bool compose(ID3D12GraphicsCommandList *cl, unsigned parity,
             ID3D12Resource *base, ID3D12Resource *low_proxy,
             ID3D12Resource *low_neural, ID3D12Resource *out,
             mark_callback mark = nullptr, void *mark_context = nullptr);

void set_intensity(float value);
float intensity();
void set_guard(float value);
float guard();
void release();
}
