#include "amd_of.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>

namespace mgpu::diag
{
void info(const char *s) { std::printf("%s\n", s); }
void warn(const char *s) { std::printf("WARN: %s\n", s); }
void error(const char *s) { std::printf("ERROR: %s\n", s); }
void info_direct(const char *s) { info(s); }
void warn_direct(const char *s) { warn(s); }
void error_direct(const char *s) { error(s); }
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
    IDXGIFactory6 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Resource *raw = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
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
    if (!ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator")) goto done;
    if (!ok(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)), "CreateCommandList")) goto done;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = d.Height = 64;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&raw)), "Create raw colour")) goto done;
    }
    if (!mgpu::amd_of::arm(device, 64, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, true, 0.70f, true) ||
        !mgpu::amd_of::flow_ready())
    {
        std::printf("FAIL: AMD arm/flow_ready\n");
        goto done;
    }
    {
        mgpu::amd_of::capture_result a{}, b{}, selected{};
        if (!mgpu::amd_of::capture(list, raw, 1, 0, true, a) || !a.flow_valid)
        {
            std::printf("FAIL: first capture\n");
            goto done;
        }
        if (!mgpu::amd_of::capture(list, raw, 2, 1, false, b) || !b.flow_valid)
        {
            std::printf("FAIL: second capture\n");
            goto done;
        }
        mgpu::amd_of::set_frame_source(0);
        if (!mgpu::amd_of::present(selected) || selected.source_frame != 1 || selected.input_slot != 0 || !selected.flow_valid)
        {
            std::printf("FAIL: previous-frame selection\n");
            goto done;
        }
        mgpu::amd_of::set_frame_source(2);
        if (!mgpu::amd_of::present(selected) || selected.source_frame != 2 || selected.flow_valid)
        {
            std::printf("FAIL: native current-frame selection\n");
            goto done;
        }
        mgpu::amd_of::set_frame_source(1);
        if (!mgpu::amd_of::present(selected) || selected.source_frame != 2 || !selected.flow_valid)
        {
            std::printf("FAIL: Present-frame selection\n");
            goto done;
        }
        std::printf("Selections: first=%llu second=%llu previous/native/Present OK\n", a.source_frame, b.source_frame);
    }
    if (!ok(list->Close(), "CommandList Close")) goto done;
    {
        ID3D12CommandList *lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
    }
    if (!ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence")) goto done;
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) { std::printf("FAIL: CreateEvent\n"); goto done; }
    if (!ok(queue->Signal(fence, 1), "Queue Signal")) goto done;
    if (!ok(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion")) goto done;
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
    {
        std::printf("FAIL: GPU completion timeout, removed hr=0x%08X\n", unsigned(device->GetDeviceRemovedReason()));
        goto done;
    }
    if (!ok(device->GetDeviceRemovedReason(), "Device after AMD dispatch")) goto done;
    std::printf("PASS: AMD context/PSO/dispatch and three frame-source modes\n");
    result = 0;

done:
    mgpu::amd_of::release();
    release(raw); release(list); release(allocator); release(queue); release(fence);
    release(device); release(adapter); release(factory);
    if (event) CloseHandle(event);
    return result;
}
