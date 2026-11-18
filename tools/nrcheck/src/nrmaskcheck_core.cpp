// Standalone DLSSNR.ControlMask A/B test. No game or game file is touched.
//
// A mask controls the neural evaluation; it is NOT an instruction to replace
// masked pixels with the source image. Every case below reads the NR output
// directly. No input/output compositing is performed by this test.
//
// Fixture aligned with dlssnr-studio/src/nr_runtime.cpp at commit a5ad2258:
// RGBA16F colour/motion/output, R32 depth/mask, full resource subrects,
// SRV inputs, complete dimensions, scaling and performance parameters.
// The split/patch masks are synthetic API probes, NOT character detectors.
#define NOMINMAX
#include "nrcheck_core.cpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
uint16_t float_to_half(float f)
{
    uint32_t x;
    memcpy(&x, &f, sizeof x);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int e = int((x >> 23) & 0xffu) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0)
    {
        if (e < -10) return uint16_t(sign);
        m |= 0x800000u;
        const unsigned shift = unsigned(14 - e);
        uint32_t h = m >> shift;
        if ((m >> (shift - 1)) & 1u) ++h;
        return uint16_t(sign | h);
    }
    if (e >= 31) return uint16_t(sign | 0x7c00u);
    uint32_t h = sign | (uint32_t(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;
    return uint16_t(h);
}

float half_to_float(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 31u, m = h & 1023u, bits;
    if (e == 0)
    {
        if (!m) bits = sign;
        else
        {
            int exponent = -14;
            while (!(m & 0x400u)) { m <<= 1; --exponent; }
            m &= 0x3ffu;
            bits = sign | (uint32_t(exponent + 127) << 23) | (m << 13);
        }
    }
    else if (e == 31) bits = sign | 0x7f800000u | (m << 13);
    else bits = sign | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

struct MaskStats
{
    uint64_t different = 0;
    double mean = 0, maximum = 0;
};

struct ImageStats
{
    bool finite = true, non_black = false, varying = false;
    double mean_absolute = 0;
};

MaskStats compare_rgb(const std::vector<uint16_t> &a, const std::vector<uint16_t> &b,
                      int x0 = -1, int x1 = -1)
{
    MaskStats s;
    uint64_t sum = 0, count = 0;
    for (size_t i = 0; i < a.size(); i += 4)
    {
        const int x = int((i / 4) % 256);
        if (x0 >= 0 && (x < x0 || x >= x1)) continue;
        for (unsigned c = 0; c < 3; ++c)
        {
            const double d = std::fabs(double(half_to_float(a[i + c])) -
                                       double(half_to_float(b[i + c])));
            if (d > 1e-6) ++s.different;
            s.maximum = std::max(s.maximum, d);
            sum += uint64_t(d * 1000000.0);
            ++count;
        }
    }
    s.mean = count ? double(sum) / (double(count) * 1000000.0) : 0;
    return s;
}

ImageStats inspect_rgb(const std::vector<uint16_t> &pixels)
{
    ImageStats s;
    double sum = 0;
    const float first = half_to_float(pixels[0]);
    for (size_t i = 0; i < pixels.size(); i += 4)
        for (unsigned c = 0; c < 3; ++c)
        {
            const float v = half_to_float(pixels[i + c]);
            if (!std::isfinite(v)) s.finite = false;
            if (std::fabs(v) > 1e-4f) s.non_black = true;
            if (std::fabs(v - first) > 1e-4f) s.varying = true;
            sum += std::fabs(double(v));
        }
    s.mean_absolute = sum / double(pixels.size() / 4 * 3);
    return s;
}

bool submit_wait(ID3D12GraphicsCommandList *cl, ID3D12CommandQueue *q,
                 ID3D12Fence *fence, HANDLE event, UINT64 value)
{
    if (FAILED(cl->Close())) return false;
    ID3D12CommandList *lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    if (FAILED(q->Signal(fence, value))) return false;
    if (fence->GetCompletedValue() < value)
    {
        if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) return false;
    }
    return true;
}

void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}
}

extern "C" __declspec(dllexport)
int nrmask_run(int neural_index, int other_index, const wchar_t *snippet_path, const wchar_t *log_path)
{
    if (log_path && *log_path) _wfopen_s(&g_out, log_path, L"wb");
    constexpr unsigned W = 256, H = 256;
    out("CONTROL MASK A/B: %ux%u; every result is a complete DIRECT NR output", W, H);
    out("No source-image replacement. Synthetic split/patch probes are not character masks.");
    ID3D12Device *other = other_index >= 0 ? make_device(unsigned(other_index), "other") : nullptr;
    ID3D12Device *dev = make_device(unsigned(neural_index), "neural");
    if (!dev) { out("RESULT: SKIPPED (neural device)"); return 3; }
    HMODULE core = load_core();
    HMODULE snip = LoadLibraryW(snippet_path);
    if (!core || !snip) { out("RESULT: SKIPPED (NGX modules)"); return 3; }
    ngx_api n;
    const char *w1, *w2, *w3, *w4, *w5, *w6;
    n.init = (pf_init)pick(core, snip, "NVSDK_NGX_D3D12_Init", &w1, "core", "snippet");
    n.caps = (pf_get_caps)pick(core, snip, "NVSDK_NGX_D3D12_GetCapabilityParameters", &w2, "core", "snippet");
    n.destroy = (pf_destroy)pick(core, snip, "NVSDK_NGX_D3D12_DestroyParameters", &w3, "core", "snippet");
    n.create = (pf_create)pick(snip, core, "NVSDK_NGX_D3D12_CreateFeature", &w4, "snippet", "core");
    n.release = (pf_release)pick(snip, core, "NVSDK_NGX_D3D12_ReleaseFeature", &w5, "snippet", "core");
    auto evaluate = (pf_evaluate)pick(snip, core, "NVSDK_NGX_D3D12_EvaluateFeature", &w6, "snippet", "core");
    auto core_iext = (pf_init_ext)GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext");
    n.s_iext = (pf_init_ext)GetProcAddress(snip, "NVSDK_NGX_D3D12_Init_Ext");
    n.s_pop = (pf_populate)GetProcAddress(snip, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    using pf_scratch = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Feature,
                                                       const NVSDK_NGX_Parameter *, size_t *);
    auto get_scratch = (pf_scratch)GetProcAddress(snip, "NVSDK_NGX_D3D12_GetScratchBufferSize");
    if (!n.caps || !n.create || !evaluate) return 3;
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&nrmask_run, &self);
        GetModuleFileNameW(self, n.data_path, MAX_PATH);
        wchar_t *slash = wcsrchr(n.data_path, L'\\');
        if (slash) slash[1] = 0;
    }
    n.common.LoggingInfo.LoggingCallback = ngx_cb;
    n.common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    NVSDK_NGX_Result r = core_iext
        ? core_iext(0x4D475055ULL, n.data_path, dev, NVSDK_NGX_Version_API, nullptr)
        : n.init(0ULL, n.data_path, dev, &n.common, NVSDK_NGX_Version_API);
    out("Core initialization: 0x%08X (%s)", unsigned(r), rname(r));
    NVSDK_NGX_Parameter *p = nullptr;
    r = n.caps(&p);
    out("GetCapabilityParameters: 0x%08X (%s)", unsigned(r), rname(r));
    if (r != NVSDK_NGX_Result_Success || !p) return 3;
    if (n.s_iext) out("Snippet Init_Ext: 0x%08X", unsigned(n.s_iext(0ULL, n.data_path, dev, NVSDK_NGX_Version_API, p)));
    if (n.s_pop) out("Snippet Populate: 0x%08X", unsigned(n.s_pop(p)));

    ID3D12CommandQueue *q = nullptr;
    ID3D12CommandAllocator *al = nullptr;
    ID3D12GraphicsCommandList *cl = nullptr;
    ID3D12Fence *fence = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 3;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 3;
    const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource *color = make_tex2d(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *mvec = make_tex2d(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *depth = make_tex2d(dev, W, H, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *mask = make_tex2d(dev, W, H, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *output = make_tex2d(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!color || !mvec || !depth || !mask || !output) return 3;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fc{}, fm{}, fd{}, fk{}, fr{};
    UINT rows = 0; UINT64 rowbytes = 0, sc = 0, sm = 0, sd = 0, sk = 0, sr = 0;
    auto dc = color->GetDesc(), dm = mvec->GetDesc(), dd = depth->GetDesc(), dk = mask->GetDesc(), dr = output->GetDesc();
    dev->GetCopyableFootprints(&dc, 0, 1, 0, &fc, &rows, &rowbytes, &sc);
    dev->GetCopyableFootprints(&dm, 0, 1, 0, &fm, &rows, &rowbytes, &sm);
    dev->GetCopyableFootprints(&dd, 0, 1, 0, &fd, &rows, &rowbytes, &sd);
    dev->GetCopyableFootprints(&dk, 0, 1, 0, &fk, &rows, &rowbytes, &sk);
    dev->GetCopyableFootprints(&dr, 0, 1, 0, &fr, &rows, &rowbytes, &sr);
    ID3D12Resource *uc = make_buffer(dev, sc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *um = make_buffer(dev, sm, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ud = make_buffer(dev, sd, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *uk = make_buffer(dev, sk, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *readback = make_buffer(dev, sr, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!uc || !um || !ud || !uk || !readback) return 3;
    std::vector<uint16_t> input(W * H * 4);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x)
        {
            const float nx = (float(x) - 128.0f) / 128.0f, ny = (float(y) - 128.0f) / 128.0f;
            const bool figure = nx * nx / 0.15f + ny * ny / 0.65f < 1.0f;
            const size_t i = (size_t(y) * W + x) * 4;
            input[i + 0] = float_to_half(figure ? 0.68f : 0.10f + 0.18f * x / W);
            input[i + 1] = float_to_half(figure ? 0.43f : 0.18f + 0.15f * y / H);
            input[i + 2] = float_to_half(figure ? 0.31f : 0.26f + 0.20f * (x + y) / (W + H));
            input[i + 3] = float_to_half(1.0f);
        }
    unsigned char *mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(uc->Map(0, &none, (void **)&mapped))) return 3;
    for (unsigned y = 0; y < H; ++y)
        memcpy(mapped + fc.Offset + size_t(y) * fc.Footprint.RowPitch, input.data() + size_t(y) * W * 4, W * 8);
    uc->Unmap(0, nullptr);
    if (FAILED(um->Map(0, &none, (void **)&mapped))) return 3;
    memset(mapped, 0, size_t(sm)); um->Unmap(0, nullptr);
    if (FAILED(ud->Map(0, &none, (void **)&mapped))) return 3;
    for (unsigned y = 0; y < H; ++y)
    {
        float *row = (float *)(mapped + fd.Offset + size_t(y) * fd.Footprint.RowPitch);
        for (unsigned x = 0; x < W; ++x) row[x] = 0.35f + 0.30f * float(y) / H;
    }
    ud->Unmap(0, nullptr);
    auto upload = [&](ID3D12Resource *dst, ID3D12Resource *src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp) {
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.pResource = dst;
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.pResource = src; s.PlacedFootprint = fp;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    };
    upload(color, uc, fc); upload(mvec, um, fm); upload(depth, ud, fd);
    transition(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, srv);
    transition(cl, mvec, D3D12_RESOURCE_STATE_COPY_DEST, srv);
    transition(cl, depth, D3D12_RESOURCE_STATE_COPY_DEST, srv);
    UINT64 serial = 1;
    if (!submit_wait(cl, q, fence, event, serial++)) return 3;

    auto set_dims = [&] {
        p->Set(NVSDK_NGX_Parameter_Width, W); p->Set(NVSDK_NGX_Parameter_Height, H);
        p->Set("CreationNodeMask", 1u); p->Set("VisibilityNodeMask", 1u);
        p->Set("DLSSNR.InputWidth", W); p->Set("DLSSNR.InputHeight", H);
        p->Set("DLSSNR.Width", W); p->Set("DLSSNR.Height", H);
        p->Set("DLSSNR.OutputWidth", W); p->Set("DLSSNR.OutputHeight", H);
        p->Set("DLSSNR.Output.Width", W); p->Set("DLSSNR.Output.Height", H);
        p->Set("DLSSNR.Enabled", 1); p->Set("DLSSNR.Upscaling", 0);
        p->Set("DLSSNR.Scale", 1.0f); p->Set("DLSSNR.ScalingRatio", 1.0f);
        p->Set("PerfQualityValue", 3u); p->Set("DLSSNR.PerfQualityValue", 3u);
        p->Set("DLSSNR.Hint.Render.Preset", 0u);
    };
    auto set_subrect = [&](const char *prefix) {
        char name[96];
        for (const char *suffix : {"BaseX", "BaseY"})
        { snprintf(name, sizeof name, "DLSSNR.%sSubrect%s", prefix, suffix); p->Set(name, 0u); }
        snprintf(name, sizeof name, "DLSSNR.%sSubrectWidth", prefix); p->Set(name, W);
        snprintf(name, sizeof name, "DLSSNR.%sSubrectHeight", prefix); p->Set(name, H);
    };
    auto bind_evaluate = [&] {
        set_dims();
        p->Set("DLSSNR.Color", color); p->Set("DLSSNR.MVec", mvec);
        p->Set("DLSSNR.Depth", depth); p->Set("DLSSNR.Output", output);
        p->Set("DLSSNR.ControlMask", mask);
        for (const char *key : {"Color", "Depth", "MVec", "Output", "ControlMask"}) set_subrect(key);
        p->Set("DLSSNR.MVecScaleX", 1.0f); p->Set("DLSSNR.MVecScaleY", 1.0f);
        p->Set("DLSSNR.DepthInverted", 0u); p->Set("DLSSNR.Reset", 1u);
        p->Set("DLSSNR.Intensity", 1.0f); p->Set("DLSSNR.LocalToneStrength", 1.0f);
        p->Set("DLSSNR.LocalStructureStrength", 1.0f); p->Set("DLSSNR.GlobalToneStrength", 1.0f);
        p->Set("DLSSNR.SkinStructureStrength", 1.0f); p->Set("DLSSNR.Style", 2.0f);
        p->Set("DLSSNR.UseAutoMask", 0u); p->Set("DLSSNR.UICorrection", 0u);
    };
    ID3D12Resource *scratch = nullptr;
    if (get_scratch)
    {
        set_dims();
        size_t bytes = 0;
        r = get_scratch(NVSDK_NGX_Feature_Reserved18, p, &bytes);
        out("GetScratchBufferSize: 0x%08X, %zu bytes", unsigned(r), bytes);
        if (r == NVSDK_NGX_Result_Success && bytes)
        {
            scratch = make_buffer(dev, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!scratch) return 3;
            p->Set("Scratch", scratch); p->Set("Scratch.SizeInBytes", unsigned(bytes));
        }
    }

    constexpr int kCases = 4;
    std::vector<uint16_t> pixels[kCases];
    bool valid[kCases] = {};
    for (int test = 0; test < kCases; ++test)
    {
        const char *names[] = {"uniform 0 / AutoMask=0", "uniform 1 / AutoMask=0",
                               "left 0, right 1 / AutoMask=0", "centre patch 0 on 1 / AutoMask=0"};
        if (FAILED(uk->Map(0, &none, (void **)&mapped))) return 3;
        for (unsigned y = 0; y < H; ++y)
        {
            float *row = (float *)(mapped + fk.Offset + size_t(y) * fk.Footprint.RowPitch);
            for (unsigned x = 0; x < W; ++x)
                row[x] = test == 0 ? 0.0f : test == 1 ? 1.0f :
                         test == 2 ? (x < W / 2 ? 0.0f : 1.0f) :
                         (x >= 80 && x < 176 && y >= 72 && y < 184 ? 0.0f : 1.0f);
        }
        uk->Unmap(0, nullptr);
        if (FAILED(al->Reset()) || FAILED(cl->Reset(al, nullptr))) return 3;
        if (test) transition(cl, mask, srv, D3D12_RESOURCE_STATE_COPY_DEST);
        upload(mask, uk, fk);
        transition(cl, mask, D3D12_RESOURCE_STATE_COPY_DEST, srv);
        set_dims();
        NVSDK_NGX_Handle *handle = nullptr;
        unsigned long seh = 0;
        r = create_guarded(n.create, cl, p, &handle, &seh);
        out("%s: CreateFeature 0x%08X (%s)", names[test], unsigned(r), rname(r));
        if (unsigned(r) == SEH_FAULT) return 2;
        if (r != NVSDK_NGX_Result_Success || !handle) return 4;
        if (!submit_wait(cl, q, fence, event, serial++)) return 5;
        if (FAILED(al->Reset()) || FAILED(cl->Reset(al, nullptr))) return 5;
        bind_evaluate();
        r = evaluate_guarded(evaluate, cl, handle, p, &seh);
        out("%s: EvaluateFeature 0x%08X (%s), exception 0x%08lX", names[test], unsigned(r), rname(r), seh);
        if (unsigned(r) == SEH_FAULT) return 2;
        if (r != NVSDK_NGX_Result_Success) return 5;
        transition(cl, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.pResource = readback; d.PlacedFootprint = fr;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.pResource = output;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        if (!submit_wait(cl, q, fence, event, serial++)) return 5;
        D3D12_RANGE read{0, SIZE_T(sr)};
        if (FAILED(readback->Map(0, &read, (void **)&mapped))) return 5;
        pixels[test].resize(W * H * 4);
        for (unsigned y = 0; y < H; ++y)
            memcpy(pixels[test].data() + size_t(y) * W * 4,
                   mapped + fr.Offset + size_t(y) * fr.Footprint.RowPitch, W * 8);
        readback->Unmap(0, &none);
        n.release(handle);
        const ImageStats image = inspect_rgb(pixels[test]);
        valid[test] = image.finite && image.non_black && image.varying;
        out("%s: complete output finite=%d nonblack=%d varying=%d mean|RGB|=%.6f",
            names[test], image.finite, image.non_black, image.varying, image.mean_absolute);
        transition(cl, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const MaskStats delta = compare_rgb(pixels[0], pixels[1]);
    const MaskStats split0 = compare_rgb(pixels[2], pixels[0]);
    const MaskStats split1 = compare_rgb(pixels[2], pixels[1]);
    const MaskStats patch_in = compare_rgb(pixels[3], pixels[1], 80, 176);
    const MaskStats patch_left = compare_rgb(pixels[3], pixels[1], 0, 80);
    out("uniform 0 vs 1: mean %.6f, max %.6f, changed %llu", delta.mean, delta.maximum, delta.different);
    out("split vs uniform 0: mean %.6f, changed %llu", split0.mean, split0.different);
    out("split vs uniform 1: mean %.6f, changed %llu", split1.mean, split1.different);
    out("patch vs uniform 1, inside x[80,176): mean %.6f, changed %llu", patch_in.mean, patch_in.different);
    out("patch vs uniform 1, outside x[0,80): mean %.6f, changed %llu", patch_left.mean, patch_left.different);
    // Do not require a zero mask to equal the input: masked pixels still come
    // from the network. Likewise a spatial mask need not match two uniform
    // cases exactly at every pixel because the network has a spatial receptive
    // field. This test checks direct complete outputs and a measurable response
    // to both uniform and spatially varying control textures.
    const bool pass = valid[0] && valid[1] && valid[2] && valid[3] &&
                      delta.different && split0.different && split1.different && patch_in.different;
    out("RESULT: %s", pass ? "PASS - spatial control textures produce complete neural outputs"
                           : "FAIL - control-texture response or complete-output validation did not hold");
    (void)other;
    return pass ? 0 : 6;
}
