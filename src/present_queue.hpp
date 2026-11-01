#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

// UI queue depth EXCLUDES the frame currently being submitted. It is not the
// swap-chain BufferCount and not the bridge's command-allocator ring depth.
namespace mgpu::present_queue
{
void prepare_desc(DXGI_SWAP_CHAIN_DESC1 &desc, bool composition);
bool arm(IDXGISwapChain3 *chain, bool composition, unsigned extra_frames);
bool wait(IDXGISwapChain3 *chain);
void request(unsigned extra_frames);
int requested();
int applied();       // -1 = unsupported or not successfully configured.
bool waitable();     // True only with a real frame-latency wait object.
}
