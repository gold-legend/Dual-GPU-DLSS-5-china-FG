// MGPU Bridge - synchronous-landed Follow + Carry + Sample + Fill experiment.
//
// The Follow, Sample, Carry and Fill shader mechanism is adapted from
// ChefDavid0815/prism-nr 1.9.1 (native/prism-core/src/pipeline.cpp).
// This port deliberately has one synchronous landed edit, not Prism's async
// launch/land queues, second landed warp, or optional luma-guided reflection fix.
//
// MIT License
// Copyright (c) 2026 ChefZC
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "prism_carry.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::prism_carry
{
namespace
{
std::atomic<bool> g_active{false};
std::atomic<bool> g_valid{false};
ID3D12Device *g_dev = nullptr;
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_bake = nullptr;
ID3D12PipelineState *g_carry = nullptr;
ID3D12PipelineState *g_fill = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_history[2] = {};
ID3D12Resource *g_warp[2] = {};
ID3D12Resource *g_landed = nullptr;
ID3D12Resource *g_scratch = nullptr;
bool g_history_srv[2] = {};
bool g_warp_srv[2] = {};
bool g_landed_srv = false;
bool g_scratch_srv = false;
unsigned g_current = 0;
UINT g_inc = 0;
unsigned g_w = 0, g_h = 0;
unsigned g_dw = 0, g_dh = 0, g_mw = 0, g_mh = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT g_depth_fmt = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT g_motion_fmt = DXGI_FORMAT_UNKNOWN;
float g_depth_tolerance = 0.02f;
float g_blend = 0.5f;
unsigned long long g_bakes = 0, g_aligns = 0, g_fallbacks = 0;

constexpr UINT RING = 8;
constexpr UINT BAKE_BASE = 0;
constexpr UINT CARRY_BASE = 12;
constexpr UINT FILL_BASE = 24;
constexpr UINT SLOTS = 36; // eight SRVs + four UAVs per dispatch

struct consts_t
{
    unsigned w, h, mw, mh, dw, dh;
    float mv_x, mv_y, depth_tolerance, blend;
    unsigned pad[10];
};
static_assert(sizeof(consts_t) == 80, "20 root constants");

const char kShaders[] = R"(
cbuffer C : register(b0)
{
  uint gW; uint gH; uint gMW; uint gMH; uint gDW; uint gDH;
  float gMVX; float gMVY; float gDepthTolerance; float gBlend;
  uint gPad[10];
};
Texture2D<float4> Base : register(t0);
Texture2D<float4> Depth : register(t1);
Texture2D<float4> Motion : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float4> WarpHist : register(t4);
Texture2D<float4> LandedEdit : register(t5);
Texture2D<float4> Scratch : register(t6);
Texture2D<float4> Evaluated : register(t7);
RWTexture2D<float4> Out0 : register(u0);
RWTexture2D<float4> NewHistory : register(u1);
RWTexture2D<float4> NewWarp : register(u2);
RWTexture2D<float4> NewLandedOrScratch : register(u3);

uint2 GuidePixel(uint2 p, uint2 size)
{
  return min(uint2((float2(p) + 0.5) * float2(size) / float2(gW, gH)), size - 1);
}
float DepthAt(uint2 p)
{
  return Depth.Load(int3(GuidePixel(p, uint2(gDW, gDH)), 0)).x;
}
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

// Recover the exact, already guarded and graded edit shown on the evaluated
// frame. Keeping it apart from later current colour is what prevents freezing
// and re-presenting an entire old neural picture.
[numthreads(16, 16, 1)]
void CS_Bake(uint3 id : SV_DispatchThreadID)
{
  if (any(id.xy >= uint2(gW, gH))) return;
  const float4 b = Base[id.xy];
  const float4 n = Evaluated[id.xy];
  const float3 e = clamp(log2(max(Dec(n.rgb), 1.0 / 512.0) /
                              max(Dec(b.rgb), 1.0 / 512.0)), -3.0, 3.0);
  const float d = DepthAt(id.xy);
  NewLandedOrScratch[id.xy] = float4(e, d);
  NewHistory[id.xy] = float4(e, d);
  NewWarp[id.xy] = float4(0.0, 0.0, d, 1.0);
}

// Prism Follow: carry only edit + launch warp. Each bilinear tap must belong
// to the same device-depth layer. Motion is sampled at its own real extent.
void Follow(uint2 p, out float3 e, out float4 warp, out bool reached)
{
  const float2 mv = Motion.Load(int3(GuidePixel(p, uint2(gMW, gMH)), 0)).xy *
                    float2(gMVX, gMVY);
  const float2 pos = float2(p) + mv;
  const int2 b = int2(floor(pos));
  const float2 f = pos - float2(b);
  const int2 last = int2(gW, gH) - 1;
  const float d = DepthAt(p);
  const float tol = gDepthTolerance * max(abs(d), 1e-4);
  e = 0.0;
  float wsum = 0.0, qsum = 0.0;
  float3 q = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j)
  {
    [unroll] for (int i = 0; i <= 1; ++i)
    {
      const int2 tap = clamp(b + int2(i, j), int2(0, 0), last);
      const float4 hv = History.Load(int3(tap, 0));
      const float wt = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) *
                       (abs(hv.a - d) <= tol ? 1.0 : 0.0);
      e += hv.rgb * wt;
      wsum += wt;
      const float4 wp = WarpHist.Load(int3(tap, 0));
      const float wq = wt * wp.w;
      q += float3(float2(tap) + wp.xy, wp.z) * wq;
      qsum += wq;
    }
  }
  const bool inside = all(pos >= -0.5) && all(pos <= float2(last) + 0.5);
  reached = inside && wsum > 1e-3;
  e = reached ? e / wsum : 0.0;
  warp = (inside && qsum > 1e-3)
       ? float4(q.xy / qsum - float2(p), q.z / qsum, 1.0) : 0.0;
}

// Prism Sample: read the landed network edit directly through the accumulated
// launch warp; do not repeatedly resample a resampled whole frame.
bool SampleLanded(uint2 p, float4 warp, out float3 e)
{
  e = 0.0;
  if (warp.w < 0.5) return false;
  const float2 pos = float2(p) + warp.xy;
  const int2 b = int2(floor(pos));
  const float2 f = pos - float2(b);
  const int2 last = int2(gW, gH) - 1;
  const float tol = gDepthTolerance * max(abs(warp.z), 1e-4);
  float nsum = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j)
  {
    [unroll] for (int i = 0; i <= 1; ++i)
    {
      const int2 tap = clamp(b + int2(i, j), int2(0, 0), last);
      const float4 le = LandedEdit.Load(int3(tap, 0));
      const float wt = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) *
                       (abs(le.a - warp.z) <= tol ? 1.0 : 0.0);
      e += le.rgb * wt;
      nsum += wt;
    }
  }
  const bool inside = all(pos >= -0.5) && all(pos <= float2(last) + 0.5);
  if (!inside || nsum <= 1e-3) return false;
  e /= nsum;
  return true;
}

[numthreads(16, 16, 1)]
void CS_Carry(uint3 id : SV_DispatchThreadID)
{
  if (any(id.xy >= uint2(gW, gH))) return;
  float3 e;
  float4 warp;
  bool reached;
  Follow(id.xy, e, warp, reached);
  float3 n;
  if (SampleLanded(id.xy, warp, n))
  {
    e = reached ? lerp(e, n, gBlend) : n;
    reached = true;
  }
  const float d = DepthAt(id.xy);
  NewLandedOrScratch[id.xy] = float4(e, reached ? d : -1.0 - d);
  NewWarp[id.xy] = warp;
}

// Prism Fill: newly revealed pixels borrow only from the same depth layer.
// The final composition always uses this frame's native colour, never an old
// colour texture. NewHistory stores the post-Fill edit for the next hold.
static const uint kRings[5] = {2u, 4u, 8u, 16u, 32u};
[numthreads(16, 16, 1)]
void CS_Fill(uint3 id : SV_DispatchThreadID)
{
  if (any(id.xy >= uint2(gW, gH))) return;
  const float4 sc = Scratch.Load(int3(id.xy, 0));
  float3 e = sc.rgb;
  float d = sc.a;
  if (d < -0.5)
  {
    d = -1.0 - d;
    e = 0.0;
    const float tol = 2.0 * gDepthTolerance * max(abs(d), 1e-4);
    const int2 last = int2(gW, gH) - 1;
    [loop] for (uint r = 0; r < 5; ++r)
    {
      float3 acc = 0.0;
      float count = 0.0;
      [unroll] for (int k = 0; k < 8; ++k)
      {
        const int2 dir = int2(k < 4 ? (k & 1 ? -1 : 1) * (k < 2 ? 1 : 0) : (k & 1 ? -1 : 1),
                              k < 4 ? (k & 1 ? -1 : 1) * (k < 2 ? 0 : 1) : (k < 6 ? 1 : -1));
        const int2 tap = int2(id.xy) + dir * int(kRings[r]);
        if (any(tap < 0) || any(tap > last)) continue;
        const float4 t = Scratch.Load(int3(tap, 0));
        if (t.a >= -0.5 && abs(t.a - d) <= tol)
        {
          acc += t.rgb;
          count += 1.0;
        }
      }
      if (count > 0.0) { e = acc / count; break; }
    }
  }
  NewHistory[id.xy] = float4(e, d);
  const float4 b = Base[id.xy];
  if (all(abs(e) <= 1e-8)) Out0[id.xy] = b;
  else Out0[id.xy] = float4(Enc(Dec(b.rgb) * exp2(e)), b.a);
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
    g_valid.store(false, std::memory_order_release);
    IUnknown *u;
    u = g_fill; rel(u); g_fill = nullptr;
    u = g_carry; rel(u); g_carry = nullptr;
    u = g_bake; rel(u); g_bake = nullptr;
    u = g_rs; rel(u); g_rs = nullptr;
    u = g_heap; rel(u); g_heap = nullptr;
    u = g_scratch; rel(u); g_scratch = nullptr;
    u = g_landed; rel(u); g_landed = nullptr;
    for (unsigned i = 0; i < 2; ++i)
    {
        u = g_history[i]; rel(u); g_history[i] = nullptr;
        u = g_warp[i]; rel(u); g_warp[i] = nullptr;
        g_history_srv[i] = g_warp_srv[i] = false;
    }
    g_landed_srv = g_scratch_srv = false;
    g_dev = nullptr;
    g_w = g_h = g_dw = g_dh = g_mw = g_mh = 0;
    g_current = 0;
    g_bakes = g_aligns = g_fallbacks = 0;
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
    const char *x1 = mgpu::config::find(buf.data(), got, "PrismX1");
    const char *on = mgpu::config::find(buf.data(), got, "PrismCarry");
    if (x1 == nullptr || atoi(x1) != 1 || on == nullptr || atoi(on) != 1) return false;
    g_depth_tolerance = key_float(buf.data(), got, "PrismCarryDepthTol", 0.02f, 0.001f, 0.25f);
    g_blend = key_float(buf.data(), got, "PrismCarryBlend", 0.5f, 0.0f, 1.0f);
    return true;
}

DXGI_FORMAT colour_view_format(DXGI_FORMAT f)
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

DXGI_FORMAT depth_view_format(DXGI_FORMAT f)
{
    if (f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_D32_FLOAT) return DXGI_FORMAT_R32_FLOAT;
    if (f == DXGI_FORMAT_R16_TYPELESS || f == DXGI_FORMAT_D16_UNORM) return DXGI_FORMAT_R16_UNORM;
    return f;
}

DXGI_FORMAT motion_view_format(DXGI_FORMAT f)
{
    if (f == DXGI_FORMAT_R16G16_TYPELESS) return DXGI_FORMAT_R16G16_FLOAT;
    if (f == DXGI_FORMAT_R32G32_TYPELESS) return DXGI_FORMAT_R32G32_FLOAT;
    if (f == DXGI_FORMAT_R16G16B16A16_TYPELESS) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (f == DXGI_FORMAT_R32G32B32A32_TYPELESS) return DXGI_FORMAT_R32G32B32A32_FLOAT;
    return f;
}

bool make_tex(ID3D12Resource **out)
{
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
    return SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(out)));
}

bool build()
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = dc != nullptr
        ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][CARRY] D3DCompile unavailable; edit alignment stays OFF.");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 8;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 20;
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
        hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&g_rs));
    if (sig != nullptr) sig->Release();
    if (FAILED(hr)) return false;

    const char *entries[3] = {"CS_Bake", "CS_Carry", "CS_Fill"};
    ID3D12PipelineState **targets[3] = {&g_bake, &g_carry, &g_fill};
    for (unsigned pass = 0; pass < 3; ++pass)
    {
        ID3DBlob *cs = nullptr;
        hr = compile(kShaders, sizeof(kShaders) - 1u, "prism_carry", nullptr, nullptr,
                     entries[pass], "cs_5_0", 0, 0, &cs, &err);
        if (FAILED(hr) || cs == nullptr)
        {
            char line[1200];
            snprintf(line, sizeof line, "[MGPU][CARRY] %s compile failed: 0x%08X %s",
                     entries[pass], (unsigned)hr,
                     err != nullptr ? (const char *)err->GetBufferPointer() : "");
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
        hr = g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(targets[pass]));
        cs->Release();
        if (FAILED(hr)) return false;
    }

    if (!make_tex(&g_landed) || !make_tex(&g_scratch)) return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!make_tex(&g_history[i]) || !make_tex(&g_warp[i])) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = RING * SLOTS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) return false;
    g_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(unsigned parity, unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)((parity % RING) * SLOTS + slot) * g_inc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(unsigned parity, unsigned slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = g_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)((parity % RING) * SLOTS + slot) * g_inc;
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

bool valid_texture(ID3D12Resource *r, unsigned w, unsigned h)
{
    if (r == nullptr || w == 0 || h == 0) return false;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == w && d.Height == h;
}

void own_to(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, bool &is_srv, bool want_srv)
{
    if (is_srv == want_srv) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = is_srv ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                      : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = want_srv ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                       : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    is_srv = want_srv;
}

void constants(ID3D12GraphicsCommandList *cl, float mvx, float mvy)
{
    consts_t c{};
    c.w = g_w; c.h = g_h; c.mw = g_mw; c.mh = g_mh; c.dw = g_dw; c.dh = g_dh;
    c.mv_x = mvx; c.mv_y = mvy;
    c.depth_tolerance = g_depth_tolerance;
    c.blend = g_blend;
    cl->SetComputeRoot32BitConstants(0, 20, &c, 0);
}

void begin(ID3D12GraphicsCommandList *cl, float mvx, float mvy)
{
    ID3D12DescriptorHeap *heaps[1] = {g_heap};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    constants(cl, mvx, mvy);
}

void write_dummy_srvs(unsigned parity, unsigned base, ID3D12Resource *dummy)
{
    for (unsigned i = 0; i < 8; ++i)
        write_srv(cpu_slot(parity, base + i), dummy, DXGI_FORMAT_R16G16B16A16_FLOAT);
}
} // namespace

bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT colour_fmt,
         unsigned depth_width, unsigned depth_height, DXGI_FORMAT depth_fmt,
         unsigned motion_width, unsigned motion_height, DXGI_FORMAT motion_fmt)
{
    release_all();
    if (dev == nullptr || width == 0 || height == 0 || depth_width == 0 || depth_height == 0 ||
        motion_width == 0 || motion_height == 0 || !read_keys())
        return false;
    g_dev = dev;
    g_w = width; g_h = height;
    g_dw = depth_width; g_dh = depth_height;
    g_mw = motion_width; g_mh = motion_height;
    g_fmt = colour_view_format(colour_fmt);
    g_depth_fmt = depth_view_format(depth_fmt);
    g_motion_fmt = motion_view_format(motion_fmt);
    if (!build())
    {
        release_all();
        return false;
    }
    g_active.store(true, std::memory_order_relaxed);
    char line[440];
    snprintf(line, sizeof line,
             "[MGPU][CARRY] per-frame edit alignment ARMED: native=%ux%u depth=%ux%u fmt=%d "
             "motion=%ux%u fmt=%d tolerance=%.3f blend=%.2f. Bake on evaluated frames; "
             "Follow+Carry/Sample+Fill on held frames. No NGX evaluation and no old-colour warp.",
             g_w, g_h, g_dw, g_dh, (int)g_depth_fmt, g_mw, g_mh, (int)g_motion_fmt,
             (double)g_depth_tolerance, (double)g_blend);
    mgpu::diag::info(line);
    return true;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

bool ready()
{
    return active() && g_valid.load(std::memory_order_acquire);
}

bool bake(ID3D12GraphicsCommandList *cl, unsigned parity,
          ID3D12Resource *base, ID3D12Resource *evaluated, ID3D12Resource *depth)
{
    if (!active() || cl == nullptr || base == evaluated ||
        !valid_texture(base, g_w, g_h) || !valid_texture(evaluated, g_w, g_h) ||
        !valid_texture(depth, g_dw, g_dh))
        return false;
    const unsigned next = g_current ^ 1u;
    own_to(cl, g_history[next], g_history_srv[next], false);
    own_to(cl, g_warp[next], g_warp_srv[next], false);
    own_to(cl, g_landed, g_landed_srv, false);
    D3D12_RESOURCE_BARRIER evaluated_state{};
    evaluated_state.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    evaluated_state.Transition.pResource = evaluated;
    evaluated_state.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    evaluated_state.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    evaluated_state.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &evaluated_state);
    begin(cl, 1.0f, 1.0f);
    write_dummy_srvs(parity, BAKE_BASE, g_landed);
    write_srv(cpu_slot(parity, BAKE_BASE + 0), base, g_fmt);
    write_srv(cpu_slot(parity, BAKE_BASE + 1), depth, g_depth_fmt);
    for (unsigned i = 2; i <= 6; ++i) // landed is UAV here, never an unused SRV too
        write_srv(cpu_slot(parity, BAKE_BASE + i), base, g_fmt);
    write_srv(cpu_slot(parity, BAKE_BASE + 7), evaluated, g_fmt);
    write_uav(cpu_slot(parity, BAKE_BASE + 8), g_history[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, BAKE_BASE + 9), g_history[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, BAKE_BASE + 10), g_warp[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, BAKE_BASE + 11), g_landed, DXGI_FORMAT_R16G16B16A16_FLOAT);
    cl->SetPipelineState(g_bake);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, BAKE_BASE));
    cl->Dispatch((g_w + 15u) / 16u, (g_h + 15u) / 16u, 1u);
    evaluated_state.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    evaluated_state.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &evaluated_state);
    g_current = next;
    g_valid.store(true, std::memory_order_release);
    ++g_bakes;
    if (g_bakes == 1 || g_bakes % 600ull == 0ull)
    {
        char line[220];
        snprintf(line, sizeof line,
                 "[MGPU][CARRY] evaluated-frame edit baked=%llu; aligned outputs=%llu; fallbacks=%llu.",
                 g_bakes, g_aligns, g_fallbacks);
        mgpu::diag::info(line);
    }
    return true;
}

bool align(ID3D12GraphicsCommandList *cl, unsigned parity,
           ID3D12Resource *base, ID3D12Resource *depth, ID3D12Resource *motion,
           ID3D12Resource *out, float motion_scale_x, float motion_scale_y)
{
    if (!ready() || cl == nullptr || base == out || depth == out || motion == out ||
        !valid_texture(base, g_w, g_h) || !valid_texture(out, g_w, g_h) ||
        !valid_texture(depth, g_dw, g_dh) || !valid_texture(motion, g_mw, g_mh))
        return false;
    const unsigned previous = g_current;
    const unsigned next = previous ^ 1u;
    own_to(cl, g_history[previous], g_history_srv[previous], true);
    own_to(cl, g_warp[previous], g_warp_srv[previous], true);
    own_to(cl, g_landed, g_landed_srv, true);
    own_to(cl, g_history[next], g_history_srv[next], false);
    own_to(cl, g_warp[next], g_warp_srv[next], false);
    own_to(cl, g_scratch, g_scratch_srv, false);
    begin(cl, motion_scale_x, motion_scale_y);

    write_dummy_srvs(parity, CARRY_BASE, g_landed);
    write_srv(cpu_slot(parity, CARRY_BASE + 0), base, g_fmt);
    write_srv(cpu_slot(parity, CARRY_BASE + 1), depth, g_depth_fmt);
    write_srv(cpu_slot(parity, CARRY_BASE + 2), motion, g_motion_fmt);
    write_srv(cpu_slot(parity, CARRY_BASE + 3), g_history[previous], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_srv(cpu_slot(parity, CARRY_BASE + 4), g_warp[previous], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, CARRY_BASE + 8), g_scratch, DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, CARRY_BASE + 9), g_history[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, CARRY_BASE + 10), g_warp[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, CARRY_BASE + 11), g_scratch, DXGI_FORMAT_R16G16B16A16_FLOAT);
    cl->SetPipelineState(g_carry);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, CARRY_BASE));
    cl->Dispatch((g_w + 15u) / 16u, (g_h + 15u) / 16u, 1u);

    own_to(cl, g_scratch, g_scratch_srv, true);
    write_dummy_srvs(parity, FILL_BASE, g_landed);
    write_srv(cpu_slot(parity, FILL_BASE + 0), base, g_fmt);
    write_srv(cpu_slot(parity, FILL_BASE + 6), g_scratch, DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, FILL_BASE + 8), out, g_fmt);
    write_uav(cpu_slot(parity, FILL_BASE + 9), g_history[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, FILL_BASE + 10), g_warp[next], DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, FILL_BASE + 11), out, g_fmt); // unused u3; scratch is SRV here
    cl->SetPipelineState(g_fill);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, FILL_BASE));
    cl->Dispatch((g_w + 15u) / 16u, (g_h + 15u) / 16u, 1u);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = out;
    cl->ResourceBarrier(1, &ub);
    g_current = next;
    ++g_aligns;
    if (g_aligns == 1 || g_aligns % 300ull == 0ull)
    {
        char line[260];
        snprintf(line, sizeof line,
                 "[MGPU][CARRY] aligned outputs=%llu (NOT NGX evaluations); baked=%llu; fallbacks=%llu; MV scale=%.3f/%.3f.",
                 g_aligns, g_bakes, g_fallbacks, (double)motion_scale_x, (double)motion_scale_y);
        mgpu::diag::info(line);
    }
    return true;
}

void reset()
{
    const bool was = g_valid.exchange(false, std::memory_order_acq_rel);
    if (was) mgpu::diag::info("[MGPU][CARRY] edit history invalidated; next eligible evaluate will bake a fresh field.");
}

void note_fallback()
{
    ++g_fallbacks;
    if (g_fallbacks <= 3 || g_fallbacks % 300ull == 0ull)
    {
        char line[200];
        snprintf(line, sizeof line,
                 "[MGPU][CARRY] alignment fallback=%llu; previous valid output held for this frame.",
                 g_fallbacks);
        mgpu::diag::warn(line);
    }
}

void release()
{
    release_all();
}
}
