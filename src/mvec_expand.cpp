#include "mvec_expand.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "diag.hpp"

namespace mgpu::mvecex
{
namespace
{
std::atomic<bool> g_active{false};
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_pso = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_out[2] = {};
bool g_output_is_srv[2] = {};
UINT g_inc = 0;
unsigned g_dst_w = 0, g_dst_h = 0;

const char kHLSL[] =
    "Texture2D<float2> gSrc : register(t0);\n"
    "RWTexture2D<float2> gDst : register(u0);\n"
    "cbuffer C : register(b0) { uint2 gSrcDim; uint2 gDstDim; };\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  if (id.x >= gDstDim.x || id.y >= gDstDim.y) return;\n"
    "  float2 q = (float2(id.xy) + 0.5f) * float2(gSrcDim) / float2(gDstDim);\n"
    "  int2 p = clamp(int2(q), int2(0, 0), int2(gSrcDim) - int2(1, 1));\n"
    "  gDst[id.xy] = gSrc.Load(int3(p, 0));\n"
    "}\n";

typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                         ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob **, ID3DBlob **);

template <typename T> void rel(T *&p)
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

void reset()
{
    g_active.store(false, std::memory_order_relaxed);
    rel(g_pso);
    rel(g_rs);
    rel(g_heap);
    rel(g_out[0]);
    rel(g_out[1]);
    g_output_is_srv[0] = g_output_is_srv[1] = false;
    g_inc = 0;
    g_dst_w = g_dst_h = 0;
}

HRESULT make_output(ID3D12Device *dev, DXGI_FORMAT format, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_dst_w;
    d.Height = g_dst_h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                        IID_PPV_ARGS(out));
}

void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}
}

bool arm(ID3D12Device *dev, bool enabled,
         unsigned src_w, unsigned src_h, unsigned dst_w, unsigned dst_h,
         DXGI_FORMAT format, ID3D12Resource *const src[2])
{
    reset();
    if (!enabled || dev == nullptr || src == nullptr || src[0] == nullptr || src[1] == nullptr ||
        src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0 ||
        (src_w == dst_w && src_h == dst_h) || src_w > dst_w || src_h > dst_h ||
        format != DXGI_FORMAT_R16G16_FLOAT)
        return false;

    g_dst_w = dst_w;
    g_dst_h = dst_h;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = dc != nullptr
        ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::warn("[MGPU][MVEX] D3DCompile unavailable; low-resolution motion expansion stays OFF.");
        reset();
        return false;
    }

    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = compile(kHLSL, std::strlen(kHLSL), "mvec_expand", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        char line[700];
        std::snprintf(line, sizeof line,
                      "[MGPU][MVEX] compute shader compilation failed hr=0x%08X %s; expansion stays OFF.",
                      (unsigned)hr, err != nullptr ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::warn(line);
        rel(err);
        rel(cs);
        reset();
        return false;
    }
    rel(err);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 2;
    rp[0].DescriptorTable.pDescriptorRanges = ranges;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0;
    rp[1].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    ID3DBlob *sig = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    rel(err);
    if (SUCCEEDED(hr))
    {
        hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      IID_PPV_ARGS(&g_rs));
        rel(sig);
    }
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    }
    rel(cs);
    for (unsigned p = 0; p < 2 && SUCCEEDED(hr); ++p)
        hr = make_output(dev, format, &g_out[p]);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr)) hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr))
    {
        char line[240];
        std::snprintf(line, sizeof line,
                      "[MGPU][MVEX] GPU object creation failed hr=0x%08X; expansion stays OFF.",
                      (unsigned)hr);
        mgpu::diag::warn(line);
        reset();
        return false;
    }

    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (unsigned p = 0; p < 2; ++p)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(p * 2u) * g_inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Format = format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(src[p], &sv, h);
        h.ptr += g_inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = format;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(g_out[p], nullptr, &uv, h);
    }

    g_active.store(true, std::memory_order_release);
    char line[420];
    std::snprintf(line, sizeof line,
                  "[MGPU][MVEX] LOSSLESS LOW-RES MOTION TRANSPORT: %ux%u RG16F on the bus -> "
                  "%ux%u RG16F on GPU 1. POINT sampling matches the original full-resolution tap; "
                  "vector values are unchanged.",
                  src_w, src_h, dst_w, dst_h);
    mgpu::diag::info(line);
    return true;
}

bool active()
{
    return g_active.load(std::memory_order_acquire);
}

ID3D12Resource *output(unsigned parity)
{
    return active() && parity < 2u ? g_out[parity] : nullptr;
}

void expand(ID3D12GraphicsCommandList *cl, unsigned parity, ID3D12Resource *src)
{
    if (!active() || cl == nullptr || src == nullptr || parity >= 2u) return;
    if (g_output_is_srv[parity])
        barrier(cl, g_out[parity], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(g_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += (UINT64)(parity * 2u) * g_inc;
    cl->SetComputeRootDescriptorTable(0, gh);
    const D3D12_RESOURCE_DESC sd = src->GetDesc();   // CPU-only metadata; no GPU wait
    const UINT constants[4] = { (unsigned)sd.Width, sd.Height, g_dst_w, g_dst_h };
    cl->SetComputeRoot32BitConstants(1, 4, constants, 0);
    cl->Dispatch((g_dst_w + 7u) / 8u, (g_dst_h + 7u) / 8u, 1u);
    barrier(cl, g_out[parity], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_output_is_srv[parity] = true;
}

void release()
{
    reset();
}
}
