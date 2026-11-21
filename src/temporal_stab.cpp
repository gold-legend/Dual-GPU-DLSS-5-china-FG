// MGPU Bridge - same-size residual/history colour stabiliser from Prism 1.1.
// The HLSL below is the golden binary's recovered kernel, verbatim in math and
// thresholds. MotionValid stays zero: the zero-vector configuration uses
// same-pixel history, exactly as the golden fallback does.

#include "temporal_stab.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::temporal_stab
{
namespace
{
std::atomic<bool> g_active{false};
ID3D12Device *g_dev = nullptr;
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_pso = nullptr;
ID3D12PipelineState *g_pso_resid = nullptr;   // R402: residual-domain mode
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_history[2] = {nullptr, nullptr};
ID3D12Resource *g_dummy_motion = nullptr;
ID3D12Resource *g_out = nullptr;              // R402: composite target in residual mode
UINT g_inc = 0;
unsigned g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
float g_strength = 0.0f;
unsigned g_adaptive = 1;
bool g_residual_mode = false;                 // R402: TemporalResidual=1
float g_delta = 0.10f;                        // R402: TemporalResidDelta (relative rate limit)
bool g_history_valid = false;
unsigned long long g_frame = 0;
unsigned g_slot = 0;
unsigned long long g_applies = 0;

constexpr UINT RING = 8;
constexpr UINT DESCRIPTORS_PER_SLOT = 5; // t0 current, t1 history, t2 motion/base, u0 out/history, u1 composite

struct consts_t
{
    unsigned w, h;
    float strength;
    unsigned adaptive, history_valid, motion_valid;
    float delta_limit;   // R402: residual rate limit (golden mode ignores it)
    unsigned pad1;
};
static_assert(sizeof(consts_t) == 32, "eight root constants");

const char kShader[] = R"(
cbuffer C : register(b0) {
  uint2 Extent; float Strength; uint Adaptive; uint HistoryValid; uint MotionValid;
};
Texture2D<float4> Current : register(t0);
Texture2D<float4> History : register(t1);
Texture2D<float2> HistoryMotion : register(t2);
RWTexture2D<float4> Output : register(u0);

float3 ReadHistory(float2 pos) {
  // AMD flow points from the current pixel to its previous-frame source.
  // Bilinear taps avoid turning sub-pixel camera motion into temporal stepping.
  pos = clamp(pos, float2(0.0, 0.0), float2(Extent) - 1.0);
  const int2 b = int2(floor(pos));
  const int2 b1 = min(b + 1, int2(Extent) - 1);
  const float2 f = frac(pos);
  const float3 a = History.Load(int3(b, 0)).rgb;
  const float3 c = History.Load(int3(int2(b1.x, b.y), 0)).rgb;
  const float3 d = History.Load(int3(int2(b.x, b1.y), 0)).rgb;
  const float3 e = History.Load(int3(b1, 0)).rgb;
  return lerp(lerp(a, c, f.x), lerp(d, e, f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Extent)) return;
  const int2 p = int2(id.xy);
  const float4 c = Current.Load(int3(p, 0));
  if (HistoryValid == 0 || Strength <= 0.0) { Output[p] = c; return; }
  float3 lo = c.rgb, hi = c.rgb;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
      int2 q = clamp(p + int2(x, y), int2(0, 0), int2(Extent) - 1);
      float3 v = Current.Load(int3(q, 0)).rgb;
      lo = min(lo, v); hi = max(hi, v);
    }
  const float2 previousPos = MotionValid
      ? float2(p) + HistoryMotion.Load(int3(p, 0)) : float2(p);
  const float3 h = ReadHistory(previousPos);
  const float3 limited = clamp(h, lo, hi);
  float3 delta = abs(c.rgb - limited);
  float3 range = max(0.04, max(abs(c.rgb), abs(limited)));
  float difference = max(delta.r / range.r, max(delta.g / range.g, delta.b / range.b));
  // Reproject first, then reject only the remaining local mismatch. This avoids
  // the old same-pixel history rejection/recovery pumping on moving objects.
  float keep = Adaptive ? 1.0 - smoothstep(0.18, 0.72, difference) : 1.0;
  float w = clamp(Strength, 0.0, 0.95) * keep;
  Output[p] = float4(lerp(c.rgb, limited, w), c.a);
}
)";

// R402: residual-domain stabiliser. The colour base is ALWAYS the current game
// frame; the neural output contributes only its residual (NR - base), and it is
// the RESIDUAL that is rate-limited and low-passed over time. A held/stale
// neural colour therefore never reaches the screen as colour - only as an
// enhancement delta on top of the live frame. Mirrors Veyra's documented
// "residual extrapolation + temporal stability" model for video/capture input.
const char kShaderResid[] = R"(
cbuffer C : register(b0) {
  uint2 Extent; float Strength; uint Adaptive; uint HistoryValid; uint MotionValid;
  float DeltaLimit; uint Pad1;
};
Texture2D<float4> Current : register(t0);   // nr_final (native, float)
Texture2D<float4> HistRes : register(t1);   // previous filtered residual
Texture2D<float4> Base : register(t2);      // CURRENT game colour, same extent
RWTexture2D<float4> OutRes : register(u0);  // filtered residual -> next history
RWTexture2D<float4> OutCol : register(u1);  // base + residual -> copied to nr_final

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Extent)) return;
  const int2 p = int2(id.xy);
  const float4 c = Current.Load(int3(p, 0));
  const float3 base = Base.Load(int3(p, 0)).rgb;
  const float3 rcur = c.rgb - base;
  float3 rf = rcur;
  if (HistoryValid != 0 && Strength > 0.0) {
    const float3 rp = HistRes.Load(int3(p, 0)).rgb;
    const float3 delta = rcur - rp;
    // Per-frame residual rate limit, scaled by the local magnitudes so dark
    // pixels are not over-clamped and bright ones cannot jump. Boiling is a
    // small high-frequency residual oscillation: this is exactly what it kills.
    const float3 lim = DeltaLimit * (0.05 + abs(rp) + 0.25 * abs(base));
    const float3 limited = rp + clamp(delta, -lim, lim);
    // Adaptive escape: a LARGE residual change is new content / a cut, not
    // noise - drop the limiter there so enhancement never lags a scene change.
    const float3 d = abs(rcur - limited);
    const float3 range = max(0.04, max(abs(rcur), abs(limited)) + 0.05);
    const float difference = max(d.r / range.r, max(d.g / range.g, d.b / range.b));
    const float keep = Adaptive ? 1.0 - smoothstep(0.18, 0.72, difference) : 1.0;
    const float w = clamp(Strength, 0.0, 0.95) * keep;
    rf = lerp(rcur, limited, w);
  }
  OutRes[p] = float4(rf, 1.0);
  OutCol[p] = float4(base + rf, c.a);
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
    u = g_pso; rel(u); g_pso = nullptr;
    u = g_pso_resid; rel(u); g_pso_resid = nullptr;
    u = g_rs; rel(u); g_rs = nullptr;
    u = g_heap; rel(u); g_heap = nullptr;
    u = g_history[0]; rel(u); g_history[0] = nullptr;
    u = g_history[1]; rel(u); g_history[1] = nullptr;
    u = g_dummy_motion; rel(u); g_dummy_motion = nullptr;
    u = g_out; rel(u); g_out = nullptr;
    g_dev = nullptr;
    g_w = g_h = 0;
    // NOTE: g_residual_mode / g_delta / g_strength are read_keys() products and
    // arm() calls release_all() AFTER read_keys() - do not clear them here.
    g_history_valid = false;
    g_frame = 0;
    g_slot = 0;
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
    g_strength = key_float(buf.data(), got, "TemporalStrength", 0.0f, 0.0f, 100.0f) / 100.0f;
    if (g_strength <= 0.0f) return false;
    g_adaptive = (unsigned)key_float(buf.data(), got, "TemporalMotionAdaptive", 1.0f, 0.0f, 1.0f);
    // R402: opt-in residual domain. Default 0 keeps the golden colour kernel.
    // (The float-format guard lives in arm(), after g_fmt is known.)
    g_residual_mode = key_float(buf.data(), got, "TemporalResidual", 0.0f, 0.0f, 1.0f) > 0.5f;
    g_delta = key_float(buf.data(), got, "TemporalResidDelta", 0.10f, 0.01f, 1.0f);
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

HRESULT make_texture(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt,
                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                     ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                        IID_PPV_ARGS(out));
}

void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}

void uav_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(unsigned slot, unsigned index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)(slot * DESCRIPTORS_PER_SLOT + index) * g_inc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(unsigned slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = g_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)(slot * DESCRIPTORS_PER_SLOT) * g_inc;
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

bool build(ID3D12Device *dev)
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = dc != nullptr
        ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][TEMPORAL] D3DCompile is unavailable; stabiliser stays off.");
        return false;
    }

    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1u, "temporal_stab", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        char line[1000];
        snprintf(line, sizeof line, "[MGPU][TEMPORAL] kernel compile failed: 0x%08X %s",
                 (unsigned)hr, err != nullptr ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::error(line);
        if (err != nullptr) err->Release();
        if (cs != nullptr) cs->Release();
        return false;
    }
    if (err != nullptr) { err->Release(); err = nullptr; }

    ID3DBlob *csr = nullptr;   // R402: residual kernel, only in residual mode
    if (g_residual_mode)
    {
        hr = compile(kShaderResid, sizeof(kShaderResid) - 1u, "temporal_stab_resid", nullptr,
                     nullptr, "main", "cs_5_0", 0, 0, &csr, &err);
        if (FAILED(hr) || csr == nullptr)
        {
            char line[1000];
            snprintf(line, sizeof line, "[MGPU][TEMPORAL][R402] residual kernel compile failed: 0x%08X %s",
                     (unsigned)hr, err != nullptr ? (const char *)err->GetBufferPointer() : "");
            mgpu::diag::error(line);
            if (err != nullptr) err->Release();
            if (csr != nullptr) csr->Release();
            cs->Release();
            return false;
        }
        if (err != nullptr) { err->Release(); err = nullptr; }
    }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;   // R402: u0 history/composite, u1 residual composite (unused by golden)
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
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
    ID3DBlob *sig = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err != nullptr) { err->Release(); err = nullptr; }
    if (SUCCEEDED(hr))
        hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      IID_PPV_ARGS(&g_rs));
    if (sig != nullptr) sig->Release();
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    }
    cs->Release();
    if (SUCCEEDED(hr) && g_residual_mode)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = csr->GetBufferPointer();
        pd.CS.BytecodeLength = csr->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso_resid));
    }
    if (csr != nullptr) csr->Release();
    if (FAILED(hr))
    {
        char line[200];
        snprintf(line, sizeof line, "[MGPU][TEMPORAL] root signature / PSO failed: 0x%08X",
                 (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }

    hr = make_texture(dev, g_w, g_h, g_fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g_history[0]);
    if (SUCCEEDED(hr))
        hr = make_texture(dev, g_w, g_h, g_fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g_history[1]);
    if (SUCCEEDED(hr))
        hr = make_texture(dev, 1, 1, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &g_dummy_motion);
    if (SUCCEEDED(hr) && g_residual_mode)
        hr = make_texture(dev, g_w, g_h, g_fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g_out);
    if (FAILED(hr)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = RING * DESCRIPTORS_PER_SLOT;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr)) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
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
    if (g_residual_mode && g_fmt != DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        // Residuals are signed; an UNORM history cannot store them. Refuse the
        // mode loudly rather than silently clipping every negative delta.
        mgpu::diag::error("[MGPU][TEMPORAL][R402] TemporalResidual=1 needs an R16G16B16A16_FLOAT "
                          "nr_final; this stream is not float - using the golden colour kernel.");
        g_residual_mode = false;
    }
    if (!build(dev))
    {
        release_all();
        return false;
    }
    g_active.store(true, std::memory_order_relaxed);
    char line[360];
    if (g_residual_mode)
        snprintf(line, sizeof line,
                 "[MGPU][TEMPORAL][R402] RESIDUAL-domain stabiliser armed: %ux%u strength=%.0f%% "
                 "delta=%.3f adaptive=%u; base = CURRENT game colour every evaluation, the neural "
                 "output contributes only a rate-limited residual (no stale neural colour on screen)",
                 g_w, g_h, (double)(g_strength * 100.0f), (double)g_delta, g_adaptive);
    else
        snprintf(line, sizeof line,
                 "[MGPU][TEMPORAL] same-size residual/history colour stabiliser armed: %ux%u "
                 "strength=%.0f%% motion-adaptive=%u flow=0; no SR/upscaling pass",
                 g_w, g_h, (double)(g_strength * 100.0f), g_adaptive);
    mgpu::diag::info(line);
    mgpu::diag::info("[MGPU][AMD-OF] colour flow unavailable; temporal falls back to same-pixel history");
    return true;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

void apply(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_final, ID3D12Resource *base)
{
    if (!active() || cl == nullptr || nr_final == nullptr) return;
    const bool resid = g_residual_mode && g_pso_resid != nullptr && g_out != nullptr && base != nullptr;

    const unsigned out_index = (unsigned)(g_frame & 1ull);
    ID3D12Resource *output = g_history[out_index];
    ID3D12Resource *previous = g_history[1u - out_index];

    // Invariant at entry: the previous output is UAV, the other history is SRV.
    // On the first frame both are UAV. Output is never also History in a dispatch.
    transition(cl, previous, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (g_history_valid)
        transition(cl, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cl, nr_final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const unsigned slot = g_slot;
    g_slot = (g_slot + 1u) % RING;
    write_srv(cpu_slot(slot, 0), nr_final, g_fmt);
    write_srv(cpu_slot(slot, 1), previous, g_fmt);
    if (resid)
        write_srv(cpu_slot(slot, 2), base, g_fmt);   // R402: CURRENT game colour (PWARP binds it the same way, no transitions)
    else
        write_srv(cpu_slot(slot, 2), g_dummy_motion, DXGI_FORMAT_R32G32_FLOAT);
    write_uav(cpu_slot(slot, 3), output, g_fmt);
    write_uav(cpu_slot(slot, 4), g_out, g_fmt);      // null in golden mode: the golden kernel has no u1

    consts_t c{};
    c.w = g_w;
    c.h = g_h;
    c.strength = g_strength;
    c.adaptive = g_adaptive;
    c.history_valid = g_history_valid ? 1u : 0u;
    c.motion_valid = 0u;
    c.delta_limit = resid ? g_delta : 0.0f;

    ID3D12DescriptorHeap *heaps[] = {g_heap};
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(resid ? g_pso_resid : g_pso);
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRoot32BitConstants(0, 8, &c, 0);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(slot));
    cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);
    uav_barrier(cl, output);
    if (resid) uav_barrier(cl, g_out);

    // Present still consumes nr_final. Copy the stabilised output back once;
    // the other history texture remains the previous frame for the next call.
    // R402: in residual mode the colour copy comes from g_out (base+residual);
    // `output` keeps the filtered residual as next frame's history.
    ID3D12Resource *const colour = resid ? g_out : output;
    transition(cl, colour, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cl, nr_final, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(nr_final, colour);
    transition(cl, nr_final, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cl, colour, D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_history_valid = true;
    ++g_frame;
    ++g_applies;
    if (g_applies == 1ull || g_applies % 300ull == 0ull)
    {
        char line[160];
        snprintf(line, sizeof line, "[MGPU][TEMPORAL][LIVE] applies=%llu history=%ux%u resid=%u",
                 g_applies, g_w, g_h, resid ? 1u : 0u);
        mgpu::diag::info(line);
    }
}

void release()
{
    release_all();
}
} // namespace mgpu::temporal_stab
