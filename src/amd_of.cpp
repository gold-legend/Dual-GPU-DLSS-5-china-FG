// Same-size temporal colour stabilisation + AMD FidelityFX Optical Flow.
// AMD's SDK is built from the vendored official sources; the two small compute
// kernels below only stabilise equal-size colours and densify the sparse flow.
#include "amd_of.hpp"
#include "diag.hpp"

#include <d3dcompiler.h>
#include <ffx_dx12.h>
#include <ffx_opticalflow.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mgpu::amd_of
{
namespace
{
constexpr unsigned RING = 8;
constexpr unsigned TEMPORAL_BASE = 0;
constexpr unsigned DENSIFY_BASE = 24;
constexpr unsigned DESCRIPTOR_COUNT = 40;

struct state
{
    ID3D12Device *device = nullptr;
    unsigned width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    ID3D12RootSignature *temporal_rs = nullptr, *densify_rs = nullptr;
    ID3D12PipelineState *temporal_pso = nullptr, *densify_pso = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    UINT increment = 0;

    // Ping-pong output/history. No resizing and no super-resolution: every
    // texture has exactly the NR output extent. The write parity comes from the
    // stream's ownership slot, so the present fence can protect both colour and MV.
    ID3D12Resource *colour[2] = {};
    bool colour_uav[2] = {};
    unsigned current = 0;
    bool history_valid = false, have_output = false;
    unsigned long long colour_frame[2] = {};
    bool colour_valid[2] = {}, motion_valid[2] = {};

    ID3D12Resource *sparse = nullptr, *scd = nullptr;
    ID3D12Resource *dense_motion[2] = {};
    bool dense_srv[2] = {true, true};
    unsigned sparse_w = 0, sparse_h = 0;

    FfxOpticalflowContext context{};
    std::vector<unsigned char> backend_scratch;
    bool context_created = false, flow_requested = false, flow_ok = false;
    bool flow_history = false, flow_failed = false;

    std::atomic<unsigned> temporal_bits{0x3f333333u}; // 0.70f
    std::atomic<bool> adaptive{true};
    std::atomic<int> source_mode{1};
    unsigned long long selected_last = 0;
    unsigned long long captures = 0, flow_dispatches = 0;
};

state &st()
{
    static state s;
    return s;
}

void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
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

D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned index)
{
    auto h = st().heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * st().increment;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned index)
{
    auto h = st().heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * st().increment;
    return h;
}

void log_error(const char *what, HRESULT hr)
{
    char line[420];
    snprintf(line, sizeof line, "[MGPU][AMD-OF] %s failed (0x%08X); ordinary NR output remains available.",
             what, (unsigned)hr);
    mgpu::diag::error(line);
}

void release_resources()
{
    auto &S = st();
    if (S.context_created)
    {
        const FfxErrorCode r = ffxOpticalflowContextDestroy(&S.context);
        if (r != FFX_OK)
        {
            char line[180];
            snprintf(line, sizeof line, "[MGPU][AMD-OF] context destroy returned %d", (int)r);
            mgpu::diag::warn(line);
        }
    }
    S.context_created = false;
    S.context = {};
    S.backend_scratch.clear();
#define RELEASE_ONE(x) do { if (S.x != nullptr) S.x->Release(); S.x = nullptr; } while (0)
    RELEASE_ONE(colour[0]); RELEASE_ONE(colour[1]);
    RELEASE_ONE(sparse); RELEASE_ONE(scd);
    RELEASE_ONE(dense_motion[0]); RELEASE_ONE(dense_motion[1]);
    RELEASE_ONE(heap); RELEASE_ONE(temporal_pso); RELEASE_ONE(densify_pso);
    RELEASE_ONE(temporal_rs); RELEASE_ONE(densify_rs);
#undef RELEASE_ONE
    S.device = nullptr;
    S.width = S.height = 0;
    S.format = DXGI_FORMAT_UNKNOWN;
    S.colour_uav[0] = S.colour_uav[1] = false;
    S.current = 0;
    S.history_valid = S.have_output = false;
    S.colour_frame[0] = S.colour_frame[1] = 0;
    S.colour_valid[0] = S.colour_valid[1] = false;
    S.motion_valid[0] = S.motion_valid[1] = false;
    S.dense_srv[0] = S.dense_srv[1] = true;
    S.selected_last = 0;
    S.sparse_w = S.sparse_h = 0;
    S.flow_requested = S.flow_ok = S.flow_history = S.flow_failed = false;
    S.captures = S.flow_dispatches = 0;
}

ID3D12Resource *texture(unsigned w, unsigned h, DXGI_FORMAT format,
                        D3D12_RESOURCE_STATES initial)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r = nullptr;
    const HRESULT hr = st().device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                            initial, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) log_error("CreateCommittedResource", hr);
    return r;
}

ID3D12Resource *ffx_texture(const FfxCreateResourceDescription &source)
{
    const auto &d = source.resourceDescription;
    if (d.type != FFX_API_RESOURCE_TYPE_TEXTURE2D)
    {
        mgpu::diag::error("[MGPU][AMD-OF] shared resource is not Texture2D; flow disabled");
        return nullptr;
    }
    return texture(d.width, d.height,
                   ffxGetDX12FormatFromSurfaceFormat((FfxApiSurfaceFormat)d.format),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

using compile_fn = HRESULT (WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                     ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                     ID3DBlob **, ID3DBlob **);

const char kTemporal[] = R"(
cbuffer C : register(b0) {
  uint2 Extent; float Strength; uint Adaptive; uint HistoryValid; uint Pad;
};
Texture2D<float4> Current : register(t0);
Texture2D<float4> History : register(t1);
RWTexture2D<float4> Output : register(u0);
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
  const float3 h = History.Load(int3(p, 0)).rgb;
  const float3 limited = clamp(h, lo, hi);
  float3 delta = abs(c.rgb - limited);
  float3 range = max(0.04, max(abs(c.rgb), abs(limited)));
  float difference = max(delta.r / range.r, max(delta.g / range.g, delta.b / range.b));
  // Large colour discontinuities reject stale history. With Adaptive=0 the
  // exact user-selected fixed weight is used; the neighbourhood clamp remains.
  float keep = Adaptive ? 1.0 - smoothstep(0.18, 0.72, difference) : 1.0;
  float w = clamp(Strength, 0.0, 0.95) * keep;
  Output[p] = float4(lerp(c.rgb, limited, w), c.a);
}
)";

const char kDensify[] = R"(
cbuffer C : register(b0) {
  uint2 SourceExtent; uint2 OpticalFlowExtent; uint2 SparseExtent; float2 VectorScale;
};
Texture2D<int2> SparseFlow : register(t0);
RWTexture2D<float2> DenseMotion : register(u0);
int2 LoadFlow(int2 p) {
  p = clamp(p, int2(0, 0), int2(SparseExtent) - 1);
  return SparseFlow.Load(int3(p, 0));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= SourceExtent)) return;
  float2 opticalPixel = (float2(id.xy) + 0.5) *
      (float2(OpticalFlowExtent) / float2(SourceExtent));
  float2 sparsePos = opticalPixel / 8.0 - 0.5;
  int2 p0 = int2(floor(sparsePos));
  float2 f = frac(sparsePos);
  float2 a = float2(LoadFlow(p0));
  float2 b = float2(LoadFlow(p0 + int2(1, 0)));
  float2 c = float2(LoadFlow(p0 + int2(0, 1)));
  float2 d = float2(LoadFlow(p0 + int2(1, 1)));
  DenseMotion[id.xy] = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) * VectorScale;
}
)";

bool root_signature(bool temporal, ID3D12RootSignature **out)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = temporal ? 2u : 1u;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = temporal ? 2u : 1u;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = temporal ? 6u : 8u;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC d{};
    d.NumParameters = 2;
    d.pParameters = params;
    ID3DBlob *blob = nullptr, *error = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (error != nullptr) error->Release();
    if (SUCCEEDED(hr)) hr = st().device->CreateRootSignature(0, blob->GetBufferPointer(),
                                                             blob->GetBufferSize(), IID_PPV_ARGS(out));
    if (blob != nullptr) blob->Release();
    if (FAILED(hr)) log_error(temporal ? "temporal root signature" : "densify root signature", hr);
    return SUCCEEDED(hr);
}

bool pipeline(bool temporal, ID3D12RootSignature *rs, ID3D12PipelineState **out)
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (compile_fn)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile)
    {
        mgpu::diag::error("[MGPU][AMD-OF] d3dcompiler_47/D3DCompile unavailable");
        return false;
    }
    const char *source = temporal ? kTemporal : kDensify;
    ID3DBlob *blob = nullptr, *error = nullptr;
    HRESULT hr = compile(source, strlen(source), temporal ? "mgpu_temporal" : "mgpu_densify",
                         nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &error);
    if (FAILED(hr) || !blob)
    {
        char line[1000];
        snprintf(line, sizeof line, "[MGPU][AMD-OF] %s shader compile failed 0x%08X: %s",
                 temporal ? "temporal" : "densify", (unsigned)hr,
                 error ? (const char *)error->GetBufferPointer() : "no compiler message");
        mgpu::diag::error(line);
        if (error) error->Release();
        if (blob) blob->Release();
        return false;
    }
    if (error) error->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.CS.pShaderBytecode = blob->GetBufferPointer();
    d.CS.BytecodeLength = blob->GetBufferSize();
    hr = st().device->CreateComputePipelineState(&d, IID_PPV_ARGS(out));
    blob->Release();
    if (FAILED(hr)) log_error(temporal ? "temporal PSO" : "densify PSO", hr);
    return SUCCEEDED(hr);
}

bool capabilities()
{
    auto &S = st();
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_2};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = DXGI_FORMAT_R16G16_SINT;
    return SUCCEEDED(S.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) &&
           sm.HighestShaderModel >= D3D_SHADER_MODEL_6_2 &&
           SUCCEEDED(S.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1)) && o1.WaveOps &&
           SUCCEEDED(S.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
           (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

bool build_flow()
{
    auto &S = st();
    if (!capabilities())
    {
        mgpu::diag::error("[MGPU][AMD-OF] capability check failed: D3D12 SM6.2 + WaveOps + R16G16_SINT typed UAV store are required");
        return false;
    }
    const size_t bytes = ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT);
    S.backend_scratch.resize(bytes);
    FfxOpticalflowContextDescription d{};
    d.flags = 0;
    d.resolution = {S.width, S.height}; // Quality: full NR output, not half resolution.
    FfxErrorCode r = ffxGetInterfaceDX12(&d.backendInterface, ffxGetDeviceDX12(S.device),
                                         S.backend_scratch.data(), S.backend_scratch.size(),
                                         FFX_OPTICALFLOW_CONTEXT_COUNT);
    if (r == FFX_OK) r = ffxOpticalflowContextCreate(&S.context, &d);
    if (r != FFX_OK)
    {
        char line[220];
        snprintf(line, sizeof line, "[MGPU][AMD-OF] FFX interface/context creation failed (%d)", (int)r);
        mgpu::diag::error(line);
        return false;
    }
    S.context_created = true;
    FfxOpticalflowSharedResourceDescriptions shared{};
    r = ffxOpticalflowGetSharedResourceDescriptions(&S.context, &shared);
    if (r != FFX_OK)
    {
        char line[180];
        snprintf(line, sizeof line, "[MGPU][AMD-OF] shared-resource query failed (%d)", (int)r);
        mgpu::diag::error(line);
        return false;
    }
    const auto &vd = shared.opticalFlowVector.resourceDescription;
    if (vd.format != FFX_API_SURFACE_FORMAT_R16G16_SINT)
    {
        mgpu::diag::error("[MGPU][AMD-OF] unexpected sparse-flow format; expected R16G16_SINT");
        return false;
    }
    S.sparse_w = vd.width;
    S.sparse_h = vd.height;
    S.sparse = ffx_texture(shared.opticalFlowVector);
    S.scd = ffx_texture(shared.opticalFlowSCD);
    S.dense_motion[0] = texture(S.width, S.height, DXGI_FORMAT_R16G16_FLOAT,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    S.dense_motion[1] = texture(S.width, S.height, DXGI_FORMAT_R16G16_FLOAT,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!S.sparse || !S.scd || !S.dense_motion[0] || !S.dense_motion[1]) return false;
    char line[520];
    snprintf(line, sizeof line,
             "[MGPU][AMD-OF] full-resolution colour flow armed: input/output=%ux%u sparse=%ux%u dense=2x%ux%u R16G16_FLOAT; AMD-flow-only, no depth created or supplied; direction=current-to-previous/source-pixels",
             S.width, S.height, S.sparse_w, S.sparse_h, S.width, S.height);
    mgpu::diag::info(line);
    return true;
}

void temporal(ID3D12GraphicsCommandList *cl, ID3D12Resource *raw,
              unsigned long long source_frame, unsigned input_slot)
{
    auto &S = st();
    const unsigned dst = input_slot & 1u;
    const unsigned src = dst ^ 1u;
    if (!S.colour_uav[dst])
    {
        barrier(cl, S.colour[dst], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        S.colour_uav[dst] = true;
    }
    const bool history = S.history_valid && S.colour_valid[src];
    if (history && S.colour_uav[src])
    {
        barrier(cl, S.colour[src], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        S.colour_uav[src] = false;
    }
    barrier(cl, raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const unsigned base = TEMPORAL_BASE + unsigned(source_frame % RING) * 3;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = S.format;
    S.device->CreateShaderResourceView(raw, &sv, cpu(base));
    S.device->CreateShaderResourceView(S.colour[src], &sv, cpu(base + 1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = S.format;
    S.device->CreateUnorderedAccessView(S.colour[dst], nullptr, &uv, cpu(base + 2));
    ID3D12DescriptorHeap *heaps[] = {S.heap};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(S.temporal_rs);
    cl->SetPipelineState(S.temporal_pso);
    struct { unsigned w, h; float strength; unsigned adaptive, valid, pad; } c{
        S.width, S.height, temporal_strength(), S.adaptive.load(std::memory_order_relaxed) ? 1u : 0u,
        history ? 1u : 0u, 0u};
    cl->SetComputeRoot32BitConstants(0, 6, &c, 0);
    cl->SetComputeRootDescriptorTable(1, gpu(base));
    cl->Dispatch((S.width + 7) / 8, (S.height + 7) / 8, 1);
    barrier(cl, raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    S.current = dst;
    S.colour_frame[dst] = source_frame;
    S.colour_valid[dst] = true;
    S.motion_valid[dst] = false; // Never pair this new colour with the old slot's motion.
    S.history_valid = true;
    S.have_output = true;
}

bool flow(ID3D12GraphicsCommandList *cl, ID3D12Resource *colour,
          unsigned long long source_frame, bool reset, unsigned slot)
{
    auto &S = st();
    if (!S.flow_ok || S.flow_failed) return false;
    barrier(cl, colour, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FfxOpticalflowDispatchDescription d{};
    d.commandList = ffxGetCommandListDX12(cl);
    d.color = ffxGetResourceDX12(colour, ffxGetResourceDescriptionDX12(colour),
                                 L"MGPU NR same-size colour", FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.opticalFlowVector = ffxGetResourceDX12(
        S.sparse, ffxGetResourceDescriptionDX12(S.sparse, FFX_API_RESOURCE_USAGE_UAV),
        L"MGPU AMD sparse flow", FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.opticalFlowSCD = ffxGetResourceDX12(
        S.scd, ffxGetResourceDescriptionDX12(S.scd, FFX_API_RESOURCE_USAGE_UAV),
        L"MGPU AMD SCD", FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.reset = reset || !S.flow_history;
    d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    d.minMaxLuminance = {0.0f, 1.0f};
    const FfxErrorCode r = ffxOpticalflowContextDispatch(&S.context, &d);
    barrier(cl, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (r != FFX_OK)
    {
        char line[200];
        snprintf(line, sizeof line, "[MGPU][AMD-OF] dispatch failed (%d); FG falls back to native presents", (int)r);
        mgpu::diag::error(line);
        S.flow_failed = true;
        S.flow_history = false;
        return false;
    }
    S.flow_history = true;
    ++S.flow_dispatches;

    // Densify into the SAME parity slot as this colour. The other slot retains
    // the previous completed frame's colour/MV pair for the frame-source menu.
    const unsigned motion_slot = slot & 1u;
    if (S.dense_srv[motion_slot])
    {
        barrier(cl, S.dense_motion[motion_slot], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        S.dense_srv[motion_slot] = false;
    }
    barrier(cl, S.sparse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const unsigned base = DENSIFY_BASE + motion_slot * 2;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R16G16_SINT;
    S.device->CreateShaderResourceView(S.sparse, &sv, cpu(base));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R16G16_FLOAT;
    S.device->CreateUnorderedAccessView(S.dense_motion[motion_slot], nullptr, &uv, cpu(base + 1));
    ID3D12DescriptorHeap *heaps[] = {S.heap};
    cl->SetDescriptorHeaps(1, heaps);
    if (d.reset)
    {
        const float zero[4]{};
        cl->ClearUnorderedAccessViewFloat(gpu(base + 1), cpu(base + 1),
                                         S.dense_motion[motion_slot], zero, 0, nullptr);
    }
    else
    {
        cl->SetComputeRootSignature(S.densify_rs);
        cl->SetPipelineState(S.densify_pso);
        struct { unsigned sw, sh, ow, oh, pw, ph; float sx, sy; } c{
            S.width, S.height, S.width, S.height, S.sparse_w, S.sparse_h, 1.0f, 1.0f};
        cl->SetComputeRoot32BitConstants(0, 8, &c, 0);
        cl->SetComputeRootDescriptorTable(1, gpu(base));
        cl->Dispatch((S.width + 7) / 8, (S.height + 7) / 8, 1);
    }
    barrier(cl, S.sparse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, S.dense_motion[motion_slot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    S.dense_srv[motion_slot] = true;

    // FG receives only this AMD colour-flow vector field. No depth texture,
    // clear, descriptor, or synthetic plane is created anywhere in this path.
    if (S.flow_dispatches <= 3 || S.flow_dispatches % 120 == 0)
    {
        char line[280];
        snprintf(line, sizeof line,
                 "[MGPU][AMD-OF] captured source=%llu reset=%d full=%ux%u flow-dispatches=%llu",
                 source_frame, d.reset ? 1 : 0, S.width, S.height, S.flow_dispatches);
        mgpu::diag::info(line);
    }
    return true;
}
} // namespace

bool arm(ID3D12Device *device, unsigned width, unsigned height, DXGI_FORMAT format,
         bool want_flow, float strength, bool adaptive_enabled)
{
    release_resources();
    auto &S = st();
    if (!device || !width || !height || format == DXGI_FORMAT_UNKNOWN) return false;
    S.device = device;
    S.width = width; S.height = height; S.format = format;
    S.flow_requested = want_flow;
    set_temporal_strength(strength);
    set_motion_adaptive(adaptive_enabled);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = DESCRIPTOR_COUNT;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&S.heap));
    if (FAILED(hr)) { log_error("descriptor heap", hr); release_resources(); return false; }
    S.increment = device->GetDescriptorHandleIncrementSize(hd.Type);
    S.colour[0] = texture(width, height, format, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    S.colour[1] = texture(width, height, format, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!S.colour[0] || !S.colour[1] ||
        !root_signature(true, &S.temporal_rs) || !root_signature(false, &S.densify_rs) ||
        !pipeline(true, S.temporal_rs, &S.temporal_pso) ||
        !pipeline(false, S.densify_rs, &S.densify_pso))
    {
        release_resources();
        return false;
    }
    S.flow_ok = want_flow && build_flow();
    if (want_flow && !S.flow_ok)
        mgpu::diag::warn("[MGPU][AMD-OF] colour flow unavailable; temporal colour remains active, XeSS-FG will not receive fabricated motion");
    char line[300];
    snprintf(line, sizeof line,
             "[MGPU][TEMPORAL] same-size residual/history colour stabiliser armed: %ux%u strength=%.0f%% motion-adaptive=%d; no SR/upscaling pass",
             width, height, double(temporal_strength() * 100.0f), motion_adaptive() ? 1 : 0);
    mgpu::diag::info(line);
    return true;
}

void release() { release_resources(); }

// No GPU commands and no state changes: capture() and present() therefore make
// exactly the same choice. A result is an indivisible colour/MV/frame-number set.
static bool select(capture_result &out)
{
    out = {};
    auto &S = st();
    if (!S.have_output) return false;
    unsigned latest = S.current;
    if (!S.colour_valid[latest])
    {
        if (S.colour_valid[latest ^ 1u]) latest ^= 1u;
        else return false;
    }
    unsigned chosen = latest;
    bool allow_flow = true;
    const int mode = S.source_mode.load(std::memory_order_relaxed);
    if (mode == 0)
    {
        const unsigned previous = latest ^ 1u;
        if (S.colour_valid[previous] && S.colour_frame[previous] < S.colour_frame[latest])
            chosen = previous;
        else
            allow_flow = false; // No completed previous frame yet: never invent one.
    }
    else if (mode == 2)
    {
        allow_flow = false; // Lowest latency is deliberately native presentation.
    }
    out.color = S.colour[chosen];
    out.color_state = S.colour_uav[chosen]
        ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    out.width = S.width;
    out.height = S.height;
    out.source_frame = S.colour_frame[chosen];
    out.input_slot = chosen;
    out.flow_valid = allow_flow && S.motion_valid[chosen] && S.flow_ok && !S.flow_failed;
    if (out.flow_valid) out.motion = S.dense_motion[chosen];
    out.reset_history = S.selected_last == 0 || out.source_frame != S.selected_last + 1ull;
    return true;
}

bool capture(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_output,
             unsigned long long source_frame, unsigned input_slot, bool reset_history,
             capture_result &out)
{
    out = {};
    auto &S = st();
    if (!cl || !nr_output || !S.device || !S.temporal_pso) return false;
    if (reset_history)
    {
        S.history_valid = false;
        S.flow_history = false;
    }
    const unsigned slot = input_slot & 1u;
    temporal(cl, nr_output, source_frame, slot);
    S.motion_valid[slot] = flow(cl, S.colour[slot], source_frame, reset_history, slot);
    if (!select(out)) return false;
    S.selected_last = out.source_frame;
    ++S.captures;
    return true;
}

bool present(capture_result &out) { return select(out); }
bool flow_ready() { return st().flow_ok && !st().flow_failed; }

void set_frame_source(int mode)
{
    mode = std::clamp(mode, 0, 2);
    const int previous = st().source_mode.exchange(mode, std::memory_order_relaxed);
    if (previous != mode)
    {
        static const char *names[3] = {"previous", "Present/latest", "current/native"};
        char line[240];
        snprintf(line, sizeof line, "[MGPU][AMD-OF] live frame source=%d (%s); selection changed",
                 mode, names[mode]);
        mgpu::diag::info(line);
    }
}
int frame_source() { return st().source_mode.load(std::memory_order_relaxed); }

void set_temporal_strength(float value)
{
    value = std::clamp(std::isfinite(value) ? value : 0.70f, 0.0f, 0.95f);
    const float previous = temporal_strength();
    unsigned bits = 0;
    static_assert(sizeof bits == sizeof value, "float bits");
    std::memcpy(&bits, &value, sizeof bits);
    st().temporal_bits.store(bits, std::memory_order_relaxed);
    if (std::fabs(previous - value) > 1e-6f)
    {
        char line[240];
        snprintf(line, sizeof line, "[MGPU][TEMPORAL] live strength=%.0f%%; next GPU dispatch uses this value",
                 double(value * 100.0f));
        mgpu::diag::info(line);
    }
}

float temporal_strength()
{
    const unsigned bits = st().temporal_bits.load(std::memory_order_relaxed);
    float value = 0.70f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

void set_motion_adaptive(bool enabled)
{
    const bool previous = st().adaptive.exchange(enabled, std::memory_order_relaxed);
    if (previous != enabled)
    {
        char line[240];
        snprintf(line, sizeof line, "[MGPU][TEMPORAL] live motion-adaptive=%d; next GPU dispatch uses this value",
                 enabled ? 1 : 0);
        mgpu::diag::info(line);
    }
}
bool motion_adaptive() { return st().adaptive.load(std::memory_order_relaxed); }
} // namespace mgpu::amd_of
