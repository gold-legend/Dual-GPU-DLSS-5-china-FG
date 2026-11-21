// Experimental DLSS Frame Generation on the neural adapter. OFF by default.
// The official NGX entry points are used directly; no third-party FG plugin is
// loaded. DLSSG1/Dave's public secondary-adapter design informed the scheduler.

#include "fg_gpu1.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

#include "diag.hpp"
#include "fg_instr.hpp"
#include "fg_pace.hpp"
#include "fg_pair_flow.hpp"
#include "gpu1_context.hpp"
#include "mgpu_ini_parser.hpp"
#include "native_gpu1.hpp"
#include "native_input.hpp"
#include "../ext/ngx/nvsdk_ngx.h"

namespace mgpu::fg
{
namespace
{
constexpr unsigned SETS = 4u;   // 4x presents 4 frames per set; one more set absorbs the longer holds
using init_ext_fn = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
using caps_fn = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter **);
using alloc_fn = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter **);
using populate_fn = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter *);
using create_fn = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using evaluate_fn = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, void *);
using release_fn = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);

struct set_t
{
    ID3D12Resource *real = nullptr;   // FINAL neural colour, never an intermediate pass.
    ID3D12Resource *gen[3] = {nullptr, nullptr, nullptr};  // FGX=4: up to 3 interpolated frames (mfi 1..3)
    native_gpu1::frame native{};       // Lease stays alive until both presents retire.
    unsigned long long frame = 0;
    float elapsed = 1.0f;            // Game frames spanned by this real neural frame.
    float phase = 0.0f;              // 0 = middle; 2/3 or 3/4 when DLSSG accepted FGPhase; 0.75 = FGX=4 last gen frame.
    unsigned gen_n = 0;              // valid gen frames in gen[0..gen_n-1] (0 = real only)
    bool handed = false;
    UINT64 fence = 0;
    std::atomic<bool> busy{false};
};

std::atomic<bool> g_on{false};
bool g_armed = false, g_diagnose = false, g_bound = false;
arm_info g_i{};
set_t g_set[SETS];
unsigned g_k = 0;
unsigned long long g_fid = 0, g_last_eval_fid = 0, g_last_eval_color = 0;
bool g_need_reset = true;

bool g_tried = false, g_up = false;
HMODULE g_snip = nullptr;
create_fn g_create = nullptr;
evaluate_fn g_eval = nullptr;
release_fn g_rel = nullptr;
NVSDK_NGX_Parameter *g_params = nullptr;
NVSDK_NGX_Handle *g_handle = nullptr;
float g_identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
float g_v2c[16] = {}, g_c2v[16] = {};
float g_fov = 90.0f, g_near = 0.1f, g_far = 10000.0f;
bool g_invert_mv = false, g_mv_pixels = true;
ID3D12Fence *g_eval_fence = nullptr;
uint64_t g_eval_value = 0;

unsigned long long g_made = 0, g_nomv = 0, g_busy_skips = 0, g_errors = 0;
unsigned long long g_evals = 0, g_fallbacks = 0, g_half_n = 0;
std::atomic<unsigned long long> g_pres_gen{0}, g_pres_real{0}, g_handoffs{0}, g_dropped{0};
double g_half_sum = 0, g_ema_ms = 0;
bool g_pace = true;
bool g_pair_flow = true;                // FGPairFlow: global default ON; 0 opts out.
int g_pace_mode = 1;                    // FGPace: 0 immediate, 1 relative, 2 scene clock, 3 absolute.
int g_fgx = 2;                          // FGX: 2 = one interpolated frame (2x), 4 = three (4x, DLSS4 MFG)
bool g_fgx_refused = false;             // a 4x evaluate was rejected: 2x for the rest of this arm
bool g_fgx_fallback_said = false;       // the "driver cannot 4x" line logged once
int g_phase_req = 0;                    // FGPhase: 0, 67 or 75; read again at each arm.
bool g_phase_refused = false, g_phase_said = false;
unsigned g_mfc_max = 0;
unsigned long long g_phase_pairs = 0, g_phase_nofit = 0;
mgpu::fgpace::scene_clock g_scene;       // bridge thread
mgpu::fgpace::clock_state g_clk;         // pacing thread
mgpu::fgpace::abs_state g_abs;           // pacing thread
int g_dcheck = 0;
mgpu::fgpace::display_gate g_dgate;      // bridge thread
LARGE_INTEGER g_dgate_t{};
double g_refresh_ms = 0.0;
unsigned long long g_dgate_real_only = 0;

struct job_t
{
    set_t *set = nullptr;
    unsigned gen_n = 0;        // 0..3 in-between frames to show before the real one
    float r = 0, g = 0, b = 0;
    LARGE_INTEGER t0{};
    double half_ms = 8;
    int pace = 1;
    double ema_ms = 0, c_real = 0, span = 0;
};
std::mutex g_mx;
std::condition_variable g_cv;
std::thread *g_thread = nullptr;
bool g_has_job = false, g_stop = false;
job_t g_job;
std::atomic<bool> g_thread_failed{false};
LARGE_INTEGER g_last_handoff{};
ID3D12Fence *g_pfence = nullptr;
HANDLE g_timer = nullptr;

bool module_dir(wchar_t *dir, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&anchor), &hm)) return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    wcscpy_s(dir, n, mod);
    return true;
}

bool read_keys(int &framegen, unsigned long long &app_id, bool &diagnose, bool &pace)
{
    framegen = 0; app_id = 0x4D475055ull; diagnose = false; pace = true;
    g_pace_mode = 1; g_phase_req = 0; g_dcheck = 0; g_pair_flow = true;
    wchar_t dir[MAX_PATH];
    if (!module_dir(dir, MAX_PATH)) return false;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;
    const char *v = mgpu::config::find(buf.data(), got, "FrameGen");
    framegen = v != nullptr ? atoi(v) : 0;
    v = mgpu::config::find(buf.data(), got, "NativeInput");
    diagnose = v != nullptr && atoi(v) == 1;
    v = mgpu::config::find(buf.data(), got, "FGPace");
    g_pace_mode = v != nullptr ? atoi(v) : 1;
    if (g_pace_mode < 0 || g_pace_mode > 3) g_pace_mode = 1;
    pace = g_pace_mode != 0;
    v = mgpu::config::find(buf.data(), got, "FGX");
    g_fgx = (v != nullptr && atoi(v) == 4) ? 4 : 2;
    v = mgpu::config::find(buf.data(), got, "FGPairFlow");
    g_pair_flow = v == nullptr || atoi(v) != 0;
    v = mgpu::config::find(buf.data(), got, "FGPhase");
    if (v != nullptr && (atoi(v) == 67 || atoi(v) == 75)) g_phase_req = atoi(v);
    v = mgpu::config::find(buf.data(), got, "FGDisplayCheck");
    g_dcheck = v != nullptr && atoi(v) == 1 ? 1 : 0;
    auto fkey = [&](const char *key, float dflt, float lo, float hi) {
        const char *x = mgpu::config::find(buf.data(), got, key);
        if (x == nullptr) return dflt;
        char *end = nullptr; const double d = strtod(x, &end);
        return end == x || d < lo || d > hi ? dflt : (float)d;
    };
    g_fov = fkey("FGFov", 90.0f, 20.0f, 170.0f);
    g_near = fkey("FGNear", 0.1f, 0.0001f, 1000.0f);
    g_far = fkey("FGFar", 10000.0f, 1.0f, 1.0e7f);
    v = mgpu::config::find(buf.data(), got, "FGInvertMV");
    g_invert_mv = v != nullptr && atoi(v) == 1;
    v = mgpu::config::find(buf.data(), got, "FGMvUnit");
    g_mv_pixels = v == nullptr || atoi(v) == 1; // 0.6.0 pixel-unit default for this build.
    v = mgpu::config::find(buf.data(), got, "NgxAppId");
    if (v != nullptr) app_id = (unsigned long long)(unsigned)atoi(v);
    g_pace = pace;
    return true;
}

HMODULE find_core()
{
    HMODULE m = GetModuleHandleW(L"_nvngx.dll");
    if (m != nullptr) return m;
    m = LoadLibraryW(L"_nvngx.dll");
    if (m != nullptr) return m;
    wchar_t dir[MAX_PATH] = {}; DWORD sz = sizeof dir;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ, nullptr, dir, &sz) == ERROR_SUCCESS)
    {
        wchar_t p[MAX_PATH * 2] = {};
        _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%s\\_nvngx.dll", dir);
        return LoadLibraryW(p);
    }
    return nullptr;
}

HMODULE load_snippet(char *where, size_t where_n)
{
    where[0] = '\0';
    wchar_t pd[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) != 0)
    {
        wchar_t root[MAX_PATH] = {}, glob[MAX_PATH] = {};
        _snwprintf_s(root, MAX_PATH, _TRUNCATE, L"%s\\NVIDIA\\NGX\\models\\dlssg\\versions", pd);
        _snwprintf_s(glob, MAX_PATH, _TRUNCATE, L"%s\\*", root);
        unsigned long long best = 0; wchar_t best_dir[MAX_PATH] = {};
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(glob, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
                unsigned long long v = 0; bool numeric = fd.cFileName[0] >= L'0' && fd.cFileName[0] <= L'9';
                for (const wchar_t *c = fd.cFileName; numeric && *c != 0; ++c)
                { if (*c < L'0' || *c > L'9') numeric = false; else v = v * 10ull + (unsigned)(*c - L'0'); }
                if (numeric && v > best) { best = v; wcscpy_s(best_dir, MAX_PATH, fd.cFileName); }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (best != 0)
        {
            wchar_t fglob[MAX_PATH] = {};
            _snwprintf_s(fglob, MAX_PATH, _TRUNCATE, L"%s\\%s\\files\\*.bin", root, best_dir);
            h = FindFirstFileW(fglob, &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                    wchar_t full[MAX_PATH] = {};
                    _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s\\files\\%s", root, best_dir, fd.cFileName);
                    HMODULE m = LoadLibraryW(full);
                    if (m == nullptr) continue;
                    if (GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature") == nullptr) { FreeLibrary(m); continue; }
                    FindClose(h);
                    snprintf(where, where_n, "driver model store %ls", full);
                    return m;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
    }
    wchar_t dir[MAX_PATH];
    if (!module_dir(dir, MAX_PATH)) return nullptr;
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%lsmgpu\\nvngx_dlssg.dll", dir);
    HMODULE m = LoadLibraryW(p);
    if (m != nullptr) snprintf(where, where_n, "mgpu folder %ls", p);
    return m;
}

bool inv4(const float m[16], float out[16])
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

void fail_for_arm(const char *why)
{
    if (!g_on.exchange(false, std::memory_order_acq_rel)) return;
    ++g_fallbacks;
    char line[440];
    snprintf(line, sizeof(line), "[MGPU][FG] disabled for this arm: %s. The worker presents the REAL final neural frame.", why);
    mgpu::diag::error(line);
}

bool ensure_feature(ID3D12GraphicsCommandList *cl)
{
    if (g_up) return true;
    if (g_tried) return false;
    g_tried = true;
    HMODULE core = find_core();
    char where[MAX_PATH * 2 + 40] = {};
    g_snip = load_snippet(where, sizeof where);
    if (core == nullptr || g_snip == nullptr)
    {
        fail_for_arm(core == nullptr ? "NGX core was not found" : "no signed DLSSG model/snippet was found");
        return false;
    }
    auto pick = [&](const char *name, bool snippet) -> FARPROC {
        FARPROC p = GetProcAddress(snippet ? g_snip : core, name);
        return p != nullptr ? p : GetProcAddress(snippet ? core : g_snip, name);
    };
    g_create = (create_fn)pick("NVSDK_NGX_D3D12_CreateFeature", true);
    g_eval = (evaluate_fn)pick("NVSDK_NGX_D3D12_EvaluateFeature", true);
    g_rel = (release_fn)pick("NVSDK_NGX_D3D12_ReleaseFeature", true);
    caps_fn caps = (caps_fn)pick("NVSDK_NGX_D3D12_GetCapabilityParameters", false);
    alloc_fn alloc = (alloc_fn)pick("NVSDK_NGX_D3D12_AllocateParameters", false);
    init_ext_fn init = (init_ext_fn)GetProcAddress(g_snip, "NVSDK_NGX_D3D12_Init_Ext");
    populate_fn populate = (populate_fn)GetProcAddress(g_snip, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    if (g_create == nullptr || g_eval == nullptr || g_rel == nullptr || alloc == nullptr)
    { fail_for_arm("the official DLSSG entry points did not resolve"); return false; }
    NVSDK_NGX_Result r = alloc(&g_params);
    if (r != NVSDK_NGX_Result_Success || g_params == nullptr)
    { g_params = nullptr; fail_for_arm("DLSSG parameter allocation failed"); return false; }
    int fgv = 0; unsigned long long app = 0; bool diag = false, pace = true;
    (void)read_keys(fgv, app, diag, pace);
    wchar_t data_path[MAX_PATH];
    if (!module_dir(data_path, MAX_PATH)) wcscpy_s(data_path, MAX_PATH, L".");
    NVSDK_NGX_Result ir = NVSDK_NGX_Result_Success;
    if (init != nullptr) ir = init(app, data_path, g_i.ndev, NVSDK_NGX_Version_API, g_params);
    if (populate != nullptr) (void)populate(g_params);
    if (caps != nullptr)
    {
        NVSDK_NGX_Parameter *cap = nullptr;
        if (caps(&cap) == NVSDK_NGX_Result_Success && cap != nullptr)
        {
            int available = -1, needs_driver = -1; unsigned init_result = 0, mfc = 0;
            (void)cap->Get("FrameGeneration.Available", &available);
            (void)cap->Get("FrameGeneration.NeedsUpdatedDriver", &needs_driver);
            (void)cap->Get("FrameGeneration.FeatureInitResult", &init_result);
            (void)cap->Get("DLSSG.MultiFrameCountMax", &mfc);
            g_mfc_max = mfc;
            char line[440];
            snprintf(line, sizeof(line), "[MGPU][FG] Init_Ext=0x%08X available=%d driver-needed=%d init=0x%08X MFC-max=%u; 2x only, snippet=%s",
                (unsigned)ir, available, needs_driver, init_result, mfc, where);
            mgpu::diag::info(line);
        }
    }
    auto *p = g_params;
    p->Set("CreationNodeMask", 1u); p->Set("VisibilityNodeMask", 1u);
    p->Set("Width", g_i.w); p->Set("Height", g_i.h);
    p->Set("DLSSG.Width", g_i.w); p->Set("DLSSG.Height", g_i.h);
    p->Set("DLSSG.BackbufferFormat", (unsigned)g_i.fmt);
    p->Set("DLSSG.InternalWidth", g_i.mvec_w != 0u ? g_i.mvec_w : g_i.w);
    p->Set("DLSSG.InternalHeight", g_i.mvec_h != 0u ? g_i.mvec_h : g_i.h);
    p->Set("DLSSG.DynamicResolution", 0u);
    mgpu::diag::info("[MGPU][FG] entering official CreateFeature(FrameGeneration) on the GPU1-local input path");
    r = g_create(cl, NVSDK_NGX_Feature_FrameGeneration, p, &g_handle);
    char line[360];
    snprintf(line, sizeof(line), "[MGPU][FG] CreateFeature on neural card %ux%u fmt=%u MV=%ux%u depth=%ux%u: 0x%08X; success is NOT visual verification",
        g_i.w, g_i.h, (unsigned)g_i.fmt, g_i.mvec_w, g_i.mvec_h, g_i.depth_w, g_i.depth_h, (unsigned)r);
    mgpu::diag::info(line);
    if (r != NVSDK_NGX_Result_Success || g_handle == nullptr)
    { g_handle = nullptr; fail_for_arm("CreateFeature(FrameGeneration) failed"); return false; }
    const float aspect = g_i.h != 0u ? (float)g_i.w / (float)g_i.h : 16.0f / 9.0f;
    const float hrad = g_fov * 3.14159265f / 180.0f;
    const float vfov = 2.0f * atanf(tanf(hrad * 0.5f) / aspect);
    const float ys = 1.0f / tanf(vfov * 0.5f), xs = ys / aspect;
    const float n = g_near, fa = g_far;
    const float A = g_i.depth_inverted ? n / (n - fa) : fa / (fa - n);
    const float B = g_i.depth_inverted ? -fa * n / (n - fa) : -n * fa / (fa - n);
    const float pm[16] = {xs,0,0,0, 0,ys,0,0, 0,0,A,1, 0,0,B,0};
    for (int k = 0; k < 16; ++k) g_v2c[k] = pm[k];
    if (!inv4(g_v2c, g_c2v))
        for (int k = 0; k < 16; ++k) g_c2v[k] = g_identity[k];
    g_up = true;
    return true;
}

void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

void set_sub(NVSDK_NGX_Parameter *p, const char *prefix, unsigned w, unsigned h)
{
    char k[96];
    snprintf(k, sizeof(k), "DLSSG.%sSubrectBaseX", prefix); p->Set(k, 0u);
    snprintf(k, sizeof(k), "DLSSG.%sSubrectBaseY", prefix); p->Set(k, 0u);
    snprintf(k, sizeof(k), "DLSSG.%sSubrectWidth", prefix); p->Set(k, w);
    snprintf(k, sizeof(k), "DLSSG.%sSubrectHeight", prefix); p->Set(k, h);
}

bool extents(ID3D12Resource *r, unsigned &w, unsigned &h, DXGI_FORMAT &fmt)
{
    if (r == nullptr) return false;
    const auto d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width == 0 || d.Height == 0 ||
        d.Width > MGPU_NATIVE_INPUT_MAX_DIM || d.Height > MGPU_NATIVE_INPUT_MAX_DIM ||
        d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1) return false;
    w = (unsigned)d.Width; h = d.Height; fmt = d.Format;
    return true;
}

NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList *cl, set_t &set, ID3D12Resource *out,
                          ID3D12Resource *mvec, ID3D12Resource *depth, float mv_el,
                          bool reset, unsigned long long fid,
                          unsigned mfc = 1u, unsigned mfi = 1u, bool pair_flow = false)
{
    unsigned rw = 0, rh = 0, ow = 0, oh = 0, mw = 0, mh = 0, dw = 0, dh = 0;
    DXGI_FORMAT rf = DXGI_FORMAT_UNKNOWN, of = DXGI_FORMAT_UNKNOWN, mf = DXGI_FORMAT_UNKNOWN, df = DXGI_FORMAT_UNKNOWN;
    if (!extents(set.real, rw, rh, rf) || !extents(out, ow, oh, of) ||
        !extents(mvec, mw, mh, mf) || !extents(depth, dw, dh, df) ||
        rw != g_i.w || rh != g_i.h || ow != rw || oh != rh || rf != g_i.fmt || of != rf ||
        mf != DXGI_FORMAT_R16G16_FLOAT)
        return NVSDK_NGX_Result_Fail;
    const float aspect = g_i.h != 0u ? (float)g_i.w / (float)g_i.h : 16.0f / 9.0f;
    const float hrad = g_fov * 3.14159265f / 180.0f;
    const float vfov = 2.0f * atanf(tanf(hrad * 0.5f) / aspect);
    const float sign = g_invert_mv ? -1.0f : 1.0f;
    // PairFlow already spans the whole real-frame interval in output pixels.
    // Never apply the game's sign, elapsed multiplier or unit conversion again.
    const float sx = pair_flow ? 1.0f : sign * g_i.mvec_scale_x * mv_el / (g_mv_pixels ? 1.0f : (float)mw);
    const float sy = pair_flow ? 1.0f : sign * g_i.mvec_scale_y * mv_el / (g_mv_pixels ? 1.0f : (float)mh);
    auto *p = g_params;
    p->Set("DLSSG.Backbuffer", set.real);
    p->Set("DLSSG.MVecs", mvec);
    p->Set("DLSSG.Depth", depth);
    p->Set("DLSSG.HUDLess", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.UI", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.UIAlpha", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.BidirectionalDistortionField", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.OutputInterpolated", out);
    p->Set("DLSSG.OutputReal", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.OutputDisableInterpolation", (ID3D12Resource *)nullptr);
    p->Set("DLSSG.MultiFrameCount", mfc);
    p->Set("DLSSG.MultiFrameIndex", mfi);
    p->Set("DLSSG.CameraViewToClip", (void *)g_v2c);
    p->Set("DLSSG.ClipToCameraView", (void *)g_c2v);
    p->Set("DLSSG.ClipToLensClip", (void *)g_identity);
    p->Set("DLSSG.ClipToPrevClip", (void *)g_identity);
    p->Set("DLSSG.PrevClipToClip", (void *)g_identity);
    p->Set("DLSSG.JitterOffsetX", 0.0f); p->Set("DLSSG.JitterOffsetY", 0.0f);
    p->Set("DLSSG.MvecScaleX", sx); p->Set("DLSSG.MvecScaleY", sy);
    p->Set("DLSSG.CameraPinholeOffsetX", 0.0f); p->Set("DLSSG.CameraPinholeOffsetY", 0.0f);
    p->Set("DLSSG.CameraPosX", 0.0f); p->Set("DLSSG.CameraPosY", 0.0f); p->Set("DLSSG.CameraPosZ", 0.0f);
    p->Set("DLSSG.CameraUpX", 0.0f); p->Set("DLSSG.CameraUpY", 1.0f); p->Set("DLSSG.CameraUpZ", 0.0f);
    p->Set("DLSSG.CameraRightX", 1.0f); p->Set("DLSSG.CameraRightY", 0.0f); p->Set("DLSSG.CameraRightZ", 0.0f);
    p->Set("DLSSG.CameraFwdX", 0.0f); p->Set("DLSSG.CameraFwdY", 0.0f); p->Set("DLSSG.CameraFwdZ", 1.0f);
    p->Set("DLSSG.CameraNear", g_near); p->Set("DLSSG.CameraFar", g_far);
    p->Set("DLSSG.CameraFOV", vfov); p->Set("DLSSG.CameraAspectRatio", aspect);
    p->Set("DLSSG.ColorBuffersHDR", 0u);
    p->Set("DLSSG.DepthInverted", g_i.depth_inverted ? 1u : 0u);
    p->Set("DLSSG.CameraMotionIncluded", 1u);
    p->Set("DLSSG.Reset", reset ? 1u : 0u);
    p->Set("DLSSG.AutomodeOverrideReset", 0u);
    p->Set("DLSSG.NotRenderingGameFrames", 0u);
    p->Set("DLSSG.OrthoProjection", 0u);
    p->Set("DLSSG.MvecInvalidValue", 0.0f); p->Set("DLSSG.MvecDilated", 0u);
    p->Set("DLSSG.MenuDetectionEnabled", 0u);
    p->Set("DLSSG.BackbufferFrameID", fid);
    set_sub(p, "MVecs", mw, mh); set_sub(p, "Depth", dw, dh);
    set_sub(p, "InputBackbuffer", rw, rh); set_sub(p, "Backbuffer", rw, rh);
    set_sub(p, "OutputInterpolated", ow, oh); set_sub(p, "OutputReal", ow, oh);
    return g_eval(cl, g_handle, p, nullptr);
}

void wait_until(const LARGE_INTEGER &t0, double ms)
{
    LARGE_INTEGER now{}, fq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    if (fq.QuadPart == 0) return;
    double wait_ms = ms - (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    if (wait_ms <= 0.2) return;
    if (wait_ms > 30.0) wait_ms = 30.0;
    if (g_timer == nullptr)
    {
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (g_timer == nullptr) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    if (g_timer != nullptr)
    {
        LARGE_INTEGER due{}; due.QuadPart = -(LONGLONG)(wait_ms * 10000.0);
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
        { (void)WaitForSingleObject(g_timer, 40); return; }
    }
    Sleep((DWORD)wait_ms);
}

bool present_job(const job_t &job, UINT64 &fence)
{
    LARGE_INTEGER fq{};
    QueryPerformanceFrequency(&fq);
    const double k = fq.QuadPart != 0 ? 1000.0 / (double)fq.QuadPart : 0.0;
    const unsigned n = job.gen_n;   // 0..3 in-between frames; the real one is index n
    auto now = [&]() -> double {
        LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
        return (double)q.QuadPart * k;
    };
    auto wait_abs = [&](double t_ms) {
        LARGE_INTEGER zero{}; wait_until(zero, t_ms);
    };
    auto present = [&](unsigned j) -> bool {
        const bool real = j >= n;
        mgpu::gpu1::present_override(real ? job.set->real : job.set->gen[j],
            real ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool ok = mgpu::gpu1::present_frame(job.r, job.g, job.b);
        mgpu::gpu1::present_override(nullptr, D3D12_RESOURCE_STATE_COMMON);
        if (ok) (real ? g_pres_real : g_pres_gen).fetch_add(1, std::memory_order_relaxed);
        fence = (std::max)(fence, mgpu::gpu1::present_last_fence_value());
        return ok;
    };
    const double t0 = (double)job.t0.QuadPart * k;
    if (k <= 0.0 || job.pace <= 1)
        return mgpu::fgpace::pair_relative(t0, job.pace == 0 ? 0.0 : job.half_ms, n, wait_abs, present);
    if (job.pace == 2 && n != 0u)
    {
        // scene times: the n gen frames at (k+1)/(n+1) of the span, the real one at the span's end
        double c[4] = {};
        for (unsigned i = 0; i < n; ++i) c[i] = job.c_real - job.span + job.span * (double)(i + 1u) / (double)(n + 1u);
        c[n] = job.c_real;
        const double period = (job.ema_ms > 0.0 ? job.ema_ms : 16.0) * (double)n / (double)(n + 1u);
        return mgpu::fgpace::pair_clock(g_clk, period, t0, job.span, c, n + 1u, now, wait_abs, present);
    }
    if (job.pace == 3 && n != 0u)
        return mgpu::fgpace::pair_absolute(g_abs, job.ema_ms > 0.0 ? job.ema_ms : 16.0, n, now, wait_abs, present);
    return present(n);
}

void thread_main()
{
    for (;;)
    {
        job_t job;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return g_has_job || g_stop; });
            if (g_stop && !g_has_job) return; // Finish an accepted real/generated pair before stopping.
            job = g_job; g_has_job = false;
        }
        UINT64 fence = 0;
        const bool ok = present_job(job, fence);
        job.set->fence = fence;
        job.set->busy.store(false, std::memory_order_release);
        if (!ok) { g_thread_failed.store(true, std::memory_order_release); return; }
    }
}

void free_sets()
{
    for (unsigned k = 0; k < SETS; ++k)
    {
        native_gpu1::retire(k, g_set[k].native);
        g_set[k].native.reset();
        if (g_set[k].real != nullptr) g_set[k].real->Release();
        for (unsigned j = 0; j < 3u; ++j)
        {
            if (g_set[k].gen[j] != nullptr) g_set[k].gen[j]->Release();
            g_set[k].gen[j] = nullptr;
        }
        g_set[k].real = nullptr;
        g_set[k].frame = 0; g_set[k].gen_n = 0; g_set[k].handed = false;
        g_set[k].elapsed = 1.0f; g_set[k].phase = 0.0f;
        g_set[k].fence = 0; g_set[k].busy.store(false);
    }
}

bool wait_fence(ID3D12Fence *fence, uint64_t value, uint32_t timeout_ms)
{
    if (fence == nullptr || value == 0 || fence->GetCompletedValue() >= value) return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) return false;
    const bool ok = SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
        WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0;
    CloseHandle(event);
    return ok;
}
} // namespace

bool arm(const arm_info &i)
{
    release();
    int fgv = 0; unsigned long long app = 0; bool diag = false, pace = true;
    const bool ini_ok = read_keys(fgv, app, diag, pace);
    g_i = i;
    g_diagnose = ini_ok && diag && i.ndev != nullptr && i.nqueue != nullptr && i.game_luid != 0;
    g_bound = false;
    if (diag || fgv == 4)
    {
        char line[512];
        snprintf(line, sizeof line,
                 "[MGPU][NATIVE] arm request: ini_ok=%d NativeInput=%d FrameGen=%d "
                 "ndev=%s nqueue=%s game_luid=0x%08X-0x%08X diagnostic_capture=%d. "
                 "NativeInput only reads and validates inputs; it does not enable interpolation.",
                 ini_ok ? 1 : 0, diag ? 1 : 0, fgv,
                 i.ndev != nullptr ? "set" : "null", i.nqueue != nullptr ? "set" : "null",
                 (unsigned)(uint32_t)(i.game_luid >> 32), (unsigned)(uint32_t)i.game_luid,
                 g_diagnose ? 1 : 0);
        mgpu::diag::info(line);
    }
    if (!ini_ok || fgv != 4) return false;
    if (i.ndev == nullptr || i.nqueue == nullptr || i.w == 0 || i.h == 0 || i.game_luid == 0) return false;
    g_tried = g_up = false; g_k = 0; g_fid = g_last_eval_fid = g_last_eval_color = 0;
    g_need_reset = true; g_eval_value = 0; g_mfc_max = 0;
    g_fgx_refused = false; g_fgx_fallback_said = false;
    g_phase_refused = g_phase_said = false; g_phase_pairs = g_phase_nofit = 0;
    g_made = g_nomv = g_busy_skips = g_errors = g_evals = g_fallbacks = 0;
    g_pres_gen.store(0); g_pres_real.store(0); g_handoffs.store(0); g_dropped.store(0);
    g_half_sum = 0; g_half_n = 0; g_ema_ms = 0; g_last_handoff.QuadPart = 0;
    g_scene.reset(); g_clk.reset(); g_clk.clear_counts(); g_abs.reset(); g_abs.clear_counts();
    g_dgate.reset(); g_dgate.clear_counts(); g_dgate_t.QuadPart = 0; g_dgate_real_only = 0;
    g_refresh_ms = g_dcheck != 0 ? mgpu::fginstr::refresh_period_ms() : 0.0;
    g_thread_failed.store(false);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = i.w; d.Height = i.h;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = i.fmt;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT hr = i.ndev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_eval_fence));
    for (unsigned k = 0; k < SETS && SUCCEEDED(hr); ++k)
    {
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = i.ndev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_set[k].real));
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (unsigned j = 0; j < 3u && SUCCEEDED(hr); ++j)
            hr = i.ndev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_set[k].gen[j]));
    }
    g_pfence = mgpu::gpu1::present_fence_addref();
    if (FAILED(hr) || g_pfence == nullptr)
    {
        free_sets();
        if (g_eval_fence != nullptr) { g_eval_fence->Release(); g_eval_fence = nullptr; }
        if (g_pfence != nullptr) { g_pfence->Release(); g_pfence = nullptr; }
        mgpu::diag::error("[MGPU][FG] buffers/fences could not be created; FG stays off");
        return false;
    }
    g_armed = true;
    mgpu::fgpair::arm(i.ndev, g_pair_flow, i.w, i.h, i.fmt);
    g_on.store(true, std::memory_order_release);
    char pace_line[560];
    snprintf(pace_line, sizeof pace_line,
        "[MGPU][FG] 0.6.0 scheduler: FGPace=%d FGPhase=%d FGX=%d FGDisplayCheck=%d FGPairFlow=%d display-ms=%.3f. "
        "FINAL colour + official GPU1-local MV/depth -> DLSSG -> present; estimated camera. Missing inputs present real colour only.",
        g_pace_mode, g_phase_req, g_fgx, g_dcheck, mgpu::fgpair::active() ? 1 : 0, g_refresh_ms);
    mgpu::diag::info(pace_line);
    mgpu::diag::info(g_fgx >= 4
        ? "[MGPU][FG] experimental 4x ARMED on the neural card: three DLSSG evaluates per real frame "
          "(mfi 1/3, 2/3, 3/3). Needs DLSSG MultiFrameCount >= 3; a refusal falls back to 2x for this arm."
        : "[MGPU][FG] experimental 2x ARMED on the neural card. FGPhase is used only after DLSSG accepts it; "
          "a rejected phase falls back to the middle frame for later pairs.");
    return true;
}

bool on() { return g_on.load(std::memory_order_relaxed); }
static uint64_t g_last_bound_frame = 0;
bool capture_requested() { return g_diagnose; }

uint64_t bind_color(uint64_t current, uint64_t previous, uint64_t seal_qpc)
{
    const bool wanted = capture_requested() && current != 0 && g_i.game_luid != 0;
    if (wanted)
    {
        native_input::bind_color_frame(current, previous, seal_qpc);
        g_bound = true;
        g_last_bound_frame = current;
        return current;
    }
    g_last_bound_frame = 0;
    if (g_bound)
    {
        native_input::bind_color_frame(0, 0, 0);
        g_bound = false;
    }
    return 0;
}

unsigned dynamic_step()
{
    const unsigned asked = g_fgx >= 4 ? 4u : 2u;
    LARGE_INTEGER now{}, fq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    const double dt = g_dgate_t.QuadPart != 0 && fq.QuadPart != 0
        ? (double)(now.QuadPart - g_dgate_t.QuadPart) * 1000.0 / (double)fq.QuadPart : 0.0;
    g_dgate_t = now;
    if (g_dgate.step(dt, g_refresh_ms, asked))
    {
        const unsigned allowed = g_dgate.allowed(asked);
        char line[440];
        snprintf(line, sizeof line,
            "[MGPU][FG][Dynamic] display=%.1f Hz neural=%.1f fps: %s. Real neural frames are always retained.",
            g_refresh_ms > 0.0 ? 1000.0 / g_refresh_ms : 0.0, g_dgate.rate(),
            allowed < 2u ? "frame generation OFF (display too slow)" : "multiplier fits; generation ON");
        mgpu::diag::info(line);
    }
    return g_dgate.m == 0u ? 0u : g_dgate.allowed(asked);
}

void record(ID3D12GraphicsCommandList *cl, ID3D12Resource *nr_final,
            ID3D12Resource *mvec, ID3D12Resource *depth, float mv_el, uint64_t game_frame)
{
    if (cl == nullptr || nr_final == nullptr || game_frame == 0 || (!on() && !g_diagnose)) return;
    if (g_thread_failed.load(std::memory_order_acquire)) { fail_for_arm("the presentation thread failed"); return; }
    set_t &set = g_set[g_k];
    if (!on())
    {
        if (g_diagnose)
            native_gpu1::prepare(g_i.ndev, g_i.nqueue, cl, g_i.game_luid, game_frame, 0, false, set.native);
        return;
    }
    unsigned dynamic = 0;
    if (g_dcheck != 0) dynamic = dynamic_step();
    else if (g_dgate.m != 0u) { g_dgate.reset(); g_dgate_t.QuadPart = 0; }
    if (g_pfence == nullptr || set.busy.load(std::memory_order_acquire) ||
        (set.fence != 0 && (g_pfence->GetCompletedValue() == UINT64_MAX ||
                           g_pfence->GetCompletedValue() < set.fence)))
    {
        ++g_busy_skips;
        return;
    }
    unsigned rw = 0, rh = 0; DXGI_FORMAT rf = DXGI_FORMAT_UNKNOWN;
    if (!extents(nr_final, rw, rh, rf) || rw != g_i.w || rh != g_i.h || rf != g_i.fmt)
    { fail_for_arm("final neural resource extent/format changed; the stream must be re-armed"); return; }

    // Match 0.6.0: create DLSSG before recording the real-colour copy and use
    // the textures already on GPU1. No second cross-adapter import is involved.
    if (dynamic != 1u && (mvec != nullptr || mgpu::fgpair::active()) && depth != nullptr && !g_up && !ensure_feature(cl)) return;
    transition(cl, nr_final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cl, set.real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(set.real, nr_final);
    transition(cl, set.real, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, nr_final, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12Resource *eval_mvec = mvec;
    bool pair_flow = false, pair_estimated = false;
    if (mgpu::fgpair::active())
    {
        ID3D12Resource *estimated = mgpu::fgpair::record(cl, set.real, g_k, &pair_estimated);
        if (estimated != nullptr) { eval_mvec = estimated; pair_flow = true; }
        if (pair_estimated && g_evals == (g_fgx >= 4 ? 3u : 1u))
            mgpu::diag::info("[MGPU][FG][PairFlow] first non-reset frame-pair estimate submitted; DLSSG MvecScale=(1,1), NR vector path unchanged.");
    }
    set.frame = game_frame; set.gen_n = 0; set.handed = false; set.phase = 0.0f;
    set.elapsed = mv_el > 0.0f ? mv_el : 1.0f;
    const unsigned long long fid = ++g_fid;

    if (dynamic == 1u)
    {
        ++g_dgate_real_only;
        g_need_reset = true;
    }
    else if (eval_mvec == nullptr || depth == nullptr)
    {
        ++g_nomv;
        g_need_reset = true;
    }
    else
    {
        const bool reset = g_need_reset || g_last_eval_fid == 0 || fid != g_last_eval_fid + 1;
        // FGX=4 (DLSS4 multi-frame generation): three evaluates of the SAME real frame,
        // MultiFrameIndex 1..3 -> one interpolated frame at 1/4, 2/4, 3/4 of the interval.
        // Needs DLSSG MultiFrameCount >= 3 (g_mfc_max, 0 = unknown: try and fall back on refusal).
        if (g_fgx >= 4 && !g_fgx_refused && (g_mfc_max == 0u || g_mfc_max >= 3u))
        {
            if (g_mfc_max != 0u && g_mfc_max < 3u)
            {
                g_fgx_refused = true;   // known-insufficient driver: the 2x path below takes over
                if (!g_fgx_fallback_said)
                {
                    g_fgx_fallback_said = true;
                    char line[300];
                    snprintf(line, sizeof line,
                        "[MGPU][FG][4x] FGX=4 needs DLSSG MultiFrameCount >= 3, driver maximum=%u; 2x for this arm.",
                        g_mfc_max);
                    mgpu::diag::warn(line);
                }
            }
            if (g_mfc_max == 0u || g_mfc_max >= 3u)
            {
                if (g_evals < 3u)
                    mgpu::diag::info("[MGPU][FG] entering 4x EvaluateFeature x3 (mfi 1/3, 2/3, 3/3) with official GPU1-local MV/depth");
                NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
                for (unsigned mfi = 1u; mfi <= 3u; ++mfi)
                {
                    r = evaluate(cl, set, set.gen[mfi - 1u], eval_mvec, depth, mv_el, reset, fid, 3u, mfi, pair_flow);
                    ++g_evals;
                    if (r != NVSDK_NGX_Result_Success)
                    {
                        if (mfi == 1u)
                        {
                            // the driver refused multi-frame generation itself: permanent 2x fallback
                            g_fgx_refused = true;
                            char line[320];
                            snprintf(line, sizeof line,
                                "[MGPU][FG][4x] DLSSG rejected MultiFrameCount=3 (0x%08X). FGX falls back to 2x for this arm.",
                                (unsigned)r);
                            mgpu::diag::warn(line);
                        }
                        else
                        {
                            ++g_errors; g_need_reset = true;
                            char line[300];
                            snprintf(line, sizeof(line), "[MGPU][FG] 4x EvaluateFeature mfi=%u failed 0x%08X errors=%llu; real colour is retained",
                                mfi, (unsigned)r, g_errors);
                            mgpu::diag::error(line);
                            if (g_errors >= 3) fail_for_arm("three DLSSG evaluations failed");
                        }
                        break;
                    }
                }
                if (!g_fgx_refused && r == NVSDK_NGX_Result_Success)
                {
                    g_last_eval_fid = fid; g_need_reset = false;
                    if (!reset)
                    {
                        set.gen_n = 3; set.phase = 0.75f; ++g_made;
                    }
                }
            }
        }
        if (g_fgx < 4 || g_fgx_refused)
        {
        unsigned mfc = 1u, mfi = 1u;
        if (g_phase_req != 0 && !reset && !g_phase_refused)
        {
            const unsigned want = g_phase_req == 67 ? 2u : 3u;
            if (g_mfc_max != 0u && want > g_mfc_max)
            {
                ++g_phase_nofit;
                if (!g_phase_said)
                {
                    g_phase_said = true;
                    char line[360];
                    snprintf(line, sizeof line,
                        "[MGPU][FG][Phase] FGPhase=%d needs frame %u/%u, driver maximum=%u; using the middle frame.",
                        g_phase_req, want, want, g_mfc_max);
                    mgpu::diag::warn(line);
                }
            }
            else mfc = mfi = want;
        }
        if (g_evals < 3u)
            mgpu::diag::info("[MGPU][FG] entering EvaluateFeature with official GPU1-local MV/depth");
        const NVSDK_NGX_Result r = evaluate(cl, set, set.gen[0], eval_mvec, depth, mv_el, reset, fid, mfc, mfi, pair_flow);
        ++g_evals;
        if (r != NVSDK_NGX_Result_Success && mfi != 1u)
        {
            g_phase_refused = true;
            char line[360];
            snprintf(line, sizeof line,
                "[MGPU][FG][Phase] DLSSG rejected frame %u/%u (0x%08X). FGPhase is disabled for this arm; later pairs use the middle frame.",
                mfi, mfc, (unsigned)r);
            mgpu::diag::warn(line);
        }
        if (r != NVSDK_NGX_Result_Success)
        {
            ++g_errors; g_need_reset = true;
            char line[300];
            snprintf(line, sizeof(line), "[MGPU][FG] EvaluateFeature color=%llu result=0x%08X errors=%llu; real colour is retained",
                (unsigned long long)game_frame, (unsigned)r, g_errors);
            mgpu::diag::error(line);
            if (g_errors >= 3) fail_for_arm("three DLSSG evaluations failed");
        }
        else
        {
            g_last_eval_fid = fid; g_need_reset = false;
            if (!reset)
            {
                set.gen_n = 1; ++g_made;
                if (mfi != 1u)
                {
                    set.phase = (float)mfi / (float)(mfc + 1u);
                    ++g_phase_pairs;
                }
            }
        }
        }
    }
    g_k = (g_k + 1u) % SETS;
}

bool handoff(float r, float g, float b)
{
    if (g_thread_failed.load(std::memory_order_acquire)) { fail_for_arm("present failed"); stop(); return false; }
    if (!on()) { stop(); return false; }
    set_t &set = g_set[(g_k + SETS - 1u) % SETS];
    const bool started = g_thread != nullptr;
    if (set.frame == 0 || set.handed || set.busy.load(std::memory_order_acquire))
    {
        // record() skipped a busy set, or this set was already handed over.
        // Once the pacing thread exists it remains the sole presenter.
        if (started) { g_dropped.fetch_add(1, std::memory_order_relaxed); return true; }
        return false;
    }
    set.handed = true;
    LARGE_INTEGER now{}, fq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    const double dt = g_last_handoff.QuadPart != 0 && fq.QuadPart != 0
        ? (double)(now.QuadPart - g_last_handoff.QuadPart) * 1000.0 / (double)fq.QuadPart : 0.0;
    if (dt > 3.0 && dt < 60.0) g_ema_ms = g_ema_ms <= 0 ? dt : g_ema_ms * 0.85 + dt * 0.15;
    g_last_handoff = now;
    job_t job{}; job.set = &set; job.gen_n = set.gen_n; job.r = r; job.g = g; job.b = b; job.t0 = now;
    job.half_ms = g_ema_ms > 0 ? g_ema_ms * 0.5 : 8.0;
    if (set.phase > 0.0f && set.phase < 1.0f)
        job.half_ms = (g_ema_ms > 0 ? g_ema_ms : 16.0) * (double)(1.0f - set.phase);
    job.pace = g_pace_mode; job.ema_ms = g_ema_ms;
    if (job.pace == 2)
        g_scene.next((double)now.QuadPart * (fq.QuadPart != 0 ? 1000.0 / (double)fq.QuadPart : 0.0),
                     dt, (double)set.elapsed, job.c_real, job.span);
    if (g_dcheck != 0 && (g_handoffs.load(std::memory_order_relaxed) % 600ull) == 598ull)
        g_refresh_ms = mgpu::fginstr::refresh_period_ms();
    g_half_sum += job.half_ms; ++g_half_n;
    set.busy.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(g_mx);
        g_stop = false;
        if (g_thread == nullptr) g_thread = new std::thread(thread_main);
        if (g_has_job)
        {
            // 0.6.0 newest-wins: return the old queued set, count one dropped
            // pair, and replace it. Never disable the whole generator here.
            g_job.set->busy.store(false, std::memory_order_release);
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        g_job = job; g_has_job = true;
    }
    g_cv.notify_one();
    const unsigned long long n = g_handoffs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 120 == 0)
    {
        unsigned long long pair_n = 0, pair_fallback = 0;
        mgpu::fgpair::stats(pair_n, pair_fallback);
        char line[760];
        snprintf(line, sizeof(line), "[MGPU][FG] handoffs=%llu generated=%llu presented-gen=%llu presented-real=%llu "
            "dropped=%llu real-fallbacks=%llu invalid-input=%llu busy=%llu errors=%llu pace=%d phase=%d dynamic-real=%llu "
            "pair-pairs=%llu pair-history-fallback=%llu half-ms=%.2f; GPU1 imports=%llu",
            n, g_made, g_pres_gen.load(), g_pres_real.load(), g_dropped.load(), g_fallbacks, g_nomv, g_busy_skips,
            g_errors, g_pace_mode, g_phase_refused ? 0 : g_phase_req, g_dgate_real_only,
            pair_n, pair_fallback, g_half_n ? g_half_sum / g_half_n : 0.0,
            (unsigned long long)native_gpu1::imported_frames());
        mgpu::diag::info(line);
    }
    return true;
}

void stop()
{
    g_on.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_stop = true;
    }
    g_cv.notify_one();
    if (g_thread != nullptr)
    {
        if (g_thread->joinable()) g_thread->join();
        delete g_thread; g_thread = nullptr;
    }
    { std::lock_guard<std::mutex> lk(g_mx); g_has_job = false; }
}

void release()
{
    const bool was = g_armed;
    stop();
    g_diagnose = false;
    g_bound = false;
    g_last_bound_frame = 0;
    uint64_t present_need = 0;
    for (const auto &set : g_set) present_need = (std::max)(present_need, set.fence);
    const bool present_drained = wait_fence(g_pfence, present_need, 2000);
    if (g_pfence != nullptr) { g_pfence->Release(); g_pfence = nullptr; }
    bool neural_drained = true;
    if (g_eval_fence != nullptr && g_i.nqueue != nullptr)
    {
        ++g_eval_value;
        neural_drained = SUCCEEDED(g_i.nqueue->Signal(g_eval_fence, g_eval_value)) &&
            wait_fence(g_eval_fence, g_eval_value, 2000);
    }
    if (!present_drained || !neural_drained)
    {
        // Deliberately retain the set textures, feature, native leases and
        // imported heap. A bounded leak at teardown is safer than a GPU UAF.
        native_input::reset();
        mgpu::diag::error("[MGPU][FG] teardown drain timed out; FG allocations retained, no memory freed beneath the GPU");
        g_armed = false;
        return;
    }
    if (g_handle != nullptr && g_rel != nullptr) (void)g_rel(g_handle);
    g_handle = nullptr; g_params = nullptr;
    // Return each provider lease while its slot/fence still exists. Only then
    // release the GPU1 textures and shared heap; reset must not orphan a lease
    // merely because its tracking fence was destroyed first.
    // PairFlow descriptors reference set.real; release them only after both
    // presentation and neural queues have drained, before freeing those sets.
    mgpu::fgpair::release();
    free_sets();
    native_gpu1::reset(g_i.nqueue, true);
    if (g_eval_fence != nullptr) { g_eval_fence->Release(); g_eval_fence = nullptr; }
    if (g_timer != nullptr) { CloseHandle(g_timer); g_timer = nullptr; }
    native_input::reset();
    if (was)
    {
        char line[400];
        snprintf(line, sizeof(line), "[MGPU][FG] released: generated=%llu presented-gen=%llu real=%llu fallbacks=%llu invalid=%llu errors=%llu",
            g_made, g_pres_gen.load(), g_pres_real.load(), g_fallbacks, g_nomv, g_errors);
        mgpu::diag::info(line);
    }
    g_armed = false; g_up = false; g_tried = false; g_i = {};
    g_last_eval_fid = g_last_eval_color = 0; g_need_reset = true;
}
} // namespace mgpu::fg
