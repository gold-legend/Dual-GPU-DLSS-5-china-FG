#include "fg_pair_flow.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "diag.hpp"

// Original, independent block matcher. This is not NVIDIA Optical Flow SDK code
// and does not contain or adapt third-party optical-flow shader source.
namespace mgpu::fgpair
{
namespace
{
constexpr unsigned SLOTS = 4u;
constexpr unsigned DOWN_STRIDE = 4u;
constexpr unsigned FLOW_STRIDE = 3u;
constexpr unsigned DOWN_BASE = 0u;
constexpr unsigned MATCH_BASE = SLOTS * DOWN_STRIDE;
constexpr unsigned EXPAND_BASE = MATCH_BASE + SLOTS * 2u * FLOW_STRIDE;
constexpr unsigned DESCRIPTORS = EXPAND_BASE + SLOTS * 2u * FLOW_STRIDE;

ID3D12Device *g_dev = nullptr;
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_down_pso = nullptr, *g_match_pso = nullptr, *g_expand_pso = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_luma[2] = {};
ID3D12Resource *g_low[2] = {};
ID3D12Resource *g_full[SLOTS] = {};
UINT g_inc = 0;
unsigned g_w = 0, g_h = 0, g_lw = 0, g_lh = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
bool g_active = false, g_have_previous = false;
bool g_low_srv[2] = {}, g_full_srv[SLOTS] = {};
unsigned g_parity = 0;
unsigned long long g_pairs = 0, g_fallbacks = 0;

const char kHLSL[] = R"HLSL(
Texture2D<float4> gColor : register(t0);
Texture2D<float2> gMotion : register(t0);
Texture2D<float> gCur : register(t0);
Texture2D<float> gPrev : register(t1);
RWTexture2D<float> gLuma : register(u0);
RWTexture2D<float2> gFlow : register(u0);
SamplerState gLinear : register(s0);

cbuffer C : register(b0)
{
    uint2 gSrcDim;
    uint2 gDstDim;
    uint gValid;
    uint3 gPad;
    float2 gMotionScale;
    float2 gPad2;
};

[numthreads(8, 8, 1)]
void CSDown(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDstDim.x || id.y >= gDstDim.y) return;
    float2 uv = (float2(id.xy) + 0.5f) / float2(gDstDim);
    float2 px = 0.35f / float2(gSrcDim);
    float3 c = gColor.SampleLevel(gLinear, uv + float2(-px.x, -px.y), 0.0f).rgb;
    c += gColor.SampleLevel(gLinear, uv + float2(px.x, -px.y), 0.0f).rgb;
    c += gColor.SampleLevel(gLinear, uv + float2(-px.x, px.y), 0.0f).rgb;
    c += gColor.SampleLevel(gLinear, uv + float2(px.x, px.y), 0.0f).rgb;
    c *= 0.25f;
    gLuma[id.xy] = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float BlockCost(int2 p, int2 d)
{
    float sum = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 o = int2(x * 2, y * 2);
            int2 q = p + d + o;
            float cur = gCur.Load(int3(p + o, 0));
            float old = gPrev.Load(int3(clamp(q, int2(0, 0), int2(gSrcDim) - int2(1, 1)), 0));
            float outside = (q.x < 0 || q.y < 0 || q.x >= (int)gSrcDim.x || q.y >= (int)gSrcDim.y) ? 0.05f : 0.0f;
            sum += abs(cur - old) + outside;
        }
    }
    return sum * (1.0f / 9.0f);
}

float Subpixel(float minus, float centre, float plus)
{
    float den = minus + plus - 2.0f * centre;
    return abs(den) > 1.0e-6f ? clamp(0.5f * (minus - plus) / den, -0.5f, 0.5f) : 0.0f;
}

[numthreads(8, 8, 1)]
void CSMatch(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDstDim.x || id.y >= gDstDim.y) return;
    int2 p = int2(id.xy);
    float2 result = float2(0.0f, 0.0f);
    if (gValid != 0)
    {
        float mean = 0.0f;
        [unroll]
        for (int y = -1; y <= 1; ++y)
            [unroll]
            for (int x = -1; x <= 1; ++x)
                mean += gCur.Load(int3(p + int2(x * 2, y * 2), 0));
        mean *= (1.0f / 9.0f);
        float detail = 0.0f;
        [unroll]
        for (int y2 = -1; y2 <= 1; ++y2)
            [unroll]
            for (int x2 = -1; x2 <= 1; ++x2)
                detail += abs(gCur.Load(int3(p + int2(x2 * 2, y2 * 2), 0)) - mean);
        detail *= (1.0f / 9.0f);

        if (detail >= 0.004f)
        {
            float best = 1.0e9f;
            int2 bestD = int2(0, 0);
            int2 centre = int2(0, 0);
            [unroll]
            for (uint stage = 0; stage < 3; ++stage)
            {
                int step = stage == 0 ? 8 : (stage == 1 ? 3 : 1);
                [unroll]
                for (int oy = -1; oy <= 1; ++oy)
                {
                    [unroll]
                    for (int ox = -1; ox <= 1; ++ox)
                    {
                        int2 d = centre + int2(ox, oy) * step;
                        float cost = BlockCost(p, d);
                        if (cost < best) { best = cost; bestD = d; }
                    }
                }
                centre = bestD;
            }
            // A poor best match is normally a scene cut or disocclusion. Zero is
            // safer than injecting an unverified vector into DLSSG.
            if (best <= 0.075f)
            {
                float xm = BlockCost(p, bestD + int2(-1, 0));
                float xp = BlockCost(p, bestD + int2(1, 0));
                float ym = BlockCost(p, bestD + int2(0, -1));
                float yp = BlockCost(p, bestD + int2(0, 1));
                float2 fine = float2(Subpixel(xm, best, xp), Subpixel(ym, best, yp));
                result = float2(bestD) + fine;
            }
        }
    }
    gFlow[id.xy] = result;
}

[numthreads(8, 8, 1)]
void CSExpand(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDstDim.x || id.y >= gDstDim.y) return;
    float2 uv = (float2(id.xy) + 0.5f) / float2(gDstDim);
    gFlow[id.xy] = gMotion.SampleLevel(gLinear, uv, 0.0f) * gMotionScale;
}
)HLSL";

template <typename T> void rel(T *&p)
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

void reset()
{
    g_active = false;
    g_have_previous = false;
    g_parity = 0;
    g_pairs = g_fallbacks = 0;
    std::fill(std::begin(g_low_srv), std::end(g_low_srv), false);
    std::fill(std::begin(g_full_srv), std::end(g_full_srv), false);
    for (auto &p : g_full) rel(p);
    for (auto &p : g_low) rel(p);
    for (auto &p : g_luma) rel(p);
    rel(g_heap);
    rel(g_expand_pso); rel(g_match_pso); rel(g_down_pso);
    rel(g_rs);
    if (g_dev != nullptr) { g_dev->Release(); g_dev = nullptr; }
    g_inc = 0; g_w = g_h = g_lw = g_lh = 0;
    g_fmt = DXGI_FORMAT_UNKNOWN;
}

void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
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

D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned index)
{
    auto h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * g_inc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned index)
{
    auto h = g_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)index * g_inc;
    return h;
}

void srv(ID3D12Resource *r, DXGI_FORMAT fmt, unsigned index)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(r, &d, cpu(index));
}

void uav(ID3D12Resource *r, DXGI_FORMAT fmt, unsigned index)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dev->CreateUnorderedAccessView(r, nullptr, &d, cpu(index));
}

HRESULT texture(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt,
                D3D12_RESOURCE_STATES initial, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        initial, nullptr, IID_PPV_ARGS(out));
}

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
    ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

HRESULT make_pso(compile_fn compile, const char *entry, ID3D12PipelineState **out)
{
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = compile(kHLSL, sizeof(kHLSL) - 1u, "fg_pair_flow", nullptr, nullptr,
        entry, "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        char line[1000];
        std::snprintf(line, sizeof line, "[MGPU][FG][PairFlow] %s compile failed 0x%08X: %s",
            entry, (unsigned)hr, err != nullptr ? (const char *)err->GetBufferPointer() : "no shader blob");
        mgpu::diag::warn(line);
        rel(err); rel(cs);
        return FAILED(hr) ? hr : E_FAIL;
    }
    rel(err);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs;
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    hr = g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(out));
    rel(cs);
    return hr;
}
}

bool arm(ID3D12Device *dev, bool enabled, unsigned w, unsigned h, DXGI_FORMAT fmt)
{
    reset();
    if (!enabled || dev == nullptr || w == 0 || h == 0 || fmt == DXGI_FORMAT_UNKNOWN) return false;
    g_dev = dev; g_dev->AddRef(); g_w = w; g_h = h; g_fmt = fmt;
    g_lw = (std::min)(640u, w); g_lh = (std::max)(16u, (h * g_lw / w) & ~1u);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2; ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[2].Constants.ShaderRegister = 0; rp[2].Constants.Num32BitValues = 12;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ShaderRegister = 0; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3; rd.pParameters = rp;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;

    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    rel(err);
    if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rs));
    rel(sig);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc != nullptr ? (compile_fn)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (SUCCEEDED(hr) && compile == nullptr) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = make_pso(compile, "CSDown", &g_down_pso);
    if (SUCCEEDED(hr)) hr = make_pso(compile, "CSMatch", &g_match_pso);
    if (SUCCEEDED(hr)) hr = make_pso(compile, "CSExpand", &g_expand_pso);

    // Luma starts readable so the first (zero-motion) pair never binds an SRV
    // still in UAV state. All later states are explicit in record().
    for (unsigned p = 0; p < 2 && SUCCEEDED(hr); ++p)
        hr = texture(dev, g_lw, g_lh, DXGI_FORMAT_R16_FLOAT,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &g_luma[p]);
    for (unsigned p = 0; p < 2 && SUCCEEDED(hr); ++p)
        hr = texture(dev, g_lw, g_lh, DXGI_FORMAT_R16G16_FLOAT,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g_low[p]);
    for (unsigned s = 0; s < SLOTS && SUCCEEDED(hr); ++s)
        hr = texture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g_full[s]);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = DESCRIPTORS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr)) hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr))
    {
        char line[280];
        std::snprintf(line, sizeof line,
            "[MGPU][FG][PairFlow] setup failed 0x%08X; FG keeps the existing zero-vector path.", (unsigned)hr);
        mgpu::diag::warn(line);
        reset();
        return false;
    }
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (unsigned s = 0; s < SLOTS; ++s)
    {
        const unsigned db = DOWN_BASE + s * DOWN_STRIDE;
        // Colour is rebound for each set only at arm; set.real is immutable until drain.
        srv(nullptr, fmt, db + 1); // unused t1 placeholder
        uav(g_luma[0], DXGI_FORMAT_R16_FLOAT, db + 2);
        uav(g_luma[1], DXGI_FORMAT_R16_FLOAT, db + 3);
        for (unsigned p = 0; p < 2; ++p)
        {
            const unsigned mb = MATCH_BASE + (s * 2u + p) * FLOW_STRIDE;
            srv(g_luma[p], DXGI_FORMAT_R16_FLOAT, mb);
            srv(g_luma[1u - p], DXGI_FORMAT_R16_FLOAT, mb + 1);
            uav(g_low[p], DXGI_FORMAT_R16G16_FLOAT, mb + 2);
            const unsigned eb = EXPAND_BASE + (s * 2u + p) * FLOW_STRIDE;
            srv(g_low[p], DXGI_FORMAT_R16G16_FLOAT, eb);
            srv(nullptr, DXGI_FORMAT_R16G16_FLOAT, eb + 1);
            uav(g_full[s], DXGI_FORMAT_R16G16_FLOAT, eb + 2);
        }
    }
    g_active = true;
    char line[520];
    std::snprintf(line, sizeof line,
        "[MGPU][FG][PairFlow] ARMED independently: %ux%u -> luma %ux%u -> current-to-previous RG16F pixels %ux%u. "
        "NR/game-MV paths unchanged; coarse-to-fine 3x3 block matching, conservative zero on low texture or poor matches; no CPU readback.",
        w, h, g_lw, g_lh, w, h);
    mgpu::diag::info(line);
    return true;
}

bool active() { return g_active; }

ID3D12Resource *record(ID3D12GraphicsCommandList *cl, ID3D12Resource *current,
                       unsigned slot, bool *estimated)
{
    if (estimated != nullptr) *estimated = false;
    if (!g_active || cl == nullptr || current == nullptr || slot >= SLOTS) return nullptr;
    const auto desc = current->GetDesc();
    if (desc.Width != g_w || desc.Height != g_h || desc.Format != g_fmt) return nullptr;

    // The set's real texture is immutable while its present/evaluation lease is
    // live. Rebinding its descriptor at each use also avoids any external import.
    srv(current, g_fmt, DOWN_BASE + slot * DOWN_STRIDE);
    const unsigned p = g_parity, old = 1u - p;
    const bool valid = g_have_previous;
    ID3D12DescriptorHeap *heaps[1] = {g_heap};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);

    // Downsample current colour into this parity's luma target.
    barrier(cl, g_luma[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetPipelineState(g_down_pso);
    const unsigned db = DOWN_BASE + slot * DOWN_STRIDE;
    cl->SetComputeRootDescriptorTable(0, gpu(db));
    cl->SetComputeRootDescriptorTable(1, gpu(db + 2u + p));
    UINT c[12] = {g_w, g_h, g_lw, g_lh, valid ? 1u : 0u, 0, 0, 0, 0, 0, 0, 0};
    cl->SetComputeRoot32BitConstants(2, 12, c, 0);
    cl->Dispatch((g_lw + 7u) / 8u, (g_lh + 7u) / 8u, 1u);
    barrier(cl, g_luma[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Estimate current -> previous in low-resolution pixels. On the first frame
    // both history textures are readable and gValid=0 makes the shader emit zero.
    if (g_low_srv[p])
        barrier(cl, g_low[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetPipelineState(g_match_pso);
    const unsigned mb = MATCH_BASE + (slot * 2u + p) * FLOW_STRIDE;
    cl->SetComputeRootDescriptorTable(0, gpu(mb));
    cl->SetComputeRootDescriptorTable(1, gpu(mb + 2));
    c[0] = g_lw; c[1] = g_lh; c[2] = g_lw; c[3] = g_lh;
    cl->SetComputeRoot32BitConstants(2, 12, c, 0);
    cl->Dispatch((g_lw + 7u) / 8u, (g_lh + 7u) / 8u, 1u);
    barrier(cl, g_low[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_low_srv[p] = true;

    // Expand to the exact full-resolution RG16F extent expected by DLSSG.
    if (g_full_srv[slot])
        barrier(cl, g_full[slot], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetPipelineState(g_expand_pso);
    const unsigned eb = EXPAND_BASE + (slot * 2u + p) * FLOW_STRIDE;
    cl->SetComputeRootDescriptorTable(0, gpu(eb));
    cl->SetComputeRootDescriptorTable(1, gpu(eb + 2));
    const float sx = (float)g_w / (float)g_lw, sy = (float)g_h / (float)g_lh;
    std::memcpy(&c[8], &sx, sizeof(sx)); std::memcpy(&c[9], &sy, sizeof(sy));
    cl->SetComputeRoot32BitConstants(2, 12, c, 0);
    cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);
    barrier(cl, g_full[slot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_full_srv[slot] = true;

    if (valid) { ++g_pairs; if (estimated != nullptr) *estimated = true; }
    else ++g_fallbacks;
    g_have_previous = true;
    g_parity = old;
    return g_full[slot];
}

void stats(unsigned long long &pairs, unsigned long long &fallbacks)
{
    pairs = g_pairs; fallbacks = g_fallbacks;
}

void release() { reset(); }
}
