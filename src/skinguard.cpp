// skinguard.cpp - [OURS] post-pass skin gating. See skinguard.hpp for the design.
//
// v6 (2026-10-24): ladder diagnostics. History of write paths on this rig:
//   - descriptor-table compute: recorded, NEVER landed (RB byte-identical)
//   - root-descriptor compute on a verified-DIRECT list: recorded, NEVER
//     landed (constant-mid-gray probe stayed game picture)
//   - fullscreen-triangle graphics pass: CopyResource RT->target LANDS
//     (target turns black), but the DRAW leaves the RT at its clear value.
//     Two live suspects: root constants not delivered to the shader, and
//     buffers transitioned to NON_PIXEL_SHADER_RESOURCE while a PIXEL shader
//     reads them (spec violation - reads return zero, blend of zeros is
//     black). v6 fixes the state bug AND ladders the diagnosis across the
//     SkinGuard ini value so one build separates every variable:
//
//     SkinGuard=1  full masked blend (buffers in PIXEL_SHADER_RESOURCE)
//     SkinGuard=2  A/B CopyResource passthrough (no draw)
//     SkinGuard=3  constant mid-gray, hardcoded, no loads, no constant reads
//                  -> if the screen is NOT gray, the draw mechanics themselves
//                     are dead
//     SkinGuard=4  constants probe: paints (gDims.x/4096, gShow/4, gRad.x/100)
//                  -> if the screen is NOT reddish-purple, root constants are
//                     not reaching the pixel shader
//
// All draw modes bind the same root signature (32-bit constants + 3 buffer
// SRVs) and run the same fullscreen-triangle VS. Only 4 bpp formats.
#include "skinguard.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "diag.hpp"

namespace
{
// ---- tuning (configure) ----
// loose chroma locus x 1px-scale luma smoothness, retrained from measured
// Endfield skin pixels (face CbCr fully overlaps pavement in colour alone;
// the smoothness gate is the real discriminator).
float g_cb = 117.0f, g_cr = 142.0f;    // skin locus centre, 8-bit Cb/Cr
float g_rin = 11.0f, g_rout = 26.0f;   // full / zero radii

// ---- GPU objects ----
std::atomic<bool> g_on{false};  // menu thread writes; bridge thread reads
int  g_mode = 0;   // 1 blend, 2 copy, 3 show-gray, 4 const-probe
ID3D12Device *g_dev = nullptr;              // borrowed (stream device)
Microsoft::WRL::ComPtr<ID3D12Resource> g_b_p1;       // pass-1 output copy (R)
Microsoft::WRL::ComPtr<ID3D12Resource> g_b_final;    // final pass copy (R)
Microsoft::WRL::ComPtr<ID3D12Resource> g_b_native;   // native frame copy (R)
Microsoft::WRL::ComPtr<ID3D12Resource> g_rt;         // blend render target
Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv{};
Microsoft::WRL::ComPtr<ID3D12RootSignature> g_rs;
Microsoft::WRL::ComPtr<ID3D12PipelineState> g_pso_blend;
Microsoft::WRL::ComPtr<ID3D12PipelineState> g_pso_show;
Microsoft::WRL::ComPtr<ID3D12PipelineState> g_pso_probe;
Microsoft::WRL::ComPtr<ID3D12PipelineState> g_pso_p1;
Microsoft::WRL::ComPtr<ID3D12PipelineState> g_pso_p2;
Microsoft::WRL::ComPtr<ID3D12PipelineState> g_pso_mask;
unsigned g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
unsigned g_bpp = 0;
UINT g_pitch = 0;
unsigned long long g_frames = 0;

// diagnostic readback (RB lines in the log): one snapshot every 300 frames,
// consumed 300 frames later. The stream thread fences every frame, so the
// copy is long complete by the time we map.
Microsoft::WRL::ComPtr<ID3D12Resource> g_rb_target;  // target texture snapshot
bool g_rb_pending = false;
UINT g_rb_pitch = 0;

// one-shot 4-way source dump (native / p1 / final / target), snapshotted at
// f==300 and written to BMP at the next 300-frame boundary, when the copies
// are guaranteed complete. Answers "which stage holds dark/garbage" in one run.
Microsoft::WRL::ComPtr<ID3D12Resource> g_rb_src[3];
bool g_dump_armed = false;

const char *k_hlsl = R"(
struct PIn { float4 pos : SV_Position; };

// fullscreen triangle from SV_VertexID, no vertex buffer
PIn vs(uint vid : SV_VertexID)
{
    PIn o;
    float2 xy = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(xy * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

cbuffer C : register(b0)
{
    float2 gCenter;    // Cb, Cr of the skin locus
    float2 gRad;       // x: full-protect radius, y: zero-protect radius
    float  gTemporal;  // reserved
    uint   gInit;      // reserved
    uint   gShow;      // reserved
    uint   _pad0;      // REQUIRED 4-byte pad: uint2 may not straddle a 16-byte
                       // cbuffer boundary, the compiler inserts it either way.
                       // Keep it explicit so the C++ 12-value send stays aligned.
    uint2  gDims;
    float2 gPad;
};

// mode 3 probe: constant gray, no loads, no constant reads. If the screen is
// not uniformly gray, the draw mechanics themselves are dead.
float4 ps_show(PIn i) : SV_Target
{
    return float4(0.5, 0.5, 0.5, 1.0);
}

// mode 4 probe: visualize the constants. Expected at 3414x1920 mode 4 rin 10:
// R = 3414/4096 = 0.83, G = 4/4 = 1.0, B = 10/100 = 0.10 - a bright
// purple-pink. Black means the root constants never reached the shader.
float4 ps_probe(PIn i) : SV_Target
{
    return float4(gDims.x / 4096.0, gShow / 4.0, gRad.x / 100.0, 1.0);
}

ByteAddressBuffer gNative : register(t0);
ByteAddressBuffer gFinal  : register(t1);
ByteAddressBuffer gP1     : register(t2);

// Four bytes per pixel. RE2 uses R10G10B10A2; the original Yihuan path uses
// R8G8B8A8. gPad.y selects the actual packing so both keep their colour.
float3 ld3(ByteAddressBuffer b, uint byte)
{
    uint w = b.Load(byte);
    if (gPad.y > 0.5)
        return float3(w & 0x3FFu, (w >> 10) & 0x3FFu, (w >> 20) & 0x3FFu) / 1023.0;
    return float3(w & 0xFFu, (w >> 8) & 0xFFu, (w >> 16) & 0xFFu) / 255.0;
}

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float skin_locus(float3 c)
{
    float cb = 128.0 + (-0.168736 * c.r - 0.331264 * c.g + 0.5       * c.b) * 255.0;
    float cr = 128.0 + ( 0.5      * c.r - 0.418688 * c.g - 0.081312 * c.b) * 255.0;
    float m  = 1.0 - smoothstep(gRad.x, gRad.y, distance(float2(cb, cr), gCenter));
    // Saturation gate: measured Endfield skin cr-cb = 25..39 (tan 阿列什)
    // and >= 10 (pale); olive/yellow cloth measures ~7.7, neutral grey ~2.
    // Threshold 8 with a 0.3 margin on cloth is safe because the gate
    // MULTIPLIES the locus ramp - worst-case cloth leak is m*0.03.
    float sat = saturate(((cr - cb) - 8.0) / 4.0);
    return m * sat;
}

// flat = 1, edges/texture = 0. 1px radius on purpose: character shading
// gradients live at 5-15px scale and would reject faces. 5 taps (centre luma
// is passed in, 4 neighbours loaded) - the 17-tap version hung GPU 1 at f=1.
float smoothness(uint2 base, float lc)
{
    const int2 mx = int2((int)gDims.x - 1, (int)gDims.y - 1);
    const int2 b0 = clamp(int2(base) + int2( 1, 0), int2(0, 0), mx);
    const int2 b1 = clamp(int2(base) + int2(-1, 0), int2(0, 0), mx);
    const int2 b2 = clamp(int2(base) + int2( 0, 1), int2(0, 0), mx);
    const int2 b3 = clamp(int2(base) + int2( 0,-1), int2(0, 0), mx);
    float acc = lc
        + luma(ld3(gNative, b0.y * (uint)gPad.x + b0.x * 4u))
        + luma(ld3(gNative, b1.y * (uint)gPad.x + b1.x * 4u))
        + luma(ld3(gNative, b2.y * (uint)gPad.x + b2.x * 4u))
        + luma(ld3(gNative, b3.y * (uint)gPad.x + b3.x * 4u));
    float hp = abs(lc - acc * 0.2f);
    return 1.0 - smoothstep(0.006, 0.025, hp);
}

// mode 5 probe: ONE raw buffer read per pixel, no math. Pass-1 copy
// passthrough - the whole screen shows the 1-layer picture. If this hangs
// the device, ANY pixel-shader raw-buffer read is fatal on this rig.
float4 ps_p1(PIn i) : SV_Target
{
    uint2 coord = uint2(i.pos.x, i.pos.y);
    coord.x = min(coord.x, gDims.x - 1u);
    coord.y = min(coord.y, gDims.y - 1u);
    const uint cb4 = coord.y * (uint)gPad.x + coord.x * 4u;
    return float4(ld3(gP1, cb4), 1.0);
}

// mode 6 probe: THREE raw buffer reads per pixel, no mask math. Both
// passthrough pictures stay visible (p1 dominant). Separates "one read is
// fatal" from "the multi-read / multi-binding case is fatal".
float4 ps_p2(PIn i) : SV_Target
{
    uint2 coord = uint2(i.pos.x, i.pos.y);
    coord.x = min(coord.x, gDims.x - 1u);
    coord.y = min(coord.y, gDims.y - 1u);
    const uint cb4 = coord.y * (uint)gPad.x + coord.x * 4u;
    float3 t = ld3(gNative, cb4) + ld3(gFinal, cb4);   // both read, summed away
    return float4(ld3(gP1, cb4) + t * 0.0001, 1.0);
}

// mode 7: mask visualisation - white = skin (kept on pass-1), black = world.
float4 ps_mask(PIn i) : SV_Target
{
    uint2 coord = uint2(i.pos.x, i.pos.y);
    coord.x = min(coord.x, gDims.x - 1u);
    coord.y = min(coord.y, gDims.y - 1u);
    const uint cb4 = coord.y * (uint)gPad.x + coord.x * 4u;
    float m = skin_locus(ld3(gNative, cb4));
    return float4(m, m, m, 1.0);
}

// mode 1: the real masked blend. Skin keeps pass-1, the world keeps pass-2.
// Locus-only mask: all 3 loads sit at the SAME address (the access pattern
// mode 6 proves safe on this driver). The 17-tap and 5-tap smoothness
// versions both hung GPU 1 at f=1 - neighbour loads at distinct addresses in
// the same warp are the prime suspect. Chroma-only will over-protect flat
// pavement (its CbCr overlaps skin); refine later if it ships.
float4 ps_blend(PIn i) : SV_Target
{
    uint2 coord = uint2(i.pos.x, i.pos.y);
    coord.x = min(coord.x, gDims.x - 1u);
    coord.y = min(coord.y, gDims.y - 1u);
    const uint cb4 = coord.y * (uint)gPad.x + coord.x * 4u;

    float3 nat = ld3(gNative, cb4);
    float m    = skin_locus(nat);

    float3 p1 = ld3(gP1, cb4);
    float3 p2 = ld3(gFinal, cb4);
    float3 o  = m * p1 + (1.0 - m) * p2;
    return float4(o, 1.0);
}
)";

unsigned fmt_bpp(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return 4;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R32_FLOAT:          return 4;
    case DXGI_FORMAT_R16_FLOAT:          return 2;
    case DXGI_FORMAT_R16_UNORM:          return 2;
    default:                             return 4;
    }
}

void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}

ID3D12Resource *make_buffer(ID3D12Device *dev, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = { 1, 0 };
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

ID3D12Resource *make_rt(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc = { 1, 0 };
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
    D3D12_CLEAR_VALUE cv{};
    cv.Format = fmt;
    memcpy(cv.Color, clear, sizeof clear);
    ID3D12Resource *r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            &cv, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

ID3D12Resource *make_readback(ID3D12Device *dev, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = { 1, 0 };
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

void footprint_loc(ID3D12Resource *buf, unsigned w, unsigned h,
                   UINT pitch, DXGI_FORMAT fmt, D3D12_TEXTURE_COPY_LOCATION *loc)
{
    loc->pResource = buf;
    loc->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    loc->PlacedFootprint.Offset = 0;
    loc->PlacedFootprint.Footprint.Width = w;
    loc->PlacedFootprint.Footprint.Height = h;
    loc->PlacedFootprint.Footprint.Depth = 1;
    loc->PlacedFootprint.Footprint.RowPitch = pitch;
    loc->PlacedFootprint.Footprint.Format = fmt;
}

void subres_loc(ID3D12Resource *tex, D3D12_TEXTURE_COPY_LOCATION *loc)
{
    loc->pResource = tex;
    loc->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    loc->SubresourceIndex = 0;
}

void copy_tex_to_buf(ID3D12GraphicsCommandList *cl, ID3D12Resource *tex, D3D12_RESOURCE_STATES tex_rest,
                     ID3D12Resource *buf, unsigned w, unsigned h, UINT pitch, DXGI_FORMAT fmt)
{
    if (tex_rest != D3D12_RESOURCE_STATE_COPY_SOURCE)
        transition(cl, tex, tex_rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION d, s;
    footprint_loc(buf, w, h, pitch, fmt, &d);
    subres_loc(tex, &s);
    cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    if (tex_rest != D3D12_RESOURCE_STATE_COPY_SOURCE)
        transition(cl, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, tex_rest);
}

// 24-bit BMP from a pitch-strided RGBA8 buffer (rows bottom-up, BGR).
bool write_bmp(const char *path, const unsigned char *data, unsigned w, unsigned h, UINT pitch)
{
    FILE *fp = std::fopen(path, "wb");
    if (fp == nullptr) return false;
    const UINT row_out = (w * 3u + 3u) & ~3u;
    const UINT imgsize = row_out * h;
    unsigned char hdr[54]{};
    hdr[0] = 'B'; hdr[1] = 'M';
    *reinterpret_cast<unsigned *>(hdr + 2) = 54u + imgsize;
    *reinterpret_cast<unsigned *>(hdr + 10) = 54u;
    *reinterpret_cast<unsigned *>(hdr + 14) = 40u;
    *reinterpret_cast<unsigned *>(hdr + 18) = w;
    *reinterpret_cast<unsigned *>(hdr + 22) = h;
    *reinterpret_cast<unsigned short *>(hdr + 26) = 1u;
    *reinterpret_cast<unsigned short *>(hdr + 28) = 24u;
    *reinterpret_cast<unsigned *>(hdr + 34) = imgsize;
    std::fwrite(hdr, 1, 54, fp);
    unsigned char *row = static_cast<unsigned char *>(std::calloc(row_out, 1));
    for (unsigned y = 0; y < h; ++y)
    {
        const unsigned char *s = data + (size_t)(h - 1u - y) * pitch;
        for (unsigned x = 0; x < w; ++x)
        {
            row[x * 3 + 0] = s[x * 4 + 2];
            row[x * 3 + 1] = s[x * 4 + 1];
            row[x * 3 + 2] = s[x * 4 + 0];
        }
        std::fwrite(row, 1, row_out, fp);
    }
    std::free(row);
    std::fclose(fp);
    return true;
}

void log_rb_stats(unsigned long long f)
{
    if (!g_rb_pending) return;
    g_rb_pending = false;
    // consume the one-shot source dump: copies queued at f==300 are complete
    if (g_dump_armed)
    {
        g_dump_armed = false;
        const char *names[4] = { "sg_native.bmp", "sg_p1.bmp", "sg_final.bmp", nullptr };
        ID3D12Resource *rbs[4] = { g_rb_src[0].Get(), g_rb_src[1].Get(), g_rb_src[2].Get(), g_rb_target.Get() };
        const char *dir = "C:\\Users\\win11\\Documents\\kimi\\tasks\\2026-11-15\\07-50-31-bfa37d58\\re2-display\\";
        for (int i = 0; i < 4; ++i)
        {
            const char *nm = (i < 3) ? names[i] : "sg_target.bmp";
            if (rbs[i] == nullptr) continue;
            void *p = nullptr;
            if (SUCCEEDED(rbs[i]->Map(0, nullptr, &p)) && p != nullptr)
            {
                char full[300];
                std::snprintf(full, sizeof full, "%s%s", dir, nm);
                const bool ok = write_bmp(full, static_cast<const unsigned char *>(p), g_w, g_h,
                                          (i < 3) ? g_pitch : g_rb_pitch);
                rbs[i]->Unmap(0, nullptr);
                char l[320];
                std::snprintf(l, sizeof l, "[MGPU][SG] DUMP %s %s", nm, ok ? "written" : "FAILED");
                mgpu::diag::info(l);
            }
        }
    }
    char out[400];
    int n = 0;
    out[0] = '\0';
    if (g_rb_target != nullptr)
    {
        void *p = nullptr;
        if (SUCCEEDED(g_rb_target->Map(0, nullptr, &p)) && p != nullptr)
        {
            const char *base = (const char *)p;
            const size_t total = (size_t)g_w * g_h;
            double sum_r = 0.0, sum_rg = 0.0; size_t gray = 0;
            for (unsigned y = 0; y < g_h; ++y)
            {
                const unsigned char *c = (const unsigned char *)(base + (size_t)y * g_rb_pitch);
                for (unsigned x = 0; x < g_w; ++x)
                {
                    const int r = c[x * 4 + 0], g = c[x * 4 + 1], b = c[x * 4 + 2];
                    sum_r += r;
                    const int rg = (r > g) ? (r - g) : (g - r);
                    const int gb = (g > b) ? (g - b) : (b - g);
                    sum_rg += rg;
                    if (rg <= 1 && gb <= 1) ++gray;
                }
            }
            g_rb_target->Unmap(0, nullptr);
            n += std::snprintf(out + n, sizeof out - n, " tgt[meanR %.1f meanRGdiff %.2f grayfrac %.3f]",
                               (float)(sum_r / (double)total), (float)(sum_rg / (double)total),
                               (float)((double)gray / (double)total));
        }
    }
    if (n > 0)
    {
        char l[480];
        std::snprintf(l, sizeof l, "[MGPU][SG] RB f=%llu%s", f, out);
        mgpu::diag::info(l);
    }
}

bool compile_one(HMODULE dc, const char *entry, const char *target, ID3DBlob **out)
{
    typedef HRESULT (WINAPI *compile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                        ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob **, ID3DBlob **);
    compile_t compile = (compile_t)GetProcAddress(dc, "D3DCompile");
    if (compile == nullptr) return false;
    Microsoft::WRL::ComPtr<ID3DBlob> err;
    HRESULT hr = compile(k_hlsl, std::strlen(k_hlsl), "skinguard", nullptr, nullptr,
                         entry, target, 0, 0, out, &err);
    if (FAILED(hr))
    {
        const char *m = (err != nullptr) ? (const char *)err->GetBufferPointer() : "?";
        char l[400];
        std::snprintf(l, sizeof l, "[MGPU][SG] shader compile failed (%s/%s): %.300s",
                      entry, target, m);
        mgpu::diag::error(l);
        return false;
    }
    return true;
}
}   // namespace

namespace mgpu::skinguard
{
void configure(float cb, float cr, float rin, float rout, float temporal)
{
    g_cb = cb; g_cr = cr; g_rin = rin; g_rout = rout;
    (void)temporal;   // v5+: no temporal history (no mask buffer)
}

bool arm(int mode, ID3D12Device *dev, unsigned width, unsigned height,
         DXGI_FORMAT fmt, const char *shape_refusal)
{
    if (mode < 0 || mode > 7) return false;
    // Build even when SkinGuard=0 so the Prism menu can enable it live.
    if (shape_refusal != nullptr)
    {
        char l[300];
        std::snprintf(l, sizeof l,
                      "[MGPU][SG] SkinGuard OFF - %s. Remove it and re-arm.", shape_refusal);
        mgpu::diag::info(l);
        return false;
    }
    if (dev == nullptr || width == 0 || height == 0) return false;

    g_bpp = fmt_bpp(fmt);
    if (g_bpp != 4u)
    {
        char l[160];
        std::snprintf(l, sizeof l, "[MGPU][SG] SkinGuard OFF - %u bpp format not supported by v6", g_bpp);
        mgpu::diag::info(l);
        return false;
    }
    g_pitch = (width * g_bpp + 255u) & ~255u;

    // Buffers first: everything else reports through them.
    const UINT64 size = (UINT64)g_pitch * height;
    g_b_p1.Attach(make_buffer(dev, size));
    g_b_final.Attach(make_buffer(dev, size));
    g_b_native.Attach(make_buffer(dev, size));
    g_rt.Attach(make_rt(dev, width, height, fmt));
    if (g_b_p1 == nullptr || g_b_final == nullptr || g_b_native == nullptr || g_rt == nullptr)
    { release(); return false; }

    // RTV heap (one descriptor, not shader-visible)
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap))))
    { release(); return false; }
    g_rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(g_rt.Get(), nullptr, g_rtv);

    // Root signature: 32-bit constants + 3 root SRVs (buffers, PS reads).
    D3D12_ROOT_PARAMETER rp[4]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 12;
    rp[0].Constants.RegisterSpace = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 0; i < 3; ++i)
    {
        rp[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[1 + i].Descriptor.ShaderRegister = (unsigned)i;
        rp[1 + i].Descriptor.RegisterSpace = 0;
        rp[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &sig, &err))) { release(); return false; }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&g_rs)))) { release(); return false; }

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (dc == nullptr) { mgpu::diag::error("[MGPU][SG] d3dcompiler_47.dll not loadable"); release(); return false; }
    Microsoft::WRL::ComPtr<ID3DBlob> vs_code, ps_blend, ps_show, ps_probe, ps_p1, ps_p2, ps_mask;
    if (!compile_one(dc, "vs", "vs_5_0", &vs_code) ||
        !compile_one(dc, "ps_blend", "ps_5_0", &ps_blend) ||
        !compile_one(dc, "ps_show", "ps_5_0", &ps_show) ||
        !compile_one(dc, "ps_probe", "ps_5_0", &ps_probe) ||
        !compile_one(dc, "ps_p1", "ps_5_0", &ps_p1) ||
        !compile_one(dc, "ps_p2", "ps_5_0", &ps_p2) ||
        !compile_one(dc, "ps_mask", "ps_5_0", &ps_mask))
    { release(); return false; }

    auto make_pso = [&](ID3DBlob *psb, ID3D12PipelineState **out) -> bool {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psd{};
        psd.pRootSignature = g_rs.Get();
        psd.VS = { vs_code->GetBufferPointer(), vs_code->GetBufferSize() };
        psd.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
        psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        // v6 bug (the whole black-screen): the desc is zero-initialized and
        // RenderTargetWriteMask defaulted to 0 = NO colour channels written.
        // The draw executed every frame and discarded every fragment at the
        // write-back stage, leaving the RT at its clear value.
        psd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psd.DepthStencilState.DepthEnable = FALSE;
        psd.DepthStencilState.StencilEnable = FALSE;
        psd.SampleMask = UINT_MAX;
        psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psd.NumRenderTargets = 1;
        psd.RTVFormats[0] = fmt;
        psd.SampleDesc = { 1, 0 };
        return SUCCEEDED(dev->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(out)));
    };
    if (!make_pso(ps_blend.Get(), &g_pso_blend) ||
        !make_pso(ps_show.Get(), &g_pso_show) ||
        !make_pso(ps_probe.Get(), &g_pso_probe) ||
        !make_pso(ps_p1.Get(), &g_pso_p1) ||
        !make_pso(ps_p2.Get(), &g_pso_p2) ||
        !make_pso(ps_mask.Get(), &g_pso_mask))
    { release(); return false; }

    g_dev = dev; g_w = width; g_h = height; g_fmt = fmt; g_frames = 0;
    g_mode = (mode > 0) ? mode : 1;
    g_on.store(mode > 0, std::memory_order_relaxed);
    char l[360];
    std::snprintf(l, sizeof l,
                  "[MGPU][SG] SkinGuard armed mode=%d enabled=%d v6-ps %ux%u fmt=%u cb=%.0f cr=%.0f rin=%.0f rout=%.0f",
                  g_mode, g_on.load(std::memory_order_relaxed) ? 1 : 0, width, height,
                  (unsigned)fmt, g_cb, g_cr, g_rin, g_rout);
    mgpu::diag::info(l);
    return true;
}

bool active() { return g_on.load(std::memory_order_relaxed); }

void set_enabled(bool on)
{
    if (g_dev == nullptr) return;
    const bool old = g_on.exchange(on, std::memory_order_relaxed);
    if (old != on)
    {
        char l[160];
        std::snprintf(l, sizeof l, "[MGPU][SG] SkinGuard %s by Prism menu (mode=%d)",
                      on ? "ON" : "OFF", g_mode);
        mgpu::diag::info(l);
    }
}

bool compose(ID3D12GraphicsCommandList *cl, unsigned parity,
             ID3D12Resource *native, ID3D12Resource *target, ID3D12Resource *final,
             unsigned w, unsigned h, D3D12_RESOURCE_STATES native_rest)
{
    if (!g_on.load(std::memory_order_relaxed) || cl == nullptr ||
        native == nullptr || target == nullptr || final == nullptr)
        return false;
    static bool s_type_logged = false;
    if (!s_type_logged)
    {
        s_type_logged = true;
        char tl[160];
        std::snprintf(tl, sizeof tl, "[MGPU][SG] compose command list type=%d (0=dir 1=copy 2=comp)",
                      (int)cl->GetType());
        mgpu::diag::info(tl);
    }
    if (w != g_w || h != g_h || g_bpp != 4u) return false;
    if (target == final) return false;   // defensive: never blend aliased inputs

    // ---- mode 2 A/B: no draw, plain in-place copy final -> target.
    if (g_mode == 2)
    {
        transition(cl, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cl, final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyResource(target, final);
        transition(cl, final, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cl, target, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ++g_frames;
        if (g_frames <= 5 || (g_frames % 300ull) == 0ull)
            mgpu::diag::info("[MGPU][SG] mode2 in-place copy (no draw)");
        return true;
    }

    // consume the previous diagnostic snapshot (its copy completed long ago)
    if ((g_frames % 300ull) == 0ull) log_rb_stats(g_frames);

    // ---- 1. texture -> buffer copies (footprints, reliable all day)
    copy_tex_to_buf(cl, native, native_rest, g_b_native.Get(), w, h, g_pitch, g_fmt);
    copy_tex_to_buf(cl, final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    g_b_final.Get(), w, h, g_pitch, g_fmt);
    copy_tex_to_buf(cl, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    g_b_p1.Get(), w, h, g_pitch, g_fmt);

    // ---- 2. buffers readable by the PIXEL shader (v6 fix: was
    // NON_PIXEL_SHADER_RESOURCE, a spec violation for PS reads - the likely
    // source of the all-black blend)
    transition(cl, g_b_native.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(cl, g_b_final.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(cl, g_b_p1.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ---- 3. fullscreen-triangle pass into our render target
    ID3D12PipelineState *pso = g_pso_blend.Get();
    if (g_mode == 3)      pso = g_pso_show.Get();
    else if (g_mode == 4) pso = g_pso_probe.Get();
    else if (g_mode == 5) pso = g_pso_p1.Get();
    else if (g_mode == 6) pso = g_pso_p2.Get();
    else if (g_mode == 7) pso = g_pso_mask.Get();

    cl->SetGraphicsRootSignature(g_rs.Get());
    cl->SetPipelineState(pso);
    // Slots must mirror the shader cbuffer EXACTLY, including the compiler's
    // 16-byte-alignment pad before uint2 gDims (declared as _pad0 there):
    //   c0,c1=gCenter  c2,c3=gRad  c4=gTemporal  c5=gInit  c6=gShow
    //   c7=_pad0  c8,c9=gDims(w,h)  c10,c11=gPad(pitch,0)
    float c[12];
    c[0] = g_cb; c[1] = g_cr; c[2] = g_rin; c[3] = g_rout;
    c[4] = 0.0f;                                    // gTemporal
    unsigned *cu = reinterpret_cast<unsigned *>(&c[5]);
    cu[0] = 0u;                                     // c5  gInit
    cu[1] = (unsigned)g_mode;                       // c6  gShow
    cu[2] = 0u;                                     // c7  _pad0 (keep zeroed)
    cu[3] = w;                                      // c8  gDims.x
    c[9] = (float)h;                                // c9  gDims.y
    c[10] = (float)g_pitch;                         // c10 gPad.x: footprint row
                                                    // pitch in BYTES (256-aligned)
    c[11] = (g_fmt == DXGI_FORMAT_R10G10B10A2_UNORM) ? 1.0f : 0.0f; // packed format
    cl->SetGraphicsRoot32BitConstants(0, 12, c, 0);
    cl->SetGraphicsRootShaderResourceView(1, g_b_native->GetGPUVirtualAddress());
    cl->SetGraphicsRootShaderResourceView(2, g_b_final->GetGPUVirtualAddress());
    cl->SetGraphicsRootShaderResourceView(3, g_b_p1->GetGPUVirtualAddress());

    const D3D12_VIEWPORT vp{ 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
    const D3D12_RECT sc{ 0, 0, (LONG)w, (LONG)h };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->OMSetRenderTargets(1, &g_rtv, FALSE, nullptr);
    cl->DrawInstanced(3, 1, 0, 0);
    ++g_frames;
    if (g_frames <= 5 || (g_frames % 300ull) == 0ull)
    {
        char l[200];
        std::snprintf(l, sizeof l, "[MGPU][SG] v6 drawn f=%llu mode=%d", g_frames, g_mode);
        mgpu::diag::info(l);
    }
    cl->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // ---- 4. buffers back to their resting COPY_DEST
    transition(cl, g_b_native.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    transition(cl, g_b_final.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    transition(cl, g_b_p1.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);

    // ---- 5. render target -> the present target texture
    transition(cl, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_DEST);
    transition(cl, g_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(target, g_rt.Get());
    transition(cl, g_rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(cl, target, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ---- 6. diagnostic snapshot (stats cadence)
    if ((g_frames % 300ull) == 0ull)
    {
        if (g_rb_target == nullptr)
        {
            g_rb_target.Attach(make_readback(g_dev, (UINT64)((w * 4u + 255u) & ~255u) * h));
            g_rb_pitch = (w * 4u + 255u) & ~255u;
            if (g_rb_target != nullptr)
                mgpu::diag::info("[MGPU][SG] RB readback buffer created (v6 build)");
        }
        if (g_rb_target != nullptr)
        {
            copy_tex_to_buf(cl, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            g_rb_target.Get(), w, h, g_rb_pitch, g_fmt);
            g_rb_pending = true;
        }
    }

    // ---- 7. one-shot source dump: snapshot the three input buffers at f==300
    if (g_frames == 300)
    {
        const UINT64 size = (UINT64)g_pitch * g_h;
        for (int i = 0; i < 3; ++i)
            if (g_rb_src[i] == nullptr)
                g_rb_src[i].Attach(make_readback(g_dev, size));
        ID3D12Resource *srcs[3] = { g_b_native.Get(), g_b_p1.Get(), g_b_final.Get() };
        for (int i = 0; i < 3; ++i)
            if (g_rb_src[i] != nullptr)
            {
                transition(cl, srcs[i], D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
                cl->CopyBufferRegion(g_rb_src[i].Get(), 0, srcs[i], 0, size);
                transition(cl, srcs[i], D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_COPY_DEST);
            }
        g_dump_armed = true;
    }
    return true;
}

void release()
{
    g_pso_blend.Reset(); g_pso_show.Reset(); g_pso_probe.Reset();
    g_pso_p1.Reset(); g_pso_p2.Reset(); g_pso_mask.Reset();
    g_rs.Reset();
    g_rtv_heap.Reset(); g_rt.Reset();
    g_b_p1.Reset(); g_b_final.Reset(); g_b_native.Reset();
    g_rb_target.Reset(); g_rb_pending = false;
    for (int i = 0; i < 3; ++i) g_rb_src[i].Reset();
    g_dump_armed = false;
    g_dev = nullptr; g_on = false; g_mode = 0; g_frames = 0;
}
}   // namespace mgpu::skinguard
