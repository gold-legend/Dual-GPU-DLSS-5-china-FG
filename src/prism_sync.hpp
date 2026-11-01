#pragma once

// MGPU Bridge - synchronous, same-frame Prism reconstruction on the neural GPU.
// Experimental and OFF unless PrismSync=1 is present in mgpu.ini.
//
// Scope of this first milestone:
//   - one or two full-resolution DLSS-NR passes, no SR/NRS/crop/foveation;
//   - with two passes, Compose still uses the original seal-frame base, never pass 1;
//   - the original input and the network answer belong to the same seal frame;
//   - Extract computes Prism's guarded log2(neural/proxy) edit;
//   - Compose applies that edit to the original input and writes a separate output;
//   - optional joint-bilateral edit filtering uses the original frame as its guide;
//   - no temporal transport, no asynchronous landing, no cadence and no FG.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::psync
{
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt);
bool active();

// Records Extract + Compose. `base` is the same-frame network input and is already
// NON_PIXEL_SHADER_RESOURCE. `neural` and `out` are UNORDERED_ACCESS on entry and
// are returned to that state. `out` must be different from both inputs.
// Returns true only when both dispatches were recorded.
bool apply(ID3D12GraphicsCommandList *cl, unsigned parity,
           ID3D12Resource *base, ID3D12Resource *neural, ID3D12Resource *out);

// Live panel controls for the values already inside Extract.
void set_intensity(float intensity);
float intensity();
void set_guard(float guard);
float guard();

void release();
}
