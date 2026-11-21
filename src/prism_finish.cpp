// MGPU Bridge - PRISM-FINISH: prism-core's grade + guard on the bridge's pass chain.
// See prism_finish.hpp for the contract and what is deliberately NOT ported.
//
// The Look pipeline (the Lk* functions) is prism-core 1.5.1's look_hlsl.h, vendored
// verbatim into prism_look_hlsl.h and embedded whole, so the grade math on this
// side is character for character the single-card author's - the only new math
// here is the wrapper: the CbCr skin/saturation guard and the in-place dispatch,
// taken from prism's pipeline.cpp (~1089) minus the e-field machinery that the
// pivot sealed. The HDR luma cap above the grade is prism's original (the neural
// result's luminance never exceeds the original's 2.5x).

#include "prism_finish.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"
#include "prism_look_hlsl.h"   // defines PRISM_LOOK_HLSL, verbatim from prism-core 1.5.1
#include "prism_grade.h"       // the custom-look table + GradeToShader packing, verbatim

namespace mgpu::pfinish
{
namespace
{
std::atomic<bool> g_active{false};

// ---- keys (defaults; live values come from the atomics below) ----
std::atomic<unsigned> g_look{0};        // 0..4 (5 falls back to 0)
std::atomic<float>    g_guard{1.0f};    // 0..1
std::atomic<unsigned> g_anime{0};       // 0 off, 1 protect anime colours from neural darkening
float    g_gamma = 2.2f;                // arm-time only (a per-dispatch pow is cheap; keep it live-read from here)
unsigned g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;

// D3D12 objects (ours; the device is borrowed, not AddRef'd)
ID3D12Device         *g_dev  = nullptr;
ID3D12RootSignature  *g_rs   = nullptr;
ID3D12PipelineState  *g_pso  = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
UINT                  g_inc  = 0;
ID3D12Resource       *g_lkcb = nullptr;   // the LkCustom (b1) buffer; look 5 packs the grade here
void                 *g_lk_mapped = nullptr;

// ---- the custom grade (Look 5), in the panel's units: THREE slots, like the prism desk ----
prism::GradeValues g_gv[prism::kGradeSlots];  // defaults come from the table
std::atomic<int>   g_slot{ 0 };               // the active slot the b1 buffer is packed from
unsigned           g_seed = 0u;
// slot 0 keeps the original file name (back-compat); slots 1/2 get their own
const wchar_t     *kGradeFiles[prism::kGradeSlots] = { L"mgpu_finish_grade.ini", L"mgpu_finish_grade2.ini", L"mgpu_finish_grade3.ini" };

void repack_grade();   // defined below; bakes the ACTIVE slot into the mapped b1 constants
void load_grade();     // defined below; overlays each slot's file onto its g_gv

unsigned long long g_applies = 0ull;
bool g_said_first = false;

// one heap, two parities, two slots each (t0 SRV, u0 UAV)
const UINT SLOTS_PER_PARITY = 2u;

// b0 constants. 8 x 32-bit (root constants max 64; we need 8).
struct consts_t
{
    unsigned w, h;
    unsigned look;
    float    guard;
    float    gamma;     // 0 = no wrap
    unsigned pad0, pad1;
};

// The wrapper kernel: guard + cap + LookApply(n, e=0), in place.
// gIn  (t0): the frame handed TO the network (the original, SRV)
// gOut (u0): the network's answer (in-place: load/modify/store, same texel same thread)
const char kWrapPrefix[] =
    "cbuffer C : register(b0)\n"
    "{\n"
    "  uint  gW;      uint  gH;\n"
    "  uint  gLook;   float gGuard;\n"
    "  float gGamma;  uint  gPad0;\n"
    "  uint  gPad1;\n"
    "};\n"
    "Texture2D<float4>   gIn  : register(t0);\n"
    "RWTexture2D<float4> gOut : register(u0);\n"
    "static const float3 kPfLuma = float3(0.2126, 0.7152, 0.0722);\n"
    "float PfLuma(float3 c) { return dot(c, kPfLuma); }\n";

const char kWrapSuffix[] =
    // 角色守护: prism's CbCr skin detector + high-saturation detector, on the
    // ORIGINAL pixel (prism sampled the advected proxy; here both textures are
    // 1:1 at the same pixel, so the plain input texel is the same coordinate).
    "[numthreads(8, 8, 1)]\n"
    "void CS_Finish(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  if (id.x >= gW || id.y >= gH) return;\n"
    "  float3 n = max(gOut[id.xy].rgb, 0.0);\n"
    "  float3 o = max(gIn[id.xy].rgb, 0.0);\n"
    "  if (gGuard > 0.0)\n"
    "  {\n"
    "    const float3 d = saturate(o) * 255.0;\n"
    "    const float cb = 128.0 - 0.168736 * d.r - 0.331264 * d.g + 0.5 * d.b;\n"
    "    const float cr = 128.0 + 0.5 * d.r - 0.418688 * d.g - 0.081312 * d.b;\n"
    "    const float2 q = float2((cb - 107.0) / 22.0, (cr - 150.0) / 18.0);\n"
    "    const float skin = exp(-dot(q, q)) * smoothstep(0.02, 0.06, PfLuma(o));\n"
    "    const float mx = max(o.r, max(o.g, o.b));\n"
    "    const float mn = min(o.r, min(o.g, o.b));\n"
    "    const float sat = smoothstep(0.35, 0.7, mx > 1e-4 ? (mx - mn) / mx : 0.0);\n"
    "    const float w = max(skin * 0.8, sat * 0.7) * gGuard;\n"
    "    n = lerp(n, o, saturate(w * 0.85));\n"
    "  }\n"
    // 二次元: widen the skin locus for cel-shaded / pale characters and include
    // vivid hair or cloth. Only neural DARKENING is rolled back toward the same
    // frame's game colour; neural brightening and the later Look grade stay intact.
    "  if (gPad0 != 0u && gGuard >= 0.0)\n"
    "  {\n"
    "    const float3 d = saturate(o) * 255.0;\n"
    "    const float cb = 128.0 - 0.168736 * d.r - 0.331264 * d.g + 0.5 * d.b;\n"
    "    const float cr = 128.0 + 0.5 * d.r - 0.418688 * d.g - 0.081312 * d.b;\n"
    "    const float2 q = float2((cb - 107.0) / 32.0, (cr - 150.0) / 26.0);\n"
    "    const float skin = exp(-dot(q, q)) * smoothstep(0.04, 0.16, PfLuma(o));\n"
    "    const float mx = max(o.r, max(o.g, o.b));\n"
    "    const float mn = min(o.r, min(o.g, o.b));\n"
    "    const float chroma = smoothstep(0.16, 0.50, mx > 1e-4 ? (mx - mn) / mx : 0.0) *\n"
    "                         smoothstep(0.08, 0.30, PfLuma(o));\n"
    "    const float mask = max(skin, chroma * 0.65);\n"
    "    const float resL = PfLuma(n) + 1.0 / 512.0;\n"
    "    const float origL = PfLuma(o) + 1.0 / 512.0;\n"
    "    const float dark = smoothstep(0.04, 0.35, log2(origL / resL));\n"
    "    n = lerp(n, o, saturate(mask * dark));\n"
    "  }\n"
    // HDR luma cap belongs to the guard path. grade_only (-1) skips it because
    // PSYNC has already done its own reconstruction/guard work.
    "  if (gGuard >= 0.0)\n"
    "  {\n"
    "    const float resL = PfLuma(n) + 1.0 / 512.0;\n"
    "    const float origL = PfLuma(o) + 1.0 / 512.0;\n"
    "    const float maxL = max(origL * 2.5, 1e-3);\n"
    "    if (resL > maxL) n *= maxL / resL;\n"
    "  }\n"
    // 色彩: prism's LookApply with e = 0 (no edit field on this side; the grade
    // works on the guarded neural result). gamma wrap: display code -> scene-ish
    // linear, grade, back - the single-card author graded the game's linear frame.
    "  float3 r = n;\n"
    // The vendored look kernel expects its caller to provide the CURRENT pixel.
    // Leaving these at their defaults made grain a whole-frame brightness random
    // variable (black fog), and made every pixel use the centre for the vignette.
    "  LkUv = (float2(id.xy) + 0.5) / float2(gW, gH);\n"
    "  LkPx = id.xy;\n"
    "  if (gGamma > 0.0) r = pow(max(r, 0.0), gGamma);\n"
    "  r = LookApply(r, float3(0.0, 0.0, 0.0), gLook, 1.0);\n"
    "  if (gGamma > 0.0) r = pow(max(r, 0.0), 1.0 / gGamma);\n"
    "  gOut[id.xy] = float4(max(r, 0.0), 1.0);\n"
    "}\n";

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
    u = g_pso;  rel(u); g_pso  = nullptr;
    u = g_rs;   rel(u); g_rs   = nullptr;
    u = g_heap; rel(u); g_heap = nullptr;
    u = g_lkcb; rel(u); g_lkcb = nullptr;
    g_lk_mapped = nullptr;
    g_dev = nullptr;
}

// ---- keys, from mgpu.ini beside the module (the residual reader, same rules) ----
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
    if (end == v) return dflt;
    if (d < (double)lo || d > (double)hi) return dflt;
    return (float)d;
}

bool key_flag(const char *buf, size_t n, const char *key)
{
    const char *v = mgpu::config::find(buf, n, key);
    return v != nullptr && atoi(v) == 1;
}

// Returns true when the Finish key is present and 1. Everything else is defaults.
bool read_keys()
{
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return false;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;   // whole-document rule
    buf[got] = '\0';
    if (!key_flag(buf.data(), got, "Finish")) return false;

    unsigned look = 0;
    {
        const char *v = mgpu::config::find(buf.data(), got, "FinishLook");
        if (v != nullptr)
        {
            const int d = atoi(v);
            if (d >= 0 && d <= 5) look = (unsigned)d;
        }
    }
    if (look > 5u) look = 5u;
    g_look.store(look, std::memory_order_relaxed);
    g_guard.store(key_float(buf.data(), got, "FinishGuard", 1.0f, 0.0f, 1.0f),
                  std::memory_order_relaxed);
    g_anime.store(key_flag(buf.data(), got, "FinishAnime") ? 1u : 0u,
                  std::memory_order_relaxed);
    g_gamma = key_float(buf.data(), got, "FinishGamma", 2.2f, 0.0f, 2.6f);
    if (g_gamma > 0.0f && g_gamma < 2.0f) g_gamma = 2.2f;
    return true;
}

// An _SRGB view would linearise on read; the kernel works on code values.
DXGI_FORMAT view_format(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:   return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                              return f;
    }
}

bool build_kernel(ID3D12Device *dev)
{
    char line[900];

    // assemble: prefix + the vendored prism look pipeline + wrapper kernel
    const size_t hlsl_len = sizeof(kWrapPrefix) - 1u + sizeof(PRISM_LOOK_HLSL) - 1u +
                            sizeof(kWrapSuffix) - 1u;
    std::vector<char> hlsl(hlsl_len + 1u, '\0');
    memcpy(hlsl.data(), kWrapPrefix, sizeof(kWrapPrefix) - 1u);
    memcpy(hlsl.data() + sizeof(kWrapPrefix) - 1u, PRISM_LOOK_HLSL, sizeof(PRISM_LOOK_HLSL) - 1u);
    memcpy(hlsl.data() + sizeof(kWrapPrefix) - 1u + sizeof(PRISM_LOOK_HLSL) - 1u,
           kWrapSuffix, sizeof(kWrapSuffix) - 1u);

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = (dc != nullptr)
                               ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][PFN] d3dcompiler_47.dll / D3DCompile not available - the "
                          "prism finish pass stays off for this arm.");
        return false;
    }
    ID3DBlob *cs = nullptr, *err = nullptr;
    // cs_5_0, not cs_5_1: FC6's d3dcompiler_47 rejects cs_5_1 (residual.cpp R316c).
    const HRESULT hr = compile(hlsl.data(), hlsl.size() - 1u, "prism_finish", nullptr, nullptr,
                               "CS_Finish", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        snprintf(line, sizeof line, "[MGPU][PFN] kernel CS_Finish failed to compile: 0x%08X %s",
                 (unsigned)hr, (err != nullptr) ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::error(line);
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    // Root: b0 constants (8), b1 CBV (the vendored LkCustom; zeroed, look 5 unported),
    // one table of t0 SRV + u0 UAV.
    D3D12_DESCRIPTOR_RANGE rng[2] = {};
    rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rng[0].NumDescriptors = 1;
    rng[0].BaseShaderRegister = 0;
    rng[0].OffsetInDescriptorsFromTableStart = 0;
    rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rng[1].NumDescriptors = 1;
    rng[1].BaseShaderRegister = 0;
    rng[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 8;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].Descriptor.ShaderRegister = 1;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 2;
    rp[2].DescriptorTable.pDescriptorRanges = rng;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = rp;

    ID3DBlob *sig = nullptr;
    HRESULT hr2 = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err != nullptr) err->Release();
    if (SUCCEEDED(hr2))
    {
        hr2 = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_PPV_ARGS(&g_rs));
        sig->Release();
    }
    if (SUCCEEDED(hr2))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr2 = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    }
    cs->Release();
    if (FAILED(hr2))
    {
        snprintf(line, sizeof line, "[MGPU][PFN] root signature / PSO failed: 0x%08X", (unsigned)hr2);
        mgpu::diag::error(line);
        return false;
    }

    // The LkCustom buffer: 18 * float4. Look 5 (the custom grade) packs its 72 constants here;
    // it stays mapped for the session - the panel rewrites it on every slider change and the
    // next dispatch reads it (upload heap, host-coherent).
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 18u * 4u * 4u;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr2 = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       IID_PPV_ARGS(&g_lkcb));
    if (FAILED(hr2)) return false;
    if (FAILED(g_lkcb->Map(0, nullptr, &g_lk_mapped)) || g_lk_mapped == nullptr)
    {
        g_lk_mapped = nullptr;
        return false;
    }
    memset(g_lk_mapped, 0, 18u * 4u * 4u);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2u * SLOTS_PER_PARITY;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr2 = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr2)) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    load_grade();       // the custom look's saved values, if the file exists
    repack_grade();     // defaults (or the file's values) into the b1 buffer
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(unsigned parity, unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += ((SIZE_T)(parity & 1u) * SLOTS_PER_PARITY + slot) * g_inc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(unsigned parity, unsigned slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += ((UINT64)(parity & 1u) * SLOTS_PER_PARITY + slot) * g_inc;
    return g;
}

// ---- the custom grade (Look 5): file I/O beside the addon, Key=Value per non-default ----
// Returns the addon's DIRECTORY (with trailing backslash); the caller appends the slot's file.
bool grade_path(wchar_t *path, size_t n)
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
    _snwprintf_s(path, n, _TRUNCATE, L"%ls", mod);
    return true;
}

// Pack the UI-units grade into the b1 constants (prism's GradeToShader, verbatim) and push it.
void repack_grade()
{
    if (g_lk_mapped == nullptr) return;
    float k[prism::kGradeConstants];
    prism::GradeToShader(g_gv[g_slot.load(std::memory_order_relaxed)], k);
    prism::GradeSeed(k, ++g_seed);
    memcpy(g_lk_mapped, k, sizeof k);
}

void load_grade()
{
    wchar_t base[MAX_PATH];
    if (!grade_path(base, MAX_PATH)) return;
    wchar_t *slash = wcsrchr(base, L'\\');
    if (slash == nullptr) return;
    for (int slot = 0; slot < prism::kGradeSlots; ++slot)
    {
        slash[1] = L'\0';
        _snwprintf_s(base, MAX_PATH, _TRUNCATE, L"%ls%ls", base, kGradeFiles[slot]);
        FILE *f = nullptr;
        if (_wfopen_s(&f, base, L"rb") != 0 || f == nullptr) continue;
        char buf[4096];
        const size_t got = fread(buf, 1, sizeof buf - 1u, f);
        fclose(f);
        buf[got] = '\0';
        int applied = 0;
        for (const char *p = buf; p != nullptr && *p != 0;)
        {
            while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') ++p;
            if (*p == 0) break;
            if (*p == '#' || *p == ';') { while (*p != 0 && *p != '\n') ++p; continue; }
            const int idx = atoi(p);
            const char *eq = strchr(p, '=');
            if (eq == nullptr) break;
            const float v = (float)strtod(eq + 1, nullptr);
            if (idx >= 0 && idx < prism::kGradeCount)
            {
                const prism::GradeSpec &s = prism::GradeInfo(idx);
                g_gv[slot].v[idx] = (v < s.min) ? s.min : (v > s.max) ? s.max : v;
                ++applied;
            }
            p = strchr(eq, '\n');
        }
        if (applied > 0)
        {
            char l[160];
            snprintf(l, sizeof l, "[MGPU][PFN] custom grade slot %d loaded: %d non-default value(s) from %ls",
                     slot, applied, kGradeFiles[slot]);
            mgpu::diag::info(l);
        }
    }
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

bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt, bool residual_on)
{
    if (dev == nullptr) return false;
    if (residual_on)
    {
        mgpu::diag::info("[MGPU][PFN] prism finish stays OFF: the residual module (ResidualPass) "
                         "is on; one post-pass at a time, theirs wins.");
        return false;
    }
    if (!read_keys()) return false;   // absent = nothing built, one file read

    g_dev = dev;
    g_w = width; g_h = height;
    g_fmt = view_format(fmt);
    if (!build_kernel(dev))
    {
        release_all();
        return false;
    }
    g_active.store(true, std::memory_order_relaxed);
    char l[300];
    snprintf(l, sizeof l,
             "[MGPU][PFN] prism finish ON: look=%u guard=%.2f anime=%u gamma=%.2f at %ux%u fmt=%d "
             "(prism-core 1.5.1 look pipeline, vendored verbatim; e-field not ported)",
             g_look.load(std::memory_order_relaxed),
             (double)g_guard.load(std::memory_order_relaxed),
             g_anime.load(std::memory_order_relaxed), (double)g_gamma,
             g_w, g_h, (int)g_fmt);
    mgpu::diag::info(l);
    return true;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

void apply(ID3D12GraphicsCommandList *cl, unsigned parity, ID3D12Resource *out, ID3D12Resource *in,
           bool grade_only)
{
    if (!active() || cl == nullptr || out == nullptr || in == nullptr) return;

    // make the network's UAV writes visible to this dispatch, then ours to whatever
    // reads the output next (the present copy)
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = out;
    cl->ResourceBarrier(1, &ub);

    write_srv(cpu_slot(parity, 0u), in, g_fmt);
    write_uav(cpu_slot(parity, 1u), out, g_fmt);
    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(g_pso);
    cl->SetComputeRootConstantBufferView(1, g_lkcb->GetGPUVirtualAddress());
    consts_t c{};
    c.w = g_w; c.h = g_h;
    c.look = g_look.load(std::memory_order_relaxed);
    c.guard = grade_only ? -1.0f : g_guard.load(std::memory_order_relaxed);
    c.gamma = g_gamma;
    c.pad0 = g_anime.load(std::memory_order_relaxed);
    if (c.look == 5u) repack_grade();   // refresh the film grain's per-frame seed
    cl->SetComputeRoot32BitConstants(0, 8, &c, 0);
    cl->SetComputeRootDescriptorTable(2, gpu_slot(parity, 0u));
    cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);
    cl->ResourceBarrier(1, &ub);

    ++g_applies;
    if (!g_said_first)
    {
        g_said_first = true;
        char l[240];
        snprintf(l, sizeof l, "[MGPU][PFN] first finish dispatch applied (look=%u guard=%.2f anime=%u grade_only=%d).",
                 c.look, (double)c.guard, c.pad0, grade_only ? 1 : 0);
        mgpu::diag::info(l);
    }
    else if ((g_applies % 600ull) == 0ull)
    {
        char l[160];
        snprintf(l, sizeof l, "[MGPU][PFN] applies=%llu", g_applies);
        mgpu::diag::info(l);
    }
}

void set_look(unsigned look)
{
    if (look > 5u) look = 5u;
    const unsigned old = g_look.exchange(look, std::memory_order_relaxed);
    if (old != look)
    {
        char l[128];
        snprintf(l, sizeof l, "[MGPU][PFN] LIVE look %u -> %u", old, look);
        mgpu::diag::info(l);
    }
}

void set_grade_value(int index, float v)
{
    if (index < 0 || index >= prism::kGradeCount) return;
    const prism::GradeSpec &s = prism::GradeInfo(index);
    if (v < s.min) v = s.min;
    if (v > s.max) v = s.max;
    prism::GradeValues &gv = g_gv[g_slot.load(std::memory_order_relaxed)];
    if (gv.v[index] == v) return;
    gv.v[index] = v;
    repack_grade();   // the next dispatch reads the new constants
}

float grade_value(int index)
{
    if (index < 0 || index >= prism::kGradeCount) return 0.0f;
    return g_gv[g_slot.load(std::memory_order_relaxed)].v[index];
}

void reset_grade()
{
    g_gv[g_slot.load(std::memory_order_relaxed)].Reset();
    repack_grade();
}

int grade_slot()
{
    return g_slot.load(std::memory_order_relaxed);
}

void set_grade_slot(int slot)
{
    if (slot < 0 || slot >= prism::kGradeSlots) return;
    if (g_slot.exchange(slot, std::memory_order_relaxed) == slot) return;
    repack_grade();   // the b1 buffer follows the active slot
}

// a copy of one slot's values (the desk reads all three for its slot segments)
prism::GradeValues grade_snapshot(int slot)
{
    if (slot < 0 || slot >= prism::kGradeSlots) slot = 0;
    return g_gv[slot];
}

// replace a whole slot (the desk's edits and presets land here), then persist it
void set_grade_values(int slot, const prism::GradeValues &gv)
{
    if (slot < 0 || slot >= prism::kGradeSlots) return;
    g_gv[slot] = gv;
    if (slot == g_slot.load(std::memory_order_relaxed)) repack_grade();
}

void save_grade_slot(int slot)
{
    if (slot < 0 || slot >= prism::kGradeSlots) return;
    wchar_t dir[MAX_PATH];
    if (!grade_path(dir, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls%ls", dir, kGradeFiles[slot]);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || f == nullptr) return;
    fprintf(f, "# PRISM-FINISH custom grade (prism-core 1.5.1 units). Key = table index, value = UI units.\n");
    fprintf(f, "# Indices: see prism_grade.h (Grade enum). Only non-default values are written.\n");
    int n = 0;
    for (int i = 0; i < prism::kGradeCount; ++i)
    {
        if (g_gv[slot].v[i] != prism::GradeInfo(i).def)
        {
            fprintf(f, "%d=%.4f\n", i, (double)g_gv[slot].v[i]);
            ++n;
        }
    }
    fclose(f);
    char l[160];
    snprintf(l, sizeof l, "[MGPU][PFN] custom grade slot %d saved: %d non-default value(s)", slot, n);
    mgpu::diag::info(l);
}

void save_grade()
{
    save_grade_slot(g_slot.load(std::memory_order_relaxed));
}

void set_guard(float guard)
{
    if (guard < 0.0f) guard = 0.0f;
    if (guard > 1.0f) guard = 1.0f;
    g_guard.store(guard, std::memory_order_relaxed);
}

void set_anime(bool enabled)
{
    const unsigned value = enabled ? 1u : 0u;
    const unsigned old = g_anime.exchange(value, std::memory_order_relaxed);
    if (old != value)
    {
        char l[128];
        snprintf(l, sizeof l, "[MGPU][PFN] LIVE anime %u -> %u", old, value);
        mgpu::diag::info(l);
    }
}

unsigned look()
{
    return g_look.load(std::memory_order_relaxed);
}

float guard()
{
    return g_guard.load(std::memory_order_relaxed);
}

bool anime()
{
    return g_anime.load(std::memory_order_relaxed) != 0u;
}

void release()
{
    release_all();
}
} // namespace mgpu::pfinish
