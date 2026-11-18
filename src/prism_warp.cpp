// Late-frame neural-edit transport for the dual-GPU presenter.
//
// This is an independent implementation of the public edit-cache principle:
// cache log2(neural / original), not an old picture. A later game frame is the
// colour anchor. Depth and colour admit history; low and high edit bands have
// separate rejection weights. Validated edits accumulate within the current
// frame's edit range; old pictures never accumulate and never feed NGX.

#include "prism_warp.hpp"

#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::pwarp
{
namespace
{
std::atomic<bool> g_active{false};
ID3D12Device *g_dev = nullptr;
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_pso = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_out = nullptr;
ID3D12Resource *g_edit[2] = {};
ID3D12Resource *g_low[2] = {};
ID3D12Resource *g_guide[2] = {};
bool g_srv[2] = {};
UINT g_inc = 0;
unsigned g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
unsigned g_cur = 0;
bool g_history_valid = false;
unsigned long long g_applies = 0, g_warped = 0, g_fallbacks = 0;

constexpr UINT kSrv = 7;
constexpr UINT kUav = 4;
constexpr UINT kSlots = kSrv + kUav;

struct consts_t
{
    unsigned w, h, mw, mh;
    unsigned dw, dh, history, depth_valid;
    float mv_x, mv_y;
    float depth_tol, colour_tol;
    unsigned depth_inverted, pad0, pad1, pad2;
};
static_assert(sizeof(consts_t) == 64, "16 root constants");

const char kShader[] = R"(
cbuffer C : register(b0)
{
  uint gW, gH, gMW, gMH;
  uint gDW, gDH, gHistory, gDepthValid;
  float gMvX, gMvY, gDepthTol, gColourTol;
  uint gDepthInverted, gPad0, gPad1, gPad2;
};
Texture2D<float4> gBase : register(t0);
Texture2D<float4> gNeural : register(t1);
Texture2D<float4> gPrevEdit : register(t2);
Texture2D<float4> gPrevLow : register(t3);
Texture2D<float2> gPrevGuide : register(t4);
Texture2D<float2> gMotion : register(t5);
Texture2D<float4> gDepth : register(t6);
RWTexture2D<float4> gOut : register(u0);
RWTexture2D<float4> gNextEdit : register(u1);
RWTexture2D<float4> gNextLow : register(u2);
RWTexture2D<float2> gNextGuide : register(u3);

float3 Dec(float3 c)
{
  c = max(c, 0.0);
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 Enc(float3 c)
{
  c = max(c, 0.0);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float LogLuma(float3 c) { return log2(max(Luma(c), 1e-4)); }
float LinDepth(float d)
{
  return gDepthInverted != 0 ? 1.0 / max(d, 1e-7) : 1.0 / max(1.0 - d, 1e-7);
}
float RelDepth(float a, float b) { return abs(a - b) / max(min(a, b), 1e-7); }
float Outside(float v, float lo, float hi) { return max(max(lo - v, v - hi), 0.0); }
int2 ClampPx(int2 p) { return clamp(p, int2(0, 0), int2(gW, gH) - int2(1, 1)); }
int2 DepthPx(float2 uv)
{
  return clamp(int2(uv * float2(gDW, gDH)), int2(0, 0), int2(gDW, gDH) - int2(1, 1));
}
int2 MotionPx(float2 uv)
{
  return clamp(int2(uv * float2(gMW, gMH)), int2(0, 0), int2(gMW, gMH) - int2(1, 1));
}
float CurrentDepth(float2 uv)
{
  return gDepthValid != 0 ? LinDepth(gDepth.Load(int3(DepthPx(uv), 0)).r) : 1.0;
}

// Current position + vector = previous position, matching DLSS-NR and cv_chain.
// At silhouettes use the nearest current surface's vector, never a background
// vector merely because it happens to cover the display pixel.
float2 MotionUv(float2 uv)
{
  float2 mv = float2(0.0, 0.0);
  if (gDepthValid == 0)
  {
    mv = gMotion.Load(int3(MotionPx(uv), 0));
  }
  else
  {
    const int2 centre = DepthPx(uv);
    int2 best = centre;
    float nearD = 3.4e38;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
      [unroll] for (int x = -1; x <= 1; ++x)
      {
        int2 p = clamp(centre + int2(x, y), int2(0, 0), int2(gDW, gDH) - int2(1, 1));
        float d = LinDepth(gDepth.Load(int3(p, 0)).r);
        if (d < nearD) { nearD = d; best = p; }
      }
    }
    float2 bestUv = (float2(best) + 0.5) / float2(gDW, gDH);
    mv = gMotion.Load(int3(MotionPx(bestUv), 0));
  }
  return mv * float2(gMvX, gMvY) / float2(gW, gH);
}

float3 FreshEdit(int2 p, out float ok)
{
  float3 b = Dec(max(gBase.Load(int3(p, 0)).rgb, 0.0));
  float3 n = Dec(max(gNeural.Load(int3(p, 0)).rgb, 0.0));
  float by = Luma(b), ny = Luma(n);
  ok = (by > 8.0 / 512.0 && ny < 0.05 * by) ? 0.0 : 1.0;
  return clamp(log2(max(n, 1.0 / 512.0) / max(b, 1.0 / 512.0)), -3.0, 3.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH) return;
  int2 p = int2(id.xy);
  float2 uv = (float2(p) + 0.5) / float2(gW, gH);
  float4 baseSample = gBase.Load(int3(p, 0));
  float3 baseCode = max(baseSample.rgb, 0.0);
  float3 base = Dec(baseCode);

  // Capture this evaluation for the next frame. The low band, isolated
  // luminance bound and current edit range share the same nine source reads.
  // Only a validated edit can accumulate; an old picture is never blended.
  float centreOk;
  float3 centre = FreshEdit(p, centreOk);
  float loY = 1e9, hiY = -1e9;
  float3 editLo = 1e9, editHi = -1e9;
  float3 lowSum = 0.0;
  float lowN = 0.0;
  [unroll] for (int y = -1; y <= 1; ++y)
  {
    [unroll] for (int x = -1; x <= 1; ++x)
    {
      int2 q = ClampPx(p + int2(x, y));
      float ok;
      float3 e = FreshEdit(q, ok);
      if (ok > 0.5)
      {
        lowSum += e;
        lowN += 1.0;
        editLo = min(editLo, e);
        editHi = max(editHi, e);
        if (x != 0 || y != 0)
        {
          float ey = Luma(e);
          loY = min(loY, ey);
          hiY = max(hiY, ey);
        }
      }
    }
  }
  if (hiY >= loY)
  {
    float ey = Luma(centre);
    centre += clamp(ey, loY - 0.1, hiY + 0.1) - ey;
  }
  if (lowN < 1.0) { editLo = 0.0; editHi = 0.0; }
  float3 low = lowN > 0.0 ? lowSum / lowN : float3(0.0, 0.0, 0.0);
  float3 freshCentre = centreOk > 0.5 ? centre : float3(0.0, 0.0, 0.0);
  gNextGuide[p] = float2(CurrentDepth(uv), LogLuma(base));

  if (gHistory == 0)
  {
    gNextEdit[p] = float4(freshCentre, centreOk);
    gNextLow[p] = float4(low, lowN / 9.0);
    // The first usable frame is bit-for-bit the ordinary neural answer. Only
    // later frames take the detail-stability path.
    gOut[p] = gNeural.Load(int3(p, 0));
    return;
  }

  float2 q = uv + MotionUv(uv);
  bool onScreen = all(q >= 0.0) && all(q <= 1.0);
  if (!onScreen)
  {
    gNextEdit[p] = float4(freshCentre, centreOk);
    gNextLow[p] = float4(low, lowN / 9.0);
    // A missing reprojection must never expose the raw game frame. The
    // current neural answer is the safe presenter fallback.
    gOut[p] = gNeural.Load(int3(p, 0));
    return;
  }

  // TAA-style current 3x3 range tolerates sub-pixel movement and aliasing while
  // still rejecting a surface whose underlying game colour actually changed.
  float yLo = 1e9, yHi = -1e9;
  [unroll] for (int ry = -1; ry <= 1; ++ry)
  {
    [unroll] for (int rx = -1; rx <= 1; ++rx)
    {
      float cy = LogLuma(Dec(max(gBase.Load(int3(ClampPx(p + int2(rx, ry)), 0)).rgb, 0.0)));
      yLo = min(yLo, cy);
      yHi = max(yHi, cy);
    }
  }

  float2 hp = q * float2(gW, gH) - 0.5;
  int2 h0 = int2(floor(hp));
  float2 hf = hp - floor(hp);
  float curD = CurrentDepth(uv);
  float3 highSum = 0.0, lowHistory = 0.0;
  float highW = 0.0, lowW = 0.0;
  [unroll] for (int k = 0; k < 4; ++k)
  {
    int2 o = int2(k & 1, k >> 1);
    int2 t = ClampPx(h0 + o);
    float wb = (o.x ? hf.x : 1.0 - hf.x) * (o.y ? hf.y : 1.0 - hf.y);
    float2 guide = gPrevGuide.Load(int3(t, 0));
    float wd = gDepthValid != 0
        ? saturate((2.0 * gDepthTol - RelDepth(guide.x, curD)) / gDepthTol) : 1.0;
    float outside = Outside(guide.y, yLo, yHi);
    // Fine detail is admitted strictly; broad lighting gets a wider gate.
    float wcHigh = saturate((gColourTol - outside) / max(0.5 * gColourTol, 1e-4));
    float wcLow = saturate((4.0 * gColourTol - outside) / max(2.0 * gColourTol, 1e-4));
    float4 he = gPrevEdit.Load(int3(t, 0));
    float4 hl = gPrevLow.Load(int3(t, 0));
    float3 high = he.rgb - hl.rgb;
    float wh = wb * wd * wcHigh * he.a;
    float wl = wb * wd * wcLow * hl.a;
    highSum += high * wh;
    lowHistory += hl.rgb * wl;
    highW += wh;
    lowW += wl;
  }
  float3 high = highW > 1e-5 ? highSum / highW : float3(0.0, 0.0, 0.0);
  float3 lowEdit = lowW > 1e-5 ? lowHistory / lowW : float3(0.0, 0.0, 0.0);

  // Single-card detail stability: filter the neural EDIT, not the picture.
  // Reprojected bands are constrained by the current 3x3 edit range before
  // they can accumulate, so a disoccluded/old-surface edit cannot trail.
  float3 freshHigh = freshCentre - low;
  high = clamp(high, editLo - low, editHi - low);
  lowEdit = clamp(lowEdit, editLo, editHi);
  float highHistoryWeight = 0.80 * saturate(highW);
  float lowHistoryWeight = 0.90 * saturate(lowW);
  float3 stableHigh = lerp(freshHigh, high, highHistoryWeight);
  float3 stableLow = lerp(low, lowEdit, lowHistoryWeight);
  if (centreOk <= 0.5) { stableHigh = 0.0; stableLow = 0.0; }
  float3 stableEdit = clamp(stableLow + stableHigh, -3.0, 3.0);
  gNextEdit[p] = float4(stableEdit, centreOk);
  gNextLow[p] = float4(stableLow, lowN / 9.0);
  if (centreOk <= 0.5) gOut[p] = gNeural.Load(int3(p, 0));
  else gOut[p] = float4(Enc(base * exp2(stableEdit)), 1.0);
}
)";

template <typename T> void rel(T *&p)
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (r == nullptr || before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

void release_all()
{
    g_active.store(false, std::memory_order_release);
    rel(g_pso); rel(g_rs); rel(g_heap); rel(g_out);
    for (unsigned i = 0; i < 2; ++i)
    {
        rel(g_edit[i]); rel(g_low[i]); rel(g_guide[i]); g_srv[i] = false;
    }
    g_dev = nullptr; g_w = g_h = 0; g_fmt = DXGI_FORMAT_UNKNOWN;
    g_cur = 0; g_history_valid = false;
    g_applies = g_warped = g_fallbacks = 0;
}

bool ini_path(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm)) return false;
    wchar_t mod[MAX_PATH];
    DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu.ini", mod);
    return true;
}

bool enabled()
{
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return false;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;
    const char *on = mgpu::config::find(buf.data(), got, "PrismWarp");
    return on != nullptr && atoi(on) == 1;
}

DXGI_FORMAT view_format(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

HRESULT make_tex(unsigned w, unsigned h, DXGI_FORMAT fmt, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                          IID_PPV_ARGS(out));
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(unsigned parity, unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)((parity & 1u) * kSlots + slot) * g_inc;
    return h;
}

void srv(unsigned parity, unsigned slot, ID3D12Resource *r, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(r, &d, cpu_slot(parity, slot));
}

void uav(unsigned parity, unsigned slot, ID3D12Resource *r, DXGI_FORMAT fmt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = fmt; d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dev->CreateUnorderedAccessView(r, nullptr, &d, cpu_slot(parity, kSrv + slot));
}

bool build()
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc != nullptr ? (decltype(&D3DCompile))GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][PWARP] D3DCompile unavailable; late edit transport stays OFF.");
        return false;
    }
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1u, "prism_warp", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        char line[1200];
        snprintf(line, sizeof line, "[MGPU][PWARP] shader compile failed 0x%08X %s",
                 (unsigned)hr, err != nullptr ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::error(line); rel(err); rel(cs); return false;
    }
    rel(err);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kSrv; ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = kUav; ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = kSrv;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0; rp[0].Constants.Num32BitValues = 16;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 2; rp[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    ID3DBlob *sig = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    rel(err);
    if (SUCCEEDED(hr))
        hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&g_rs));
    rel(sig);
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs; pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    }
    rel(cs);
    if (SUCCEEDED(hr)) hr = make_tex(g_w, g_h, g_fmt, &g_out);
    for (unsigned i = 0; i < 2 && SUCCEEDED(hr); ++i)
    {
        hr = make_tex(g_w, g_h, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_edit[i]);
        if (SUCCEEDED(hr)) hr = make_tex(g_w, g_h, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_low[i]);
        if (SUCCEEDED(hr)) hr = make_tex(g_w, g_h, DXGI_FORMAT_R32G32_FLOAT, &g_guide[i]);
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2 * kSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr)) hr = g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr))
    {
        char line[240];
        snprintf(line, sizeof line, "[MGPU][PWARP] GPU object creation failed 0x%08X; experiment OFF.", (unsigned)hr);
        mgpu::diag::error(line); return false;
    }
    g_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}
} // namespace

bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt)
{
    release_all();
    if (dev == nullptr || width == 0 || height == 0 || !enabled()) return false;
    g_dev = dev; g_w = width; g_h = height; g_fmt = view_format(fmt);
    if (!build()) { release_all(); return false; }
    g_active.store(true, std::memory_order_release);
    char line[420];
    snprintf(line, sizeof line,
             "[MGPU][PWARP] LATE EDIT A/B ON: previous log-ratio edit -> current game colour, "
             "%ux%u fmt=%d; nearest-surface MV + per-tap depth/colour rejection + independent "
             "low/high bands; current-bounded edit stability, no old-picture present, no NGX history feedback.",
             g_w, g_h, (int)g_fmt);
    mgpu::diag::info(line);
    return true;
}

bool active() { return g_active.load(std::memory_order_acquire); }

ID3D12Resource *apply(ID3D12GraphicsCommandList *cl, unsigned parity,
                      ID3D12Resource *base, ID3D12Resource *neural,
                      ID3D12Resource *motion, float motion_scale_x, float motion_scale_y,
                      ID3D12Resource *depth, bool depth_inverted)
{
    if (!active() || cl == nullptr || base == nullptr || neural == nullptr || g_out == nullptr)
        return nullptr;
    D3D12_RESOURCE_DESC bd = base->GetDesc(), nd = neural->GetDesc();
    if (bd.Width != g_w || bd.Height != g_h || nd.Width != g_w || nd.Height != g_h)
    {
        invalidate(); return nullptr;
    }
    if (motion == nullptr)
    {
        invalidate();
        ++g_fallbacks;
        if (g_fallbacks == 1 || g_fallbacks % 600ull == 0)
            mgpu::diag::warn("[MGPU][PWARP] no complete cross-frame motion path: ordinary current neural frame; history invalidated.");
        return nullptr;
    }
    D3D12_RESOURCE_DESC md = motion->GetDesc();
    if (md.Width == 0 || md.Height == 0) { invalidate(); return nullptr; }
    D3D12_RESOURCE_DESC dd{};
    if (depth != nullptr) dd = depth->GetDesc();
    const bool depth_ok = depth != nullptr &&
        (dd.Format == DXGI_FORMAT_R32_FLOAT || dd.Format == DXGI_FORMAT_R16_FLOAT) &&
        dd.Width != 0 && dd.Height != 0;

    const unsigned next = g_cur, prev = 1u - g_cur;
    const bool had_history = g_history_valid;
    barrier(cl, neural, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!g_srv[prev])
    {
        barrier(cl, g_edit[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(cl, g_low[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(cl, g_guide[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_srv[prev] = true;
    }
    if (g_srv[next])
    {
        barrier(cl, g_edit[next], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(cl, g_low[next], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(cl, g_guide[next], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_srv[next] = false;
    }

    const unsigned p = parity & 1u;
    srv(p, 0, base, g_fmt);
    srv(p, 1, neural, g_fmt);
    srv(p, 2, g_edit[prev], DXGI_FORMAT_R16G16B16A16_FLOAT);
    srv(p, 3, g_low[prev], DXGI_FORMAT_R16G16B16A16_FLOAT);
    srv(p, 4, g_guide[prev], DXGI_FORMAT_R32G32_FLOAT);
    srv(p, 5, motion, (DXGI_FORMAT)md.Format);
    srv(p, 6, depth_ok ? depth : base, depth_ok ? (DXGI_FORMAT)dd.Format : g_fmt);
    uav(p, 0, g_out, g_fmt);
    uav(p, 1, g_edit[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    uav(p, 2, g_low[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    uav(p, 3, g_guide[next], DXGI_FORMAT_R32G32_FLOAT);

    consts_t c{};
    c.w = g_w; c.h = g_h; c.mw = (unsigned)md.Width; c.mh = md.Height;
    c.dw = depth_ok ? (unsigned)dd.Width : 1u; c.dh = depth_ok ? dd.Height : 1u;
    c.history = had_history ? 1u : 0u; c.depth_valid = depth_ok ? 1u : 0u;
    c.mv_x = motion_scale_x; c.mv_y = motion_scale_y;
    c.depth_tol = 0.08f; c.colour_tol = 0.50f;
    c.depth_inverted = depth_inverted ? 1u : 0u;

    ID3D12DescriptorHeap *heaps[1] = {g_heap};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(g_pso);
    cl->SetComputeRoot32BitConstants(0, 16, &c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += (UINT64)(p * kSlots) * g_inc;
    cl->SetComputeRootDescriptorTable(1, gh);
    cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);

    barrier(cl, g_edit[next], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(cl, g_low[next], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(cl, g_guide[next], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_srv[next] = true;
    barrier(cl, g_edit[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, g_low[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, g_guide[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_srv[prev] = false;
    barrier(cl, neural, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = g_out;
    cl->ResourceBarrier(1, &ub);

    g_cur = next;
    g_history_valid = true;
    ++g_applies;
    if (had_history) ++g_warped;
    if (g_applies == 1 || g_applies % 600ull == 0)
    {
        char line[300];
        snprintf(line, sizeof line,
                 "[MGPU][PWARP][LIVE] applies=%llu reanchored=%llu fallbacks=%llu motion=%ux%u "
                 "scale=%.3f/%.3f depth=%s; current neural answer captured for the next frame.",
                 g_applies, g_warped, g_fallbacks, (unsigned)md.Width, md.Height,
                 (double)motion_scale_x, (double)motion_scale_y,
                 !depth_ok ? "colour-only" : (dd.Format == DXGI_FORMAT_R16_FLOAT ? "R16" : "R32"));
        mgpu::diag::info(line);
    }
    return g_out;
}

void invalidate() { g_history_valid = false; }
void release() { release_all(); }
} // namespace mgpu::pwarp
