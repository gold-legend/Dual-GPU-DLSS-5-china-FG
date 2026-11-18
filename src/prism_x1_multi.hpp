#pragma once

#include <windows.h>
#include <d3d12.h>

// Three independent PX1 instances. A pass never reads another pass's history,
// GuidedFit coefficients, descriptor heap or intensity/guard state.
namespace mgpu::prism_x1m
{
constexpr unsigned kMaxPasses = 3;

bool arm(unsigned pass, ID3D12Device *dev, unsigned width, unsigned height,
         DXGI_FORMAT fmt, unsigned low_width, unsigned low_height);
bool active(unsigned pass);
bool ab_enabled(unsigned pass);

bool down(unsigned pass, ID3D12GraphicsCommandList *cl, unsigned parity,
          ID3D12Resource *base, ID3D12Resource *low_proxy,
          unsigned low_width, unsigned low_height);

bool compose(unsigned pass, ID3D12GraphicsCommandList *cl, unsigned parity,
             ID3D12Resource *base, ID3D12Resource *mask_base,
             ID3D12Resource *low_proxy, ID3D12Resource *low_neural, ID3D12Resource *out,
             unsigned low_width, unsigned low_height,
             ID3D12Resource *depth, bool limit_residual);

void set_intensity(unsigned pass, float value);
float intensity(unsigned pass);
void set_guard(unsigned pass, float value);
float guard(unsigned pass);
void reset_history(unsigned pass);
void release(unsigned pass);
void release_all();
}
