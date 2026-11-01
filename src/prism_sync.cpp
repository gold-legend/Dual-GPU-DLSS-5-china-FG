// MGPU Bridge - synchronous same-frame Prism reconstruction on the neural GPU.
// See prism_sync.hpp for the deliberately narrow first-milestone contract.

#include "prism_sync.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::psync
{
namespace
{
std::atomic<bool> g_active{false};
ID3D12Device *g_dev = nullptr;
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_extract = nullptr;
ID3D12PipelineState *g_compose = nullptr;
ID3D12PipelineState *g_compose_guided = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_edit = nullptr;
UINT g_inc = 0;
unsigned g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
std::atomic<float> g_intensity{1.0f};
std::atomic<float> g_guard{1.0f};
unsigned g_filter = 1;
unsigned long long g_applies = 0;

// Two parities x two passes x (two SRVs + one UAV). Descriptors are never
// rewritten between two passes in the same command list: they are read when
// the GPU executes the list, not when the CPU records it.
constexpr UINT SLOTS_PER_PASS = 3;
constexpr UINT PASSES_PER_PARITY = 2;
constexpr UINT SLOTS_PER_PARITY = SLOTS_PER_PASS * PASSES_PER_PARITY;

float g_detail = 0.85f;

struct consts_t
{
    unsigned w, h;
    float intensity;
    float guard;
    unsigned filter;
    float detail;
    unsigned pad1, pad2;
};

const char kShaders[] = R"(
cbuffer C : register(b0)
{
  uint gW; uint gH; float gIntensity; float gGuard;
  uint gFilter; float gDetail; uint gPad1; uint gPad2;
};
Texture2D<float4> gBase : register(t0);
Texture2D<float4> gOther : register(t1);
RWTexture2D<float4> gOut : register(u0);

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
float Smooth(float a, float b, float x)
{
  const float t = saturate((x - a) / (b - a));
  return t * t * (3.0 - 2.0 * t);
}

// Prism Compose's CbCr skin + strong-colour guard and chroma clamp.
float3 GuardEdit(float3 pd, float3 p, float3 e)
{
  const float3 d = pd * 255.0;
  const float cb = 128.0 - 0.168736 * d.r - 0.331264 * d.g + 0.5 * d.b;
  const float cr = 128.0 + 0.5 * d.r - 0.418688 * d.g - 0.081312 * d.b;
  const float2 q = float2((cb - 107.0) / 22.0, (cr - 150.0) / 18.0);
  const float skin = exp(-dot(q, q)) * Smooth(0.02, 0.06, Luma(p));
  const float mx = max(p.r, max(p.g, p.b));
  const float mn = min(p.r, min(p.g, p.b));
  const float w = max(skin * 0.8, Smooth(0.35, 0.7, mx > 1e-4 ? (mx - mn) / mx : 0.0) * 0.7) * gGuard;
  const float y = Luma(e);
  const float3 kept = (e - y) * (1.0 - w);
  return y + kept + (clamp(kept, -0.08, 0.08) - kept) * w;
}

// Prism's synchronous edit extraction. Base and neural are display-referred
// textures from the SAME MGPU seal frame; Dec/Enc bracket only the ratio math.
[numthreads(8, 8, 1)]
void CS_Extract(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH) return;
  const float3 pd = max(gBase[id.xy].rgb, 0.0);
  const float3 p = Dec(pd);
  const float3 n = Dec(max(gOther[id.xy].rgb, 0.0));
  float3 e = clamp(log2(max(n, 1.0 / 512.0) / max(p, 1.0 / 512.0)), -3.0, 3.0);

  // Guided Compose follows Prism's original order: fit the raw edit first, then
  // apply Intensity and the character guard. The established paths keep theirs.
  if (gFilter != 2)
  {
    e = GuardEdit(pd, p, e * gIntensity);
  }
  gOut[id.xy] = float4(e, log2(max(Luma(p), 1e-4)));
}

// Same-frame joint-bilateral edit filtering (Prism UpEdit, Span=1 here), then
// multiplicative reconstruction on the original pixel. Zero edit is a direct
// copy, which makes the identity invariant exact instead of approximate.
[numthreads(8, 8, 1)]
void CS_Compose(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH) return;
  const float3 baseCode = max(gBase[id.xy].rgb, 0.0);
  const float3 base = Dec(baseCode);
  float3 e = gOther[id.xy].rgb;

  if (gFilter == 1)
  {
    const int2 last = int2(gW, gH) - 1;
    const float2 f = float2(id.xy);
    const int2 b = int2(floor(f));
    const float yFull = log2(max(Luma(base), 1e-4));
    float3 acc = 0.0, bil = 0.0;
    float ws = 0.0;
    [unroll] for (int j = -1; j <= 2; ++j)
    {
      [unroll] for (int i = -1; i <= 2; ++i)
      {
        const int2 q = clamp(b + int2(i, j), int2(0, 0), last);
        const float4 sample = gOther.Load(int3(q, 0));
        const float2 dd = f - float2(b + int2(i, j));
        const float r = (yFull - sample.a) / 0.15;
        const float w = exp(-dot(dd, dd) / 1.28) * exp(-0.5 * r * r);
        acc += sample.rgb * w;
        ws += w;
        if (i >= 0 && i <= 1 && j >= 0 && j <= 1)
          bil += sample.rgb * ((1.0 - abs(dd.x)) * (1.0 - abs(dd.y)));
      }
    }
    e = ws > 1e-4 ? acc / ws : bil;
  }

  if (all(abs(e) <= 1e-8))
    gOut[id.xy] = float4(baseCode, gBase[id.xy].a); // exact identity
  else
    gOut[id.xy] = float4(Enc(base * exp2(e)), gBase[id.xy].a);
}

// Prism GuidedFit + UpGuided fused for the synchronous full-size edit. At
// Span=1 UpGuided's bilinear sample lands exactly on the coefficient texel, so
// the intermediate A/B textures are unnecessary: each 16x16 group loads 24x24
// edit/guide samples, fits 20x20 5x5 windows, averages 16x16 coefficients and
// composes immediately. This adds no frame history and no cross-frame state.
static const float kGfRef = -2.5;
static const float kGfEps = 0.01;
groupshared float4 gfIE[576];
groupshared float3 gfSA[400], gfSB[400];
[numthreads(16, 16, 1)]
void CS_ComposeGuided(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
  const int2 last = int2(gW, gH) - 1;
  const int2 org = int2(gid.xy) * 16 - 4;
  for (uint i = gi; i < 576; i += 256)
  {
    const int2 q = clamp(org + int2(i % 24, i / 24), int2(0, 0), last);
    const float4 v = gOther.Load(int3(q, 0));
    gfIE[i] = float4(v.a - kGfRef, v.rgb);
  }
  GroupMemoryBarrierWithGroupSync();

  for (uint n = gi; n < 400; n += 256)
  {
    const int c = int(n / 20 + 2) * 24 + int(n % 20 + 2);
    float mI = 0.0, mII = 0.0;
    float3 mP = 0.0, mIP = 0.0;
    [unroll] for (int j = -2; j <= 2; ++j)
    {
      [unroll] for (int k = -2; k <= 2; ++k)
      {
        const float4 v = gfIE[c + j * 24 + k];
        mI += v.x; mII += v.x * v.x; mP += v.yzw; mIP += v.x * v.yzw;
      }
    }
    mI /= 25.0; mII /= 25.0; mP /= 25.0; mIP /= 25.0;
    // DetailKeep bounds only a falling slope: the edit may remove at most the
    // configured share of local contrast, while raising contrast is unbounded.
    const float3 a = max((mIP - mI * mP) / (max(mII - mI * mI, 0.0) + kGfEps), -gDetail);
    gfSA[n] = a;
    gfSB[n] = mP - a * mI;
  }
  GroupMemoryBarrierWithGroupSync();

  const int2 t = int2(gi % 16, gi / 16), px = int2(gid.xy) * 16 + t;
  if (px.x >= (int)gW || px.y >= (int)gH) return;
  float3 A = 0.0, B = 0.0;
  [unroll] for (int j = 0; j <= 4; ++j)
  {
    [unroll] for (int k = 0; k <= 4; ++k)
    {
      const int c = (t.y + j) * 20 + t.x + k;
      A += gfSA[c]; B += gfSB[c];
    }
  }

  const float3 baseCode = max(gBase[px].rgb, 0.0);
  const float3 base = Dec(baseCode);
  const float yFull = log2(max(Luma(base), 1e-4));
  float3 e = clamp((A / 25.0) * (yFull - kGfRef) + B / 25.0, -3.0, 3.0) * gIntensity;
  e = GuardEdit(baseCode, base, e);
  if (all(abs(e) <= 1e-8))
    gOut[px] = float4(baseCode, gBase[px].a);
  else
    gOut[px] = float4(Enc(base * exp2(e)), gBase[px].a);
}
)";

typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                         ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob **, ID3DBlob **);

void rel(IUnknown *&p)
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

void release_all()
{
    g_active.store(false, std::memory_order_relaxed);
    IUnknown *u;
    u = g_compose_guided; rel(u); g_compose_guided = nullptr;
    u = g_compose; rel(u); g_compose = nullptr;
    u = g_extract; rel(u); g_extract = nullptr;
    u = g_rs; rel(u); g_rs = nullptr;
    u = g_heap; rel(u); g_heap = nullptr;
    u = g_edit; rel(u); g_edit = nullptr;
    g_dev = nullptr;
    g_w = g_h = 0;
    g_applies = 0;
}

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

float key_float(const char *buf, size_t n, const char *key, float dflt, float lo, float hi)
{
    const char *v = mgpu::config::find(buf, n, key);
    if (v == nullptr) return dflt;
    char *end = nullptr;
    const double d = strtod(v, &end);
    if (end == v || d < (double)lo || d > (double)hi) return dflt;
    return (float)d;
}

bool read_keys()
{
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return false;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;
    buf[got] = '\0';
    const char *on = mgpu::config::find(buf.data(), got, "PrismSync");
    if (on == nullptr || atoi(on) != 1) return false;
    g_intensity.store(key_float(buf.data(), got, "PrismSyncIntensity", 1.0f, 0.0f, 3.0f),
                      std::memory_order_relaxed);
    g_guard.store(key_float(buf.data(), got, "PrismSyncGuard", 1.0f, 0.0f, 1.0f),
                  std::memory_order_relaxed);
    g_filter = (unsigned)key_float(buf.data(), got, "PrismSyncFilter", 1.0f, 0.0f, 2.0f);
    g_detail = key_float(buf.data(), got, "PrismSyncDetailKeep", 0.85f, 0.0f, 1.0f);
    return true;
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

bool build(ID3D12Device *dev)
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = dc != nullptr
        ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][PSYNC] D3DCompile is unavailable; synchronous Prism stays off.");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2; // t0 = same-frame base, t1 = neural/edit
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 8;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 2;
    rp[1].DescriptorTable.pDescriptorRanges = ranges;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err != nullptr) { err->Release(); err = nullptr; }
    if (SUCCEEDED(hr))
        hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      IID_PPV_ARGS(&g_rs));
    if (sig != nullptr) sig->Release();
    if (FAILED(hr))
    {
        char line[180];
        snprintf(line, sizeof line, "[MGPU][PSYNC] root signature failed: 0x%08X", (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }

    const char *entries[3] = {"CS_Extract", "CS_Compose", "CS_ComposeGuided"};
    ID3D12PipelineState **targets[3] = {&g_extract, &g_compose, &g_compose_guided};
    for (unsigned pass = 0; pass < 3; ++pass)
    {
        const char *entry = entries[pass];
        ID3DBlob *cs = nullptr;
        hr = compile(kShaders, sizeof(kShaders) - 1u, "prism_sync", nullptr, nullptr,
                     entry, "cs_5_0", 0, 0, &cs, &err);
        if (FAILED(hr) || cs == nullptr)
        {
            char line[1000];
            snprintf(line, sizeof line, "[MGPU][PSYNC] %s compile failed: 0x%08X %s",
                     entry, (unsigned)hr, err != nullptr ? (const char *)err->GetBufferPointer() : "");
            mgpu::diag::error(line);
            if (err != nullptr) err->Release();
            if (cs != nullptr) cs->Release();
            return false;
        }
        if (err != nullptr) { err->Release(); err = nullptr; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(targets[pass]));
        cs->Release();
        if (FAILED(hr))
        {
            char line[200];
            snprintf(line, sizeof line, "[MGPU][PSYNC] %s PSO failed: 0x%08X", entry, (unsigned)hr);
            mgpu::diag::error(line);
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = g_w;
    rd.Height = g_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                      IID_PPV_ARGS(&g_edit));
    if (FAILED(hr)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2u * SLOTS_PER_PARITY;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr)) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

UINT slot_index(unsigned parity, unsigned pass, unsigned slot)
{
    return (parity & 1u) * SLOTS_PER_PARITY + pass * SLOTS_PER_PASS + slot;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(unsigned parity, unsigned pass, unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot_index(parity, pass, slot) * g_inc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(unsigned parity, unsigned pass)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = g_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)slot_index(parity, pass, 0) * g_inc;
    return h;
}

void write_srv(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *r, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(r, &d, h);
}

void write_uav(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *r, DXGI_FORMAT fmt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dev->CreateUnorderedAccessView(r, nullptr, &d, h);
}
} // namespace

bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt)
{
    if (dev == nullptr || width == 0 || height == 0 || !read_keys()) return false;
    release_all();
    g_dev = dev;
    g_w = width;
    g_h = height;
    g_fmt = view_format(fmt);
    if (!build(dev))
    {
        release_all();
        return false;
    }
    g_active.store(true, std::memory_order_relaxed);
    char line[360];
    snprintf(line, sizeof line,
             "[MGPU][PSYNC] synchronous same-frame Prism ON: %ux%u fmt=%d intensity=%.2f guard=%.2f "
             "filter=%s detail=%.2f; full-resolution x1/x2 only; temporal/cadence/FG are OFF.",
             g_w, g_h, (int)g_fmt, (double)g_intensity.load(std::memory_order_relaxed),
             (double)g_guard.load(std::memory_order_relaxed),
             g_filter == 2 ? "guided" : (g_filter == 1 ? "joint-bilateral" : "direct"),
             (double)g_detail);
    mgpu::diag::info(line);
    return true;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

bool apply(ID3D12GraphicsCommandList *cl, unsigned parity,
           ID3D12Resource *base, ID3D12Resource *neural, ID3D12Resource *out)
{
    if (!active() || cl == nullptr || base == nullptr || neural == nullptr || out == nullptr ||
        base == neural || base == out || neural == out)
        return false;

    // NGX's UAV writes become SRV reads. tex_in (base) is already in that state.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = neural;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);

    ID3D12DescriptorHeap *heaps[1] = {g_heap};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    consts_t c{};
    c.w = g_w; c.h = g_h;
    c.intensity = g_intensity.load(std::memory_order_relaxed);
    c.guard = g_guard.load(std::memory_order_relaxed);
    c.filter = g_filter;
    c.detail = g_detail;
    cl->SetComputeRoot32BitConstants(0, 8, &c, 0);

    write_srv(cpu_slot(parity, 0, 0), base, g_fmt);
    write_srv(cpu_slot(parity, 0, 1), neural, g_fmt);
    write_uav(cpu_slot(parity, 0, 2), g_edit, DXGI_FORMAT_R16G16B16A16_FLOAT);
    cl->SetPipelineState(g_extract);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, 0));
    cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);

    b.Transition.pResource = g_edit;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);

    write_srv(cpu_slot(parity, 1, 0), base, g_fmt);
    write_srv(cpu_slot(parity, 1, 1), g_edit, DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, 1, 2), out, g_fmt);
    cl->SetPipelineState(g_filter == 2 ? g_compose_guided : g_compose);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, 1));
    if (g_filter == 2)
        cl->Dispatch((g_w + 15u) / 16u, (g_h + 15u) / 16u, 1u);
    else
        cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);

    D3D12_RESOURCE_BARRIER restore[2] = {};
    restore[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore[0].Transition.pResource = g_edit;
    restore[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    restore[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restore[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    restore[1] = restore[0];
    restore[1].Transition.pResource = neural;
    cl->ResourceBarrier(2, restore);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = out;
    cl->ResourceBarrier(1, &ub);

    ++g_applies;
    if (g_applies == 1 || g_applies % 600ull == 0)
    {
        char line[180];
        snprintf(line, sizeof line,
                 "[MGPU][PSYNC] synchronous Extract+Compose applied=%llu (same-frame base/neural, separate output).",
                 g_applies);
        mgpu::diag::info(line);
    }
    return true;
}

void set_intensity(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 3.0f) value = 3.0f;
    const float old = g_intensity.exchange(value, std::memory_order_relaxed);
    if (old != value)
    {
        char line[144];
        snprintf(line, sizeof line, "[MGPU][PSYNC] LIVE intensity %.2f -> %.2f", (double)old, (double)value);
        mgpu::diag::info(line);
    }
}

float intensity()
{
    return g_intensity.load(std::memory_order_relaxed);
}

void set_guard(float guard)
{
    if (guard < 0.0f) guard = 0.0f;
    if (guard > 1.0f) guard = 1.0f;
    const float old = g_guard.exchange(guard, std::memory_order_relaxed);
    if (old != guard)
    {
        char line[128];
        snprintf(line, sizeof line, "[MGPU][PSYNC] LIVE guard %.2f -> %.2f", (double)old, (double)guard);
        mgpu::diag::info(line);
    }
}

float guard()
{
    return g_guard.load(std::memory_order_relaxed);
}

void release()
{
    release_all();
}
} // namespace mgpu::psync
