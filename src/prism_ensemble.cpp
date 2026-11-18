// PRISM three-pass same-frame ensemble.
//
// Pass 1 is copied before pass 3 reuses tex_out. All three NGX handles still
// own independent temporal histories. The composite is deliberately memoryless:
// median same-frame RGB residuals reject a single divergent answer, then a
// current-frame 3x3 luminance neighbourhood clips isolated bright outliers.

#include "prism_ensemble.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::ensemble
{
namespace
{
std::atomic<bool> g_active{false};
ID3D12Device *g_dev = nullptr;
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_pso = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_first = nullptr;
ID3D12Resource *g_output = nullptr;
UINT g_inc = 0;
unsigned g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
bool g_snapshot = false;
unsigned long long g_composites = 0;

constexpr UINT kSrvCount = 4;
constexpr UINT kDescriptorCount = 5;

struct consts_t
{
    unsigned w, h, pad0, pad1;
};

const char kShader[] = R"(
cbuffer C : register(b0)
{
  uint gW, gH, gPad0, gPad1;
};
Texture2D<float4> gOriginal : register(t0);
Texture2D<float4> gFirst : register(t1);
Texture2D<float4> gSecond : register(t2);
Texture2D<float4> gThird : register(t3);
RWTexture2D<float4> gOutput : register(u0);

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float Median3(float a, float b, float c)
{
  return max(min(a, b), min(max(a, b), c));
}
float3 Median3(float3 a, float3 b, float3 c)
{
  return float3(Median3(a.r, b.r, c.r),
                Median3(a.g, b.g, c.g),
                Median3(a.b, b.b, c.b));
}
int2 ClampPx(int2 p)
{
  return clamp(p, int2(0, 0), int2(gW, gH) - int2(1, 1));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH) return;
  const int2 p = int2(id.xy);
  const float3 original = max(gOriginal.Load(int3(p, 0)).rgb, 0.0);
  const float3 a = max(gFirst.Load(int3(p, 0)).rgb, 0.0);
  const float3 b = max(gSecond.Load(int3(p, 0)).rgb, 0.0);
  const float3 c = max(gThird.Load(int3(p, 0)).rgb, 0.0);

  // A single pass cannot pull the result past the other two. No old picture,
  // velocity, exponential blend or feedback enters this composite.
  float3 result = original + Median3(a - original, b - original, c - original);
  result = max(result, 0.0);

  float loY = 3.4e38;
  float hiY = -3.4e38;
  [unroll] for (int y = -1; y <= 1; ++y)
  {
    [unroll] for (int x = -1; x <= 1; ++x)
    {
      const float3 n = max(gOriginal.Load(int3(ClampPx(p + int2(x, y)), 0)).rgb, 0.0);
      const float ly = log2(max(Luma(n), 1.0 / 512.0));
      loY = min(loY, ly);
      hiY = max(hiY, ly);
    }
  }
  const float ry = log2(max(Luma(result), 1.0 / 512.0));
  const float clampedY = clamp(ry, loY - 0.10, hiY + 0.10);
  if (abs(clampedY - ry) > 1e-5)
    result *= exp2(clampedY - ry);

  gOutput[p] = float4(max(result, 0.0), 1.0);
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
    u = g_output; rel(u); g_output = nullptr;
    u = g_first; rel(u); g_first = nullptr;
    u = g_heap; rel(u); g_heap = nullptr;
    u = g_pso; rel(u); g_pso = nullptr;
    u = g_rs; rel(u); g_rs = nullptr;
    g_dev = nullptr;
    g_snapshot = false;
}

bool ini_enabled()
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", path);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;
    const char *v = mgpu::config::find(buf.data(), got, "PrismEnsemble");
    return v != nullptr && atoi(v) == 1;
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

bool make_texture(ID3D12Device *dev, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES state,
                  ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = g_w;
    rd.Height = g_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  state, nullptr, IID_PPV_ARGS(out)));
}

bool build(ID3D12Device *dev)
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = dc != nullptr
        ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][ENSEMBLE] D3DCompile unavailable; ensemble OFF.");
        return false;
    }
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1u, "prism_ensemble", nullptr,
                         nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        char line[900];
        snprintf(line, sizeof line, "[MGPU][ENSEMBLE] shader compile failed: 0x%08X %s",
                 (unsigned)hr, err != nullptr ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::error(line);
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kSrvCount;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = kSrvCount;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 4;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 2;
    rp[1].DescriptorTable.pDescriptorRanges = ranges;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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
    if (FAILED(hr))
    {
        char line[180];
        snprintf(line, sizeof line, "[MGPU][ENSEMBLE] root/PSO failed: 0x%08X", (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kDescriptorCount * 2u;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!make_texture(dev, g_fmt, D3D12_RESOURCE_STATE_COPY_DEST, &g_first) ||
        !make_texture(dev, g_fmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g_output))
    {
        mgpu::diag::error("[MGPU][ENSEMBLE] native-size texture allocation failed.");
        return false;
    }
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(unsigned parity, unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += ((SIZE_T)(parity & 1u) * kDescriptorCount + slot) * g_inc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(unsigned parity)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = g_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)(parity & 1u) * kDescriptorCount * g_inc;
    return h;
}

void write_srv(unsigned parity, unsigned slot, ID3D12Resource *r)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = g_fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(r, &d, cpu_slot(parity, slot));
}

void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}
} // namespace

bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt)
{
    if (dev == nullptr || !ini_enabled()) return false;
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
    char line[320];
    snprintf(line, sizeof line,
             "[MGPU][ENSEMBLE] ON at %ux%u fmt=%d: three independent same-frame NR inputs; "
             "median residual + current-frame 3x3 luminance guard; no cross-frame composite history.",
             g_w, g_h, (int)g_fmt);
    mgpu::diag::info(line);
    return true;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

void begin_frame()
{
    g_snapshot = false;
}

bool capture_first(ID3D12GraphicsCommandList *cl, ID3D12Resource *first)
{
    if (!active() || cl == nullptr || first == nullptr || g_first == nullptr) return false;
    barrier(cl, first, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(g_first, first);
    barrier(cl, first, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_snapshot = true;
    return true;
}

ID3D12Resource *compose(ID3D12GraphicsCommandList *cl, unsigned parity,
                        ID3D12Resource *original, ID3D12Resource *second,
                        ID3D12Resource *third)
{
    if (!active() || !g_snapshot || cl == nullptr || original == nullptr ||
        second == nullptr || third == nullptr)
        return nullptr;

    barrier(cl, g_first, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(cl, second, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(cl, third, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    write_srv(parity, 0, original);
    write_srv(parity, 1, g_first);
    write_srv(parity, 2, second);
    write_srv(parity, 3, third);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = g_fmt;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dev->CreateUnorderedAccessView(g_output, nullptr, &ud, cpu_slot(parity, 4));

    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(g_pso);
    const consts_t c{g_w, g_h, 0u, 0u};
    cl->SetComputeRoot32BitConstants(0, 4, &c, 0);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity));
    cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = g_output;
    cl->ResourceBarrier(1, &ub);
    barrier(cl, third, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, second, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, g_first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);

    ++g_composites;
    if (g_composites == 1u || g_composites % 120u == 0u)
    {
        char line[200];
        snprintf(line, sizeof line,
                 "[MGPU][ENSEMBLE] composites=%llu: P1/P2/P3 same original, snapshot+fuse recorded.",
                 g_composites);
        mgpu::diag::info(line);
    }
    return g_output;
}

void release()
{
    release_all();
}
} // namespace mgpu::ensemble
