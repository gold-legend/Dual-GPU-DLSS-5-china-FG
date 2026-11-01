#pragma once

// Same-size colour temporal stabilisation and optional AMD FidelityFX Optical Flow.
// No upsampling or super-resolution is performed in this module. AMD OF supplies
// colour-derived motion to XeSS-FG; DLSS-NR keeps its own game depth/MV contract.

#include <d3d12.h>

namespace mgpu::amd_of
{
struct capture_result
{
    ID3D12Resource *color = nullptr;
    ID3D12Resource *motion = nullptr;
    D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    unsigned width = 0, height = 0;
    unsigned long long source_frame = 0;
    unsigned input_slot = 0; // AMD colour/MV parity, used for present-fence ownership.
    bool flow_valid = false; // False also means deliberate native presentation.
    bool reset_history = false;
};

// Called on the bridge thread after the NR stream has been drained/created.
// Failure never disables the ordinary NR path. want_flow controls only the AMD
// optical-flow half; the same-size temporal stabiliser is independent of it.
bool arm(ID3D12Device *device, unsigned width, unsigned height, DXGI_FORMAT format,
         bool want_flow, float temporal_strength, bool motion_adaptive);
void release();

// nr_output enters in UNORDERED_ACCESS and is restored to that state. Colour,
// motion and source_frame in one result always describe the same source frame.
// One command list, one queue, no CPU wait.
bool capture(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_output,
             unsigned long long source_frame, unsigned input_slot, bool reset_history,
             capture_result &out);

// Select exactly the same colour/frame as capture(), including its resource state.
bool present(capture_result &out);
bool flow_ready();

// 0 = previous completed source frame + its own flow; 1 = latest source + FG;
// 2 = latest source with native presentation (no fabricated future frame).
void set_frame_source(int mode);
int frame_source();

void set_temporal_strength(float value);
float temporal_strength();
void set_motion_adaptive(bool enabled);
bool motion_adaptive();
}
