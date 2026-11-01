#include "present_queue.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>

namespace mgpu::diag
{
void info(const char *s) { std::printf("%s\n", s); }
void warn(const char *s) { std::printf("WARN: %s\n", s); }
void error(const char *s) { std::printf("ERROR: %s\n", s); }
}

namespace
{
template <typename T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }
bool ok(HRESULT hr, const char *what)
{
    if (SUCCEEDED(hr)) return true;
    std::printf("FAIL: %s hr=0x%08X\n", what, unsigned(hr));
    return false;
}
}

int main()
{
    const wchar_t *class_name = L"MgpuPresentQueueSmoke";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = class_name;
    if (!RegisterClassW(&wc)) { std::printf("FAIL: RegisterClassW\n"); return 1; }
    HWND hwnd = CreateWindowExW(0, class_name, L"queue smoke", WS_OVERLAPPEDWINDOW,
                                0, 0, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { std::printf("FAIL: CreateWindowExW\n"); return 1; }

    IDXGIFactory6 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    IDXGISwapChain1 *chain1 = nullptr;
    IDXGISwapChain3 *chain3 = nullptr;
    int result = 1;

    if (!ok(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) goto done;
    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1 *candidate = nullptr;
        if (FAILED(factory->EnumAdapters1(i, &candidate))) break;
        DXGI_ADAPTER_DESC1 d{};
        candidate->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && wcsstr(d.Description, L"5060") != nullptr)
        {
            adapter = candidate;
            std::printf("Adapter: %ls\n", d.Description);
            break;
        }
        candidate->Release();
    }
    if (!adapter) { std::printf("FAIL: no 5060 adapter found\n"); goto done; }
    if (!ok(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice")) goto done;
    {
        D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        if (!ok(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue")) goto done;
    }
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 256;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3; // Intentionally fixed: this test does not change BufferCount.
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        mgpu::present_queue::prepare_desc(desc, false);
        std::printf("Swapchain: BufferCount=%u flags=0x%X\n", desc.BufferCount, desc.Flags);
        if (!ok(factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &chain1),
                "CreateSwapChainForHwnd")) goto done;
    }
    if (!ok(chain1->QueryInterface(IID_PPV_ARGS(&chain3)), "Query IDXGISwapChain3")) goto done;
    // An occluded chain does not advance presents or signal its latency object.
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    if (!mgpu::present_queue::arm(chain3, false, 0))
    {
        std::printf("FAIL: arm queue 0\n");
        goto done;
    }
    for (int extra = 0; extra <= 2; ++extra)
    {
        mgpu::present_queue::request(unsigned(extra));
        // Two cycles per setting: the first submits the initial frame, the
        // second proves that the real wait object can complete afterwards.
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            if (!mgpu::present_queue::wait(chain3)) { std::printf("FAIL: wait queue %d cycle %d\n", extra, cycle); goto done; }
            UINT actual = 0;
            if (!ok(chain3->GetMaximumFrameLatency(&actual), "GetMaximumFrameLatency")) goto done;
            const bool pass = mgpu::present_queue::requested() == extra &&
                              mgpu::present_queue::applied() == extra &&
                              mgpu::present_queue::waitable() && actual == UINT(extra + 1) &&
                              chain3->GetFrameLatencyWaitableObject() != nullptr;
            std::printf("%s: extra=%d cycle=%d requested=%d applied=%d DXGI=%u waitable=%d\n",
                        pass ? "PASS" : "FAIL", extra, cycle, mgpu::present_queue::requested(),
                        mgpu::present_queue::applied(), actual, int(mgpu::present_queue::waitable()));
            if (!pass || !ok(chain3->Present(1, 0), "Present")) goto done;
        }
    }
    if (mgpu::present_queue::arm(chain3, true, 1) ||
        mgpu::present_queue::applied() != -1 || mgpu::present_queue::waitable())
    {
        std::printf("FAIL: DirectComposition must report unsupported, not a fake queue\n");
        goto done;
    }
    std::printf("PASS: DirectComposition honestly reports unsupported\n");
    std::printf("PASS: real HWND swapchain queue 0/1/2, wait object and fixed BufferCount\n");
    result = 0;

done:
    release(chain3); release(chain1); release(queue); release(device); release(adapter); release(factory);
    DestroyWindow(hwnd);
    UnregisterClassW(class_name, GetModuleHandleW(nullptr));
    return result;
}
