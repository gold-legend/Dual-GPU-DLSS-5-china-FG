// MGPU Bridge - reduced-network Prism x1, synchronous on the neural GPU.
//
// This is the reduced synchronous part of prism-nr's public pipeline, adapted
// to the display-referred proxy that MGPU already transports. It is NOT a port
// of Prism's HDR Orig/Encode stage, temporal history, Carry or asynchronous
// launch/land loop. Keeping those stages out is deliberate: this experiment
// changes network extent and same-frame reconstruction on evaluated frames.
// Cadence 1 or fixed 15 is supported; held frames reuse output and are not new evaluations.

#ifndef PRISM_X1_NAMESPACE
#define PRISM_X1_NAMESPACE prism_x1
#endif
#ifndef PRISM_X1_PASS_ID
#define PRISM_X1_PASS_ID "1"
#endif

#include "prism_x1.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::PRISM_X1_NAMESPACE
{
namespace
{
constexpr const char *kPassId = PRISM_X1_PASS_ID;
std::atomic<bool> g_active{false};
std::atomic<bool> g_ab_enabled{true};
ID3D12Device *g_dev = nullptr;
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_down = nullptr;
ID3D12PipelineState *g_fit = nullptr;
ID3D12PipelineState *g_compose = nullptr;
ID3D12PipelineState *g_capture = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
ID3D12Resource *g_a = nullptr;
ID3D12Resource *g_b = nullptr;
ID3D12Resource *g_history = nullptr;
UINT g_inc = 0;
unsigned g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
std::atomic<float> g_intensity{1.0f};
std::atomic<float> g_guard{1.0f};
float g_detail = 0.85f;
unsigned long long g_downs = 0, g_applies = 0, g_limit_frames = 0;
bool g_history_valid = false;
bool g_limit_residual = false;
bool g_depth_valid = false;

// Eight recording slots, each with four DISJOINT ten-descriptor tables.
// The root signature maps SRV t0..t7 to slots 0..7 and UAV u0..u1 to slots
// 8..9. A descriptor is read when the GPU executes, not when the CPU records.
constexpr UINT RING = 8;
constexpr UINT DOWN_BASE = 0;
constexpr UINT FIT_BASE = 10;
constexpr UINT COMPOSE_BASE = 20;
constexpr UINT CAPTURE_BASE = 30;
constexpr UINT SLOTS = 40;
static_assert(FIT_BASE - DOWN_BASE == 10 && COMPOSE_BASE - FIT_BASE == 10 &&
              CAPTURE_BASE - COMPOSE_BASE == 10 && SLOTS - CAPTURE_BASE == 10,
              "PX1 descriptor tables must not overlap");

struct consts_t
{
    unsigned w, h, lw, lh;
    float intensity, guard, detail, scale_x, scale_y;
    unsigned pad0, pad1, pad2;
};
static_assert(sizeof(consts_t) == 48, "12 root constants");

const char kShaders[] = R"(
cbuffer C : register(b0)
{
  uint gW; uint gH; uint gLW; uint gLH;
  float gIntensity; float gGuard; float gDetail;
  float gScaleX; float gScaleY; uint gPad0; uint gPad1; uint gPad2;
};
Texture2D<float4> gBase : register(t0);
Texture2D<float4> gOther : register(t1);
Texture2D<float4> gGuide : register(t2);
Texture2D<float4> gSmallInput : register(t3);
Texture2D<float4> gSmallOutput : register(t4);
Texture2D<float4> gHistory : register(t5);
Texture2D<float4> gDepth : register(t6);
RWTexture2D<float4> gOut0 : register(u0);
RWTexture2D<float4> gOut1 : register(u1);

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

// X1-only residual limiter. Normal changes pass through untouched; an abnormal
// one-frame luma jump is capped in log2 residual space. A scene-sized discontinuity
// is accepted as a cut, preventing a long cross-fade. gPad0/gPad1 are enable/valid.
float LimitLumaJump(float current, float previous)
{
  if (gPad0 == 0 || gPad1 == 0) return current;
  const float delta = current - previous;
  if (abs(delta) >= 0.75) return current;
  return previous + clamp(delta, -0.12, 0.12);
}
float3 LimitResidualJump(float3 e, float previous)
{
  const float y = Luma(e);
  return e + (LimitLumaJump(y, previous) - y);
}

// Prism Down: bilinear filtering in linear light, not point sampling of
// display-code values. gOther is the small network's output in later passes.
[numthreads(16, 16, 1)]
void CS_Down(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gLW || id.y >= gLH) return;
  const float2 f = (float2(id.xy) + 0.5) / float2(gScaleX, gScaleY) - 0.5;
  const int2 b = int2(floor(f)), last = int2(gW, gH) - 1;
  const float2 a = f - float2(b);
  float3 c = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j)
  {
    [unroll] for (int i = 0; i <= 1; ++i)
    {
      const int2 q = clamp(b + int2(i, j), int2(0, 0), last);
      c += Dec(gBase.Load(int3(q, 0)).rgb) *
           ((i ? a.x : 1.0 - a.x) * (j ? a.y : 1.0 - a.y));
    }
  }
  gOut0[id.xy] = float4(Enc(c), 1.0);
}

// Prism GuidedFit: each 5x5 low-resolution window fits the neural edit as a
// linear function of proxy log-luma. Only the edit is enlarged, never a neural
// picture from another frame. kGfRef centres the offsets for FP16 storage.
static const float kGfRef = -2.5;
static const float kGfEps = 0.01;
groupshared float4 gfIE[576];
groupshared float3 gfSA[400], gfSB[400];
[numthreads(16, 16, 1)]
void CS_GuidedFit(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
  const int2 last = int2(gLW, gLH) - 1;
  const int2 org = int2(gid.xy) * 16 - 4;
  for (uint i = gi; i < 576; i += 256)
  {
    const int2 q = clamp(org + int2(i % 24, i / 24), int2(0, 0), last);
    const float3 p = Dec(gBase.Load(int3(q, 0)).rgb);
    const float3 n = Dec(gOther.Load(int3(q, 0)).rgb);
    float3 e = clamp(log2(max(n, 1.0 / 512.0) / max(p, 1.0 / 512.0)), -3.0, 3.0);
    e = LimitResidualJump(e, gHistory.Load(int3(q, 0)).a);
    gfIE[i] = float4(log2(max(Luma(p), 1e-4)) - kGfRef, e);
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
    // Match Prism: DetailKeep is the retained detail, so the reduction cap is
    // -(1 - DetailKeep), not -DetailKeep.
    const float3 a = max((mIP - mI * mP) /
                         (max(mII - mI * mI, 0.0) + kGfEps), -(1.0 - gDetail));
    gfSA[n] = a;
    gfSB[n] = mP - a * mI;
  }
  GroupMemoryBarrierWithGroupSync();

  const int2 t = int2(gi % 16, gi / 16), px = int2(gid.xy) * 16 + t;
  if (px.x >= (int)gLW || px.y >= (int)gLH) return;
  float3 A = 0.0, B = 0.0;
  [unroll] for (int j = 0; j <= 4; ++j)
  {
    [unroll] for (int k = 0; k <= 4; ++k)
    {
      const int c = (t.y + j) * 20 + t.x + k;
      A += gfSA[c]; B += gfSB[c];
    }
  }
  const float3 centerE = gfIE[(t.y + 2) * 24 + t.x + 2].yzw;
  gOut0[px] = float4(A / 25.0, Luma(centerE));
  gOut1[px] = float4(B / 25.0, 0.0);
}

float4 LowBilinear(Texture2D<float4> tex, float2 f)
{
  const int2 last = int2(gLW, gLH) - 1, b = int2(floor(f));
  const float2 a = f - float2(b);
  float4 v = 0.0;
  [unroll] for (int j = 0; j <= 1; ++j)
  {
    [unroll] for (int i = 0; i <= 1; ++i)
    {
      const int2 q = clamp(b + int2(i, j), int2(0, 0), last);
      v += tex.Load(int3(q, 0)) *
           ((i ? a.x : 1.0 - a.x) * (j ? a.y : 1.0 - a.y));
    }
  }
  return v;
}

float SkinMask(float3 pd, float3 p)
{
  const float3 d = pd * 255.0;
  const float cb = 128.0 - 0.168736 * d.r - 0.331264 * d.g + 0.5 * d.b;
  const float cr = 128.0 + 0.5 * d.r - 0.418688 * d.g - 0.081312 * d.b;
  const float2 q = float2((cb - 107.0) / 22.0, (cr - 150.0) / 18.0);
  return exp(-dot(q, q)) * Smooth(0.02, 0.06, Luma(p));
}

float3 GuardEdit(float3 pd, float3 p, float3 e)
{
  const float skin = SkinMask(pd, p);
  const float mx = max(p.r, max(p.g, p.b));
  const float mn = min(p.r, min(p.g, p.b));
  const float w = max(skin * 0.8,
      Smooth(0.35, 0.7, mx > 1e-4 ? (mx - mn) / mx : 0.0) * 0.7) * gGuard;
  const float y = Luma(e);
  const float3 kept = (e - y) * (1.0 - w);
  return y + kept + (clamp(kept, -0.08, 0.08) - kept) * w;
}

// Asymmetric display-referred tone residual. The neural network contributes
// luminance structure only; the current game pixel remains the colour anchor.
// Shadows retain a soft floor instead of clipping to black, while highlights
// use a continuous shoulder instead of MGPU's hard 2.5x / 2.0 clamp.
float ToneResidual(float origY, float editY)
{
  const float shadow = 1.0 - Smooth(0.035, 0.32, origY);
  if (editY < 0.0)
  {
    // Dark protection is part of the tone pipeline, not the character-guard
    // slider. Never amplify neural darkening; retain 30% luma in deep shadow.
    const float minY = origY * lerp(0.05, 0.30, shadow);
    return max(editY, minY - origY);
  }
  const float bright = Smooth(0.45, 0.90, origY);
  const float headroom = max((0.20 + origY * 1.60) * lerp(1.0, 0.65, bright), 1e-4);
  const float lifted = editY * gIntensity;
  return lifted / (1.0 + lifted / headroom);
}

// Without usable game vectors, suppress only locally stale neural residuals.
// Stable pixels keep the full effect. Changed pixels where the network still
// resembles the previous input lose at most 85% of the edit, never the base.
float LocalHistoryKeep(float2 f, uint2 nativeId)
{
  if (gPad0 != 0 || gPad1 == 0) return 1.0;
  const float4 previous = LowBilinear(gHistory, f);
  const float3 currentCode = LowBilinear(gSmallInput, f).rgb;
  const float3 neuralCode = LowBilinear(gSmallOutput, f).rgb;
  const float3 current = Dec(currentCode), old = Dec(previous.rgb), neural = Dec(neuralCode);
  const float cy = max(Luma(current), 1e-4), py = max(Luma(old), 1e-4);
  const float ny = max(Luma(neural), 1e-4);
  const float colourDelta = abs(log2(cy / py)) + 0.5 * Luma(abs(currentCode - previous.rgb));
  const float changed = Smooth(0.06, 0.35, colourDelta);
  const float stale = Smooth(0.02, 0.25,
      abs(log2(ny / cy)) - abs(log2(ny / py)));
  float reject = changed * max(stale, 0.35);
  if (gPad2 != 0)
  {
    const float currentDepth = gDepth[nativeId].r;
    const float previousDepth = previous.a;
    const float depthDelta = abs(currentDepth - previousDepth) /
                             max(max(currentDepth, previousDepth), 1e-4);
    reject = max(reject, Smooth(0.015, 0.08, depthDelta));
  }
  return 1.0 - 0.85 * saturate(reject);
}

// Compose keeps stable Prism outside skin. Skin uses the actual low-resolution
// neural residual for realism, game chroma for colour, and native-rate grain
// restored after intensity so 300% cannot amplify the grain itself.
[numthreads(16, 16, 1)]
void CS_Compose(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH) return;
  const float4 baseSample = gBase[id.xy];
  const float3 baseCode = max(baseSample.rgb, 0.0);
  const float3 base = Dec(baseCode);
  const float yFull = log2(max(Luma(base), 1e-4));
  const float2 f = (float2(id.xy) + 0.5) * float2(gScaleX, gScaleY) - 0.5;
  const float historyKeep = 1.0; // PX1 stable path: no per-pixel history rejection flicker.
  float3 rawEdit = clamp(LowBilinear(gOther, f) * (yFull - kGfRef) +
                         LowBilinear(gGuide, f), -3.0, 3.0) * historyKeep;
  float3 e = rawEdit * gIntensity;
  // Negative-luma protection is always active. The character-guard slider is
  // reserved for chroma anti-roughness and cannot reintroduce crushed blacks.
  if (gIntensity > 1.0)
  {
    const float amplifiedY = Luma(e);
    if (amplifiedY < 0.0)
      e += Luma(rawEdit) - amplifiedY;
  }
  e = GuardEdit(baseCode, base, e);
  float3 result = Enc(base * exp2(e));
  const float skin = saturate(SkinMask(baseCode, base));
  if (skin > 1e-4)
  {
    const float3 smallInput = LowBilinear(gSmallInput, f);
    const float3 smallOutput = LowBilinear(gSmallOutput, f);
    float editY = Luma(smallOutput - smallInput);
    const float3 lowP = Dec(smallInput);
    const float3 lowN = Dec(smallOutput);
    const float3 lowE = clamp(log2(max(lowN, 1.0 / 512.0) /
                                   max(lowP, 1.0 / 512.0)), -3.0, 3.0);
    const float rawLogY = Luma(lowE);
    const float limitedLogY = LimitLumaJump(rawLogY, LowBilinear(gHistory, f).a);
    if (abs(rawLogY) > 1e-4)
      editY *= limitedLogY / rawLogY;
    editY *= historyKeep;
    const float origY = Luma(baseCode);
    const float3 lumaDir = lerp(float3(1.0, 1.0, 1.0),
                                baseCode / (origY + 1e-5), saturate(origY * 50.0));
    const float targetY = max(origY + ToneResidual(origY, editY), 0.0);
    const float3 mgpuCode = lumaDir * targetY;
    // Convert only to run the established anti-roughness guard; the residual
    // itself remains display-referred and is never reconstructed from A/B.
    float3 mgpuEdit = clamp(log2(max(Dec(mgpuCode), 1.0 / 512.0) /
                                 max(base, 1.0 / 512.0)), -3.0, 3.0);
    mgpuEdit = GuardEdit(baseCode, base, mgpuEdit);
    const float3 mgpuGuarded = Enc(base * exp2(mgpuEdit));
    result = lerp(result, mgpuGuarded, skin);

    // MGPU 0.4's adaptive native high-frequency recovery, at the original
    // NRSharpness=0.50. It is applied once, after both residual and intensity.
    const int2 last = int2(gW, gH) - 1;
    const int2 p = int2(id.xy);
    const float3 cE = gBase.Load(int3(min(p.x + 1, last.x), p.y, 0)).rgb;
    const float3 cW = gBase.Load(int3(max(p.x - 1, 0), p.y, 0)).rgb;
    const float3 cS = gBase.Load(int3(p.x, min(p.y + 1, last.y), 0)).rgb;
    const float3 cN = gBase.Load(int3(p.x, max(p.y - 1, 0), 0)).rgb;
    const float lE = Luma(cE), lW = Luma(cW), lS = Luma(cS), lN = Luma(cN);
    const float lM = Luma(baseCode);
    const float minL = min(lM, min(min(lE, lW), min(lS, lN)));
    const float maxL = max(lM, max(max(lE, lW), max(lS, lN)));
    const float range = maxL - minL;
    if (range > 1e-5)
    {
      const float3 crossAvg = (cE + cW + cS + cN) * 0.25;
      const float3 highFreq = baseCode - crossAvg;
      const float adaptiveScale = saturate(1.0 - range / (maxL + 1e-4));
      const float w = 0.5 * (0.2 + 0.8 * adaptiveScale) * skin;
      result = max(result + highFreq * w, 0.0);
    }
  }
  gOut0[id.xy] = float4(result, baseSample.a);
}

// Save only the current low-resolution input and its depth for next frame's
// local confidence test. This is our metadata, not NGX's internal history.
[numthreads(16, 16, 1)]
void CS_Capture(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gLW || id.y >= gLH) return;
  float depth = 0.0;
  if (gPad2 != 0)
  {
    const float2 f = (float2(id.xy) + 0.5) / float2(gScaleX, gScaleY);
    const int2 q = clamp(int2(f), int2(0, 0), int2(gW, gH) - 1);
    depth = gOther[q].r;
  }
  gOut0[id.xy] = float4(gBase[id.xy].rgb, depth);
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
    g_ab_enabled.store(true, std::memory_order_relaxed);
    IUnknown *u;
    u = g_capture; rel(u); g_capture = nullptr;
    u = g_compose; rel(u); g_compose = nullptr;
    u = g_fit; rel(u); g_fit = nullptr;
    u = g_down; rel(u); g_down = nullptr;
    u = g_rs; rel(u); g_rs = nullptr;
    u = g_heap; rel(u); g_heap = nullptr;
    u = g_a; rel(u); g_a = nullptr;
    u = g_b; rel(u); g_b = nullptr;
    u = g_history; rel(u); g_history = nullptr;
    g_dev = nullptr;
    g_w = g_h = 0;
    g_downs = g_applies = g_limit_frames = 0;
    g_history_valid = false;
    g_limit_residual = false;
    g_depth_valid = false;
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

float key_fallback(const char *buf, size_t n, const char *key, const char *old_key,
                   float dflt, float lo, float hi)
{
    const float fallback = key_float(buf, n, old_key, dflt, lo, hi);
    return key_float(buf, n, key, fallback, lo, hi);
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
    const char *on = mgpu::config::find(buf.data(), got, "PrismX1");
    if (on == nullptr || atoi(on) != 1) return false;
    const char *passes = mgpu::config::find(buf.data(), got, "Passes");
    const char *cadence = mgpu::config::find(buf.data(), got, "NRCadence");
    const int pass_count = passes != nullptr ? atoi(passes) : 1;
    const int cadence_count = cadence != nullptr ? atoi(cadence) : 1;
    const char *chain = mgpu::config::find(buf.data(), got, "ChainWarp");
    const bool chain4 = cadence_count == 4 && chain != nullptr && atoi(chain) == 1;
    if ((pass_count < 1 || pass_count > 3) ||
        (cadence_count != 1 && cadence_count != 2 && !chain4))
    {
        mgpu::diag::error("[MGPU][PX1] requires Passes=1, 2 or 3 and NRCadence=1, 2 (paired hold), or 4 with ChainWarp=1; the retired 1.5 mode stays OFF.");
        return false;
    }
    const char *sync = mgpu::config::find(buf.data(), got, "PrismSync");
    if (sync != nullptr && atoi(sync) == 1)
    {
        mgpu::diag::error("[MGPU][PX1] PrismX1=1 requires PrismSync=0; reduced path stays OFF.");
        return false;
    }
    g_intensity.store(key_fallback(buf.data(), got, "PrismX1Intensity", "PrismSyncIntensity",
                                   1.0f, 0.0f, 3.0f), std::memory_order_relaxed);
    g_guard.store(key_fallback(buf.data(), got, "PrismX1Guard", "PrismSyncGuard",
                               1.0f, 0.0f, 1.0f), std::memory_order_relaxed);
    g_detail = key_fallback(buf.data(), got, "PrismX1DetailKeep", "PrismSyncDetailKeep",
                            0.85f, 0.0f, 1.0f);
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

bool build()
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = dc != nullptr
        ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][PX1] D3DCompile unavailable; reduced path stays OFF.");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 8;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 12;
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
    if (FAILED(hr))
    {
        char line[180];
        snprintf(line, sizeof line, "[MGPU][PX1] root signature failed: 0x%08X", (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }

    const char *entries[4] = {"CS_Down", "CS_GuidedFit", "CS_Compose", "CS_Capture"};
    ID3D12PipelineState **targets[4] = {&g_down, &g_fit, &g_compose, &g_capture};
    for (unsigned pass = 0; pass < 4; ++pass)
    {
        ID3DBlob *cs = nullptr;
        hr = compile(kShaders, sizeof(kShaders) - 1u, "prism_x1", nullptr, nullptr,
                     entries[pass], "cs_5_0", 0, 0, &cs, &err);
        if (FAILED(hr) || cs == nullptr)
        {
            char line[1200];
            snprintf(line, sizeof line, "[MGPU][PX1] %s compile failed: 0x%08X %s",
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
        if (FAILED(hr))
        {
            char line[200];
            snprintf(line, sizeof line, "[MGPU][PX1] %s PSO failed: 0x%08X",
                     entries[pass], (unsigned)hr);
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
    hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                        IID_PPV_ARGS(&g_a));
    if (SUCCEEDED(hr))
        hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                            IID_PPV_ARGS(&g_b));
    if (SUCCEEDED(hr))
        hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                            IID_PPV_ARGS(&g_history));
    if (FAILED(hr)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = RING * SLOTS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr)) return false;
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
    if (r == nullptr) return false;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           d.Width >= w && d.Height >= h && w != 0 && h != 0;
}

void constants(ID3D12GraphicsCommandList *cl, unsigned low_width, unsigned low_height)
{
    consts_t c{};
    c.w = g_w; c.h = g_h; c.lw = low_width; c.lh = low_height;
    c.intensity = g_intensity.load(std::memory_order_relaxed);
    c.guard = g_guard.load(std::memory_order_relaxed);
    c.detail = g_detail;
    c.scale_x = (float)low_width / (float)g_w;
    c.scale_y = (float)low_height / (float)g_h;
    c.pad0 = g_limit_residual ? 1u : 0u;
    c.pad1 = g_history_valid ? 1u : 0u;
    c.pad2 = g_depth_valid ? 1u : 0u;
    cl->SetComputeRoot32BitConstants(0, 12, &c, 0);
}

void barrier_to(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
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

bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt,
         unsigned low_width, unsigned low_height)
{
    // Equal extents are valid for the GPU0-native-NR isolation path: Down is
    // bypassed by the caller, while GuidedFit/Compose simply use scale 1.0.
    if (dev == nullptr || width == 0 || height == 0 || low_width == 0 || low_height == 0 ||
        low_width > width || low_height > height || !read_keys())
        return false;
    release_all();
    g_dev = dev;
    g_w = width; g_h = height;
    g_fmt = view_format(fmt);
    if (!build())
    {
        release_all();
        return false;
    }
    g_active.store(true, std::memory_order_relaxed);
    char line[400];
    snprintf(line, sizeof line,
             "[MGPU][PX1][P%s] reduced same-frame Prism ON: native=%ux%u low=%ux%u fmt=%d "
             "intensity=%.2f guard=%.2f detail=%.2f; current-frame Down+GuidedFit+Compose; "
             "independent per-pass history; no temporal edit transport/FG.",
             kPassId, g_w, g_h, low_width, low_height, (int)g_fmt,
             (double)g_intensity.load(std::memory_order_relaxed),
             (double)g_guard.load(std::memory_order_relaxed), (double)g_detail);
    mgpu::diag::info(line);
    return true;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

bool ab_enabled()
{
    return active() && g_ab_enabled.load(std::memory_order_relaxed);
}

bool toggle_ab()
{
    if (!active()) return false;
    const bool enabled = !g_ab_enabled.load(std::memory_order_relaxed);
    g_ab_enabled.store(enabled, std::memory_order_relaxed);
    g_history_valid = false;
    mgpu::diag::warn(enabled
        ? "[MGPU][PX1][AB] PX1 enabled: reduced Down + GuidedFit + Compose on the next recorded frame."
        : "[MGPU][PX1][AB] Original NRS enabled: the original Down + residual resolve runs instead; transport and depth are unchanged.");
    return enabled;
}

bool down(ID3D12GraphicsCommandList *cl, unsigned parity,
          ID3D12Resource *base, ID3D12Resource *low_proxy,
          unsigned low_width, unsigned low_height)
{
    if (!ab_enabled() || cl == nullptr || !valid_texture(base, g_w, g_h) ||
        !valid_texture(low_proxy, low_width, low_height) || base == low_proxy)
        return false;
    if (low_width >= g_w || low_height >= g_h) return false;

    ID3D12DescriptorHeap *heaps[1] = {g_heap};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    constants(cl, low_width, low_height);
    write_srv(cpu_slot(parity, DOWN_BASE), base, g_fmt);
    write_srv(cpu_slot(parity, DOWN_BASE + 1), base, g_fmt);       // t1 unused by Down
    write_srv(cpu_slot(parity, DOWN_BASE + 2), base, g_fmt);       // t2 unused by Down
    write_srv(cpu_slot(parity, DOWN_BASE + 3), base, g_fmt);       // t3 unused by Down
    write_srv(cpu_slot(parity, DOWN_BASE + 4), base, g_fmt);       // t4 unused by Down
    write_srv(cpu_slot(parity, DOWN_BASE + 5), base, g_fmt);       // t5 unused by Down
    write_srv(cpu_slot(parity, DOWN_BASE + 6), base, g_fmt);       // t6 unused by Down
    write_srv(cpu_slot(parity, DOWN_BASE + 7), base, g_fmt);       // t7 unused by Down
    write_uav(cpu_slot(parity, DOWN_BASE + 8), low_proxy, g_fmt); // root-table u0
    write_uav(cpu_slot(parity, DOWN_BASE + 9), low_proxy, g_fmt); // u1 unused by Down
    cl->SetPipelineState(g_down);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, DOWN_BASE));
    cl->Dispatch((low_width + 15u) / 16u, (low_height + 15u) / 16u, 1u);
    ++g_downs;
    return true;
}

bool compose(ID3D12GraphicsCommandList *cl, unsigned parity,
             ID3D12Resource *base, ID3D12Resource *mask_base,
             ID3D12Resource *low_proxy, ID3D12Resource *low_neural, ID3D12Resource *out,
             unsigned low_width, unsigned low_height,
             ID3D12Resource *depth, bool limit_residual,
             mark_callback mark, void *mark_context)
{
    // TEST A (one variable): limiter OFF. pad0=0 keeps LimitLumaJump inert and
    // routes history through CS_Capture, i.e. the pure current-frame path. The
    // +/-0.12 log2 slew cap lags neural convergence behind motion, which reads
    // as ghosting; the a9da0f83 binary does not ghost, so its pad0 was likely 0.
    g_limit_residual = false;
    (void)limit_residual;
    if (!ab_enabled() || cl == nullptr || !valid_texture(base, g_w, g_h) ||
        !valid_texture(mask_base, g_w, g_h) || !valid_texture(out, g_w, g_h) ||
        !valid_texture(low_proxy, low_width, low_height) ||
        !valid_texture(low_neural, low_width, low_height) ||
        base == out || low_proxy == low_neural ||
        low_width > g_w || low_height > g_h)
        return false;
    g_depth_valid = false;
    if (valid_texture(depth, g_w, g_h))
    {
        const D3D12_RESOURCE_DESC dd = depth->GetDesc();
        g_depth_valid = (dd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
            (dd.Format == DXGI_FORMAT_R32_FLOAT || dd.Format == DXGI_FORMAT_D32_FLOAT ||
             dd.Format == DXGI_FORMAT_R32_TYPELESS);
    }

    ID3D12DescriptorHeap *heaps[1] = {g_heap};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    constants(cl, low_width, low_height);
    write_srv(cpu_slot(parity, FIT_BASE), low_proxy, g_fmt);
    write_srv(cpu_slot(parity, FIT_BASE + 1), low_neural, g_fmt);
    write_srv(cpu_slot(parity, FIT_BASE + 2), low_proxy, g_fmt);   // t2 unused by GuidedFit
    write_srv(cpu_slot(parity, FIT_BASE + 3), low_proxy, g_fmt);   // t3 unused by GuidedFit
    write_srv(cpu_slot(parity, FIT_BASE + 4), low_neural, g_fmt);  // t4 unused by GuidedFit
    write_srv(cpu_slot(parity, FIT_BASE + 5), g_history, DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_srv(cpu_slot(parity, FIT_BASE + 6), low_proxy, g_fmt);  // t6 unused by GuidedFit
    write_srv(cpu_slot(parity, FIT_BASE + 7), mask_base, g_fmt); // t7 unused by GuidedFit
    write_uav(cpu_slot(parity, FIT_BASE + 8), g_a, DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_uav(cpu_slot(parity, FIT_BASE + 9), g_b, DXGI_FORMAT_R16G16B16A16_FLOAT);
    cl->SetPipelineState(g_fit);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, FIT_BASE));
    cl->Dispatch((low_width + 15u) / 16u, (low_height + 15u) / 16u, 1u);
    if (mark != nullptr) mark(mark_context, 20u); // PX1 GuidedFit ends

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    for (unsigned i = 0; i < 2; ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = i == 0 ? g_a : g_b;
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cl->ResourceBarrier(2, barriers);

    write_srv(cpu_slot(parity, COMPOSE_BASE), base, g_fmt);
    write_srv(cpu_slot(parity, COMPOSE_BASE + 1), g_a, DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_srv(cpu_slot(parity, COMPOSE_BASE + 2), g_b, DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_srv(cpu_slot(parity, COMPOSE_BASE + 3), low_proxy, g_fmt);
    write_srv(cpu_slot(parity, COMPOSE_BASE + 4), low_neural, g_fmt);
    write_srv(cpu_slot(parity, COMPOSE_BASE + 5), g_history, DXGI_FORMAT_R16G16B16A16_FLOAT);
    write_srv(cpu_slot(parity, COMPOSE_BASE + 6), g_depth_valid ? depth : base,
              g_depth_valid ? DXGI_FORMAT_R32_FLOAT : g_fmt);
    write_srv(cpu_slot(parity, COMPOSE_BASE + 7), mask_base, g_fmt);
    write_uav(cpu_slot(parity, COMPOSE_BASE + 8), out, g_fmt); // root-table u0
    write_uav(cpu_slot(parity, COMPOSE_BASE + 9), out, g_fmt); // u1 unused by Compose
    cl->SetPipelineState(g_compose);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, COMPOSE_BASE));
    cl->Dispatch((g_w + 15u) / 16u, (g_h + 15u) / 16u, 1u);

    for (unsigned i = 0; i < 2; ++i)
    {
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    cl->ResourceBarrier(2, barriers);
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = out;
    cl->ResourceBarrier(1, &ub);

    const bool had_history = g_history_valid && g_limit_residual;
    if (!g_limit_residual)
    {
        const bool had_local_history = g_history_valid;
        barrier_to(cl, g_history, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        constants(cl, low_width, low_height);
        write_srv(cpu_slot(parity, CAPTURE_BASE), low_proxy, g_fmt);
        write_srv(cpu_slot(parity, CAPTURE_BASE + 1), g_depth_valid ? depth : low_proxy,
                  g_depth_valid ? DXGI_FORMAT_R32_FLOAT : g_fmt);
        write_srv(cpu_slot(parity, CAPTURE_BASE + 2), low_proxy, g_fmt);
        write_srv(cpu_slot(parity, CAPTURE_BASE + 3), g_a, DXGI_FORMAT_R16G16B16A16_FLOAT);
        write_srv(cpu_slot(parity, CAPTURE_BASE + 4), low_proxy, g_fmt);
        write_srv(cpu_slot(parity, CAPTURE_BASE + 5), low_proxy, g_fmt);
        write_srv(cpu_slot(parity, CAPTURE_BASE + 6), low_proxy, g_fmt);
        write_srv(cpu_slot(parity, CAPTURE_BASE + 7), mask_base, g_fmt);
        write_uav(cpu_slot(parity, CAPTURE_BASE + 8), g_history, DXGI_FORMAT_R16G16B16A16_FLOAT);
        write_uav(cpu_slot(parity, CAPTURE_BASE + 9), g_history, DXGI_FORMAT_R16G16B16A16_FLOAT);
        cl->SetPipelineState(g_capture);
        cl->SetComputeRootDescriptorTable(1, gpu_slot(parity, CAPTURE_BASE));
        cl->Dispatch((low_width + 15u) / 16u, (low_height + 15u) / 16u, 1u);
        barrier_to(cl, g_history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_history_valid = true;
        if (had_local_history && ++g_limit_frames == 1ull)
        {
            char line[260];
            snprintf(line, sizeof line,
                     "[MGPU][PX1][P%s] same-frame de-speckle active: eight-neighbour residual luma bounds +/-0.10 log2; no history reprojection, full intensity retained.",
                     kPassId);
            mgpu::diag::info(line);
        }
    }
    else
    {
        barrier_to(cl, g_a, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier_to(cl, g_history, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(g_history, g_a);
        barrier_to(cl, g_a, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier_to(cl, g_history, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_history_valid = true;
        if (had_history && ++g_limit_frames == 1ull)
            mgpu::diag::info("[MGPU][PX1] X1 residual jump limiter active: cap=+/-0.12 log2, scene-cut threshold=0.75; 300% intensity unchanged.");
    }
    if (mark != nullptr) mark(mark_context, 21u); // PX1 Compose ends

    ++g_applies;
    if (g_applies == 1 || g_applies % 600ull == 0ull)
    {
        char line[240];
        snprintf(line, sizeof line,
                 "[MGPU][PX1][P%s] current-frame GuidedFit+Compose applied=%llu low=%ux%u native=%ux%u.",
                 kPassId, g_applies, low_width, low_height, g_w, g_h);
        mgpu::diag::info(line);
    }
    return true;
}

void set_intensity(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 3.0f) value = 3.0f;
    g_intensity.store(value, std::memory_order_relaxed);
}

float intensity()
{
    return g_intensity.load(std::memory_order_relaxed);
}

void set_guard(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    g_guard.store(value, std::memory_order_relaxed);
}

float guard()
{
    return g_guard.load(std::memory_order_relaxed);
}

void reset_history()
{
    g_history_valid = false;
}

void release()
{
    release_all();
}
}
