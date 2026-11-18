#include "native_gpu1.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include "diag.hpp"
#include "native_pixels.hpp"

namespace mgpu::native_gpu1
{
namespace
{
constexpr unsigned SLOTS = 3u;
struct slot_t
{
    ID3D12Resource *mvec = nullptr;
    ID3D12Resource *depth = nullptr;
    // A dedicated short copy list lets its completion signal follow the
    // actual ExecuteCommandLists, rather than precede the neural list.
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    uint64_t completed = 0;
    uint64_t heap = 0;
    uint64_t generation = 0;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    bool resting_npsr = false; // New textures start in COPY_DEST, not NPSR.
    bool held_by_fg = false;
    native_input::lease orphan{}; // Retained when copy retirement is unproven.
};

slot_t g_slots[SLOTS]{};
ID3D12Heap *g_heap = nullptr;
ID3D12Resource *g_linear = nullptr;
ID3D12Fence *g_producer = nullptr;
ID3D12Fence *g_completion = nullptr;
HANDLE g_heap_handle = nullptr, g_fence_handle = nullptr;
uint64_t g_heap_id = 0, g_fence_id = 0, g_heap_bytes = 0, g_slot_bytes = 0;
uint64_t g_layout_generation = 0, g_completion_value = 0;
uint32_t g_layout_w = 0, g_layout_h = 0;
DXGI_FORMAT g_layout_depth = DXGI_FORMAT_UNKNOWN;
native_input::pixel_validator g_validator;
uint64_t g_qpc_frequency = 0, g_verified = 0, g_imported = 0;
char g_reason[256] = "native input has not been requested";
bool g_broken = false;

void note(const char *text, bool force = false)
{
    if (text == nullptr || *text == '\0') text = "native GPU1 input refused";
    if (!force && std::strcmp(g_reason, text) == 0) return;
    std::snprintf(g_reason, sizeof(g_reason), "%s", text);
    static unsigned reports = 0;
    if (force || (reports++ % 60u) == 0u)
    {
        char line[480];
        std::snprintf(line, sizeof(line), "[MGPU][NATIVE-GPU1] %s", g_reason);
        mgpu::diag::info(line);
    }
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

bool completed(ID3D12Fence *fence, uint64_t value)
{
    if (fence == nullptr || value == 0) return false;
    const uint64_t now = fence->GetCompletedValue();
    return now != UINT64_MAX && now >= value; // UINT64_MAX means device removal.
}

bool drain(ID3D12CommandQueue *queue, uint32_t timeout_ms)
{
    if (g_broken) return false;
    if (g_completion == nullptr) return true;
    if (queue == nullptr) return false;
    const uint64_t target = g_completion_value;
    if (!completed(g_completion, target) && target != 0)
    {
        if (FAILED(queue->Signal(g_completion, ++g_completion_value))) return false;
        const uint64_t submitted = g_completion_value;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) return false;
        const bool ok = SUCCEEDED(g_completion->SetEventOnCompletion(submitted, event)) &&
            WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0 && completed(g_completion, submitted);
        CloseHandle(event);
        if (!ok) return false;
    }
    return true;
}

void release_allocations()
{
    for (auto &s : g_slots)
    {
        if (s.list != nullptr) s.list->Release();
        if (s.allocator != nullptr) s.allocator->Release();
        if (s.mvec != nullptr) s.mvec->Release();
        if (s.depth != nullptr) s.depth->Release();
        s.list = nullptr; s.allocator = nullptr;
        s.mvec = nullptr; s.depth = nullptr;
        s.completed = 0;
        s.heap = s.generation = 0;
        s.width = s.height = 0;
        s.depth_format = DXGI_FORMAT_UNKNOWN;
        s.resting_npsr = false;
        s.held_by_fg = false;
    }
    if (g_linear != nullptr) g_linear->Release();
    if (g_heap != nullptr) g_heap->Release();
    if (g_producer != nullptr) g_producer->Release();
    if (g_completion != nullptr) g_completion->Release();
    if (g_heap_handle != nullptr) CloseHandle(g_heap_handle);
    if (g_fence_handle != nullptr) CloseHandle(g_fence_handle);
    g_linear = nullptr; g_heap = nullptr; g_producer = nullptr; g_completion = nullptr;
    g_heap_handle = g_fence_handle = nullptr;
    g_heap_id = g_fence_id = g_heap_bytes = g_slot_bytes = 0;
    g_layout_generation = 0; g_layout_w = g_layout_h = 0;
    g_layout_depth = DXGI_FORMAT_UNKNOWN;
    g_completion_value = 0;
}

bool changed(const mgpu_native_input_v1 &f)
{
    const auto &t = f.transfer;
    return g_heap == nullptr || g_heap_id != t.heap_handle || g_fence_id != t.fence_handle ||
        g_heap_bytes != t.heap_bytes || g_slot_bytes != t.slot_bytes ||
        g_layout_generation != f.generation || g_layout_w != f.mv_width ||
        g_layout_h != f.mv_height || g_layout_depth != (DXGI_FORMAT)t.depth_copy_format;
}

bool open_heap(ID3D12Device *device, ID3D12CommandQueue *queue,
               const mgpu_native_input_v1 &f)
{
    if (!changed(f)) return true;
    for (const auto &s : g_slots)
        if (s.held_by_fg || s.orphan.held)
        {
            note("native heap changed before a GPU1-consumed acknowledgement");
            return false;
        }
    if (!drain(queue, 100))
    {
        note("native heap changed before earlier neural work retired; real frames only");
        return false;
    }
    release_allocations();
    g_broken = false;
    const auto &t = f.transfer;
    HRESULT hr = device->OpenSharedHandle((HANDLE)(uintptr_t)t.heap_handle, IID_PPV_ARGS(&g_heap));
    if (SUCCEEDED(hr)) hr = device->OpenSharedHandle((HANDLE)(uintptr_t)t.fence_handle, IID_PPV_ARGS(&g_producer));
    if (FAILED(hr))
    {
        char why[128];
        std::snprintf(why, sizeof(why), "OpenSharedHandle(native heap/fence) failed: 0x%08X", (unsigned)hr);
        note(why, true);
        release_allocations();
        return false;
    }
    if (g_heap->GetDesc().SizeInBytes < t.heap_bytes)
    {
        note("opened native heap is smaller than its ABI descriptor", true);
        release_allocations();
        return false;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = t.heap_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    hr = device->CreatePlacedResource(g_heap, 0, &bd, D3D12_RESOURCE_STATE_COMMON,
                                      nullptr, IID_PPV_ARGS(&g_linear));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_completion));
    if (FAILED(hr))
    {
        note("GPU1 could not create its linear view or completion fence", true);
        release_allocations();
        return false;
    }
    g_heap_id = t.heap_handle; g_fence_id = t.fence_handle;
    g_heap_bytes = t.heap_bytes; g_slot_bytes = t.slot_bytes;
    g_layout_generation = f.generation;
    g_layout_w = f.mv_width; g_layout_h = f.mv_height;
    g_layout_depth = (DXGI_FORMAT)t.depth_copy_format;
    // OpenSharedHandle does not duplicate the provider's HANDLE. Own distinct
    // duplicates so teardown can never close a handle still used by GPU0.
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)t.heap_handle,
        GetCurrentProcess(), &g_heap_handle, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)t.fence_handle,
        GetCurrentProcess(), &g_fence_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        note("could not duplicate native transport handles for GPU1 ownership", true);
        release_allocations();
        return false;
    }
    char line[300];
    std::snprintf(line, sizeof(line),
        "[MGPU][NATIVE-GPU1] imported heap %llu bytes; shared fence and exact neural-frame pixels are required before FG",
        (unsigned long long)t.heap_bytes);
    mgpu::diag::info(line);
    return true;
}

bool make_slot(ID3D12Device *device, unsigned index, const mgpu_native_input_v1 &f)
{
    auto &s = g_slots[index];
    if (s.mvec != nullptr && s.depth != nullptr && s.allocator != nullptr && s.list != nullptr &&
        s.heap == f.transfer.heap_handle && s.generation == f.generation && s.width == f.mv_width &&
        s.height == f.mv_height && s.depth_format == (DXGI_FORMAT)f.transfer.depth_copy_format) return true;
    if (s.held_by_fg || s.orphan.held || (s.completed != 0 && !completed(g_completion, s.completed)))
    {
        note("native texture slot changed before the previous GPU1 copy retired");
        return false;
    }
    if (s.list != nullptr) s.list->Release();
    if (s.allocator != nullptr) s.allocator->Release();
    if (s.mvec != nullptr) s.mvec->Release();
    if (s.depth != nullptr) s.depth->Release();
    s = {}; // The old completed lease is gone; fresh textures rest in COPY_DEST.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = f.mv_width; d.Height = f.mv_height; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Format = (DXGI_FORMAT)f.transfer.mv_copy_format;
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.mvec));
    d.Format = (DXGI_FORMAT)f.transfer.depth_copy_format;
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.depth));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&s.allocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s.allocator, nullptr, IID_PPV_ARGS(&s.list));
    if (SUCCEEDED(hr)) hr = s.list->Close();
    if (FAILED(hr))
    {
        if (s.list != nullptr) s.list->Release();
        if (s.allocator != nullptr) s.allocator->Release();
        if (s.mvec != nullptr) s.mvec->Release();
        if (s.depth != nullptr) s.depth->Release();
        s = {};
        note("GPU1-local motion-vector/depth allocation failed", true);
        return false;
    }
    s.heap = f.transfer.heap_handle; s.generation = f.generation;
    s.width = f.mv_width; s.height = f.mv_height;
    s.depth_format = (DXGI_FORMAT)f.transfer.depth_copy_format;
    return true;
}

bool record_copy(ID3D12CommandQueue *queue, unsigned index,
                 const mgpu_native_input_v1 &f, frame &out)
{
    const auto &t = f.transfer;
    auto &s = g_slots[index];
    if (s.allocator == nullptr || s.list == nullptr || s.held_by_fg || s.orphan.held ||
        FAILED(queue->Wait(g_producer, t.fence_value)))
    {
        note("neural queue could not wait for the native cross-adapter fence", true);
        return false;
    }
    HRESULT hr = s.allocator->Reset();
    if (SUCCEEDED(hr)) hr = s.list->Reset(s.allocator, nullptr);
    if (FAILED(hr))
    {
        note("GPU1 native-copy command list could not be reset", true);
        return false;
    }
    if (s.resting_npsr)
    {
        transition(s.list, s.mvec, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(s.list, s.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    for (unsigned pass = 0; pass < 2u; ++pass)
    {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = g_linear;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = f.cookie * t.slot_bytes + (pass == 0u ? t.mv_offset : t.depth_offset);
        src.PlacedFootprint.Footprint.Format = pass == 0u ? (DXGI_FORMAT)t.mv_copy_format : (DXGI_FORMAT)t.depth_copy_format;
        src.PlacedFootprint.Footprint.Width = f.mv_width;
        src.PlacedFootprint.Footprint.Height = f.mv_height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pass == 0u ? t.mv_row_pitch : t.depth_row_pitch;
        dst.pResource = pass == 0u ? s.mvec : s.depth;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        s.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(s.list, s.mvec, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(s.list, s.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.resting_npsr = true;
    hr = s.list->Close();
    if (FAILED(hr))
    {
        note("GPU1 native-copy command list could not be closed", true);
        return false;
    }
    ID3D12CommandList *lists[] = {s.list};
    queue->ExecuteCommandLists(1, lists); // Same queue: precedes the open neural list.
    if (FAILED(queue->Signal(g_completion, ++g_completion_value)))
    {
        // The shared buffer may already be executing on GPU1. Keep its lease
        // until teardown proves a drain; do not let GPU0 overwrite that heap.
        s.orphan = std::move(out.input);
        g_broken = true;
        note("native-copy completion signal failed; allocation and provider lease retained", true);
        return false;
    }
    s.completed = g_completion_value;
    out.mvec = s.mvec; out.depth = s.depth; out.copied = true;
    s.held_by_fg = true;
    ++g_imported;
    return true;
}
} // namespace

frame::frame(frame &&other) noexcept { *this = std::move(other); }
frame &frame::operator=(frame &&other) noexcept
{
    if (this == &other) return *this;
    input = std::move(other.input);
    mvec = other.mvec; depth = other.depth; copied = other.copied;
    other.mvec = nullptr; other.depth = nullptr; other.copied = false;
    return *this;
}
frame::~frame() { reset(); }
void frame::reset()
{
    input.reset(); mvec = nullptr; depth = nullptr; copied = false;
}

bool prepare(ID3D12Device *device, ID3D12CommandQueue *queue,
             ID3D12GraphicsCommandList *list, uint64_t game_luid,
             uint64_t color_frame, unsigned slot, bool import_for_fg, frame &out)
{
    // list remains open for the final neural work. The dedicated copy list is
    // submitted first on the SAME queue, so its textures are ready before it.
    if (out.copied || slot >= SLOTS || device == nullptr || queue == nullptr || list == nullptr)
    {
        note("native GPU1 prepare received a live or invalid destination");
        return false;
    }
    out.reset();
    if (g_qpc_frequency == 0)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        g_qpc_frequency = (uint64_t)frequency.QuadPart;
    }
    if (g_broken) return false;
    native_input::lease acquired;
    if (!native_input::acquire_for_color(color_frame, game_luid, acquired)) return false;
    mgpu_native_input_sample_v1 pixels{};
    if (!native_input::sample(acquired, pixels)) return false;
    (void)g_validator.observe(acquired.data, pixels, g_qpc_frequency);
    if (!g_validator.authorize(acquired.data))
    {
        note(g_validator.reason());
        return false;
    }
    ++g_verified;
    if (!import_for_fg)
    {
        if (g_verified == 1u || g_verified % 60u == 0u) note(g_validator.reason(), true);
        return false;
    }
    const auto header = native_input::inspect_header(acquired.data, game_luid, acquired.data.generation, 1);
    if (!header.compatible)
    {
        note(header.reason);
        return false;
    }
    out.input = std::move(acquired);
    if (!open_heap(device, queue, out.input.data) || !make_slot(device, slot, out.input.data))
    {
        out.reset();
        return false;
    }
    if (!record_copy(queue, slot, out.input.data, out))
    {
        if (!g_broken) out.reset();
        return false;
    }
    if ((g_imported == 1u || g_imported % 120u == 0u))
    {
        const auto &r = g_validator.last_report();
        char line[360];
        std::snprintf(line, sizeof(line),
            "[MGPU][NATIVE-GPU1] imported %llu: color=%llu previous=%llu native=%llu, median=%.3f px P90=%.3f px; NOT yet an FG visual test",
            (unsigned long long)g_imported, (unsigned long long)out.input.data.game_frame,
            (unsigned long long)out.input.data.previous_game_frame,
            (unsigned long long)out.input.data.frame_id, r.median_error_px, r.p90_error_px);
        mgpu::diag::info(line);
    }
    return true;
}

void retire(unsigned slot, frame &out)
{
    if (slot >= SLOTS) return;
    auto &s = g_slots[slot];
    if (out.input.held && (!completed(g_completion, s.completed) || s.orphan.held))
    {
        // Never release the provider acknowledgement before GPU1 consumption
        // is proven. The caller's reset now destroys only an empty lease.
        g_broken = true;
        s.held_by_fg = true;
        if (!s.orphan.held) s.orphan = std::move(out.input);
        note("GPU1 copy retirement is unproven; native provider lease retained", true);
        return;
    }
    s.held_by_fg = false;
}

bool current(const frame &out)
{
    return out.copied && out.input.held && g_layout_generation == out.input.data.generation &&
        native_input::current(out.input);
}

void reset(ID3D12CommandQueue *queue, bool final_release)
{
    if (!drain(queue, final_release ? 2000u : 100u))
    {
        g_broken = true;
        note("neural queue did not drain; imported resources and provider leases retained", true);
        return;
    }
    for (auto &s : g_slots) { s.held_by_fg = false; s.orphan.reset(); }
    release_allocations();
    g_validator.reset();
    g_qpc_frequency = 0;
    g_verified = g_imported = 0;
    g_broken = false;
    note("native GPU1 resources reset");
}
const char *reason() { return g_reason; }
const char *pixel_reason() { return g_validator.reason(); }
uint64_t verified_frames() { return g_verified; }
uint64_t imported_frames() { return g_imported; }
} // namespace mgpu::native_gpu1
