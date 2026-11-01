#include "present_queue.hpp"
#include "diag.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace mgpu::present_queue
{
namespace
{
std::atomic<int> g_requested{0};
std::atomic<int> g_attempted{-1};
std::atomic<int> g_applied{-1};
std::atomic<bool> g_waitable{false};
std::atomic<bool> g_composition{false};
std::atomic<bool> g_first_present{true};

bool apply(IDXGISwapChain2 *chain, int extra)
{
    if (chain == nullptr) return false;
    extra = std::clamp(extra, 0, 2);
    g_attempted.store(extra, std::memory_order_relaxed);
    g_applied.store(-1, std::memory_order_relaxed);
    g_waitable.store(false, std::memory_order_relaxed);
    // The UI's 0/1/2 does not include the current frame; DXGI includes it.
    const HRESULT set_hr = chain->SetMaximumFrameLatency(UINT(extra + 1));
    UINT actual = 0;
    const HRESULT get_hr = chain->GetMaximumFrameLatency(&actual);
    HANDLE handle = chain->GetFrameLatencyWaitableObject();
    const bool ok = SUCCEEDED(set_hr) && SUCCEEDED(get_hr) &&
                    actual == UINT(extra + 1) && handle != nullptr;
    g_applied.store(ok ? extra : -1, std::memory_order_relaxed);
    g_waitable.store(ok, std::memory_order_relaxed);
    char line[520];
    if (ok)
        snprintf(line, sizeof line,
                 "[MGPU][PRESENT-QUEUE] extra=%u applied and read back: DXGI maximum frame latency=%u; real wait object=%p",
                 unsigned(extra), actual, handle);
    else
        snprintf(line, sizeof line,
                 "[MGPU][PRESENT-QUEUE] extra=%u NOT APPLIED: set_hr=0x%08X get_hr=0x%08X actual_latency=%u wait_object=%p; output continues without claiming a queue limit",
                 unsigned(extra), (unsigned)set_hr, (unsigned)get_hr, actual, handle);
    if (ok) mgpu::diag::info(line);
    else mgpu::diag::warn(line);
    return ok;
}
}

void prepare_desc(DXGI_SWAP_CHAIN_DESC1 &desc, bool composition)
{
    if (!composition) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
}

bool arm(IDXGISwapChain3 *chain, bool composition, unsigned extra_frames)
{
    request(extra_frames);
    g_attempted.store(-1, std::memory_order_relaxed);
    g_applied.store(-1, std::memory_order_relaxed);
    g_waitable.store(false, std::memory_order_relaxed);
    g_first_present.store(true, std::memory_order_relaxed);
    g_composition.store(composition, std::memory_order_relaxed);
    if (composition)
    {
        mgpu::diag::warn("[MGPU][PRESENT-QUEUE] DirectComposition does not expose this HWND waitable-chain queue control; no queue limit is claimed");
        return false;
    }
    if (chain == nullptr) return false;
    IDXGISwapChain2 *chain2 = nullptr;
    const HRESULT qi = chain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&chain2));
    if (FAILED(qi) || chain2 == nullptr) return false;
    const bool ok = apply(chain2, int(requested()));
    chain2->Release();
    return ok;
}

bool wait(IDXGISwapChain3 *chain)
{
    if (chain == nullptr || g_composition.load(std::memory_order_relaxed)) return true;
    IDXGISwapChain2 *chain2 = nullptr;
    const HRESULT qi = chain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&chain2));
    if (FAILED(qi) || chain2 == nullptr)
    {
        g_applied.store(-1, std::memory_order_relaxed);
        g_waitable.store(false, std::memory_order_relaxed);
        return true; // Explicitly unsupported in the panel; retain the ordinary present path.
    }
    const int extra = requested();
    if (g_attempted.load(std::memory_order_relaxed) != extra) (void)apply(chain2, extra);
    HANDLE handle = g_waitable.load(std::memory_order_relaxed)
        ? chain2->GetFrameLatencyWaitableObject() : nullptr;
    chain2->Release();
    if (handle == nullptr) return true;
    // Before the first Present there is no completed frame whose retirement
    // could signal this object. Allow that one submission, then wait normally.
    if (g_first_present.exchange(false, std::memory_order_relaxed)) return true;
    const DWORD result = WaitForSingleObject(handle, 5000);
    if (result == WAIT_OBJECT_0) return true;
    char line[240];
    snprintf(line, sizeof line,
             "[MGPU][PRESENT-QUEUE] real frame-latency wait did not complete: wait=0x%08X extra=%d",
             (unsigned)result, extra);
    mgpu::diag::error(line);
    return false;
}

void request(unsigned extra_frames)
{
    g_requested.store(std::clamp(int(extra_frames), 0, 2), std::memory_order_relaxed);
}
int requested() { return g_requested.load(std::memory_order_relaxed); }
int applied() { return g_applied.load(std::memory_order_relaxed); }
bool waitable() { return g_waitable.load(std::memory_order_relaxed); }
} // namespace mgpu::present_queue
