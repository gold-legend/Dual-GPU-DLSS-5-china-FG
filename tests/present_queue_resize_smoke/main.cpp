#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>

namespace {
template <typename T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

struct Context {
    IDXGIFactory6 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    HWND hwnd = nullptr;
};

IDXGISwapChain3 *make_chain(Context &c) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = 1280; d.Height = 720;
    d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    IDXGISwapChain1 *chain1 = nullptr;
    IDXGISwapChain3 *chain3 = nullptr;
    HRESULT hr = c.factory->CreateSwapChainForHwnd(c.queue, c.hwnd, &d, nullptr, nullptr, &chain1);
    if (FAILED(hr)) { std::printf("Create failed: 0x%08X\n", unsigned(hr)); return nullptr; }
    hr = chain1->QueryInterface(IID_PPV_ARGS(&chain3));
    chain1->Release();
    if (FAILED(hr)) { std::printf("QI failed: 0x%08X\n", unsigned(hr)); return nullptr; }
    return chain3;
}
}

int main() {
    const wchar_t *name = L"MgpuPresentQueueResizeSmoke";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    if (!RegisterClassW(&wc)) return 2;
    Context c;
    c.hwnd = CreateWindowExW(0, name, name, WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720,
                             nullptr, nullptr, wc.hInstance, nullptr);
    int result = 2;
    if (!c.hwnd) goto done;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&c.factory)))) goto done;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *candidate = nullptr;
        if (FAILED(c.factory->EnumAdapters1(i, &candidate))) break;
        DXGI_ADAPTER_DESC1 d{}; candidate->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && wcsstr(d.Description, L"5060")) {
            c.adapter = candidate; std::printf("Adapter: %ls\n", d.Description); break;
        }
        candidate->Release();
    }
    if (!c.adapter || FAILED(D3D12CreateDevice(c.adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)))) goto done;
    {
        D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        if (FAILED(c.device->CreateCommandQueue(&q, IID_PPV_ARGS(&c.queue)))) goto done;
    }
    {
        IDXGISwapChain3 *chain = make_chain(c);
        if (!chain) goto done;
        DXGI_SWAP_CHAIN_DESC1 before{}; chain->GetDesc1(&before);
        const HRESULT hr = chain->ResizeBuffers(2, 3840, 2160, DXGI_FORMAT_R10G10B10A2_UNORM, 0);
        std::printf("DROP_WAITABLE: before_flags=0x%X resize_flags=0x0 hr=0x%08X\n", before.Flags, unsigned(hr));
        chain->Release();
        if (hr != E_INVALIDARG) { std::printf("UNEXPECTED: game failure was not reproduced\n"); goto done; }
    }
    {
        IDXGISwapChain3 *chain = make_chain(c);
        if (!chain) goto done;
        DXGI_SWAP_CHAIN_DESC1 before{}; chain->GetDesc1(&before);
        const HRESULT hr = chain->ResizeBuffers(2, 3840, 2160, DXGI_FORMAT_R10G10B10A2_UNORM, before.Flags);
        std::printf("PRESERVE_FLAGS: before_flags=0x%X resize_flags=0x%X hr=0x%08X\n", before.Flags, before.Flags, unsigned(hr));
        chain->Release();
        if (FAILED(hr)) goto done;
    }
    std::printf("PASS: dropping the waitable flag reproduces E_INVALIDARG; preserving it fixes the same resize\n");
    result = 0;
done:
    release(c.queue); release(c.device); release(c.adapter); release(c.factory);
    if (c.hwnd) DestroyWindow(c.hwnd);
    UnregisterClassW(name, GetModuleHandleW(nullptr));
    return result;
}
