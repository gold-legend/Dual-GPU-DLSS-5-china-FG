// MGPU Bridge - R333 StageCopy (GPU 0 async staging). See gpu0_stage.hpp for
// what it is for and the contract it keeps. Design: Dave (davidm71), OPT5.

#include "gpu0_stage.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::stage
{
namespace
{
const unsigned MAX_RING = 16u;

std::atomic<bool> g_up{false};

unsigned           g_ring = 0u;
unsigned long long g_slot_bytes = 0ull;
int                g_mode = 0;                        // 1 = original full-slot staging; 2 = colour only
ID3D12Device      *g_dev = nullptr;                   // borrowed, used for first-frame colour allocation
ID3D12Resource    *g_gxfer = nullptr;                 // borrowed, the caller owns it
ID3D12Resource    *g_stage = nullptr;                 // DEFAULT heap, game device, VRAM (mode 1 only)
ID3D12CommandQueue *g_cq = nullptr;                   // COPY queue, game device
ID3D12CommandAllocator *g_ca = nullptr;
ID3D12GraphicsCommandList *g_cl[MAX_RING] = {};       // prerecorded: stage slot k -> gxfer slot k
ID3D12Fence       *g_sf = nullptr;                    // game queue -> copy queue hand-off
unsigned long long g_last = 0ull;                     // last frame handed to the copy queue
unsigned long long g_submits = 0ull, g_fail = 0ull, g_bp_waits = 0ull;

// Mode 2 stages colour first, then depth as an independent lane. Motion vectors
// and the seal keep the original direct path. Separate depth lists mean a depth
// setup failure can never invalidate a colour copy already recorded this frame.
ID3D12Resource *g_colour[MAX_RING] = {};
D3D12_RESOURCE_DESC g_colour_desc{};
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_colour_fp{};
UINT64 g_colour_relative = 0ull;
bool g_colour_direct = false;
bool g_colour_lists_built = false;

ID3D12Resource *g_depth[MAX_RING] = {};
ID3D12GraphicsCommandList *g_depth_cl[MAX_RING] = {};
D3D12_RESOURCE_DESC g_depth_desc{};
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_depth_fp{};
UINT64 g_depth_relative = 0ull;
bool g_depth_direct = false;
bool g_depth_lists_built = false;

ID3D12Resource *g_mvec[MAX_RING] = {};
ID3D12GraphicsCommandList *g_mvec_cl[MAX_RING] = {};
ID3D12CommandAllocator *g_mvec_ca = nullptr;   // separate: MVec is recorded before colour
D3D12_RESOURCE_DESC g_mvec_desc{};
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_mvec_fp{};
UINT64 g_mvec_relative = 0ull;
bool g_mvec_direct = false;
bool g_mvec_lists_built = false;

// DMA timing (optional): copy-queue timestamps, two per slot, resolved into a
// READBACK buffer by the slot's own list.
ID3D12QueryHeap *g_cts = nullptr;
ID3D12Resource  *g_tsrb = nullptr;
const UINT64    *g_tsrb_cpu = nullptr;
UINT64           g_cts_freq = 0ull;
unsigned long long g_sampled = 0ull;                  // last fence value sampled
double   g_dma_sum = 0.0, g_dma_max = 0.0;
unsigned g_dma_n = 0u;
unsigned long long g_report_at = 1200ull;

bool ini_path(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu.ini", mod);
    return true;
}

// 1 keeps Dave's original full-slot path. 2 is the isolated colour-only test.
int read_key()
{
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return 0;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 0;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 0;       // whole-document rule
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "StageCopy");
    if (v == nullptr) return 0;
    const int mode = atoi(v);
    return (mode == 1 || mode == 2) ? mode : 0;
}

HRESULT make_buf(ID3D12Device *dev, UINT64 bytes, D3D12_HEAP_TYPE type, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RESOURCE_STATES st = (type == D3D12_HEAP_TYPE_READBACK)
                                       ? D3D12_RESOURCE_STATE_COPY_DEST
                                       : D3D12_RESOURCE_STATE_COMMON;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(out));
}

void free_all()
{
    g_up.store(false, std::memory_order_relaxed);
    for (unsigned k = 0; k < MAX_RING; ++k)
    {
        if (g_cl[k] != nullptr) { g_cl[k]->Release(); g_cl[k] = nullptr; }
        if (g_colour[k] != nullptr) { g_colour[k]->Release(); g_colour[k] = nullptr; }
        if (g_depth_cl[k] != nullptr) { g_depth_cl[k]->Release(); g_depth_cl[k] = nullptr; }
        if (g_depth[k] != nullptr) { g_depth[k]->Release(); g_depth[k] = nullptr; }
        if (g_mvec_cl[k] != nullptr) { g_mvec_cl[k]->Release(); g_mvec_cl[k] = nullptr; }
        if (g_mvec[k] != nullptr) { g_mvec[k]->Release(); g_mvec[k] = nullptr; }
    }
    if (g_cts != nullptr) { g_cts->Release(); g_cts = nullptr; }
    if (g_tsrb != nullptr)
    {
        if (g_tsrb_cpu != nullptr) g_tsrb->Unmap(0, nullptr);
        g_tsrb->Release(); g_tsrb = nullptr;
    }
    g_tsrb_cpu = nullptr;
    if (g_sf != nullptr) { g_sf->Release(); g_sf = nullptr; }
    if (g_mvec_ca != nullptr) { g_mvec_ca->Release(); g_mvec_ca = nullptr; }
    if (g_ca != nullptr) { g_ca->Release(); g_ca = nullptr; }
    if (g_cq != nullptr) { g_cq->Release(); g_cq = nullptr; }
    if (g_stage != nullptr) { g_stage->Release(); g_stage = nullptr; }
    g_gxfer = nullptr;
    g_dev = nullptr;
    g_mode = 0;
    g_colour_desc = D3D12_RESOURCE_DESC{};
    g_colour_fp = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
    g_colour_relative = 0ull;
    g_colour_direct = false;
    g_colour_lists_built = false;
    g_depth_desc = D3D12_RESOURCE_DESC{};
    g_depth_fp = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
    g_depth_relative = 0ull;
    g_depth_direct = false;
    g_depth_lists_built = false;
    g_mvec_desc = D3D12_RESOURCE_DESC{};
    g_mvec_fp = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
    g_mvec_relative = 0ull;
    g_mvec_direct = false;
    g_mvec_lists_built = false;
    g_ring = 0u; g_slot_bytes = 0ull; g_last = 0ull;
    g_cts_freq = 0ull; g_sampled = 0ull;
    g_dma_sum = g_dma_max = 0.0; g_dma_n = 0u;
    g_report_at = 1200ull;
}

// Fold the DMA time of the newest frame the copy queue has completed into the
// running mean. gfence's completed value is the frame whose slot has landed, so
// that slot's two stamps have resolved. Each fence value is sampled once.
void dma_sample(ID3D12Fence *gfence)
{
    if (g_tsrb_cpu == nullptr || gfence == nullptr) return;
    const unsigned long long done = (unsigned long long)gfence->GetCompletedValue();
    if (done == 0ull || done <= g_sampled) return;
    g_sampled = done;
    const unsigned k = (unsigned)((done - 1ull) % g_ring);
    const UINT64 a = g_tsrb_cpu[k * 2u], b = g_tsrb_cpu[k * 2u + 1u];
    if (g_cts_freq == 0ull || b <= a) return;
    const double ms = (double)(b - a) * 1000.0 / (double)g_cts_freq;
    if (ms > 100.0) return;   // a stale or unwritten pair
    g_dma_sum += ms; ++g_dma_n;
    if (ms > g_dma_max) g_dma_max = ms;
}
} // namespace

bool arm(ID3D12Device *gdev, ID3D12CommandQueue *gq, ID3D12Resource *gxfer,
         unsigned long long heap_bytes, unsigned long long slot_bytes, unsigned ring)
{
    free_all();
    g_mode = read_key();
    if (g_mode == 0) return false;   // key absent / 0: the module does not exist
    char line[700];
    if (gdev == nullptr || gq == nullptr || gxfer == nullptr || ring == 0u || ring > MAX_RING ||
        slot_bytes == 0ull || heap_bytes < slot_bytes * ring)
    {
        mgpu::diag::warn("[MGPU][R333] StageCopy enabled but the arm handed no usable heap/ring - OFF for this run.");
        free_all();
        return false;
    }
    g_ring = ring; g_slot_bytes = slot_bytes; g_gxfer = gxfer; g_dev = gdev;

    HRESULT h = S_OK;
    if (g_mode == 1)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;   // GPU 0 VRAM: the fast write the game's list makes
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = heap_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        h = gdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&g_stage));
    }
    if (SUCCEEDED(h))
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        h = gdev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_cq));
    }
    if (SUCCEEDED(h)) h = gdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_ca));
    if (SUCCEEDED(h)) h = gdev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_sf));

    // Copy-queue timestamps are optional; their absence only loses the DMA ms.
    if (SUCCEEDED(h))
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
        HRESULT th = gdev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof o3);
        if (SUCCEEDED(th) && !o3.CopyQueueTimestampQueriesSupported) th = E_NOTIMPL;
        if (SUCCEEDED(th))
        {
            D3D12_QUERY_HEAP_DESC qh{};
            qh.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
            qh.Count = ring * 2u;
            th = gdev->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_cts));
        }
        if (SUCCEEDED(th)) th = make_buf(gdev, (UINT64)ring * 16ull, D3D12_HEAP_TYPE_READBACK, &g_tsrb);
        if (SUCCEEDED(th))
        {
            void *p = nullptr;
            th = g_tsrb->Map(0, nullptr, &p);
            if (SUCCEEDED(th) && p != nullptr) { memset(p, 0, (size_t)ring * 16u); g_tsrb_cpu = (const UINT64 *)p; }
            else th = E_FAIL;
        }
        if (SUCCEEDED(th)) th = g_cq->GetTimestampFrequency(&g_cts_freq);
        if (FAILED(th) || g_cts_freq == 0ull)
        {
            if (g_cts != nullptr) { g_cts->Release(); g_cts = nullptr; }
            if (g_tsrb != nullptr) { if (g_tsrb_cpu != nullptr) g_tsrb->Unmap(0, nullptr); g_tsrb->Release(); g_tsrb = nullptr; }
            g_tsrb_cpu = nullptr; g_cts_freq = 0ull;
        }
    }

    if (g_mode == 1)
    {
        // Original full-slot path: stage slot k -> gxfer slot k.
        for (unsigned k = 0; k < ring && SUCCEEDED(h); ++k)
        {
            h = gdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_ca, nullptr, IID_PPV_ARGS(&g_cl[k]));
            if (FAILED(h)) break;
            const UINT64 off = (UINT64)k * slot_bytes;
            if (g_cts != nullptr) g_cl[k]->EndQuery(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u);
            g_cl[k]->CopyBufferRegion(gxfer, off, g_stage, off, slot_bytes);
            if (g_cts != nullptr)
            {
                g_cl[k]->EndQuery(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u + 1u);
                g_cl[k]->ResolveQueryData(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u, 2, g_tsrb, (UINT64)k * 16ull);
            }
            h = g_cl[k]->Close();
        }
    }

    if (FAILED(h))
    {
        snprintf(line, sizeof line,
                 "[MGPU][R333] StageCopy FAILED TO COME UP hr=0x%08X - OFF for this run; the game's queue "
                 "copies straight to the shared heap as the shipped build does.", (unsigned)h);
        mgpu::diag::error(line);
        free_all();
        return false;
    }
    if (g_mode == 1)
    {
        snprintf(line, sizeof line,
                 "[MGPU][R333] StageCopy=1 UP (original full-slot path): %u slots of %llu bytes, DMA timing %s.",
                 ring, slot_bytes, (g_cts != nullptr) ? "ON" : "OFF");
    }
    else
    {
        snprintf(line, sizeof line,
                 "[MGPU][R333] StageCopy=2 ARMED (colour-only texture staging): depth, motion vectors and seal "
                 "remain on the direct path; colour COPY lists are built when the first colour footprint arrives; "
                 "DMA timing %s.", (g_cts != nullptr) ? "ON" : "OFF");
    }
    mgpu::diag::info(line);
    g_up.store(true, std::memory_order_release);
    return true;
}

bool active()
{
    return g_up.load(std::memory_order_relaxed);
}

ID3D12Resource *dst(ID3D12Resource *gxfer)
{
    return (g_up.load(std::memory_order_relaxed) && g_mode == 1 && g_stage != nullptr) ? g_stage : gxfer;
}

void colour_fallback(const char *what, HRESULT h)
{
    for (unsigned k = 0; k < MAX_RING; ++k)
    {
        if (g_colour[k] != nullptr) { g_colour[k]->Release(); g_colour[k] = nullptr; }
        if (g_mode == 2 && g_cl[k] != nullptr) { g_cl[k]->Release(); g_cl[k] = nullptr; }
    }
    g_colour_lists_built = false;
    g_colour_direct = true;
    char line[420];
    snprintf(line, sizeof line,
             "[MGPU][R333] StageCopy=2 colour fallback at %s hr=0x%08X: colour and all other lanes now copy "
             "directly to the shared heap; no staged slot is submitted.", what, (unsigned)h);
    mgpu::diag::error(line);
}

bool build_colour_lists()
{
    if (g_colour_lists_built) return true;
    if (g_mode != 2 || g_colour_direct || g_dev == nullptr || g_gxfer == nullptr || g_ca == nullptr)
        return false;

    HRESULT h = g_ca->Reset();
    for (unsigned k = 0; k < g_ring && SUCCEEDED(h); ++k)
    {
        if (g_colour[k] == nullptr) { h = E_POINTER; break; }
        if (g_cl[k] == nullptr)
            h = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_ca, nullptr, IID_PPV_ARGS(&g_cl[k]));
        else
            h = g_cl[k]->Reset(g_ca, nullptr);
        if (FAILED(h)) break;

        if (g_cts != nullptr) g_cl[k]->EndQuery(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_colour[k];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_cl[k]->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION cs{}, cd{};
        cs.pResource = g_colour[k];
        cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.SubresourceIndex = 0;
        cd.pResource = g_gxfer;
        cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        cd.PlacedFootprint = g_colour_fp;
        cd.PlacedFootprint.Offset = (UINT64)k * g_slot_bytes + g_colour_relative;
        g_cl[k]->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_cl[k]->ResourceBarrier(1, &b);
        if (g_cts != nullptr)
        {
            g_cl[k]->EndQuery(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u + 1u);
            g_cl[k]->ResolveQueryData(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u, 2, g_tsrb, (UINT64)k * 16ull);
        }
        h = g_cl[k]->Close();
    }
    if (FAILED(h))
    {
        colour_fallback("COPY-list build", h);
        return false;
    }
    g_colour_lists_built = true;
    mgpu::diag::info("[MGPU][R333] StageCopy=2 colour COPY lists UP: one texture footprint per ring slot; "
                     "the game queue writes local VRAM and the COPY queue expands colour into gxfer.");
    return true;
}

bool copy_colour(ID3D12GraphicsCommandList *gl,
                 const D3D12_TEXTURE_COPY_LOCATION &src,
                 const D3D12_TEXTURE_COPY_LOCATION &dst)
{
    if (g_mode != 2 || !active() || g_colour_direct || gl == nullptr ||
        src.pResource == nullptr || dst.pResource != g_gxfer ||
        src.Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        dst.Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
        return false;

    const UINT64 off = dst.PlacedFootprint.Offset;
    if (!g_colour_lists_built)
    {
        if (g_colour_direct || off == 0ull || g_slot_bytes == 0ull)
        {
            colour_fallback("first footprint validation", E_INVALIDARG);
            return false;
        }
        const UINT64 relative = off % g_slot_bytes;
        const UINT64 slot_off = off - relative;
        if (relative >= g_slot_bytes || slot_off / g_slot_bytes >= g_ring)
        {
            colour_fallback("first footprint range", E_INVALIDARG);
            return false;
        }

        g_colour_desc = src.pResource->GetDesc();
        g_colour_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        g_colour_fp = dst.PlacedFootprint;
        g_colour_fp.Offset = 0;
        g_colour_relative = relative;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT h = S_OK;
        for (unsigned k = 0; k < g_ring && SUCCEEDED(h); ++k)
        {
            h = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &g_colour_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&g_colour[k]));
        }
        if (FAILED(h))
        {
            colour_fallback("staging texture allocation", h);
            return false;
        }
        if (!build_colour_lists()) return false;
    }

    const unsigned k = (unsigned)((off - g_colour_relative) / g_slot_bytes);
    if (k >= g_ring || g_colour[k] == nullptr) return false;
    D3D12_TEXTURE_COPY_LOCATION staged = dst;
    staged.pResource = g_colour[k];
    staged.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    staged.SubresourceIndex = 0;
    staged.PlacedFootprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
    gl->CopyTextureRegion(&staged, 0, 0, 0, &src, nullptr);
    return true;
}

void depth_fallback(const char *what, HRESULT h)
{
    for (unsigned k = 0; k < MAX_RING; ++k)
    {
        if (g_depth_cl[k] != nullptr) { g_depth_cl[k]->Release(); g_depth_cl[k] = nullptr; }
        if (g_depth[k] != nullptr) { g_depth[k]->Release(); g_depth[k] = nullptr; }
    }
    g_depth_lists_built = false;
    g_depth_direct = true;
    char line[420];
    snprintf(line, sizeof line,
             "[MGPU][R333] StageCopy=2 depth fallback at %s hr=0x%08X: depth returns to the direct path; "
             "the already-recorded colour lane remains staged and valid.", what, (unsigned)h);
    mgpu::diag::error(line);
}

bool build_depth_lists()
{
    if (g_depth_lists_built) return true;
    if (g_mode != 2 || g_depth_direct || g_dev == nullptr || g_gxfer == nullptr || g_ca == nullptr)
        return false;

    // Do not reset g_ca here: the colour lists for this first submission have
    // already been recorded from it. A separate list per depth slot is enough.
    HRESULT h = S_OK;
    for (unsigned k = 0; k < g_ring && SUCCEEDED(h); ++k)
    {
        if (g_depth[k] == nullptr) { h = E_POINTER; break; }
        h = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_ca, nullptr,
                                     IID_PPV_ARGS(&g_depth_cl[k]));
        if (FAILED(h)) break;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_depth[k];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_depth_cl[k]->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION ds{}, dt{};
        ds.pResource = g_depth[k];
        ds.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        ds.SubresourceIndex = 0;
        dt.pResource = g_gxfer;
        dt.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dt.PlacedFootprint = g_depth_fp;
        dt.PlacedFootprint.Offset = (UINT64)k * g_slot_bytes + g_depth_relative;
        g_depth_cl[k]->CopyTextureRegion(&dt, 0, 0, 0, &ds, nullptr);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_depth_cl[k]->ResourceBarrier(1, &b);
        h = g_depth_cl[k]->Close();
    }
    if (FAILED(h))
    {
        depth_fallback("COPY-list build", h);
        return false;
    }
    g_depth_lists_built = true;
    mgpu::diag::info("[MGPU][R333] StageCopy=2 depth COPY lists UP: colour and depth now use independent "
                     "local-texture staging lanes; motion vectors and seal remain direct.");
    return true;
}

bool copy_depth(ID3D12GraphicsCommandList *gl,
                const D3D12_TEXTURE_COPY_LOCATION &src,
                const D3D12_TEXTURE_COPY_LOCATION &dst)
{
    if (g_mode != 2 || !active() || g_colour_direct || !g_colour_lists_built || g_depth_direct ||
        gl == nullptr || src.pResource == nullptr || dst.pResource != g_gxfer ||
        src.Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        dst.Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
        return false;

    const UINT64 off = dst.PlacedFootprint.Offset;
    if (!g_depth_lists_built)
    {
        if (off == 0ull || g_slot_bytes == 0ull)
        {
            depth_fallback("first footprint validation", E_INVALIDARG);
            return false;
        }
        const UINT64 relative = off % g_slot_bytes;
        const UINT64 slot_off = off - relative;
        if (relative >= g_slot_bytes || slot_off / g_slot_bytes >= g_ring)
        {
            depth_fallback("first footprint range", E_INVALIDARG);
            return false;
        }

        g_depth_desc = src.pResource->GetDesc();
        g_depth_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        g_depth_fp = dst.PlacedFootprint;
        g_depth_fp.Offset = 0;
        g_depth_relative = relative;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT h = S_OK;
        for (unsigned k = 0; k < g_ring && SUCCEEDED(h); ++k)
        {
            h = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &g_depth_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&g_depth[k]));
        }
        if (FAILED(h))
        {
            depth_fallback("staging texture allocation", h);
            return false;
        }
        if (!build_depth_lists()) return false;
    }

    if (off < g_depth_relative) return false;
    const unsigned k = (unsigned)((off - g_depth_relative) / g_slot_bytes);
    if (k >= g_ring || g_depth[k] == nullptr) return false;
    D3D12_TEXTURE_COPY_LOCATION staged = dst;
    staged.pResource = g_depth[k];
    staged.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    staged.SubresourceIndex = 0;
    staged.PlacedFootprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
    gl->CopyTextureRegion(&staged, 0, 0, 0, &src, nullptr);
    return true;
}

bool copy_mvec(ID3D12GraphicsCommandList *gl,
               const D3D12_TEXTURE_COPY_LOCATION &src,
               const D3D12_TEXTURE_COPY_LOCATION &dst)
{
    // This source already keeps motion vectors on the direct path (see the
    // depth-lane contract above). Returning false makes the caller record that
    // direct copy; do not pretend the unused staging members are a live lane.
    (void)gl; (void)src; (void)dst;
    return false;
}

// StageCopy off: exactly the old call.
// StageCopy on: the game's queue signals g_sf (its copies into the staging ring are
// done), the copy queue waits on that, moves every slot not yet moved up to f into
// gxfer, and signals gfence. Then the game's queue is told to wait - ON THE GPU -
// for gfence f+2-RING, so the slot its next frame writes has been moved out.
bool signal(ID3D12CommandQueue *gq, ID3D12Fence *gfence, unsigned long long f)
{
    if (!active() || g_cq == nullptr || g_sf == nullptr)
        return SUCCEEDED(gq->Signal(gfence, f));
    // A first-frame colour setup failure happens before any staged slot is
    // handed over. Keep every lane direct instead of executing empty lists.
    if (g_mode == 2 && (g_colour_direct || !g_colour_lists_built))
        return SUCCEEDED(gq->Signal(gfence, f));
    if (f <= g_last) return false;   // already handed over

    unsigned long long from = g_last + 1ull;
    if (f - from + 1ull > (unsigned long long)g_ring) from = f + 1ull - g_ring;   // older slots already overwritten

    if (FAILED(gq->Signal(g_sf, f)))
    {
        // Nothing reaches gxfer for these frames. Signalling gfence anyway keeps GPU 1
        // from waiting forever; the seal checker will see stale slots and say so.
        ++g_fail;
        g_last = f;
        return SUCCEEDED(gq->Signal(gfence, f));
    }
    if (FAILED(g_cq->Wait(g_sf, f))) ++g_fail;
    ID3D12CommandList *ls[MAX_RING * 2u];
    UINT n = 0;
    for (unsigned long long k = from; k <= f; ++k)
    {
        const unsigned slot = (unsigned)((k - 1ull) % g_ring);
        ls[n++] = g_cl[slot];
        if (g_mode == 2 && g_depth_lists_built && !g_depth_direct)
            ls[n++] = g_depth_cl[slot];
    }
    g_cq->ExecuteCommandLists(n, ls);
    const bool ok = SUCCEEDED(g_cq->Signal(gfence, f));
    if (!ok) ++g_fail;
    g_last = f;
    g_submits += f - from + 1ull;

    if (f + 2ull > (unsigned long long)g_ring)
    {
        // Back-pressure, Dave's: a queue wait, never a thread wait. Counted when the
        // fence has not already passed the value (the DMA is RING-2 frames behind).
        const unsigned long long need = f + 2ull - g_ring;
        if ((unsigned long long)gfence->GetCompletedValue() < need) ++g_bp_waits;
        if (FAILED(gq->Wait(gfence, need))) ++g_fail;
    }

    dma_sample(gfence);
    if (f >= g_report_at)
    {
        g_report_at = f + 1200ull;
        char line[520];
        snprintf(line, sizeof line,
                 "[MGPU][R333] StageCopy f=%llu: slots staged %llu | failures %llu | back-pressure waits armed "
                 "behind the DMA %llu | staging DMA %s%.2f ms mean, %.2f ms max over %u sampled slots (off the "
                 "game's queue; -1 = not measured). In-frame GPU 0 copy ms: Gpu0Timers=1.",
                 f, g_submits, g_fail, g_bp_waits,
                 (g_dma_n != 0u) ? "" : "n/a ", (g_dma_n != 0u) ? g_dma_sum / (double)g_dma_n : -1.0,
                 (g_dma_n != 0u) ? g_dma_max : -1.0, g_dma_n);
        mgpu::diag::info(line);
        g_dma_sum = g_dma_max = 0.0; g_dma_n = 0u;
    }
    return ok;
}

void release()
{
    if (g_cq != nullptr)
    {
        // Drain: the copy queue writes gxfer, which the caller frees next. Bounded.
        ID3D12Fence *df = nullptr;
        ID3D12Device *dev = nullptr;
        if (SUCCEEDED(g_cq->GetDevice(IID_PPV_ARGS(&dev))) && dev != nullptr &&
            SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&df))) &&
            SUCCEEDED(g_cq->Signal(df, 1)) && df->GetCompletedValue() < 1)
        {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev != nullptr)
            {
                if (SUCCEEDED(df->SetEventOnCompletion(1, ev)))
                {
                    if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0)
                        mgpu::diag::warn("[MGPU][R333] release: the GPU 0 copy queue did not drain in 2 s; "
                                         "freeing anyway (the stream's own drain already passed gfence).");
                }
                CloseHandle(ev);
            }
        }
        if (df != nullptr) df->Release();
        if (dev != nullptr) dev->Release();
        char line[300];
        snprintf(line, sizeof line, "[MGPU][R333] StageCopy released: %llu slots staged, %llu failures, %llu back-pressure waits.",
                 g_submits, g_fail, g_bp_waits);
        mgpu::diag::info(line);
    }
    free_all();
    g_submits = g_fail = g_bp_waits = 0ull;
}
}
