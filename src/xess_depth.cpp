// XeSS-FG requires depth and motion vectors on the same pixel grid.
// This explicit point-resize preserves depth layers; it is not a colour blit.
#include "xess_depth.hpp"
#include "diag.hpp"

#include <cstdio>

namespace mgpu::xess_depth
{
namespace
{
constexpr unsigned SLOTS = 2;
constexpr unsigned RING = 8;
ID3D12Device *g_dev = nullptr;
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_pso = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_out[SLOTS] = {};
bool g_srv[SLOTS] = {};
unsigned g_sw = 0, g_sh = 0, g_tw = 0, g_th = 0;
DXGI_FORMAT g_sf = DXGI_FORMAT_UNKNOWN;
UINT g_inc = 0;

struct constants { unsigned sw, sh, tw, th; };
const char kShader[] = R"(
cbuffer C : register(b0) { uint SW; uint SH; uint TW; uint TH; };
Texture2D<float> Source : register(t0);
RWTexture2D<float> Target : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (any(id.xy >= uint2(TW, TH))) return;
  uint2 p = min(uint2((float2(id.xy) + 0.5) * float2(SW, SH) / float2(TW, TH)),
                uint2(SW - 1, SH - 1));
  Target[id.xy] = Source.Load(int3(p, 0));
}
)";

using compile_fn = HRESULT (WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                     ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                     ID3DBlob **, ID3DBlob **);

void release_all()
{
    for (unsigned i = 0; i < SLOTS; ++i)
    {
        if (g_out[i] != nullptr) g_out[i]->Release();
        g_out[i] = nullptr;
        g_srv[i] = false;
    }
    if (g_heap != nullptr) g_heap->Release();
    if (g_pso != nullptr) g_pso->Release();
    if (g_rs != nullptr) g_rs->Release();
    g_heap = nullptr; g_pso = nullptr; g_rs = nullptr; g_dev = nullptr;
    g_sw = g_sh = g_tw = g_th = 0;
    g_sf = DXGI_FORMAT_UNKNOWN;
}

bool build(ID3D12Device *dev, const D3D12_RESOURCE_DESC &sd,
           unsigned tw, unsigned th)
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc != nullptr ? (compile_fn)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][XESS-DEPTH] D3DCompile unavailable; FG stays off for these frames.");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 2;
    rp[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (err != nullptr) { err->Release(); err = nullptr; }
    if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                                      blob->GetBufferSize(), IID_PPV_ARGS(&g_rs));
    if (blob != nullptr) { blob->Release(); blob = nullptr; }
    if (FAILED(hr)) return false;
    hr = compile(kShader, sizeof(kShader) - 1, "xess_depth", nullptr, nullptr,
                 "main", "cs_5_0", 0, 0, &blob, &err);
    if (FAILED(hr) || blob == nullptr)
    {
        char line[1000];
        snprintf(line, sizeof line, "[MGPU][XESS-DEPTH] shader compile failed 0x%08X: %s",
                 (unsigned)hr, err != nullptr ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::error(line);
        if (err != nullptr) err->Release();
        if (blob != nullptr) blob->Release();
        return false;
    }
    if (err != nullptr) err->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs;
    pd.CS.pShaderBytecode = blob->GetBufferPointer();
    pd.CS.BytecodeLength = blob->GetBufferSize();
    hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    blob->Release();
    if (FAILED(hr)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = RING * 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = tw; rd.Height = th;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (unsigned i = 0; i < SLOTS; ++i)
    {
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                       IID_PPV_ARGS(&g_out[i])))) return false;
        g_srv[i] = true; // creation state must match the next explicit barrier
    }
    g_dev = dev; g_sw = (unsigned)sd.Width; g_sh = sd.Height;
    g_tw = tw; g_th = th; g_sf = sd.Format;
    char line[400];
    snprintf(line, sizeof line,
             "[MGPU][XESS-DEPTH] explicit point-resize armed: depth=%ux%u fmt=%d -> %ux%u R32_FLOAT, two independent slots. Required by Intel: depth/MV must share one extent.",
             g_sw, g_sh, (int)g_sf, g_tw, g_th);
    mgpu::diag::info(line);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(unsigned parity, unsigned index)
{
    auto h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)((parity % RING) * 2 + index) * g_inc;
    return h;
}
}

bool prepare(ID3D12Device *device, ID3D12GraphicsCommandList *cl,
             ID3D12Resource *source, unsigned slot,
             unsigned target_width, unsigned target_height,
             ID3D12Resource **out)
{
    if (out == nullptr) return false;
    *out = nullptr;
    if (device == nullptr || cl == nullptr || source == nullptr || slot >= SLOTS ||
        target_width == 0 || target_height == 0) return false;
    const auto sd = source->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sd.Width == 0 || sd.Height == 0)
        return false;
    if (sd.Width == target_width && sd.Height == target_height)
    {
        *out = source;
        return true;
    }
    if (g_dev != nullptr && (g_dev != device || g_sw != sd.Width || g_sh != sd.Height ||
                            g_sf != sd.Format || g_tw != target_width || g_th != target_height))
    {
        mgpu::diag::error("[MGPU][XESS-DEPTH] geometry changed in a live stream; refusing unsafe resource replacement.");
        return false;
    }
    if (g_dev == nullptr && !build(device, sd, target_width, target_height))
    {
        release_all();
        return false;
    }
    const unsigned parity = slot; // caller maps the source frame to its input slot
    if (g_srv[slot])
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_out[slot];
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        g_srv[slot] = false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = sd.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(source, &sv, cpu_handle(parity, 0));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(g_out[slot], nullptr, &uv, cpu_handle(parity, 1));
    ID3D12DescriptorHeap *heaps[] = {g_heap};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    const constants c{(unsigned)sd.Width, sd.Height, target_width, target_height};
    cl->SetComputeRoot32BitConstants(0, 4, &c, 0);
    auto gh = g_heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += (UINT64)((parity % RING) * 2) * g_inc;
    cl->SetComputeRootDescriptorTable(1, gh);
    cl->SetPipelineState(g_pso);
    cl->Dispatch((target_width + 7) / 8, (target_height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_out[slot];
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    g_srv[slot] = true;
    *out = g_out[slot];
    return true;
}

void release() { release_all(); }
}
