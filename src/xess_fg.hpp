#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace mgpu::xessfg
{
    // Colour and AMD optical-flow motion must belong to the same source frame.
    // The FG stage owns a constant placeholder depth; callers never supply or
    // expose real game depth here. Input resources are borrowed, never released.
    struct frame_inputs
    {
        ID3D12Resource *color = nullptr;
        ID3D12Resource *motion = nullptr;
        D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COPY_DEST;
        unsigned motion_width = 0, motion_height = 0;
        float motion_scale_x = 1.0f, motion_scale_y = 1.0f;
        float render_ms = 16.6667f;
        unsigned long long source_frame = 0;
        unsigned input_slot = 0;
        bool reset_history = false;
        // Live UI request. The proxy may remain created while FG is off; in
        // that state Intel presents one native frame instead of interpolating.
        bool generation_requested = true;
        // Number of generated frames per source frame: 1 = X2, 2 = X3.
        unsigned interpolated_frames = 1;
    };

    struct stats
    {
        bool active = false;
        bool generating = false;
        unsigned last_frames_presented = 0;
        int last_frame_result = 0;
        int last_error = 0;
        double output_fps = 0.0;
        unsigned long long application_presents = 0;
        unsigned long long generated_frames = 0;
    };

    // Creates the XeSS proxy instead of a native HWND swap chain. On any failure,
    // destroys the partial contexts and returns false so the caller can use its
    // original CreateSwapChainForHwnd path.
    bool create_swapchain(ID3D12Device *device, ID3D12CommandQueue *queue,
                          IDXGIFactory2 *factory, HWND hwnd,
                          const DXGI_SWAP_CHAIN_DESC1 &desc, bool inverted_depth,
                          unsigned interpolated_frames, IDXGISwapChain3 **out_swapchain);

    bool active();
    void begin_frame();
    bool tag_frame(ID3D12GraphicsCommandList *command_list, const frame_inputs &inputs);
    void end_frame(HRESULT present_result);
    void read_stats(stats &out);

    // The caller releases every IDXGISwapChain and back-buffer reference first.
    void shutdown();
}
