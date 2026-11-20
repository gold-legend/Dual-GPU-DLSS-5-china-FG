// MGPU Bridge - POVERLAY implementation. See poverlay.hpp for the contract.
//
// This is a port of prism-core 1.5.1's overlay.cpp onto the mgpu bridge. The drawing engine
// (draw list, glyph atlas, D3D12 submit), the widget kit (barcode, segments, bar sliders, the
// grading desk) and the input neutralisation are prism's verbatim. What changed: the data
// source (mgpu ui_state + pfinish instead of the prism pipeline), the row set (mgpu's: no
// tiers, no cadence, no target fps - the owner's decisions), persistence (mgpu.ini keys the
// bridge already owns), and the queue/swapchain discovery (ReShade hands both to us at
// present; no vtable scanning).
#include "poverlay.hpp"

#include <windows.h>
#include <intrin.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dwrite_3.h>
#include <dxgi1_6.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

#include "font_mono_regular.h"
#include "font_mono_bold.h"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "prism_finish.hpp"
#include "prism_sync.hpp"
#include "prism_x1_multi.hpp"
#include "skinguard.hpp"
#include "prism_grade.h"
#include "residual.hpp"
#include "t119_keys.hpp"

namespace mgpu::poverlay
{
namespace {

#define LOG(...) do { char lb[512]; snprintf(lb, sizeof lb, __VA_ARGS__); mgpu::diag::info(lb); } while (0)

double Now()
{
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

// ------------------------------------------------------------------ palette (prism DESIGN.md)
constexpr uint32_t Rgba(uint32_t rgb, float a = 1.0f)
{
    return ((rgb >> 16) & 0xff) | (rgb & 0xff00) | ((rgb & 0xff) << 16) | (uint32_t(a * 255.0f + 0.5f) << 24);
}
constexpr uint32_t kWhite = Rgba(0xffffff), kReadout = Rgba(0x8c8c8c), kQuiet = Rgba(0x5c5c5c),
                   kHair = Rgba(0x262626), kHairFaint = Rgba(0x1a1a1a), kBlack = Rgba(0x000000),
                   kReady = Rgba(0x3ddc97), kCaution = Rgba(0xffc400), kBlocked = Rgba(0xff3b30),
                   kInfo = Rgba(0x4da3ff);
uint32_t Fade(uint32_t c, float a) { return (c & 0x00ffffff) | (uint32_t(float(c >> 24) * std::clamp(a, 0.0f, 1.0f) + 0.5f) << 24); }

// ------------------------------------------------------------------ settings of the panel itself
struct PanelConfig
{
    int key = VK_MULTIPLY;    // numpad * (user's pick 2026-10-29; Home belongs to ReShade's own UI)
    float scale = 1.0f;       // OverlayScale, on top of the resolution's
    bool hint = true;         // OverlayHint: the "Home opens the panel" toast at start
    bool hud = false;         // OverlayHud: reserved (the fps corner is a separate task)
    float x = 32, y = 32;     // OverlayX/Y: the panel's top-left, in panel units
    float nits = 203.0f;      // OverlayNits: the panel's white on HDR back buffers
    int hdr = -1;             // OverlayHdr: -1 auto, 0 SDR, 1 scRGB, 2 HDR10
    bool motion = true;       // OverlayMotion
    bool block = false;       // OverlayBlockInput: opt-in; never trap gameplay by default
    bool pad = true;          // OverlayPad
} cfg;

// The panel's own ini, beside the add-on: mgpu_panel.ini.
std::wstring PanelIniPath()
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return L"";
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"";
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash) slash[1] = L'\0';
    return std::wstring(mod) + L"mgpu_panel.ini";
}

int KeyFromName(const std::wstring &s, int fallback)
{
    if (s.empty()) return fallback;
    if (!lstrcmpiW(s.c_str(), L"none")) return 0;
    static const std::pair<const wchar_t *, int> names[] = {
        {L"Home", VK_HOME}, {L"End", VK_END}, {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE},
        {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Pause", VK_PAUSE}, {L"ScrollLock", VK_SCROLL},
        {L"Multiply", VK_MULTIPLY}, {L"*", VK_MULTIPLY}, {L"Divide", VK_DIVIDE}, {L"Subtract", VK_SUBTRACT},
        {L"Add", VK_ADD}, {L"Decimal", VK_DECIMAL}};
    for (const auto &[name, vk] : names)
        if (!lstrcmpiW(s.c_str(), name)) return vk;
    if ((s[0] == L'F' || s[0] == L'f') && s.size() > 1)
    {
        const int n = _wtoi(s.c_str() + 1);
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : fallback;
    }
    const int n = _wtoi(s.c_str());
    return n > 0 && n < 256 ? n : fallback;
}

const wchar_t *KeyName(int vk)
{
    static thread_local wchar_t buf[8];
    for (const auto &[name, k] : {std::pair<const wchar_t *, int>{L"Home", VK_HOME}, {L"End", VK_END}, {L"Insert", VK_INSERT},
                                  {L"Delete", VK_DELETE}, {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Pause", VK_PAUSE},
                                  {L"ScrollLock", VK_SCROLL}, {L"Multiply", VK_MULTIPLY}, {L"*", VK_MULTIPLY},
                                  {L"Divide", VK_DIVIDE}, {L"Subtract", VK_SUBTRACT}, {L"Add", VK_ADD}, {L"Decimal", VK_DECIMAL}})
        if (vk == k) return name;
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        std::swprintf(buf, 8, L"F%d", vk - VK_F1 + 1);
        return buf;
    }
    std::swprintf(buf, 8, L"#%d", vk);
    return buf;
}

void LoadPanelConfig()
{
    const std::wstring path = PanelIniPath();
    if (path.empty()) return;
    wchar_t b[64] = {};
    auto get = [&](const wchar_t *k) {
        b[0] = 0;
        GetPrivateProfileStringW(L"Panel", k, L"", b, 64, path.c_str());
        return std::wstring(b);
    };
    auto num = [&](const wchar_t *k, double d, double lo, double hi) {
        const std::wstring v = get(k);
        const double x = v.empty() ? d : _wtof(v.c_str());
        return std::clamp(x, lo, hi);
    };
    cfg.key = KeyFromName(get(L"OverlayKey"), VK_MULTIPLY);
    cfg.scale = float(num(L"OverlayScale", 1.0, 0.5, 3.0));
    cfg.x = float(num(L"OverlayX", 32, 0, 10000));
    cfg.y = float(num(L"OverlayY", 32, 0, 10000));
    cfg.nits = float(num(L"OverlayNits", 203, 80, 1000));
    cfg.hint = get(L"OverlayHint") != L"0";
    cfg.hud = get(L"OverlayHud") == L"1";
    cfg.motion = get(L"OverlayMotion") != L"0";
    cfg.block = get(L"OverlayBlockInput") == L"1";
    cfg.pad = get(L"OverlayPad") != L"0";
    const std::wstring hdr = get(L"OverlayHdr");
    cfg.hdr = hdr == L"0" ? 0 : hdr == L"1" ? 1 : hdr == L"2" ? 2 : -1;
}

void SavePanelValue(const wchar_t *key, const std::wstring &v)
{
    const std::wstring path = PanelIniPath();
    if (!path.empty()) WritePrivateProfileStringW(L"Panel", key, v.c_str(), path.c_str());
}

// ------------------------------------------------------------------ input: what the game sees
// While the panel is open (blocking), the game's reads of keys, cursor, raw input and XInput
// come back neutral and its window gets no mouse or key messages. Our own calls pass through.
std::atomic<bool> blocking{false};
const uint8_t *selfBegin = nullptr;
const uint8_t *selfEnd = nullptr;
bool FromSelf(void *ret) { return ret >= selfBegin && ret < selfEnd; }

using AsyncKeyFn = SHORT(WINAPI *)(int);
using KeyboardStateFn = BOOL(WINAPI *)(PBYTE);
using CursorPosFn = BOOL(WINAPI *)(LPPOINT);
using SetCursorPosFn = BOOL(WINAPI *)(int, int);
using ClipCursorFn = BOOL(WINAPI *)(const RECT *);
using RawDataFn = UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using RawBufferFn = UINT(WINAPI *)(PRAWINPUT, PUINT, UINT);
using PadStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
AsyncKeyFn realAsyncKey = GetAsyncKeyState, realKeyState = GetKeyState;
KeyboardStateFn realKeyboardState = GetKeyboardState;
CursorPosFn realGetCursorPos = GetCursorPos;
SetCursorPosFn realSetCursorPos = SetCursorPos;
ClipCursorFn realClipCursor = ClipCursor;
RawDataFn realRawData = GetRawInputData;
RawBufferFn realRawBuffer = GetRawInputBuffer;
using SetWindowPosFn = BOOL (WINAPI *)(HWND, HWND, int, int, int, int, UINT);
SetWindowPosFn realSetWindowPosSWP = SetWindowPos;
POINT frozenCursor = {};                          // where the game's cursor was when the panel opened
RECT gameClip = {};
bool gameClipped = false;  // the game's cursor clip, restored on close

SHORT WINAPI HkAsyncKey(int vk) { return blocking && !FromSelf(_ReturnAddress()) ? 0 : realAsyncKey(vk); }

static BOOL WINAPI HkSetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    return realSetWindowPosSWP(h, after, x, y, cx, cy, flags);
}
SHORT WINAPI HkKeyState(int vk) { return blocking && !FromSelf(_ReturnAddress()) ? 0 : realKeyState(vk); }
BOOL WINAPI HkKeyboardState(PBYTE k)
{
    if (blocking && !FromSelf(_ReturnAddress()) && k)
    {
        std::memset(k, 0, 256);
        return TRUE;
    }
    return realKeyboardState(k);
}
BOOL WINAPI HkGetCursorPos(LPPOINT p)
{
    if (blocking && !FromSelf(_ReturnAddress()) && p)
    {
        *p = frozenCursor;
        return TRUE;
    }
    return realGetCursorPos(p);
}
BOOL WINAPI HkSetCursorPos(int x, int y)
{
    if (blocking && !FromSelf(_ReturnAddress()))
    {
        frozenCursor = {x, y};
        return TRUE;
    }
    return realSetCursorPos(x, y);
}
// Convert a clip rect to physical screen pixels. DPI-unaware games (RE2)
// pass LOGICAL rects (2194x1234 on this 175% rig); if such a rect reaches
// ClipCursor on a thread whose awareness was raised (by any component), the
// values are misread as PHYSICAL and the cursor is trapped in the top-left
// corner of the 4K screen - measured live as clip 0,0->2194,1234 physical.
// Reading the CALLING thread's awareness makes the conversion exact in both
// cases and removes all dependence on whichever thread happens to call.
static RECT clip_rect_to_physical(const RECT &r)
{
    if (GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) != DPI_AWARENESS_UNAWARE)
        return r;   // aware caller: values are already physical
    UINT dpi = 168;
    int sysDpi = 96;
    const DPI_AWARENESS_CONTEXT prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (HWND gw = (HWND)mgpu::gpu1::dcomp_game_hwnd()) dpi = GetDpiForWindow(gw);
    if (HDC dc = GetDC(nullptr)) { sysDpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc); }
    SetThreadDpiAwarenessContext(prev);
    if (dpi == 0 || sysDpi == 0 || dpi == (UINT)sysDpi) return r;
    const double k = (double)dpi / (double)sysDpi;
    RECT o;
    o.left = (int)std::lround(r.left * k); o.top = (int)std::lround(r.top * k);
    o.right = (int)std::lround(r.right * k); o.bottom = (int)std::lround(r.bottom * k);
    return o;
}

BOOL WINAPI HkClipCursor(const RECT *r)
{
    if (blocking && !FromSelf(_ReturnAddress()))
    {
        gameClipped = r != nullptr;
        if (r) gameClip = *r;
        return TRUE;
    }
    RECT fixed{};
    const RECT *pr = nullptr;
    if (r) { fixed = clip_rect_to_physical(*r); pr = &fixed; }
    // Apply on a per-monitor-aware thread so the physical values are taken
    // literally; the game's own thread awareness no longer matters.
    const DPI_AWARENESS_CONTEXT prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const BOOL ok = realClipCursor(pr);
    SetThreadDpiAwarenessContext(prev);
    return ok;
}
// Movement, presses and the wheel are taken out; releases stay, so nothing held when the panel
// opened is left stuck down in the game.
void Neutral(RAWINPUT *ri)
{
    if (ri->header.dwType == RIM_TYPEMOUSE)
    {
        RAWMOUSE &m = ri->data.mouse;
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE))
        {
            m.lLastX = 0;
            m.lLastY = 0;
        }
        m.usButtonFlags &= RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                           RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        m.usButtonData = 0;
    }
    else if (ri->header.dwType == RIM_TYPEKEYBOARD && !(ri->data.keyboard.Flags & RI_KEY_BREAK))
    {
        ri->data.keyboard.VKey = 0xff;
        ri->data.keyboard.MakeCode = 0;
    }
}
UINT WINAPI HkRawData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    const UINT r = realRawData(h, cmd, data, size, header);
    if (blocking && data && cmd == RID_INPUT && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER)) Neutral(static_cast<RAWINPUT *>(data));
    return r;
}
UINT WINAPI HkRawBuffer(PRAWINPUT data, PUINT size, UINT header)
{
    const UINT r = realRawBuffer(data, size, header);
    if (blocking && data && r != UINT(-1))
    {
        RAWINPUT *ri = data;
        for (UINT i = 0; i < r; ++i)
        {  // NEXTRAWINPUTBLOCK, 8-byte aligned in a 64-bit process
            Neutral(ri);
            ri = (RAWINPUT *)((ULONG_PTR(ri) + ri->header.dwSize + 7) & ~ULONG_PTR(7));
        }
    }
    return r;
}
// XInput, per module the game may load. A blocked read is a neutral pad with a packet number of
// its own, so the game takes it as a change of state.
struct PadHook
{
    const char *module;
    PadStateFn real = nullptr;
    bool done = false;
};
PadHook padHooks[] = {{"xinput1_4.dll"}, {"xinput1_3.dll"}, {"xinput9_1_0.dll"}, {"xinputuap.dll"}};
template <int I>
DWORD WINAPI HkPadState(DWORD i, XINPUT_STATE *st)
{
    const DWORD r = padHooks[I].real(i, st);
    if (blocking && r == ERROR_SUCCESS && st && !FromSelf(_ReturnAddress()))
    {
        st->Gamepad = {};
        st->dwPacketNumber ^= 0x40000000u;
    }
    return r;
}
void HookPads()
{
    void *detours[] = {(void *)&HkPadState<0>, (void *)&HkPadState<1>, (void *)&HkPadState<2>, (void *)&HkPadState<3>};
    for (int i = 0; i < 4; ++i)
    {
        PadHook &p = padHooks[i];
        if (p.done) continue;
        HMODULE m = GetModuleHandleA(p.module);
        if (!m) continue;
        p.done = true;
        void *target = (void *)GetProcAddress(m, "XInputGetState");
        if (target && MH_CreateHook(target, detours[i], (void **)&p.real) == MH_OK && MH_EnableHook(target) == MH_OK)
            LOG("[MGPU][POV] %s hooked", p.module);
    }
}

// Our own gamepad reads.
PadStateFn padRead = nullptr;

// The game window's messages: none of the mouse or keys reach the game while the panel is open.
HWND gameWnd = nullptr;
WNDPROC gameProc = nullptr;
std::atomic<int> wheel{0};

// Own key/button state, tracked from the hooked wndproc. 2026-10-29: the
// panel used to read everything through GetAsyncKeyState, and that path is
// provably dead in this environment (ReShade 6.8 sits in the process and the
// raw reads came back 0 for keys that were physically down - see the keys-diag
// lines in ReShade.log). The window messages still arrive at our wndproc, so
// once they do, they are the source of truth for the panel's Key().
std::atomic<uint8_t> ownKey[256] = {};
std::atomic<unsigned> ownKeySeq{0};   // bumped on every event; 0 = wndproc has seen nothing yet
// 2026-11-14: RE2 (RE Engine) never dispatches KEYBOARD messages to its wndproc (Raw
// Input game; proven by the keydn-hunt log: 0 keydowns while the user pressed). Mouse
// messages do arrive, so ownKeySeq goes non-zero and the old "trust the wndproc for
// everything once anything arrived" rule silently zeroed every keyboard read. Track a
// per-key "the wndproc has genuinely seen this key" bitmap; Key() trusts the wndproc
// only for keys that have actually produced a message, and the (proven alive) raw
// GetAsyncKeyState for the rest.
std::atomic<uint32_t> ownKeySeen[8] = {};
// 2026-11-14: physical presses counted edge-only. Two failure modes fixed at once:
// (a) a tap shorter than a poll interval left the level back at 0 before any PollInput ran -
//     the press vanished entirely, so the panel "would not open";
// (b) holding the key auto-repeated WM_KEYDOWN. Level-edge polling survived the repeats,
// but rapid re-taps landed at arbitrary parity. Counting non-repeat KEYDOWNs of the toggle
// key and applying the delta's parity per poll makes both a tap and a hold exactly one toggle.
std::atomic<unsigned> ownPressSeq{0};   // bumped on every NON-REPEAT KEYDOWN of cfg.key

struct DpiSizeTranslation
{
    int game_w = 0, game_h = 0;
    int logical_w = 0, logical_h = 0;
    bool pending = false;
};
static thread_local DpiSizeTranslation dpi_size;

static void translate_dpi_unaware_game_size(HWND h, UINT m, LPARAM &l)
{
    if (!mgpu::gpu1::dcomp_overlay_mode() || h != (HWND)mgpu::gpu1::dcomp_game_hwnd() ||
        GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(h)) != DPI_AWARENESS_UNAWARE) return;
    if (m == WM_WINDOWPOSCHANGING && l)
    {
        WINDOWPOS *pos = reinterpret_cast<WINDOWPOS *>(l);
        if ((pos->flags & SWP_NOSIZE) || pos->cx <= 0 || pos->cy <= 0) return;
        RECT current{};
        if (pos->flags == 0 && GetClientRect(h, &current) &&
            pos->cx == current.right - current.left && pos->cy == current.bottom - current.top)
        {
            // Windows repeats the DPI-unaware activation size once more after the
            // real resolution change. The host already has exactly this extent;
            // treating the duplicate as a new game resolution would shrink twice.
            pos->flags |= SWP_NOSIZE;
            dpi_size.pending = false;
            LOG("[MGPU][R243-DPI] ignored duplicate automatic host sizing %dx%d", pos->cx, pos->cy);
            return;
        }
        HMONITOR monitor = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO logical{}, physical{};
        logical.cbSize = sizeof logical;
        physical.cbSize = sizeof physical;
        if (!GetMonitorInfoW(monitor, &logical)) return;
        const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const BOOL have_physical = GetMonitorInfoW(monitor, &physical);
        SetThreadDpiAwarenessContext(previous);
        if (!have_physical) return;
        const int logical_monitor_w = logical.rcMonitor.right - logical.rcMonitor.left;
        const int logical_monitor_h = logical.rcMonitor.bottom - logical.rcMonitor.top;
        const int physical_monitor_w = physical.rcMonitor.right - physical.rcMonitor.left;
        const int physical_monitor_h = physical.rcMonitor.bottom - physical.rcMonitor.top;
        if (logical_monitor_w <= 0 || logical_monitor_h <= 0 || physical_monitor_w <= 0 || physical_monitor_h <= 0) return;
        const int converted_w = std::max(1, MulDiv(pos->cx, logical_monitor_w, physical_monitor_w));
        const int converted_h = std::max(1, MulDiv(pos->cy, logical_monitor_h, physical_monitor_h));
        if (converted_w == pos->cx && converted_h == pos->cy) return;
        dpi_size.game_w = pos->cx;
        dpi_size.game_h = pos->cy;
        dpi_size.logical_w = converted_w;
        dpi_size.logical_h = converted_h;
        dpi_size.pending = true;
        pos->cx = converted_w;
        pos->cy = converted_h;
        LOG("[MGPU][R243-DPI] translated host extent to %dx%d logical; game extent stays %dx%d",
            converted_w, converted_h, dpi_size.game_w, dpi_size.game_h);
    }
    else if (m == WM_SIZE && dpi_size.pending &&
             LOWORD(l) == (DWORD)dpi_size.logical_w && HIWORD(l) == (DWORD)dpi_size.logical_h)
    {
        l = (LPARAM)MAKELPARAM(dpi_size.game_w, dpi_size.game_h);
        dpi_size.pending = false;
        LOG("[MGPU][R243-DPI] WM_SIZE restored to game extent %dx%d", dpi_size.game_w, dpi_size.game_h);
    }
}

LRESULT CALLBACK HkWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_SIZE || m == WM_DPICHANGED || m == WM_WINDOWPOSCHANGING)
    {
        static std::atomic<unsigned> geometryLogCount{0};
        const unsigned n = geometryLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 64)
        {
            RECT logical{}, physical{};
            GetClientRect(h, &logical);
            const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            GetClientRect(h, &physical);
            const UINT dpi = GetDpiForWindow(h);
            SetThreadDpiAwarenessContext(previous);
            const WINDOWPOS *pos = (m == WM_WINDOWPOSCHANGING) ? reinterpret_cast<const WINDOWPOS *>(l) : nullptr;
            LOG("[MGPU][R243-DPI] wnd msg=0x%04x hwnd=%p tid=%lu logical=%ldx%ld physical=%ldx%ld dpi=%u "
                "size=%ldx%ld requested=%dx%d flags=0x%x",
                m, (void *)h, GetCurrentThreadId(),
                (long)(logical.right - logical.left), (long)(logical.bottom - logical.top),
                (long)(physical.right - physical.left), (long)(physical.bottom - physical.top), dpi,
                (long)LOWORD(l), (long)HIWORD(l), pos ? pos->cx : 0, pos ? pos->cy : 0, pos ? pos->flags : 0u);
        }
    }
    translate_dpi_unaware_game_size(h, m, l);
    if (m == WM_MOUSEWHEEL) wheel += GET_WHEEL_DELTA_WPARAM(w);
    switch (m)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        ownKeySeen[(w & 0xff) >> 5].fetch_or(1u << ((w & 0xff) & 31), std::memory_order_relaxed);
        ownKey[w & 0xff].store(1, std::memory_order_relaxed);
        ownKeySeq.fetch_add(1, std::memory_order_relaxed);
        // 2026-11-14 physical-key hunt: the panel opens via PostMessage but not via
        // the user's real key. Log every distinct keydown (throttled 1/s per vk) and
        // always the toggle key, with the raw async state, so one test run shows
        // whether the physical key reaches this wndproc and as which VK.
        {
            static double keyLogBase = -1;
            static uint32_t keyLogLast[256] = {};
            static uint8_t keyLogSeen[256] = {};
            static uint8_t keyLogCount = 0;
            if (keyLogBase < 0) keyLogBase = Now();
            const uint8_t vk = w & 0xff;
            const double t = Now() - keyLogBase;
            if (t < 120.0 && keyLogCount < 200 &&
                (vk == (cfg.key & 0xff) || !keyLogSeen[vk] || t - double(keyLogLast[vk]) > 1.0))
            {
                if (keyLogSeen[vk]) ++keyLogCount;
                keyLogSeen[vk] = 1;
                keyLogLast[vk] = uint32_t(t);
                LOG("[MGPU][POV] keydn vk=0x%02x repeat=%u raw_async=%u fg=%p game=%p t=%.1f",
                    unsigned(vk), unsigned((l & 0x40000000) != 0),
                    unsigned((realAsyncKey(vk) & 0x8000) != 0), (void *)GetForegroundWindow(),
                    (void *)gameWnd, t);
            }
        }
        if ((w & 0xff) == (cfg.key & 0xff) && !(l & (LPARAM)0x40000000))
            ownPressSeq.fetch_add(1, std::memory_order_relaxed);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        ownKey[w & 0xff].store(0, std::memory_order_relaxed);
        ownKeySeq.fetch_add(1, std::memory_order_relaxed);
        break;
    case WM_LBUTTONDOWN:
        ownKey[VK_LBUTTON].store(1, std::memory_order_relaxed);
        ownKeySeq.fetch_add(1, std::memory_order_relaxed);
        break;
    case WM_LBUTTONUP:
        ownKey[VK_LBUTTON].store(0, std::memory_order_relaxed);
        ownKeySeq.fetch_add(1, std::memory_order_relaxed);
        break;
    case WM_KILLFOCUS:   // leave nothing stuck down when the window loses focus
        for (auto &k : ownKey) k.store(0, std::memory_order_relaxed);
        ownKeySeq.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }
    if (blocking)
    {
        const bool altF4 = m == WM_SYSKEYDOWN && w == VK_F4;
        const bool release = m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_LBUTTONUP || m == WM_RBUTTONUP ||
                             m == WM_MBUTTONUP || m == WM_XBUTTONUP;
        if (!release && ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST && !altF4))) return 0;
        if (m == WM_INPUT) return DefWindowProcW(h, m, w, l);
    }
    return CallWindowProcW(gameProc, h, m, w, l);
}

void HookInput()
{
    // MinHook needs its one-time init before any MH_CreateHook* - prism does this in
    // core.cpp. Without it every MH_CreateHookApi below returns MH_ERROR_NOT_INITIALIZED
    // and the panel cannot neutralise the game's input while open.
    if (const auto r = MH_Initialize(); r != MH_OK && r != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG("[MGPU][POV] MinHook init failed (%d): input hooks off", int(r));
        return;
    }
    auto api = [](const wchar_t *mod, const char *name, void *detour, void **real) {
        if (MH_CreateHookApi(mod, name, detour, real) != MH_OK)
        {
            LOG("[MGPU][POV] %s not hooked", name);
            return;
        }
        void *target = (void *)GetProcAddress(GetModuleHandleW(mod), name);
        if (target) MH_EnableHook(target);
    };
    api(L"user32", "GetAsyncKeyState", (void *)&HkAsyncKey, (void **)&realAsyncKey);
    api(L"user32", "GetKeyState", (void *)&HkKeyState, (void **)&realKeyState);
    api(L"user32", "GetKeyboardState", (void *)&HkKeyboardState, (void **)&realKeyboardState);
    api(L"user32", "GetCursorPos", (void *)&HkGetCursorPos, (void **)&realGetCursorPos);
    api(L"user32", "SetCursorPos", (void *)&HkSetCursorPos, (void **)&realSetCursorPos);
    api(L"user32", "ClipCursor", (void *)&HkClipCursor, (void **)&realClipCursor);
    api(L"user32", "GetRawInputData", (void *)&HkRawData, (void **)&realRawData);
    api(L"user32", "GetRawInputBuffer", (void *)&HkRawBuffer, (void **)&realRawBuffer);
    api(L"user32", "SetWindowPos", (void *)&HkSetWindowPos, (void **)&realSetWindowPosSWP);
}

void SetBlocking(bool on)
{
    if (on == blocking) return;
    LOG("[MGPU][POV][INPUT-GUARD] game input %s", on ? "BLOCKED (visible panel)" : "RELEASED");
    // Save and restore the game's cursor clip in ONE fixed coordinate space.
    // GetClipCursor/ClipCursor return/interpret the rect in the CALLING
    // thread's DPI awareness. Saving on an unaware thread (logical 2194x1234)
    // and restoring on a per-monitor-aware one installed those logical values
    // as a PHYSICAL clip - the cursor was trapped in the top-left corner of
    // the 4K screen until the game happened to re-clip. Pin both calls to
    // DPI-unaware so the round trip reproduces the game's own clip exactly.
    const DPI_AWARENESS_CONTEXT clipPrev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    if (on)
    {
        RECT clip, desk;
        GetClipCursor(&clip);
        GetWindowRect(GetDesktopWindow(), &desk);
        gameClipped = clip.left > desk.left || clip.top > desk.top || clip.right < desk.right || clip.bottom < desk.bottom;
        gameClip = clip;
        blocking = true;
        realClipCursor(nullptr);
    }
    else
    {
        blocking = false;
        if (gameClipped) realClipCursor(&gameClip);
    }
    SetThreadDpiAwarenessContext(clipPrev);
    if (on)
        realGetCursorPos(&frozenCursor);
    else
        realSetCursorPos(frozenCursor.x, frozenCursor.y);
}

// ------------------------------------------------------------------ draw list
struct Vtx
{
    float x, y, u, v;
    uint32_t c;
};
std::vector<Vtx> verts;
struct Box
{
    float x0, y0, x1, y1;
};
Box clip = {-1e9f, -1e9f, 1e9f, 1e9f};

void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c)
{
    if ((c >> 24) == 0) return;
    const float cx0 = std::max(x0, clip.x0), cy0 = std::max(y0, clip.y0), cx1 = std::min(x1, clip.x1),
                cy1 = std::min(y1, clip.y1);
    if (cx0 >= cx1 || cy0 >= cy1) return;
    if (u0 >= 0.0f && (cx0 != x0 || cy0 != y0 || cx1 != x1 || cy1 != y1))
    {
        const float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
        u0 += (cx0 - x0) * du;
        u1 -= (x1 - cx1) * du;
        v0 += (cy0 - y0) * dv;
        v1 -= (y1 - cy1) * dv;
    }
    const Vtx a = {cx0, cy0, u0, v0, c}, b = {cx1, cy0, u1, v0, c}, d = {cx0, cy1, u0, v1, c},
              e = {cx1, cy1, u1, v1, c};
    verts.insert(verts.end(), {a, b, d, b, e, d});
}
void Fill(float x0, float y0, float x1, float y1, uint32_t c) { Quad(x0, y0, x1, y1, -1, -1, -1, -1, c); }
void Frame(float x0, float y0, float x1, float y1, float t, uint32_t c)
{
    Fill(x0, y0, x1, y0 + t, c);
    Fill(x0, y1 - t, x1, y1, c);
    Fill(x0, y0 + t, x0 + t, y1 - t, c);
    Fill(x1 - t, y0 + t, x1, y1 - t, c);
}
void Tri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t c)
{
    verts.insert(verts.end(), {Vtx{ax, ay, -1, -1, c}, Vtx{bx, by, -1, -1, c}, Vtx{cx, cy, -1, -1, c}});
}
void LineSeg(float ax, float ay, float bx, float by, float w, uint32_t c)
{
    const float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    const float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    Tri(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, c);
    Tri(ax + nx, ay + ny, bx - nx, by - ny, ax - nx, ay - ny, c);
}

// ------------------------------------------------------------------ glyphs
constexpr int kAtlas = 2048;
std::vector<uint8_t> atlas(size_t(kAtlas) * kAtlas);
int shelfX = 1, shelfY = 1, shelfH = 0;
bool atlasDirty = true;
float ui = 1.0f;  // panel units: 1.0 at 1080p, quantized to quarters

enum Style
{
    kTitle,
    kBody,
    kLabel,
    kSmall,
    kValue,
    kValueLarge,
    kStyles
};
IDWriteFactory *dw = nullptr;
struct Face
{
    IDWriteFontFace *f = nullptr;
    float upem = 1, ascent = 0, descent = 0;
} faces[4];
struct Glyph
{
    int16_t x = 0, y = 0, w = 0, h = 0, left = 0, top = 0;
    float advance = 0;
    bool ok = false;
};
std::unordered_map<uint64_t, Glyph> glyphs;
std::unordered_map<uint64_t, uint16_t> glyphIndex;  // (face, code point) -> glyph

IDWriteFontFace *SystemFace(const wchar_t *const *families, DWRITE_FONT_WEIGHT weight)
{
    IDWriteFontCollection *col = nullptr;
    if (FAILED(dw->GetSystemFontCollection(&col, FALSE))) return nullptr;
    IDWriteFontFace *face = nullptr;
    for (const wchar_t *const *fam = families; *fam && !face; ++fam)
    {
        UINT32 i = 0;
        BOOL exists = FALSE;
        if (FAILED(col->FindFamilyName(*fam, &i, &exists)) || !exists) continue;
        IDWriteFontFamily *family = nullptr;
        IDWriteFont *font = nullptr;
        if (SUCCEEDED(col->GetFontFamily(i, &family)) &&
            SUCCEEDED(family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
            font->CreateFontFace(&face);
        if (font) font->Release();
        if (family) family->Release();
    }
    col->Release();
    return face;
}

bool InitFonts()
{
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), (IUnknown **)&dw))) return false;
    // JetBrains Mono from memory (Windows 10 1703+), Consolas otherwise.
    IDWriteFactory5 *dw5 = nullptr;
    if (SUCCEEDED(dw->QueryInterface(__uuidof(IDWriteFactory5), (void **)&dw5)))
    {
        IDWriteInMemoryFontFileLoader *loader = nullptr;
        if (SUCCEEDED(dw5->CreateInMemoryFontFileLoader(&loader)) && SUCCEEDED(dw5->RegisterFontFileLoader(loader)))
        {
            const unsigned char *data[2] = {kMonoRegular, kMonoBold};
            const UINT32 size[2] = {sizeof kMonoRegular, sizeof kMonoBold};
            for (int i = 0; i < 2; ++i)
            {
                IDWriteFontFile *file = nullptr;
                if (SUCCEEDED(loader->CreateInMemoryFontFileReference(dw5, data[i], size[i], nullptr, &file)))
                {
                    dw5->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &faces[i].f);
                    file->Release();
                }
            }
        }
        dw5->Release();
    }
    static const wchar_t *mono[] = {L"Consolas", L"Courier New", nullptr};
    static const wchar_t *han[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimHei", L"Segoe UI", nullptr};
    if (!faces[0].f) faces[0].f = SystemFace(mono, DWRITE_FONT_WEIGHT_REGULAR);
    if (!faces[1].f) faces[1].f = SystemFace(mono, DWRITE_FONT_WEIGHT_BOLD);
    faces[2].f = SystemFace(han, DWRITE_FONT_WEIGHT_REGULAR);
    faces[3].f = SystemFace(han, DWRITE_FONT_WEIGHT_BOLD);
    for (Face &f : faces)
    {
        if (!f.f) return false;
        DWRITE_FONT_METRICS m;
        f.f->GetMetrics(&m);
        f.upem = float(m.designUnitsPerEm);
        f.ascent = float(m.ascent);
        f.descent = float(m.descent);
    }
    return true;
}

uint16_t GlyphOf(int face, uint32_t cp)
{
    const uint64_t key = (uint64_t(face) << 32) | cp;
    auto it = glyphIndex.find(key);
    if (it != glyphIndex.end()) return it->second;
    UINT16 g = 0;
    faces[face].f->GetGlyphIndices(&cp, 1, &g);
    glyphIndex[key] = g;
    return g;
}

// The glyph rasterised at one of four sub-pixel x offsets, its coverage in the atlas.
const Glyph &Raster(int face, float px, uint16_t gi, int bucket)
{
    const uint64_t key = (uint64_t(face) << 60) | (uint64_t(bucket) << 56) | (uint64_t(px * 8.0f + 0.5f) << 16) | gi;
    auto it = glyphs.find(key);
    if (it != glyphs.end()) return it->second;
    Glyph &g = glyphs[key];
    DWRITE_GLYPH_METRICS gm;
    faces[face].f->GetDesignGlyphMetrics(&gi, 1, &gm, FALSE);
    g.advance = gm.advanceWidth * px / faces[face].upem;
    const FLOAT adv = 0;
    const DWRITE_GLYPH_OFFSET off = {};
    DWRITE_GLYPH_RUN run = {faces[face].f, px, 1, &gi, &adv, &off, FALSE, 0};
    IDWriteGlyphRunAnalysis *an = nullptr;
    if (FAILED(dw->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC,
                                          DWRITE_MEASURING_MODE_NATURAL, bucket * 0.25f, 0.0f, &an)))
        return g;
    RECT b;
    an->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &b);
    const int w = b.right - b.left, h = b.bottom - b.top;
    if (w > 0 && h > 0 && w < 256 && h < 256)
    {
        if (shelfX + w + 1 > kAtlas)
        {
            shelfX = 1;
            shelfY += shelfH + 1;
            shelfH = 0;
        }
        if (shelfY + h + 1 > kAtlas)
        {  // full: start over (the next frames re-rasterise what they use)
            glyphs.clear();
            std::fill(atlas.begin(), atlas.end(), 0);
            shelfX = shelfY = 1;
            shelfH = 0;
            atlasDirty = true;
            an->Release();
            return Raster(face, px, gi, bucket);
        }
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        an->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &b, rgb.data(), UINT(rgb.size()));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                // the green channel is the linear coverage
                const float a = rgb[size_t(y) * w * 3 + size_t(x) * 3 + 1] / 255.0f;
                atlas[size_t(shelfY + y) * kAtlas + shelfX + x] = uint8_t(std::min(255.0, std::pow(a, 0.82) * 255.0 + 0.5));
            }
        g.x = int16_t(shelfX);
        g.y = int16_t(shelfY);
        g.w = int16_t(w);
        g.h = int16_t(h);
        g.left = int16_t(b.left);
        g.top = int16_t(b.top);
        g.ok = true;
        shelfX += w + 1;
        shelfH = std::max(shelfH, h);
        atlasDirty = true;
    }
    an->Release();
    return g;
}

// prism's FaceFor, verbatim semantics: the mono face owns Latin/digits/punct
// only while it actually has the glyph; anything else falls to the CJK face.
// (The first port keyed the face off the codepoint alone, so a character the
// embedded mono lacks rasterised as glyph 0 - the vertical-bar soup.)
int FaceFor(uint32_t cp, bool bold, uint16_t *gi)
{
    const int mono = bold ? 1 : 0;
    if (cp < 0x2e80 && (*gi = GlyphOf(mono, cp)) != 0) return mono;
    *gi = GlyphOf(mono + 2, cp);
    return mono + 2;
}

// [0, 1, 2] -> [regular, bold, regular] per prism's style table (kept identical).
struct StyleDef
{
    float size;
    bool bold;
} styles[kStyles] = {{15, true}, {13, false}, {11, false}, {14, false}, {22, true}, {12, false}};

float LineHeight(Style s) { return std::round(styles[s].size * 1.45f * ui); }

float Text(float x, float y, const std::wstring &s, Style st, uint32_t c, bool draw = true)
{
    const float px = styles[st].size * ui;
    const Face &han = faces[styles[st].bold ? 3 : 2];
    const float asc = han.ascent * px / han.upem, desc = han.descent * px / han.upem;
    const float baseline = std::round(y + (LineHeight(st) - (asc + desc)) * 0.5f + asc);
    float pen = x;
    for (size_t i = 0; i < s.size(); ++i)
    {
        uint32_t cp = s[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < s.size())
        {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            ++i;
        }
        uint16_t gi = 0;
        const int face = FaceFor(cp, styles[st].bold, &gi);
        const float ix = std::floor(pen);
        const int bucket = std::min(3, int((pen - ix) * 4.0f));
        const Glyph &g = Raster(face, px, gi, bucket);
        if (draw && g.ok)
        {
            const float gx = ix + g.left, gy = baseline + g.top;
            Quad(gx, gy, gx + g.w, gy + g.h, float(g.x) / kAtlas, float(g.y) / kAtlas,
                 float(g.x + g.w) / kAtlas, float(g.y + g.h) / kAtlas, c);
        }
        pen += g.advance;
    }
    return pen - x;
}

float Measure(const std::wstring &s, Style st) { return Text(0, 0, s, st, 0, false); }
float TextRight(float right, float y, const std::wstring &s, Style st, uint32_t c)
{
    const float w = Measure(s, st);
    Text(right - w, y, s, st, c);
    return w;
}

std::vector<std::wstring> Wrap(const std::wstring &s, Style st, float width)
{
    std::vector<std::wstring> lines;
    std::wstring line;
    size_t lastSpace = std::wstring::npos;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const wchar_t ch = s[i];
        line += ch;
        if (ch == L' ')
        {
            lastSpace = line.size() - 1;
            continue;
        }
        if (ch < 0x2e80 && lastSpace != std::wstring::npos && ch != L' ')
        {
            if (Measure(line.substr(0, lastSpace + 1), st) > width && lastSpace > 0)
            {
                lines.push_back(line.substr(0, lastSpace));
                line = line.substr(lastSpace + 1);
                lastSpace = std::wstring::npos;
                continue;
            }
        }
        if (Measure(line, st) > width)
        {
            if (ch < 0x2e80 && lastSpace != std::wstring::npos)
            {
                lines.push_back(line.substr(0, lastSpace + 1));
                line = line.substr(lastSpace + 2);
            }
            else
            {
                lines.push_back(line.substr(0, line.size() - 1));
                line = std::wstring(1, ch);
            }
            lastSpace = std::wstring::npos;
        }
    }
    if (!line.empty()) lines.push_back(line);
    for (std::wstring &l : lines)
    {
        while (!l.empty() && l.front() == L' ') l.erase(0, 1);
        while (!l.empty() && l.back() == L' ') l.pop_back();
    }
    return lines;
}

// ------------------------------------------------------------------ GPU
const char *kHlsl = R"(
cbuffer K : register(b0) { float2 InvView; uint Mode; float Nits; };
Texture2D<float> Atlas : register(t0);
SamplerState Point : register(s0);
struct V { float2 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };
struct P { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };
P VS(V v) {
  P o;
  o.p = float4(v.p * InvView * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  o.uv = v.uv; o.c = v.c;
  return o;
}
float3 Lin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 Pq(float3 l) {
  const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  const float3 p = pow(max(l, 0.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
// Mode 0: an SDR back buffer (sRGB-encoded values). 1: scRGB (linear, 1.0 = 80 nits). 2: HDR10 (PQ, BT.2020).
float4 PS(P i) : SV_Target {
  float a = i.c.a;
  if (i.uv.x >= 0.0) {
    float cov = Atlas.SampleLevel(Point, i.uv, 0);
    const float ink = dot(i.c.rgb, float3(0.2126, 0.7152, 0.0722));
    if (Mode == 1u) cov = pow(cov, lerp(0.35, 2.2, ink));
    else if (Mode == 2u) cov = pow(cov, lerp(2.0, 0.45, ink));
    a *= cov;
  }
  float3 c = i.c.rgb;
  if (Mode == 1u) {
    c = Lin(c) * (Nits / 80.0);
  } else if (Mode == 2u) {
    const float3 l = Lin(c);
    const float3 w = float3(dot(l, float3(0.6274, 0.3293, 0.0433)), dot(l, float3(0.0691, 0.9195, 0.0114)), dot(l, float3(0.0164, 0.0880, 0.8956)));
    c = Pq(w * (Nits / 10000.0));
  }
  return float4(c * a, a);
}
)";

constexpr UINT kSlots = 3;
struct Slot
{
    ID3D12CommandAllocator *alloc = nullptr;
    ID3D12Resource *vb = nullptr;
    size_t vbSize = 0;
    void *vbMap = nullptr;
    ID3D12Resource *up = nullptr;
    uint64_t fence = 0;
};
struct Gpu
{
    ID3D12Device *device = nullptr;
    ID3D12RootSignature *root = nullptr;
    ID3DBlob *vs = nullptr, *ps = nullptr;
    std::unordered_map<int, ID3D12PipelineState *> pso;  // per render target format
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    uint64_t fenceValue = 0;
    HANDLE event = nullptr;
    Slot slots[kSlots];
    UINT slot = 0;
    ID3D12Resource *atlasTex = nullptr;
    bool atlasReadable = false;
    ID3D12DescriptorHeap *srvHeap = nullptr, *rtvHeap = nullptr;
    bool ok = false, failed = false;
    bool used = false;       // slot claimed for a device (even if that device failed)
    bool atlasDirty = true;  // per-device: the master atlasDirty in Raster() fans out here at Submit
};
// One overlay context per D3D12 device. This process owns several devices
// (game, probe, the bridge's private T3 device) and each may present its own
// swap chain. The 2026-10-29 log showed the overlay pinned to a 1280x720 ghost
// chain while the real game window ran 3840x2160 - never visible. Per-device
// contexts are the fix.
Gpu gpus[4];

bool GpuFail(Gpu &g, const char *what, HRESULT hr)
{
    LOG("[MGPU][POV] panel off: %s (0x%08lx)", what, hr);
    g.failed = true;
    return false;
}

bool InitGpu(Gpu &g, ID3D12Device *d)
{
    HRESULT hr;
    g.device = d;
    d->AddRef();
    using CompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob **, ID3DBlob **);
    auto compile =
        (CompileFn)(void *)GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile");
    if (!compile) return GpuFail(g, "no d3dcompiler_47", E_FAIL);
    ID3DBlob *err = nullptr;
    if (FAILED(hr = compile(kHlsl, std::strlen(kHlsl), "panel", nullptr, nullptr, "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &g.vs, &err)) ||
        FAILED(hr = compile(kHlsl, std::strlen(kHlsl), "panel", nullptr, nullptr, "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &g.ps, &err)))
    {
        LOG("[MGPU][POV] panel shader: %s", err ? (const char *)err->GetBufferPointer() : "?");
        return GpuFail(g, "shader", hr);
    }
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = {1, &range};
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ID3D12RootSignature *root = nullptr;
    ID3DBlob *blob = nullptr;
    if (FAILED(hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(hr = d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                           (void **)&root)))
        return GpuFail(g, "root signature", hr);
    g.root = root;
    for (Slot &s : g.slots)
        if (FAILED(hr = d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&s.alloc)))
            return GpuFail(g, "allocator", hr);
    if (FAILED(hr = d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.slots[0].alloc, nullptr,
                                         __uuidof(ID3D12GraphicsCommandList), (void **)&g.list)))
        return GpuFail(g, "list", hr);
    g.list->Close();
    if (FAILED(hr = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&g.fence))) return GpuFail(g, "fence", hr);
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kAtlas;
    td.Height = kAtlas;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    if (FAILED(hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               __uuidof(ID3D12Resource), (void **)&g.atlasTex)))
        return GpuFail(g, "atlas", hr);
    D3D12_DESCRIPTOR_HEAP_DESC sh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(hr = d->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), (void **)&g.srvHeap)) ||
        FAILED(hr = d->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void **)&g.rtvHeap)))
        return GpuFail(g, "descriptor heaps", hr);
    d->CreateShaderResourceView(g.atlasTex, nullptr, g.srvHeap->GetCPUDescriptorHandleForHeapStart());
    g.ok = true;
    LOG("[MGPU][POV] panel: gpu context up on device %p", (void *)d);
    return true;
}

// The overlay context for this device, creating it on first use. A device
// that failed stays claimed (used) so we do not retry it every frame, but it
// never blocks another device's context.
Gpu *GpuFor(ID3D12Device *d)
{
    for (Gpu &g : gpus)
        if (g.used && g.device == d)
            return g.ok ? &g : nullptr;
    for (Gpu &g : gpus)
    {
        if (g.used) continue;
        g.used = true;
        return InitGpu(g, d) ? &g : nullptr;
    }
    LOG("[MGPU][POV] panel: device table full at %p", (void *)d);
    return nullptr;
}

ID3D12PipelineState *PsoFor(Gpu &g, DXGI_FORMAT f)
{
    auto it = g.pso.find(int(f));
    if (it != g.pso.end()) return it->second;
    const D3D12_INPUT_ELEMENT_DESC il[] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                           {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                           {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g.root;
    pd.VS = {g.vs->GetBufferPointer(), g.vs->GetBufferSize()};
    pd.PS = {g.ps->GetBufferPointer(), g.ps->GetBufferSize()};
    D3D12_RENDER_TARGET_BLEND_DESC &b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_ONE;
    b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.InputLayout = {il, 3};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = f;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *p = nullptr;
    const HRESULT hr = g.device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&p);
    if (FAILED(hr)) LOG("[MGPU][POV] panel: no pipeline for back buffer format %d (0x%08lx)", int(f), hr);
    g.pso[int(f)] = p;
    return p;
}

// ------------------------------------------------------------------ the swap chains
// A (swap chain, queue, gpu-context) triple per presenting chain. The game,
// the probe and the bridge's ghost path each own at least one chain; the
// panel must follow the real window, not whichever presented first.
struct Chain
{
    IDXGISwapChain *sc = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    Gpu *gpu = nullptr;
    bool unsupported = false;
    bool said_no_window = false;   // the no-visible-window refusal is retryable; log it once
    unsigned probes = 0;           // ... but keep a slow heartbeat so a stuck refusal is visible
} chains[4];

Chain *ChainFor(IDXGISwapChain *sc)
{
    for (Chain &c : chains)
        if (c.sc == sc) return &c;
    Chain *slot = nullptr;
    for (Chain &c : chains)
        if (!c.sc)
        {
            slot = &c;
            break;
        }
    if (!slot) slot = &chains[0];  // table full: recycle the first slot
    if (slot->queue) slot->queue->Release();
    *slot = Chain{};
    slot->sc = sc;
    return slot;
}

int ColourMode(IDXGISwapChain *sc, DXGI_FORMAT f)
{
    // RE2 SDR-only build: do not infer HDR from a 10-bit chain or the monitor.
    // This keeps the Prism panel in the same sRGB mode as the neural output.
    (void)sc;
    (void)f;
    return 0;
}

bool Submit(Chain &c, UINT bw, UINT bh, DXGI_FORMAT fmt)
{
    IDXGISwapChain *sc = c.sc;
    Gpu &g = *c.gpu;
    // Fan the master glyph-dirty flag out to every live device context, then
    // clear it: each gpu uploads the atlas on its own next Submit.
    if (atlasDirty)
    {
        for (Gpu &x : gpus)
            if (x.ok) x.atlasDirty = true;
        atlasDirty = false;
    }
    IDXGISwapChain3 *sc3 = nullptr;
    static bool said3 = false, saidBuf = false, saidPso = false;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&sc3)))
    {
        if (!said3)
        {
            said3 = true;
            LOG("[MGPU][POV] panel: no IDXGISwapChain3 on %p", (void *)sc);
        }
        return false;
    }
    const UINT index = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource *bb = nullptr;
    const HRESULT hr = sc3->GetBuffer(index, __uuidof(ID3D12Resource), (void **)&bb);
    const int mode = ColourMode(sc, fmt);
    sc3->Release();
    if (FAILED(hr) || !bb)
    {
        if (!saidBuf)
        {
            saidBuf = true;
            LOG("[MGPU][POV] panel: GetBuffer(%u) failed 0x%08lx", index, hr);
        }
        return false;
    }
    ID3D12PipelineState *pso = PsoFor(g, fmt);
    if (!pso)
    {
        if (!saidPso)
        {
            saidPso = true;
            LOG("[MGPU][POV] panel: no pipeline for format %d", int(fmt));
        }
        bb->Release();
        return false;
    }

    Slot &s = g.slots[g.slot];
    g.slot = (g.slot + 1) % kSlots;
    if (s.fence && g.fence->GetCompletedValue() < s.fence)
    {
        g.fence->SetEventOnCompletion(s.fence, g.event);
        WaitForSingleObject(g.event, 200);
    }
    const size_t need = std::max<size_t>(verts.size() * sizeof(Vtx), 64 * 1024);
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    auto buffer = [&](size_t bytes) -> ID3D12Resource * {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *r = nullptr;
        return SUCCEEDED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           __uuidof(ID3D12Resource), (void **)&r))
                   ? r
                   : nullptr;
    };
    if (s.vbSize < need)
    {
        if (s.vb) s.vb->Release();
        s.vbSize = need * 2;
        s.vb = buffer(s.vbSize);
        s.vbMap = nullptr;
        if (!s.vb || FAILED(s.vb->Map(0, nullptr, &s.vbMap)))
        {
            s.vb = nullptr;
            s.vbSize = 0;
            bb->Release();
            return false;
        }
    }
    std::memcpy(s.vbMap, verts.data(), verts.size() * sizeof(Vtx));

    s.alloc->Reset();
    ID3D12GraphicsCommandList *l = g.list;
    l->Reset(s.alloc, pso);
    auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br = {};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        l->ResourceBarrier(1, &br);
    };
    if (g.atlasDirty)
    {
        // The whole atlas through this slot's upload buffer (only when new glyphs appeared).
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        UINT rows;
        UINT64 rowBytes, total;
        const D3D12_RESOURCE_DESC td = g.atlasTex->GetDesc();
        g.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        if (!s.up) s.up = buffer(total);
        uint8_t *m = nullptr;
        if (s.up && SUCCEEDED(s.up->Map(0, nullptr, (void **)&m)))
        {
            for (UINT y = 0; y < rows; ++y)
                std::memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &atlas[size_t(y) * kAtlas], kAtlas);
            s.up->Unmap(0, nullptr);
            if (g.atlasReadable) barrier(g.atlasTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = g.atlasTex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = s.up;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(g.atlasTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g.atlasReadable = true;
            g.atlasDirty = false;
        }
    }
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = fmt;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.device->CreateRenderTargetView(bb, &rd, rtv);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp = {0, 0, float(bw), float(bh), 0, 1};
    const D3D12_RECT sr = {0, 0, LONG(bw), LONG(bh)};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sr);
    l->SetGraphicsRootSignature(g.root);
    ID3D12DescriptorHeap *heaps[] = {g.srvHeap};
    l->SetDescriptorHeaps(1, heaps);
    struct
    {
        float ix, iy;
        uint32_t mode;
        float nits;
    } k = {1.0f / bw, 1.0f / bh, uint32_t(mode), cfg.nits};
    l->SetGraphicsRoot32BitConstants(0, 4, &k, 0);
    l->SetGraphicsRootDescriptorTable(1, g.srvHeap->GetGPUDescriptorHandleForHeapStart());
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW vbv = {s.vb->GetGPUVirtualAddress(), UINT(verts.size() * sizeof(Vtx)), sizeof(Vtx)};
    l->IASetVertexBuffers(0, 1, &vbv);
    if (g.atlasReadable) l->DrawInstanced(UINT(verts.size()), 1, 0, 0);
    barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    l->Close();
    ID3D12CommandList *lists[] = {l};
    c.queue->ExecuteCommandLists(1, lists);
    s.fence = ++g.fenceValue;
    c.queue->Signal(g.fence, s.fence);
    bb->Release();
    return true;
}

// ------------------------------------------------------------------ input for the panel
struct Input
{
    float mx = -1, my = -1;  // cursor in back buffer pixels
    bool mouseMoved = false, down = false, pressed = false, released = false;
    int wheelSteps = 0;
    bool up = false, dn = false, left = false, right = false, enter = false, back = false, toggle = false;
    bool side = false;   // Tab, or Y on a gamepad: between the panel and the grading desk
    bool reset = false;  // Backspace or Delete, or X on a gamepad: the focused control back to neutral
    int page = 0;        // LB / RB on a gamepad: the grading desk's previous or next page
    bool fine = false;   // Shift held: the smallest steps
    bool pad = false;    // the last navigation came from a gamepad
    bool padConnected = false;
    WORD padHeld = 0;  // gamepad buttons down now
} in;

struct Repeat
{
    bool was = false;
    double next = 0;
};
bool Rep(bool down, Repeat &r, double now)
{
    if (down && !r.was)
    {
        r.was = true;
        r.next = now + 0.38;
        return true;
    }
    if (down && now >= r.next)
    {
        r.next = now + 0.055;
        return true;
    }
    r.was = down;
    return false;
}
bool Key(int vk)
{
    // Per-key source of truth (2026-11-14): trust the wndproc-tracked state ONLY for
    // keys that have genuinely produced a window message. Raw-Input games (RE2) deliver
    // mouse messages but no keyboard messages, so a global "wndproc is alive" latch
    // silently zeroed every keyboard read. For keys the wndproc has never seen, use
    // GetAsyncKeyState - proven alive in this process by the mult_raw diag lines.
    if (vk > 0 && vk < 256 &&
        (ownKeySeen[vk >> 5].load(std::memory_order_relaxed) >> (vk & 31)) & 1u)
        return ownKey[vk].load(std::memory_order_relaxed) != 0;
    return (realAsyncKey(vk) & 0x8000) != 0;
}

bool Foreground()
{
    HWND f = GetForegroundWindow();
    if (!f) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(f, &pid);
    if (pid == GetCurrentProcessId()) return true;
    // In the independent-window layout the game keeps foreground for normal
    // play while the neural picture is topmost and click-through. NW.js may
    // put its visible parent in a different process from this GPU child, so a
    // PID-only test prevents Multiply from reopening Prism during gameplay.
    HWND game = reinterpret_cast<HWND>(mgpu::gpu1::dcomp_game_hwnd());
    return game != nullptr && GetAncestor(game, GA_ROOT) == GetAncestor(f, GA_ROOT);
}

void PollInput(UINT bw, UINT bh, bool open)
{
    const double now = Now();
    const bool fg = Foreground();
    static bool toggleWas = false, lWas = false;
    static POINT lastCursor = {};
    static Repeat rUp, rDn, rLeft, rRight;
    static WORD padWas = 0;
    static Repeat pUp, pDn, pLeft, pRight;
    in.mouseMoved = in.pressed = in.released = false;
    in.up = in.dn = in.left = in.right = in.enter = in.back = in.toggle = in.side = in.reset = false;
    in.page = 0;
    in.wheelSteps = 0;
    const int wh = wheel.exchange(0);
    {
        static std::atomic<unsigned> poll_said{0};
        if ((poll_said.fetch_add(1, std::memory_order_relaxed) % 120) == 0)
            LOG("[MGPU][POV] poll: fg=%d ownSeq=%u gameWnd=%p fgHwnd=%p key=0x%02x t=%d mult_raw=%d",
                (int)fg, ownKeySeq.load(std::memory_order_relaxed), (void *)gameWnd,
                (void *)GetForegroundWindow(), cfg.key, (int)((cfg.key && Key(cfg.key)) ? 1 : 0),
                int((realAsyncKey(VK_MULTIPLY) & 0x8000) != 0));
    }
    // The toggle is driven by the press counter, not the key level: a tap shorter than a
    // poll interval still lands (the counter holds it), and a hold auto-repeat counts once.
    static unsigned pressSeq = 0;
    static unsigned rawPressSeqLast = 0;
    static bool rawKeyWas = false;
    static unsigned rawPressSeq = 0;
    if (!fg)
    {
        toggleWas = Key(cfg.key);
        lWas = false;
        in.down = false;
        // A press while the game is not in the foreground cannot be a panel toggle
        // (the panel would not have been visible); drop it so the toggle does not
        // fire unexpectedly on the next refocus.
        pressSeq = ownPressSeq.load(std::memory_order_relaxed);
        rawKeyWas = cfg.key && (realAsyncKey(cfg.key) & 0x8000) != 0;  // no phantom edge on refocus
        return;
    }
    const unsigned ps = ownPressSeq.load(std::memory_order_relaxed);
    const unsigned presses = ps - pressSeq;   // physical presses since the last poll
    pressSeq = ps;
    // 2026-11-14 RE2 (RE Engine) never dispatches keyboard messages to its wndproc -
    // the game reads Raw Input, so the ownPressSeq channel above sees zero keydowns
    // while the user is pressing (proven: 0 keydn log lines during a real press test).
    // GetAsyncKeyState IS alive (mult_raw=1 held for seconds in the same log), so
    // count 0->1 edges of the raw state only when this key has never arrived through
    // the hooked window procedure. Stellar Blade delivers BOTH channels for the same
    // physical press; adding them double-counted it and made the panel toggle unstable.
    const bool rawNow = cfg.key && (realAsyncKey(cfg.key) & 0x8000) != 0;
    if (rawNow && !rawKeyWas)
    {
        ++rawPressSeq;
        LOG("[MGPU][POV] raw key edge vk=0x%02x (press #%u)", unsigned(cfg.key & 0xff), rawPressSeq);
    }
    rawKeyWas = rawNow;
    const bool ownChannel = cfg.key &&
        (ownKeySeen[(cfg.key & 0xff) >> 5].load(std::memory_order_relaxed) &
         (1u << ((cfg.key & 0xff) & 31))) != 0;
    const unsigned rawPresses = rawPressSeq - rawPressSeqLast;
    rawPressSeqLast = rawPressSeq;
    const unsigned totalPresses = ownChannel ? presses : rawPresses;
    if (totalPresses != 0) in.toggle = true;
    toggleWas = totalPresses != 0;
    // The cursor, in back buffer pixels (the window's client area may be scaled to it).
    POINT p;
    if (gameWnd && realGetCursorPos(&p))
    {
        if (p.x != lastCursor.x || p.y != lastCursor.y) in.mouseMoved = true;
        lastCursor = p;
        ScreenToClient(gameWnd, &p);
        RECT cr;
        GetClientRect(gameWnd, &cr);
        const float cw = float(std::max<LONG>(1, cr.right - cr.left)), ch = float(std::max<LONG>(1, cr.bottom - cr.top));
        in.mx = float(p.x) * bw / cw;
        in.my = float(p.y) * bh / ch;
    }
    const bool l = Key(VK_LBUTTON);
    in.pressed = l && !lWas;
    in.released = !l && lWas;
    in.down = l;
    lWas = l;
    in.wheelSteps = wh / WHEEL_DELTA;
    if (in.mouseMoved || in.pressed) in.pad = false;
    if (open)
    {
        in.up = Rep(Key(VK_UP), rUp, now);
        in.dn = Rep(Key(VK_DOWN), rDn, now);
        in.left = Rep(Key(VK_LEFT), rLeft, now);
        in.right = Rep(Key(VK_RIGHT), rRight, now);
        static bool enterWas = false, escWas = false, tabWas = false, resetWas = false;
        const bool e = Key(VK_RETURN) || Key(VK_SPACE), x = Key(VK_ESCAPE), tb = Key(VK_TAB), rs = Key(VK_BACK) || Key(VK_DELETE);
        in.enter = e && !enterWas;
        in.back = x && !escWas;
        in.side = tb && !tabWas;
        in.reset = rs && !resetWas;
        enterWas = e;
        escWas = x;
        tabWas = tb;
        resetWas = rs;
        in.fine = Key(VK_SHIFT);
        if (in.up || in.dn || in.left || in.right || in.enter) in.pad = false;
        // DIAG (2026-10-29 keys-dead report): 12 s of raw key state + fired events
        // after each open, one burst per open. Read from ReShade.log [MGPU][POV].
        {
            static double diagUntil = -1;
            static uint32_t diagN = 0;
            if (diagUntil < 0)
            {
                diagUntil = now + 12.0;
                diagN = 0;
            }
            if (now < diagUntil && diagN < 40)
            {
                static bool dUp = false, dDn = false, dEn = false, dL = false, dOwn = false;
                const bool ru = (realAsyncKey(VK_UP) & 0x8000) != 0, rd = (realAsyncKey(VK_DOWN) & 0x8000) != 0,
                           re = (realAsyncKey(VK_RETURN) & 0x8000) != 0;
                // 2026-10-29 slider-dead report: the button read shares the
                // GetAsyncKeyState path. Log BOTH the raw API and the
                // wndproc-tracked state so the next test shows which one is
                // alive (raw=1 own=0 => API works, messages don't; raw=0
                // own=1 => the API is hooked dead, the wndproc path is the
                // fix; both 0 => neither sees the button).
                const bool rl = (realAsyncKey(VK_LBUTTON) & 0x8000) != 0;
                const bool ol = ownKey[VK_LBUTTON].load(std::memory_order_relaxed) != 0;
                if (ru != dUp || rd != dDn || re != dEn || rl != dL || ol != dOwn || in.up || in.dn || in.enter ||
                    in.pressed || in.down)
                {
                    dUp = ru;
                    dDn = rd;
                    dEn = re;
                    dL = rl;
                    dOwn = ol;
                    ++diagN;
                    LOG("[MGPU][POV] keys diag: raw U%u D%u E%u L%u | own L%u seq=%u | fired up=%d dn=%d enter=%d "
                        "Lpress=%d Ldown=%d | pad=%d fg=%d",
                        unsigned(ru), unsigned(rd), unsigned(re), unsigned(rl), unsigned(ol),
                        ownKeySeq.load(std::memory_order_relaxed), int(in.up), int(in.dn), int(in.enter),
                        int(in.pressed), int(in.down), int(in.pad), int(fg));
                }
            }
            if (now >= diagUntil && diagN < 41 && diagN > 0)
            {
                diagN = 41;  // close the burst; rearm on the next open
                LOG("[MGPU][POV] keys diag end (%u events)", diagN - 1);
                diagUntil = -1;
            }
        }
    }
    // The gamepad: View + Y opens and closes; the d-pad or left stick moves, A changes, B closes.
    in.padConnected = false;
    in.padHeld = 0;
    if (cfg.pad && padRead)
    {
        static double nextProbe = 0;
        static int pad = -1;
        XINPUT_STATE st = {};
        if (pad >= 0 && padRead(DWORD(pad), &st) != ERROR_SUCCESS) pad = -1;
        if (pad < 0 && now >= nextProbe)
        {
            nextProbe = now + 2.0;  // probing absent pads is slow: once in a while
            for (int i = 0; i < 4 && pad < 0; ++i)
                if (padRead(DWORD(i), &st) == ERROR_SUCCESS) pad = i;
        }
        if (pad >= 0)
        {
            in.padConnected = true;
            const WORD b = st.Gamepad.wButtons, pressed = WORD(b & ~padWas);
            padWas = b;
            in.padHeld = b;
            if ((b & XINPUT_GAMEPAD_BACK) && (pressed & XINPUT_GAMEPAD_Y))
            {
                in.toggle = true;
                in.pad = true;
            }
            if (open)
            {
                const SHORT ly = st.Gamepad.sThumbLY, lx = st.Gamepad.sThumbLX;
                const bool u = (b & XINPUT_GAMEPAD_DPAD_UP) || ly > 20000, d = (b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -20000;
                const bool le = (b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -20000, ri = (b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > 20000;
                const bool pu = Rep(u, pUp, now), pd = Rep(d, pDn, now), pl = Rep(le, pLeft, now), pr = Rep(ri, pRight, now);
                if (pu || pd || pl || pr || (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B))) in.pad = true;
                in.up |= pu;
                in.dn |= pd;
                in.left |= pl;
                in.right |= pr;
                in.enter |= (pressed & XINPUT_GAMEPAD_A) != 0;
                in.back |= (pressed & XINPUT_GAMEPAD_B) != 0;
                in.reset |= (pressed & XINPUT_GAMEPAD_X) != 0;
                if ((pressed & XINPUT_GAMEPAD_Y) && !(b & XINPUT_GAMEPAD_BACK)) in.side = true;
                if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) in.page = -1;
                if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) in.page = 1;
                if (pressed & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER |
                               XINPUT_GAMEPAD_RIGHT_SHOULDER))
                    in.pad = true;
            }
        }
    }
}

// ------------------------------------------------------------------ the bridge data adapter
// The panel's model of the world: one snapshot of everything it shows or changes, read from
// gpu1's ui_state + pfinish, applied back through the same setters the ImGui panel used.
struct OvSet
{
    bool enabled = false, split = false, armed = false;
    int look = 0, modelStyle = 0, passes = 1, cadence = 1, gradeSlot = 0, fpsCap = 0;
    float intensity = 1.0f, guard = 1.0f, faceMask = 0.30f, nrs = 0.75f, splitPos = 0.5f;
    bool nrsOn = false;
    unsigned outW = 0, outH = 0, netW = 0, netH = 0;
    prism::GradeValues grade[prism::kGradeSlots];
};

double panel_savedAt = -10;  // the "已保存" indicator's last save (declared here: ApplySet below uses it)

struct OvStats
{
    static constexpr int kMax = 256;
    float ms[kMax] = {};  // the last frames' times, oldest first
    int frames = 0;
    float renderFps = 0, evalFps = 0, presentFps = 0;
    double evalMs = 0;
    bool tsOk = false;
    unsigned outW = 0, outH = 0, netW = 0, netH = 0;
} g_stats;

// The panel used to report the logical window size (2194x1234 at 175% DPI)
// as the output. The actual present chain is physical pixels, so when the
// game window covers the monitor we report the monitor's physical size.
static void physical_output_extent(unsigned fallback_w, unsigned fallback_h,
                                   unsigned &out_w, unsigned &out_h)
{
    out_w = fallback_w;
    out_h = fallback_h;

    HWND host = (HWND)mgpu::gpu1::dcomp_game_hwnd();
    if (host == nullptr) return;

    HMONITOR monitor = MonitorFromWindow(host, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) return;

    RECT client{};
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;

    const DPI_AWARENESS_CONTEXT previous =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const BOOL client_ok = GetClientRect(host, &client);
    const BOOL monitor_ok = GetMonitorInfoW(monitor, &mi);
    SetThreadDpiAwarenessContext(previous);

    if (!client_ok || !monitor_ok) return;

    const int cw = client.right - client.left;
    const int ch = client.bottom - client.top;
    const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (cw <= 0 || ch <= 0 || mw <= 0 || mh <= 0) return;

    // A fullscreen physical client rect may be 3839x2159; as long as it
    // substantially covers the monitor, report the physical monitor size.
    if (cw >= mw - 2 && ch >= mh - 2)
    {
        out_w = (unsigned)mw;
        out_h = (unsigned)mh;
    }
}

void ReadSet(OvSet &s)
{
    mgpu::gpu1::ui_state st;
    mgpu::gpu1::ui_read(st);
    s.armed = st.armed;
    s.enabled = st.neural;
    s.intensity = st.intensity[0];
    s.faceMask = st.face_mask_strength;
    s.modelStyle = std::clamp(int(st.style + 0.5f), 0, 2);
    s.passes = int(st.passes);
    s.cadence = int(st.nr_cadence);
    s.fpsCap = mgpu::t119::frame_cap();
    s.nrsOn = st.nrs_on;
    // The old global row is a master for the new per-pass scales. Mirror pass 1
    // so an edit cannot snap back while NRScale1/2/3 override the legacy value.
    s.nrs = st.nrs_pscale[0] > 0.05f ? st.nrs_pscale[0] : st.nrs_scale;
    s.split = st.present_mode == 2;
    s.splitPos = st.split_pos;
    s.outW = st.nrs_native_w;
    s.outH = st.nrs_native_h;
    physical_output_extent(st.nrs_native_w, st.nrs_native_h, s.outW, s.outH);
    s.netW = st.nrs_work_w;
    s.netH = st.nrs_work_h;
    if (mgpu::pfinish::active())
    {
        s.look = int(mgpu::pfinish::look());
        s.guard = mgpu::pfinish::guard();
        s.gradeSlot = mgpu::pfinish::grade_slot();
        for (int i = 0; i < prism::kGradeSlots; ++i) s.grade[i] = mgpu::pfinish::grade_snapshot(i);
    }
    if (mgpu::psync::active())
    {
        s.intensity = mgpu::psync::intensity();
        s.guard = mgpu::psync::guard();
    }
    // PX1 owns character protection on the reduced per-frame chain. Finish is
    // grade-only there, so its guard value must not overwrite the live slider.
    if (mgpu::prism_x1m::active(0))
        s.guard = mgpu::prism_x1m::guard(0);
}

void NoteStats()
{
    mgpu::gpu1::ui_state st;
    mgpu::gpu1::ui_read(st);
    if (st.fps_consumed > 1.0)
    {
        if (g_stats.frames < OvStats::kMax) g_stats.ms[g_stats.frames++] = float(1000.0 / st.fps_consumed);
        else
        {
            std::memmove(g_stats.ms, g_stats.ms + 1, sizeof(g_stats.ms[0]) * (OvStats::kMax - 1));
            g_stats.ms[OvStats::kMax - 1] = float(1000.0 / st.fps_consumed);
        }
    }
    g_stats.renderFps = float(st.fps_produced);
    g_stats.evalFps = float(st.fps_evaluated);
    g_stats.presentFps = float(st.present_fps);
    g_stats.evalMs = st.gpu1_eval_ms;
    g_stats.tsOk = st.gpu1_ts_ok;
}

// Push the edited snapshot back through the bridge's own setters (the same ones the ImGui
// panel drove). Grade slots persist on change.
void ApplySet(const OvSet &s)
{
    mgpu::gpu1::ui_state st;
    mgpu::gpu1::ui_read(st);
    if (st.neural != s.enabled) mgpu::gpu1::ui_set_neural(s.enabled);
    if (mgpu::psync::active())
    {
        if (std::fabs(mgpu::psync::intensity() - s.intensity) > 1e-4f)
        {
            mgpu::psync::set_intensity(s.intensity);
            (void)mgpu::gpu1::ui_ini_write_float("PrismSyncIntensity", s.intensity);
        }
    }
    else if (std::fabs(st.intensity[0] - s.intensity) > 1e-4f)
    {
        mgpu::gpu1::ui_set_intensity(0u, s.intensity);
        // This overlay writes its own settings directly; PanelPersist may be off.
        (void)mgpu::gpu1::ui_ini_write_float("Intensity", s.intensity);
        for (unsigned i = 0; i < st.max_passes; ++i)
        {
            char key[32];
            snprintf(key, sizeof key, "Intensity%u", i + 1u);
            (void)mgpu::gpu1::ui_ini_write_float(key, s.intensity);
        }
        (void)mgpu::gpu1::ui_ini_write_word("Preset", "manual");
    }
    const int modelStyle = std::clamp(s.modelStyle, 0, 2);
    if (int(st.style + 0.5f) != modelStyle)
        mgpu::gpu1::ui_set_tuning_value(3, float(modelStyle));
    if (std::fabs(st.face_mask_strength - s.faceMask) > 1e-4f)
        mgpu::gpu1::ui_set_face_mask_strength(s.faceMask);
    if (int(st.passes) != s.passes) mgpu::gpu1::ui_set_passes(unsigned(s.passes));
    if (int(st.nr_cadence) != s.cadence) mgpu::gpu1::ui_set_cadence(unsigned(s.cadence));
    if (mgpu::t119::frame_cap() != s.fpsCap)
    {
        mgpu::t119::set_frame_cap(s.fpsCap);
        (void)mgpu::gpu1::ui_ini_write("FrameCap", s.fpsCap);
    }
    const float oldMaster = st.nrs_pscale[0] > 0.05f ? st.nrs_pscale[0] : st.nrs_scale;
    if (s.nrsOn && std::fabs(oldMaster - s.nrs) > 1e-4f)
    {
        // A real master, not the old ui_nrs_stage call hidden by per-pass overrides.
        (void)mgpu::gpu1::ui_ini_write_float("NRScale", s.nrs);
        for (unsigned i = 0; i < std::clamp(st.passes, 1u, 3u); ++i)
        {
            char key[16];
            snprintf(key, sizeof key, "NRScale%u", i + 1u);
            (void)mgpu::gpu1::ui_ini_write_float(key, s.nrs);
            mgpu::gpu1::ui_set_scale(i, s.nrs);
        }
    }
    const int wantMode = s.split ? 2 : 0;
    if (int(st.present_mode) != wantMode) mgpu::gpu1::ui_set_present_mode(wantMode);
    if (mgpu::pfinish::active())
    {
        if (mgpu::pfinish::look() != unsigned(s.look))
        {
            mgpu::pfinish::set_look(unsigned(s.look));
            (void)mgpu::gpu1::ui_ini_write("FinishLook", s.look);
        }
        if (mgpu::pfinish::grade_slot() != s.gradeSlot) mgpu::pfinish::set_grade_slot(s.gradeSlot);
        bool grade_changed = false;
        for (int i = 0; i < prism::kGradeSlots; ++i)
            if (mgpu::pfinish::grade_snapshot(i) != s.grade[i])
            {
                mgpu::pfinish::set_grade_values(i, s.grade[i]);
                mgpu::pfinish::save_grade_slot(i);
                grade_changed = true;
            }
        // R375: the grade table only reaches the picture at Look 5; editing a grade
        // under a preset look silently did nothing. Switch for the operator.
        if (grade_changed && mgpu::pfinish::look() != 5u)
        {
            mgpu::pfinish::set_look(5u);
            (void)mgpu::gpu1::ui_ini_write("FinishLook", 5);
        }
    }
    bool px1_guard_owner = false;
    bool px1_guard_changed = false;
    for (unsigned i = 0; i < mgpu::prism_x1m::kMaxPasses; ++i)
    {
        if (!mgpu::prism_x1m::active(i)) continue;
        px1_guard_owner = true;
        if (std::fabs(mgpu::prism_x1m::guard(i) - s.guard) > 1e-3f)
        {
            mgpu::prism_x1m::set_guard(i, s.guard);
            px1_guard_changed = true;
        }
    }
    if (px1_guard_changed)
        (void)mgpu::gpu1::ui_ini_write_float("PrismX1Guard", s.guard);
    if (!px1_guard_owner && mgpu::psync::active())
    {
        if (std::fabs(mgpu::psync::guard() - s.guard) > 1e-3f)
        {
            mgpu::psync::set_guard(s.guard);
            (void)mgpu::gpu1::ui_ini_write_float("PrismSyncGuard", s.guard);
        }
    }
    // PX1 protects during residual composition; Finish performs the final
    // character-guard composition. One slider owns both, including 0%=off.
    if (mgpu::pfinish::active() &&
        std::fabs(mgpu::pfinish::guard() - s.guard) > 1e-3f)
    {
        mgpu::pfinish::set_guard(s.guard);
        (void)mgpu::gpu1::ui_ini_write_float("FinishGuard", s.guard);
    }
    panel_savedAt = Now();
}

// ------------------------------------------------------------------ the panel
enum Row
{
    kOn,
    kLook,
    kIntensity,
    kModelStyle,
    kPasses,
    kCadence,
    kNetScale,
    kGuard,
    kFaceMask,
    kFpsCap,
    kSplit,
    kFrameGen,
    kP1Net,
    kP1Res,
    kP2Net,
    kP2Res,
    kP3Net,
    kP3Res,
    kRows
};
struct Panel
{
    std::atomic<bool> open{false};
    double openedAt = 0, closedAt = -10;
    int focus = kLook;
    int hoverOption = -1;  // the option under the cursor in the focused row, for its help
    enum Drag
    {
        kNone,
        kMove,
        kSlide,
        kSplitLine
    } drag = kNone;
    int dragRow = -1;
    float dragDX = 0, dragDY = 0;
    float seg[kRows + 8][2] = {};  // the inverted block's animated left and right, per segmented row
    bool segInit[kRows + 8] = {};
    int side = 0;  // where the keys and the gamepad go: 0 the panel, 1 the grading desk
    float pw = 0, ph = 0;
    // The grading desk.
    int gPage = 0, gFocus = 2;
    int gChannel = 0, gAttr = 1;
    int gDragPoint = -1, gDragWheel = -1;
    float gDragY = 0, gDragFrom = 0;
    double gClickAt = -1;
    int gClickParam = -1;  // a double click resets
    double changedAt = -1;
    std::wstring toast;
    uint32_t toastMark = kReady;
    double toastAt = -10, toastFor = 1.8;
    bool hinted = false;
    double startedAt = 0;
    float graphTop = 20.0f;  // the frame-time graph's scale (ms), eased
    float px0 = 0, py0 = 0;   // the panel's pixel origin this frame
} panel;

// Input may be blocked only after the panel has actually been submitted to a
// presenting chain.  An open flag alone is not proof that the player can see
// it (the chain may be covered, unbound, or have failed its draw).
double panelDrawnAt = -10.0;
bool inputGuardSaid = false;

float EaseOutExpo(float t) { return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }
float U(float v) { return std::round(v * ui); }  // panel units to whole pixels
float Hair() { return std::max(1.0f, std::floor(ui)); }

void Toast(const std::wstring &s, uint32_t mark = kReady, double seconds = 1.8)
{
    panel.toast = s;
    panel.toastMark = mark;
    panel.toastAt = Now();
    panel.toastFor = seconds;
}

std::wstring Fmt(const wchar_t *f, double v)
{
    wchar_t b[32];
    std::swprintf(b, 32, f, v);
    return b;
}

// A bar run for a value from 0 to `max`: one 2px bar per step, filled up to the value, with a
// taller bar at each reference value and a 2px caution mark under the playground range.
struct Bars
{
    float x0, y0, x1, y1;
};
Bars BarSlider(float x0, float y, float x1, float h, float value, float max, float ref, float playground, bool enabled, bool hot)
{
    const float bw = std::max(2.0f, std::round(2.0f * ui)), gap = std::max(1.0f, std::round(2.0f * ui));
    const int n = int((x1 - x0 + gap) / (bw + gap));
    const float span = n * (bw + gap) - gap;
    const float top = y, bottom = y + h;
    for (int i = 0; i < n; ++i)
    {
        const float v = (i + 0.5f) / n * max;
        const float bx = x0 + i * (bw + gap);
        const bool filled = v <= value + 1e-4f;
        const bool isRef = ref > 0 && std::fabs(v - ref) < max / n * 0.5f;
        const float bt = isRef ? top - U(3) : top + U(3);
        uint32_t c = filled ? (enabled ? (hot ? kWhite : Rgba(0xe6e6e6)) : kQuiet) : (isRef ? kQuiet : kHair);
        Fill(bx, bt, bx + bw, bottom, c);
        if (playground > 0 && v >= playground)
            Fill(bx, bottom + U(3), bx + bw + (i + 1 < n ? gap : 0), bottom + U(3) + std::max(2.0f, U(2)),
                 enabled ? kCaution : kQuiet);
    }
    return {x0, top, x0 + span, bottom};
}

// Segmented options; the chosen one is inverted, the block slides between options.
// Returns the option clicked (or -1); `hover` gets the option under the cursor.
// An off switch's "off" (quietOff) is a #262626 block: the panel's white goes to what is on.
int Segments(int row, float x0, float y, float x1, float h, const std::vector<std::wstring> &opts, int chosen,
             uint32_t enabledMask, uint32_t playgroundMask, int *hover, float dt, bool quietOff = false)
{
    const int n = int(opts.size());
    const float w = (x1 - x0) / n;
    const float t = Hair();
    int clicked = -1;
    *hover = -1;
    // The block: eased toward the chosen option.
    float &a = panel.seg[row][0];
    float &b = panel.seg[row][1];
    const float ta = chosen >= 0 ? std::round(x0 + chosen * w) : x0, tb = chosen >= 0 ? std::round(x0 + (chosen + 1) * w) : x0;
    if (!panel.segInit[row] || !cfg.motion)
    {
        a = ta;
        b = tb;
        panel.segInit[row] = true;
    }
    const float k = 1.0f - std::exp(-dt / 0.045f);
    a += (ta - a) * k;
    b += (tb - b) * k;
    if (std::fabs(a - ta) < 0.5f) a = ta;
    if (std::fabs(b - tb) < 0.5f) b = tb;
    Frame(x0, y, x1, y + h, t, kHair);
    for (int i = 1; i < n; ++i)
    {
        const float sx = std::round(x0 + i * w);
        Fill(sx, y + t, sx + t, y + h - t, kHair);
    }
    const bool quiet = quietOff && chosen == 0;
    if (chosen >= 0 && b > a) Fill(a, y, b, y + h, quiet ? kHair : kWhite);
    for (int i = 0; i < n; ++i)
    {
        const float sx0 = std::round(x0 + i * w), sx1 = std::round(x0 + (i + 1) * w);
        const bool en = (enabledMask >> i) & 1;
        const bool over = in.mx >= sx0 && in.mx < sx1 && in.my >= y && in.my < y + h && !in.pad;
        if (over) *hover = i;
        // Ink: black where the block covers the label's centre, else white, grey or quiet.
        const float cx = (sx0 + sx1) * 0.5f;
        const bool inverted = chosen >= 0 && cx >= a && cx < b;
        uint32_t ink = inverted ? (quiet ? kWhite : kBlack) : !en ? kQuiet : over ? kWhite : kReadout;
        const float tw = Measure(opts[i], kBody);
        Text(std::round(cx - tw * 0.5f), y + std::round((h - LineHeight(kBody)) * 0.5f), opts[i], kBody, ink);
        if ((playgroundMask >> i) & 1)
            Fill(sx0 + U(6), y + h + U(3), sx1 - U(6), y + h + U(3) + std::max(2.0f, U(2)), en ? kCaution : kQuiet);
        if (over && in.pressed) clicked = i;
    }
    return clicked;
}

// The arrow cursor (the game usually hides the system one), drawn as triangles.
void Cursor(float x, float y)
{
    const float s = std::max(1.0f, ui * 0.9f);
    auto arrow = [&](float g, uint32_t c) {
        const float pts[7][2] = {{0, 0}, {0, 16}, {4, 12.5f}, {7, 19}, {9.5f, 18}, {6.5f, 11.5f}, {11.5f, 11.5f}};
        auto P = [&](int i, float *px, float *py) {
            const float cx = 3.5f, cy = 11.0f;  // grow around the arrow's middle for the outline
            *px = x + (cx + (pts[i][0] - cx) * g) * s;
            *py = y + (cy + (pts[i][1] - cy) * g) * s;
        };
        float ax, ay, bx, by, cx2, cy2;
        const int tris[5][3] = {{0, 1, 2}, {0, 2, 5}, {0, 5, 6}, {2, 3, 4}, {2, 4, 5}};
        for (auto &t : tris)
        {
            P(t[0], &ax, &ay);
            P(t[1], &bx, &by);
            P(t[2], &cx2, &cy2);
            Tri(ax, ay, bx, by, cx2, cy2, c);
        }
    };
    arrow(1.18f, kBlack);
    arrow(1.0f, kWhite);
}

const wchar_t *RowLabel(int r)
{
    static const wchar_t *labels[kRows] = {
        L"神经渲染", L"色彩", L"强度", L"模型风格", L"遍数", L"节奏", L"网络分辨率", L"角色守护", L"人脸遮罩", L"FPS 对齐", L"分屏对比", L"帧生成",
        L"P1 网络", L"P1 分辨率", L"P2 网络", L"P2 分辨率", L"P3 网络", L"P3 分辨率"};
    return labels[r];
}

std::wstring RowHelp(int r, const OvSet &s, int option)
{
    if (r == kP1Net || r == kP2Net || r == kP3Net)
        return L"这一遍跑哪个网络：Light-FP8 小而快，Full-FP8 大而精细。改动实时重建该遍，并写入 ini 下次启动沿用。";
    if (r == kP1Res || r == kP2Res || r == kP3Res)
        return L"这一遍网络的工作分辨率，30% 到 100% 独立可调，100% 是原生全分辨率直跑。每遍可以不同：比如第一遍 55% 出结构、第二遍 80% 补细节。改动实时生效并写入 ini。";
    switch (r)
    {
    case kOn:
        return L"关闭后画面就是游戏原画，副卡不再跑网络。改动实时生效。";
    case kLook: {
        static const wchar_t *looks[6] = {
            L"网络原样的色彩。",
            L"保留原画的颜色，只取网络的光影和质感；强改动被柔和压住，皮肤上更克制。AI 味最低，适合原画党。",
            L"颜色更鲜更干净：越灰的颜色加得越多，灰和白保持中性，皮肤只跟一小部分。适合二次元和卡通风格。",
            L"收住颜色，白色微冷，暗部微青，像美剧和电影的质感，皮肤保留自然血色。适合科技和现代题材。",
            L"深对比加浓郁色彩，暗部偏青、高光微暖。真正的重度渲染。",
            L"你自己的调色：Tab 或手柄 Y 进调色台，影调、曲线、色彩、三向色轮、色相、胶片六页 66 项参数，三个方案随时切换。"};
        return looks[std::clamp(option >= 0 ? option : s.look, 0, 5)];
    }
    case kIntensity:
        return L"网络改动的放大倍数，100% 是网络原样输出，200% 是 MGPU 原生上限，300% 是 Prism 扩展强度。只放大网络自身的改动，不放大原生画面的像素噪声。";
    case kModelStyle:
        return L"原版 MGPU 的 A／B／C 模型风格，分别对应 DLSSNR.Style 0／1／2。选择后实时生效，并保留到下次启动。";
    case kPasses:
        return L"网络在自己的结果上再跑几遍，效果更强，开销随遍数增加。三遍各自使用独立的网络、工作分辨率和融合历史。";
    case kCadence:
        return L"固定每帧运行网络，不持帧、不做异步 WARP。每一遍都消费当前帧输入，避免隔帧合成引入运动拖影和周期闪烁。";
    case kNetScale:
        return L"全局网络分辨率主控，30% 到 100%。调整后同步到当前所有遍；需要不同尺寸时，再使用下方的 P1／P2／P3 独立分辨率。";
    case kGuard:
        return L"角色守护强度，0% 关闭，100% 完整启用。沿用原有多遍之间的角色区域保护，不是 DLSSNR.ControlMask；拖动、滚轮或方向键可实时调节并保存。";
    case kFaceMask:
        return L"人脸遮罩默认 30%，独立调节 NVIDIA 自动人脸／皮肤区域的结构处理，与角色守护无关。遮罩内仍有神经画面，0% 也不关闭神经渲染；每一遍都实时生效并保存。";
    case kFpsCap:
        return L"限制主卡游戏生产帧率，给副卡留出处理时间，减少未读帧被覆盖。0 为关闭；这是主卡整体限帧，不是单独拖慢副卡。";
    case kSplit:
        return L"左边原画，右边神经画面。面板打开时可以拖动画面上的分割线。";
    case kFrameGen:
        return L"在最终神经画面之后运行副卡 DLSS 帧生成，使用真实运动向量、深度和相机输入。2x 每真实帧生成 1 帧；4x 每真实帧生成 3 帧（需驱动 DLSSG MultiFrameCount>=3，不支持时自动退回 2x）。切换后必须重启游戏才生效；默认关闭，Dynamic 不自动开启。";
    }
    return L"";
}

// Frame-time barcode: one bar per rendered frame, newest on the right; the height is the frame
// time, a caution bar is half again the median, a blocked bar two and a half times it.
void FrameBars(float x0, float y0, float x1, float y1, const OvStats &st, float dt, bool withScale)
{
    const float bw = std::max(2.0f, std::round(2.0f * ui)), gap = std::max(1.0f, std::round(1.0f * ui));
    const int n = std::min(st.frames, int((x1 - x0 + gap) / (bw + gap)));
    if (n <= 0) return;
    std::vector<float> v(st.ms + st.frames - n, st.ms + st.frames);
    std::vector<float> sorted = v;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float median = sorted[sorted.size() / 2];
    const float want = std::max(8.0f, std::ceil(median * 2.2f / 4.0f) * 4.0f);
    panel.graphTop += (want - panel.graphTop) * (1.0f - std::exp(-dt / 0.6f));
    const float top = panel.graphTop, h = y1 - y0;
    for (int i = 0; i < n; ++i)
    {
        const float ms = v[size_t(i)];
        const float bx = x1 - (n - i) * (bw + gap) + gap;
        const float bh = std::max(Hair(), std::round(std::min(1.0f, ms / top) * h));
        const uint32_t c = ms > median * 2.5f ? kBlocked : ms > median * 1.5f ? kCaution : i == n - 1 ? kWhite : kReadout;
        Fill(bx, y1 - bh, bx + bw, y1, c);
    }
    if (withScale)
    {
        Fill(x0, y0, x1, y0 + Hair(), kHairFaint);
        TextRight(x1, y0 - LineHeight(kLabel) - U(2), Fmt(L"%.0f ms", top), kLabel, kQuiet);
    }
}

// Sorted copy for percentiles.
float Percentile(const OvStats &st, float q)
{
    if (st.frames < 10) return 0;
    std::vector<float> v(st.ms, st.ms + st.frames);
    const size_t k = std::min(v.size() - 1, size_t(q * v.size()));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

void DrawPanel(UINT bw, UINT bh, float dt)
{
    const double now = Now();
    OvSet s;
    ReadSet(s);

    const float W = U(700), pad = U(20), rowH = U(38), labelW = U(80);
    // Height: header, telemetry, rows, help, footer. The per-pass rows follow the pass count.
    // 2026-11-14: the row count outgrew one column and the tail rows were buried at the
    // bottom of a screen-tall panel. Two columns: everything is visible at a glance.
    const float headH = U(52), telH = U(170), helpH = U(64), footH = U(40);
    // Twelve base rows (including DLSS FG) plus network/resolution per pass.
    // Character guard and NVIDIA face mask are separate strength sliders.
    const int visRows = 12 + 2 * std::clamp(s.passes, 1, 3);
    if (panel.focus >= visRows) panel.focus = visRows - 1;
    const int rowsLeft = (visRows + 1) / 2;   // rows in the first column; the rest go right
    const float colW = (W - pad * 2.0f) / 2.0f;
    const float H = headH + telH + rowH * float(rowsLeft) + U(12) + helpH + footH;
    panel.pw = W;
    panel.ph = H;
    float x0 = std::round(cfg.x * ui), y0 = std::round(cfg.y * ui);
    x0 = std::clamp(x0, 0.0f, std::max(0.0f, float(bw) - W));
    y0 = std::clamp(y0, 0.0f, std::max(0.0f, float(bh) - H));
    panel.px0 = x0;
    panel.py0 = y0;
    const float x1 = x0 + W, y1 = y0 + H;
    const float t = Hair();

    // Opening: a wipe from the left edge.
    const float open = cfg.motion ? EaseOutExpo(float(now - panel.openedAt) / 0.18f) : 1.0f;
    clip = {x0, y0, x0 + std::max(1.0f, W * open), y1};

    Fill(x0, y0, x1, y1, Rgba(0x000000, 0.93f));
    Frame(x0, y0, x1, y1, t, kHair);

    // Header: name, version, state.
    {
        const float cy = y0 + std::round((headH - LineHeight(kTitle)) * 0.5f);
        Text(x0 + pad, cy, L"MGPU Prism 2.4", kTitle, kWhite);
        const wchar_t *state;
        uint32_t mark;
        if (!s.armed)
        {
            state = L"等待画面";
            mark = kInfo;
        }
        else if (!s.enabled)
        {
            state = L"已关闭";
            mark = kCaution;
        }
        else
        {
            state = L"运行中";
            mark = kReady;
        }
        const float sw = TextRight(x1 - pad, y0 + std::round((headH - LineHeight(kBody)) * 0.5f), state, kBody, kReadout);
        const float mx = x1 - pad - sw - U(10);
        Fill(mx, y0 + std::round(headH * 0.5f - U(7)), mx + std::max(2.0f, U(2)), y0 + std::round(headH * 0.5f + U(7)), mark);
        Fill(x0, y0 + headH - t, x1, y0 + headH, kHair);
        // Dragging the header moves the panel.
        const bool overHead = in.mx >= x0 && in.mx < x1 && in.my >= y0 && in.my < y0 + headH;
        if (overHead && in.pressed)
        {
            panel.drag = Panel::kMove;
            panel.dragDX = in.mx - x0;
            panel.dragDY = in.my - y0;
        }
    }

    // Telemetry.
    float y = y0 + headH;
    {
        const float gx0 = x0 + pad, gx1 = x1 - pad;
        const float graphY0 = y + U(28), graphY1 = y + U(70);
        FrameBars(gx0, graphY0, gx1, graphY1, g_stats, dt, true);
        const float barStep = std::max(2.0f, std::round(2.0f * ui)) + std::max(1.0f, std::round(1.0f * ui));
        const int shown = std::min(g_stats.frames, int((gx1 - gx0) / barStep));
        const std::wstring graphLabel = shown > 0 ? L"最近 " + std::to_wstring(shown) + L" 次采样" : L"传输帧时间";
        Text(gx0, y + U(8), graphLabel, kLabel, kReadout);
        // The primary readout is compositor delivery; game production is separate below.
        const float colW = (gx1 - gx0) / 4.0f, ly = graphY1 + U(12), vy = ly + LineHeight(kLabel);
        const float p99 = Percentile(g_stats, 0.99f), med = Percentile(g_stats, 0.5f);
        const std::wstring labels[4] = {L"画面 FPS", L"传输帧时间", L"传输低帧", L"MGPU GPU"};
        const std::wstring values[4] = {g_stats.presentFps > 0 ? Fmt(L"%.1f", g_stats.presentFps) : L"—",
                                        med > 0 ? Fmt(L"%.1f ms", med) : L"—", p99 > 0 ? Fmt(L"%.0f", 1000.0 / p99) : L"—",
                                        g_stats.tsOk && g_stats.evalMs > 0 ? Fmt(L"%.1f ms", g_stats.evalMs) : L"—"};
        for (int i = 0; i < 4; ++i)
        {
            Text(gx0 + colW * i, ly, labels[i], kLabel, kReadout);
            Text(gx0 + colW * i, vy, values[i], kValue, kWhite);
        }
        // What runs at which size.
        std::wstring sizes;
        if (s.outW)
        {
            wchar_t b[128];
            std::swprintf(b, 128, L"输出 %u×%u", s.outW, s.outH);
            sizes = b;
            if (s.netW && (s.netW != s.outW || s.netH != s.outH))
            {
                std::swprintf(b, 128, L"　网络 %u×%u", s.netW, s.netH);
                sizes += b;
            }
        }
        else
        {
            sizes = L"等待游戏的 DLSS 画面";
        }
        const float sizeY = vy + LineHeight(kValue) + U(6);
        Text(gx0, sizeY, sizes, kLabel, kQuiet);
        wchar_t rateText[128];
        std::swprintf(rateText, 128, L"神经计算 %.1f fps　·　游戏生产 %.1f fps",
                      (double)g_stats.evalFps, (double)g_stats.renderFps);
        Text(gx0, sizeY + LineHeight(kLabel) + U(2), rateText, kLabel, kQuiet);
        y += telH;
        Fill(x0, y - t, x1, y, kHair);
    }

    // Rows, two columns.
    y += U(6);
    int hoverRow = -1;
    for (int r = 0; r < visRows; ++r)
    {
        const int c = r < rowsLeft ? 0 : 1, i = c ? r - rowsLeft : r;
        const float colX0 = x0 + pad + float(c) * colW, colX1 = colX0 + colW;
        const float ry = y + float(i) * rowH;
        if (in.mx >= colX0 && in.mx < colX1 && in.my >= ry && in.my < ry + rowH && !in.pad) hoverRow = r;
    }
    if (hoverRow >= 0 && in.mouseMoved && panel.drag == Panel::kNone)
    {
        panel.focus = hoverRow;
        panel.side = 0;
    }
    if (in.side) panel.side = s.look == 5 ? 1 - panel.side : 0;
    if (s.look != 5) panel.side = 0;
    const bool keysHere = panel.side == 0;
    // Keyboard and gamepad: up and down move the focus, left and right change the value.
    if (keysHere && in.up) panel.focus = (panel.focus + visRows - 1) % visRows;
    if (keysHere && in.dn) panel.focus = (panel.focus + 1) % visRows;
    const int step = !keysHere ? 0 : in.right ? 1 : in.left ? -1 : 0;
    panel.hoverOption = -1;
    OvSet n = s;
    bool changed = false;
    for (int r = 0; r < visRows; ++r)
    {
        const int c = r < rowsLeft ? 0 : 1, i = c ? r - rowsLeft : r;
        const float colX0 = x0 + pad + float(c) * colW, colX1 = colX0 + colW;
        const float cx0 = colX0 + labelW, cx1 = colX1 - pad;
        const float ry = y + float(i) * rowH;
        const bool focused = panel.focus == r && keysHere;
        const bool rowEnabled = true;
        const float ty = ry + std::round((rowH - LineHeight(kBody)) * 0.5f);
        if (focused) Fill(colX0 + U(2), ry + U(10), colX0 + U(2) + std::max(2.0f, U(2)), ry + rowH - U(10), kWhite);
        Text(colX0 + U(10), ty, RowLabel(r), kBody, !rowEnabled ? kQuiet : focused ? kWhite : kReadout);
        const float sh = U(26), sy = ry + std::round((rowH - sh) * 0.5f);
        int hover = -1, clicked = -1;
        const bool keyStep = focused && step != 0;
        switch (r)
        {
        case kOn:
        case kSplit: {
            bool v = r == kOn ? n.enabled : n.split;
            clicked = Segments(r, cx1 - U(120), sy, cx1, sh, {L"关", L"开"}, v ? 1 : 0, 3, 0, &hover, dt, true);
            bool nv = v;
            if (clicked >= 0) nv = clicked == 1;
            if (keyStep) nv = step > 0;
            if (focused && in.enter) nv = !v;
            if (nv != v)
            {
                if (r == kOn)
                {
                    n.enabled = nv;
                    changed = true;
                }
                else
                {
                    n.split = nv;
                    changed = true;
                }
            }
            break;
        }
        case kLook: {
            clicked = Segments(r, cx0, sy, cx1, sh, {L"默认", L"高还原", L"高饱和", L"低饱和", L"浓烈", L"自定义"},
                               std::clamp(n.look, 0, 5), 63, 0, &hover, dt);
            int want = clicked;
            if (keyStep) want = std::clamp(n.look + step, 0, 5);
            if (want >= 0 && want != n.look)
            {
                n.look = want;
                changed = true;
            }
            if (focused && in.enter && n.look == 5) panel.side = 1;  // into the grading desk
            if (focused) panel.hoverOption = hover;
            break;
        }
        case kIntensity:
        case kGuard:
        case kFaceMask: {
            const bool isI = r == kIntensity;
            float *const value = isI ? &n.intensity : (r == kGuard ? &n.guard : &n.faceMask);
            const float max = isI ? 300.0f : 100.0f, val = *value * 100.0f;
            const float vw = U(52);
            const Bars b = BarSlider(cx0, sy + U(5), cx1 - vw - U(8), sh - U(14), val, max, 100.0f, isI ? 150.0f : 0.0f,
                                     true, focused);
            TextRight(cx1, ty, Fmt(L"%.0f%%", val), kValue, focused ? kWhite : kReadout);
            float nv = val;
            const bool over = in.mx >= b.x0 - U(4) && in.mx < b.x1 + U(4) && in.my >= ry && in.my < ry + rowH && !in.pad;
            if (over && in.pressed)
            {
                panel.drag = Panel::kSlide;
                panel.dragRow = r;
            }
            if (panel.drag == Panel::kSlide && panel.dragRow == r && in.down)
                nv = std::round(std::clamp((in.mx - b.x0) / (b.x1 - b.x0), 0.0f, 1.0f) * max / 5.0f) * 5.0f;
            if (over && in.wheelSteps) nv = std::clamp(nv + in.wheelSteps * 5.0f, 0.0f, max);
            if (keyStep) nv = std::clamp(std::round((nv + step * 10.0f) / 10.0f) * 10.0f, 0.0f, max);
            if (std::fabs(nv - val) > 0.01f)
            {
                *value = nv / 100.0f;
                changed = true;
            }
            break;
        }
        case kModelStyle: {
            const int cur = std::clamp(n.modelStyle, 0, 2);
            clicked = Segments(r, cx1 - U(180), sy, cx1, sh, {L"A", L"B", L"C"}, cur, 7, 0, &hover, dt);
            int want = clicked;
            if (keyStep) want = std::clamp(cur + step, 0, 2);
            if (focused && in.enter) want = (cur + 1) % 3;
            if (want >= 0 && want != cur)
            {
                n.modelStyle = want;
                changed = true;
            }
            if (focused) panel.hoverOption = hover;
            break;
        }
        case kPasses: {
            const int cur = std::clamp(n.passes - 1, 0, 2);
            clicked = Segments(r, cx0, sy, cx1, sh, {L"1", L"2", L"3"}, cur, 7, 0, &hover, dt);
            int want = clicked;
            if (keyStep) want = std::clamp(cur + step, 0, 2);
            if (want >= 0 && want != cur)
            {
                n.passes = want + 1;
                changed = true;
            }
            break;
        }
        case kP1Net:
        case kP2Net:
        case kP3Net: {
            // 0.5.1 per-pass network: Light-FP8 (small, FPS+) vs Full-FP8 (big). Live rebuild
            // through ui_set_network, ini mirror for the next launch (pass 1 at x2+ -> NRPreset1x2).
            const unsigned pi = unsigned(r - kP1Net) / 2u;
            static const char *const nrKey[3] = {"NRPreset1", "NRPreset2", "NRPreset3"};
            mgpu::gpu1::ui_state stn;
            mgpu::gpu1::ui_read(stn);
            const bool live = stn.armed && stn.nr_ok;
            const char *key_now = (pi == 0u && stn.passes >= 2u) ? "NRPreset1x2" : nrKey[pi];
            const int cur = live ? (stn.nr_preset_pass[pi] == 1 ? 1 : 0)
                                 : (mgpu::gpu1::ui_ini_read(nrKey[pi], mgpu::gpu1::ui_ini_read("NRPreset", 0)) == 1 ? 1 : 0);
            clicked = Segments(r, cx0, sy, cx1, sh, {L"Light", L"Full"}, cur, 3, 0, &hover, dt);
            int want = clicked;
            if (focused && (in.enter || keyStep)) want = 1 - cur;
            if (want >= 0 && want != cur)
            {
                (void)mgpu::gpu1::ui_ini_write(key_now, want);
                if (live) mgpu::gpu1::ui_set_network(pi, want);
            }
            if (focused) panel.hoverOption = hover;
            break;
        }
        case kP1Res:
        case kP2Res:
        case kP3Res: {
            // R370: per-pass resolution slider, 30..100% (100% = a native full pass on
            // this pass). Writes NRScale1/2/3 to the ini and applies live through
            // ui_set_scale - no recreate, a mask flip + Reset on that pass's next evaluate.
            const unsigned pi = unsigned(r - kP1Res) / 2u;
            mgpu::gpu1::ui_state stn;
            mgpu::gpu1::ui_read(stn);
            const bool live = stn.armed && stn.nr_ok;
            float cur;
            if (live)
                cur = stn.nrs_pscale[pi] > 0.05f ? stn.nrs_pscale[pi] : stn.nrs_scale;
            else
            {
                char pk[16];
                snprintf(pk, sizeof pk, "NRScale%u", pi + 1u);
                cur = mgpu::gpu1::ui_ini_read_float(pk, -1.0f);
                if (cur < 0.05f)
                    cur = (pi == 0u) ? mgpu::gpu1::ui_ini_read_float("NRScale", 1.0f) : 1.0f;
                cur = std::clamp(cur, 0.30f, 1.00f);
            }
            // A drag only previews here. Apply and persist once on release: applying
            // during the drag resets DLSS-NR history repeatedly and leaves old/new
            // viewpoints blended together. Wheel and keyboard edits apply at once.
            static float pendingScale[3] = {-1.0f, -1.0f, -1.0f};
            const float shown = pendingScale[pi] > 0.0f ? pendingScale[pi] : cur;
            const float max = 100.0f;
            const float val = (shown - 0.30f) / 0.70f * max;
            const float vw = U(52);
            const Bars b =
                BarSlider(cx0, sy + U(5), cx1 - vw - U(8), sh - U(14), val, max, 0.0f, 0.0f, true, focused);
            TextRight(cx1, ty, shown >= 0.999f ? L"原生" : Fmt(L"%.0f%%", shown * 100.0), kValue,
                      focused ? kWhite : kReadout);
            float nv = val;
            const bool over = in.mx >= b.x0 - U(4) && in.mx < b.x1 + U(4) && in.my >= ry && in.my < ry + rowH && !in.pad;
            if (over && in.pressed)
            {
                panel.drag = Panel::kSlide;
                panel.dragRow = r;
            }
            if (panel.drag == Panel::kSlide && panel.dragRow == r && in.down)
                nv = std::round(std::clamp((in.mx - b.x0) / (b.x1 - b.x0), 0.0f, 1.0f) * max / 5.0f) * 5.0f;
            if (over && in.wheelSteps) nv = std::clamp(nv + in.wheelSteps * 5.0f, 0.0f, max);
            if (keyStep) nv = std::clamp(std::round((nv + step * 5.0f) / 5.0f) * 5.0f, 0.0f, max);
            const float scale = 0.30f + nv / max * 0.70f;
            if (std::fabs(scale - shown) > 1e-4f) pendingScale[pi] = scale;
            const bool dragging = panel.drag == Panel::kSlide && panel.dragRow == r && in.down;
            if (!dragging && pendingScale[pi] > 0.0f)
            {
                const float apply = pendingScale[pi];
                pendingScale[pi] = -1.0f;
                char pk[16];
                snprintf(pk, sizeof pk, "NRScale%u", pi + 1u);
                (void)mgpu::gpu1::ui_ini_write_float(pk, apply);
                if (live) mgpu::gpu1::ui_set_scale(pi, apply);
            }
            break;
        }
        case kNetScale: {
            const float max = 100.0f;
            const float val = n.nrsOn ? (n.nrs - 0.30f) / 0.70f * max : 0.0f;
            const float ref = (0.75f - 0.30f) / 0.70f * max;
            const float vw = U(52);
            const Bars b = BarSlider(cx0, sy + U(5), cx1 - vw - U(8), sh - U(14),
                                     n.nrsOn ? val : 0.0f, max, ref, 0.0f, n.nrsOn, focused);
            TextRight(cx1, ty, n.nrsOn ? (n.nrs >= 0.999f ? L"原生" : Fmt(L"%.0f%%", n.nrs * 100.0)) : L"—",
                      kValue, focused ? kWhite : n.nrsOn ? kReadout : kQuiet);
            float nv = val;
            const bool over = n.nrsOn && in.mx >= b.x0 - U(4) && in.mx < b.x1 + U(4) &&
                              in.my >= ry && in.my < ry + rowH && !in.pad;
            if (over && in.pressed)
            {
                panel.drag = Panel::kSlide;
                panel.dragRow = r;
            }
            if (panel.drag == Panel::kSlide && panel.dragRow == r && in.down)
                nv = std::round(std::clamp((in.mx - b.x0) / (b.x1 - b.x0), 0.0f, 1.0f) * max / 5.0f) * 5.0f;
            if (over && in.wheelSteps) nv = std::clamp(nv + in.wheelSteps * 5.0f, 0.0f, max);
            if (keyStep && n.nrsOn) nv = std::clamp(std::round((nv + step * 5.0f) / 5.0f) * 5.0f, 0.0f, max);
            const float scale = 0.30f + nv / max * 0.70f;
            if (n.nrsOn && std::fabs(scale - n.nrs) > 1e-4f)
            {
                n.nrs = scale;
                changed = true;
            }
            break;
        }
        case kFpsCap: {
            const float vw = U(52);
            const Bars b = BarSlider(cx0, sy + U(5), cx1 - vw - U(8), sh - U(14),
                                     float(n.fpsCap), 240.0f, 60.0f, 0.0f, true, focused);
            TextRight(cx1, ty, n.fpsCap ? Fmt(L"%.0f fps", double(n.fpsCap)) : L"关",
                      kValue, focused ? kWhite : kReadout);
            int nv = n.fpsCap;
            const bool over = in.mx >= b.x0 - U(4) && in.mx < b.x1 + U(4) &&
                              in.my >= ry && in.my < ry + rowH && !in.pad;
            if (over && in.pressed)
            {
                panel.drag = Panel::kSlide;
                panel.dragRow = r;
            }
            if (panel.drag == Panel::kSlide && panel.dragRow == r && in.down)
                nv = int(std::round(std::clamp((in.mx - b.x0) / (b.x1 - b.x0), 0.0f, 1.0f) * 240.0f / 5.0f) * 5.0f);
            if (over && in.wheelSteps) nv = std::clamp(nv + in.wheelSteps * 5, 0, 240);
            if (keyStep) nv = std::clamp(nv + step * 10, 0, 240);
            if (nv > 0 && nv < 20) nv = nv >= 10 ? 20 : 0;
            if (nv != n.fpsCap)
            {
                n.fpsCap = nv;
                changed = true;
            }
            break;
        }
        case kFrameGen: {
            const int cur = mgpu::gpu1::ui_ini_read("FrameGen", 0) == 4
                ? (mgpu::gpu1::ui_ini_read("FGX", 2) == 4 ? 2 : 1) : 0;
            clicked = Segments(r, cx1 - U(180), sy, cx1, sh, {L"关", L"2x", L"4x"}, cur, 3, 0, &hover, dt, true);
            int want = clicked;
            if (keyStep) want = step > 0 ? (cur + 1) % 3 : (cur + 2) % 3;
            if (focused && in.enter) want = (cur + 1) % 3;
            if (want >= 0 && want != cur)
            {
                if (want == 1 || want == 2)
                {
                    // Official 0.6.0 DLSS-FG defaults. Dynamic stays as the user
                    // configured it; this switch never enables it implicitly.
                    (void)mgpu::gpu1::ui_ini_write("FGMulti", 2);
                    if (want == 1) (void)mgpu::gpu1::ui_ini_write("FGPhase", 75);
                    (void)mgpu::gpu1::ui_ini_write("FGMvScaleNow", 1);
                    (void)mgpu::gpu1::ui_ini_write("FGMvUnit", 1);
                    (void)mgpu::gpu1::ui_ini_write("FGSkipCarry", 1);
                    (void)mgpu::gpu1::ui_ini_write("FGPace", 1);
                    (void)mgpu::gpu1::ui_ini_write("FGSkipJump", 0);
                    (void)mgpu::gpu1::ui_ini_write("FGX", want == 2 ? 4 : 2);
                    (void)mgpu::gpu1::ui_ini_write("FrameGen", 4);
                    Toast(want == 2 ? L"帧生成 4x 已保存，重启游戏后生效" : L"帧生成 2x 已保存，重启游戏后生效", kCaution, 3.0);
                }
                else
                {
                    (void)mgpu::gpu1::ui_ini_write("FrameGen", 0);
                    Toast(L"帧生成已关闭，重启游戏后生效", kCaution, 3.0);
                }
            }
            if (focused) panel.hoverOption = hover;
            break;
        }
        case kCadence: {
            if (n.cadence != 1)
            {
                n.cadence = 1;
                changed = true;
            }
            clicked = Segments(r, cx1 - U(120), sy, cx1, sh, {L"每帧"}, 0, 1, 0, &hover, dt);
            break;
        }
        }
        const int rowsInCol = c ? visRows - rowsLeft : rowsLeft;
        if (i + 1 < rowsInCol) Fill(colX0 + U(10), ry + rowH - t, colX1 - pad, ry + rowH, kHairFaint);
    }
    if (panel.drag == Panel::kSlide && !in.down) panel.drag = Panel::kNone;
    if (changed)
    {
        ApplySet(n);
        panel.changedAt = now;
    }
    y += rowH * float(rowsLeft) + U(6);
    Fill(x0, y - t, x1, y, kHair);

    // Help for the focused row.
    {
        float hy = y + U(10);
        for (const std::wstring &line : Wrap(RowHelp(panel.focus, n, keysHere ? panel.hoverOption : -1), kSmall, W - pad * 2))
        {
            Text(x0 + pad, hy, line, kSmall, kReadout);
            hy += LineHeight(kSmall);
        }
        y += helpH;
        Fill(x0, y - t, x1, y, kHair);
    }

    // Footer: the keys, and the save state.
    {
        const float fy = y + std::round((footH - LineHeight(kLabel)) * 0.5f);
        std::wstring keys;
        if (in.pad)
            keys = n.look == 5 ? L"View + Y 收起　十字键 选择和调整　Y 调色台　B 收起"
                               : L"View + Y 收起　十字键 选择和调整　A 切换　B 收起";
        else
            keys = std::wstring(KeyName(cfg.key)) +
                   (n.look == 5 ? L" 收起　↑↓ 选择　←→ 调整　Tab 调色台" : L" 收起　↑↓ 选择　←→ 调整　拖动标题栏移动");
        Text(x0 + pad, fy, keys, kLabel, kQuiet);
        const float since = float(now - panel_savedAt);
        if (since < 2.0f) TextRight(x1 - pad, fy, L"已保存", kLabel, Fade(kReadout, since < 1.4f ? 1.0f : (2.0f - since) / 0.6f));
    }
    clip = {-1e9f, -1e9f, 1e9f, 1e9f};

    // Moving the panel.
    if (panel.drag == Panel::kMove)
    {
        if (in.down)
        {
            cfg.x = std::max(0.0f, (in.mx - panel.dragDX) / ui);
            cfg.y = std::max(0.0f, (in.my - panel.dragDY) / ui);
        }
        else
        {
            panel.drag = Panel::kNone;
            SavePanelValue(L"OverlayX", Fmt(L"%.0f", cfg.x));
            SavePanelValue(L"OverlayY", Fmt(L"%.0f", cfg.y));
        }
    }
}

// ------------------------------------------------------------------ the grading desk (Look=5, grade.h)
// Beside the panel while the look is 自定义: the tone curve and a hue ring at the top, six pages
// of controls, three slots, and a preset to start from. The hue ring is the one place colour fills
// more than a stroke: there the hues are the data.
constexpr float kPi = 3.14159265f;
constexpr float kHueCentres[6] = {29, 110, 142, 195, 264, 328};  // look_hlsl.h's six hues, in Oklab degrees

// A hue's colour: Oklab at a lightness and chroma, to sRGB.
uint32_t HueInk(float deg, float L = 0.75f, float C = 0.13f, float alpha = 1.0f)
{
    const float h = deg * kPi / 180.0f, a = C * std::cos(h), b = C * std::sin(h);
    float l = L + 0.3963377774f * a + 0.2158037573f * b, m = L - 0.1055613458f * a - 0.0638541728f * b,
          s = L - 0.0894841775f * a - 1.2914855480f * b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    const float rgb[3] = {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                          -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                          -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
    uint32_t out = 0;
    for (float c : rgb)
    {
        c = std::clamp(c, 0.0f, 1.0f);
        const float e = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        out = (out << 8) | uint32_t(e * 255.0f + 0.5f);
    }
    return Rgba(out, alpha);
}
uint32_t ChannelInk(int c) { return c == 1 ? HueInk(29, 0.68f, 0.2f) : c == 2 ? HueInk(142, 0.75f, 0.2f) : c == 3 ? HueInk(264, 0.62f, 0.2f) : kWhite; }

// A by-hue control at a hue (degrees): the shader's raised-cosine blend of the six.
float ByHue(const prism::GradeValues &g, int base, float deg)
{
    float sum = 0, wsum = 0;
    for (int i = 0; i < 6; ++i)
    {
        float d = std::fabs(deg - kHueCentres[i]);
        d = std::fmod(d, 360.0f);
        d = std::min(d, 360.0f - d);
        const float w = d < 61.9f ? 0.5f + 0.5f * std::cos(kPi * d / 61.9f) : 0.0f;
        sum += w * g.v[base + i];
        wsum += w;
    }
    return wsum > 1e-3f ? sum / wsum : 0.0f;
}

std::wstring GradeText(int i, float v)
{
    switch (prism::GradeInfo(i).unit)
    {
    case prism::kUStops:
        return std::fabs(v) < 0.005f ? L"0.00" : Fmt(L"%+.2f", v);
    case prism::kUHue:
        return Fmt(L"%.0f°", v);
    case prism::kUTurn:
        return std::fabs(v) < 0.5f ? L"0°" : Fmt(L"%+.0f°", v);
    case prism::kUSigned:
        return std::fabs(v) < 0.5f ? L"0" : Fmt(L"%+.0f", v);
    default:
        return Fmt(L"%.0f", v);
    }
}
// Steps for the keys and the wheel (Shift: the finest).
float GradeStep(int i, bool fine)
{
    const prism::GradeSpec &p = prism::GradeInfo(i);
    if (p.unit == prism::kUStops) return fine ? 0.01f : 0.1f;
    if (p.unit == prism::kUHue) return fine ? 1.0f : 5.0f;
    if (p.unit == prism::kUTurn) return 1.0f;
    return fine ? 1.0f : 5.0f;
}
float GradeNudge(int i, float v, float by)
{
    const prism::GradeSpec &p = prism::GradeInfo(i);
    if (p.unit == prism::kUHue)
    {
        v = std::fmod(v + by + 360.0f, 360.0f);
        return std::round(v);
    }
    const float st = p.unit == prism::kUStops ? 0.01f : 1.0f;
    return std::clamp(std::round((v + by) / st) * st, p.min, p.max);
}

// A control's bar run: centred controls fill from neutral, the rest from the left; a hue control is
// a strip of its hues with the value as a tall white bar.
Bars GradeSlider(float x0, float y, float x1, float h, int param, float value, bool hot)
{
    const prism::GradeSpec &p = prism::GradeInfo(param);
    const float bw = std::max(2.0f, std::round(2.0f * ui)), gap = std::max(1.0f, std::round(2.0f * ui));
    const int n = int((x1 - x0 + gap) / (bw + gap));
    const float span = n * (bw + gap) - gap;
    const float tv = (value - p.min) / (p.max - p.min), c0 = p.min < 0 ? -p.min / (p.max - p.min) : 0.0f;
    const float lo = std::min(tv, c0), hi = std::max(tv, c0), half = 0.5f / n;
    for (int i = 0; i < n; ++i)
    {
        const float f = (i + 0.5f) / n, bx = x0 + i * (bw + gap);
        const bool atValue = std::fabs(f - tv) <= half;
        if (p.unit == prism::kUHue)
        {
            if (atValue)
                Fill(bx - gap * 0.5f, y - U(3), bx + bw + gap * 0.5f, y + h + U(3), hot ? kWhite : Rgba(0xe6e6e6));
            else
                Fill(bx, y + U(3), bx + bw, y + h, HueInk(p.min + f * (p.max - p.min), 0.72f, 0.12f, hot ? 1.0f : 0.55f));
            continue;
        }
        const bool isRef = p.min < 0 && std::fabs(f - c0) <= half;
        const bool filled = hi - lo > 1e-4f && f >= lo - half * 0.5f && f <= hi + half * 0.5f;
        const uint32_t c = filled ? (hot ? kWhite : Rgba(0xe6e6e6)) : isRef ? kQuiet : kHair;
        Fill(bx, isRef ? y - U(3) : y + U(3), bx + bw, y + h, c);
    }
    return {x0, y, x0 + span, y + h};
}

void DrawGrade(UINT bw, UINT bh, float dt)
{
    const double now = Now();
    OvSet s;
    ReadSet(s);
    OvSet n = s;
    bool changed = false;
    const int slot = std::clamp(n.gradeSlot, 0, prism::kGradeSlots - 1);

    const float W = U(440), pad = U(20), rowH = U(34), labelW = U(92);
    const float headH = U(52), scopeH = U(180), tabsH = U(46), rowsH = rowH * 8.0f + U(6), presetH = U(46),
                helpH = U(64), footH = U(36);
    const float H = headH + scopeH + tabsH + rowsH + presetH + helpH + footH;
    // Beside the panel: on its right if there is room, else on its left, else over it.
    float x0 = panel.px0 + panel.pw + U(12);
    if (x0 + W > float(bw)) x0 = panel.px0 - U(12) - W;
    if (x0 < 0) x0 = std::clamp(panel.px0, 0.0f, std::max(0.0f, float(bw) - W));
    const float y0 = std::clamp(panel.py0, 0.0f, std::max(0.0f, float(bh) - H));
    const float x1 = x0 + W, y1 = y0 + H, t = Hair();
    const float open = cfg.motion ? EaseOutExpo(float(now - panel.openedAt) / 0.18f) : 1.0f;
    clip = {x0, y0, x0 + std::max(1.0f, W * open), y1};
    Fill(x0, y0, x1, y1, Rgba(0x000000, 0.93f));
    Frame(x0, y0, x1, y1, t, kHair);

    const bool over = in.mx >= x0 && in.mx < x1 && in.my >= y0 && in.my < y1 && !in.pad;
    if (over && in.mouseMoved && panel.drag == Panel::kNone) panel.side = 1;
    const bool keys = panel.side == 1;

    // The page's rows: a control, or -1 the curve's channel, -2 the hue page's control.
    static const wchar_t *pageNames[6] = {L"影调", L"曲线", L"色彩", L"色轮", L"色相", L"胶片"};
    if (keys && in.page) panel.gPage = (panel.gPage + in.page + 6) % 6;
    std::vector<int> rows;
    switch (panel.gPage)
    {
    case 0:
        for (int i = prism::kGExposure; i <= prism::kGMidtones; ++i) rows.push_back(i);
        break;
    case 1:
        rows.push_back(-1);
        for (int i = 0; i < prism::kCurvePoints; ++i) rows.push_back(prism::kGCurve + panel.gChannel * prism::kCurvePoints + i);
        break;
    case 2:
        for (int i = prism::kGTemp; i <= prism::kGMix; ++i) rows.push_back(i);
        break;
    case 3:
        for (int i = prism::kGShadowHue; i <= prism::kGHiAmt; ++i) rows.push_back(i);
        break;
    case 4:
        rows.push_back(-2);
        for (int i = 0; i < 6; ++i)
            rows.push_back((panel.gAttr == 0 ? prism::kGHueShift : panel.gAttr == 1 ? prism::kGHueSat : prism::kGHueLight) + i);
        break;
    default:
        for (int i = prism::kGDensity; i <= prism::kGGrain; ++i) rows.push_back(i);
        break;
    }
    const int items = 2 + int(rows.size()) + 1, presetItem = items - 1;
    panel.gFocus = std::clamp(panel.gFocus, 0, items - 1);
    if (keys && in.up) panel.gFocus = (panel.gFocus + items - 1) % items;
    if (keys && in.dn) panel.gFocus = (panel.gFocus + 1) % items;
    const int step = keys ? (in.right ? 1 : in.left ? -1 : 0) : 0;
    auto focusMark = [&](float ry, float h) { Fill(x0 + U(8), ry + U(10), x0 + U(8) + std::max(2.0f, U(2)), ry + h - U(10), kWhite); };
    int hover = -1;

    // Header: the name, and the three slots.
    {
        const float cy = y0 + std::round((headH - LineHeight(kTitle)) * 0.5f);
        const float nw = Text(x0 + pad, cy, L"调色台", kTitle, kWhite);
        Text(x0 + pad + nw + U(8), cy + U(2), L"自定义", kLabel, kQuiet);
        if (keys && panel.gFocus == 0) focusMark(y0, headH);
        const float sh = U(26), sy = y0 + std::round((headH - sh) * 0.5f);
        int want = Segments(kRows + 0, x1 - pad - U(186), sy, x1 - pad, sh, {L"方案 1", L"方案 2", L"方案 3"}, slot, 7, 0, &hover, dt);
        if (keys && panel.gFocus == 0 && step) want = std::clamp(slot + step, 0, prism::kGradeSlots - 1);
        if (want >= 0 && want != slot)
        {
            n.gradeSlot = want;
            changed = true;
        }
        Fill(x0, y0 + headH - t, x1, y0 + headH, kHair);
    }
    prism::GradeValues &cur = n.grade[std::clamp(n.gradeSlot, 0, prism::kGradeSlots - 1)];

    // Scopes: the tone curve as a bar run, and the hue ring.
    float y = y0 + headH;
    {
        const float ax0 = x0 + pad, ax1 = x0 + pad + U(206), ay0 = y + U(30), ay1 = y + scopeH - U(16);
        Text(ax0, y + U(10), L"影调曲线", kLabel, kReadout);
        TextRight(ax1, y + U(10), L"−8 → +6 档", kLabel, kQuiet);
        auto mapX = [&](float l) { return ax0 + (l + 8.0f) / 14.0f * (ax1 - ax0); };
        auto mapY = [&](float o) { return ay1 - std::clamp((o + 8.0f) / 14.0f, 0.0f, 1.0f) * (ay1 - ay0); };
        Fill(ax0, mapY(0), ax1, mapY(0) + t, kHairFaint);
        Fill(mapX(0), ay0, mapX(0) + t, ay1, kHairFaint);
        Fill(ax0, ay1, ax1, ay1 + t, kHair);
        float k[prism::kGradeConstants];
        prism::GradeToShader(cur, k);
        bool channelOn[4] = {true, false, false, false};
        for (int c = 1; c < 4; ++c)
            for (int i = 0; i < prism::kCurvePoints; ++i) channelOn[c] |= cur.v[prism::kGCurve + c * prism::kCurvePoints + i] != 0.0f;
        const float bwid = std::max(2.0f, std::round(2.0f * ui)), gap = std::max(1.0f, std::round(1.0f * ui)),
                    cap = std::max(2.0f, U(2));
        const int nb = int((ax1 - ax0 + gap) / (bwid + gap));
        for (int i = 0; i < nb; ++i)
        {
            const float bx = ax0 + i * (bwid + gap), l = -8.0f + (i + 0.5f) / nb * 14.0f;
            const float o = prism::GradeTone(cur, l), yy = mapY(o);
            Fill(bx, yy, bx + bwid, ay1, Rgba(0x262626));
            if (i % 2 == 0) Fill(bx, mapY(l), bx + bwid, mapY(l) + t, kQuiet);  // no change
            for (int c = 1; c < 4; ++c)
                if (channelOn[c])
                {
                    const float yc = mapY(o + prism::GradeCurveAt(k + 48 + c * prism::kCurvePoints, o));
                    Fill(bx, yc, bx + bwid, yc + cap, ChannelInk(c));
                }
            Fill(bx, yy, bx + bwid, yy + cap, kWhite);
        }
        // The curve's points on its page: drag them up and down.
        if (panel.gPage == 1)
        {
            const int c = panel.gChannel;
            const float pxPerStop = (ay1 - ay0) / 14.0f;
            for (int i = 0; i < prism::kCurvePoints; ++i)
            {
                const float l = prism::kCurveStops[i], o = prism::GradeTone(cur, l);
                const float px = mapX(l), py = mapY(c == 0 ? o : o + prism::GradeCurveAt(k + 48 + c * prism::kCurvePoints, o));
                const bool focused = keys && panel.gFocus == 3 + i, dragging = panel.gDragPoint == i;
                const float r = U(focused || dragging ? 6 : 4);
                Fill(px - r - t, py - r - t, px + r + t, py + r + t, kBlack);
                if (focused || dragging)
                    Fill(px - r, py - r, px + r, py + r, ChannelInk(c));
                else
                    Frame(px - r, py - r, px + r, py + r, std::max(1.0f, U(1.5f)), ChannelInk(c));
                if (in.pressed && !in.pad && std::fabs(in.mx - px) < U(10) && std::fabs(in.my - py) < U(10))
                {
                    panel.gDragPoint = i;
                    panel.gDragY = in.my;
                    panel.gDragFrom = cur.v[prism::kGCurve + c * prism::kCurvePoints + i];
                    panel.gFocus = 3 + i;
                    panel.side = 1;
                }
            }
            if (panel.gDragPoint >= 0)
            {
                if (in.down)
                {
                    const int pi = prism::kGCurve + c * prism::kCurvePoints + panel.gDragPoint;
                    const float nv = GradeNudge(pi, panel.gDragFrom, -(in.my - panel.gDragY) / pxPerStop / 1.5f * 100.0f);
                    if (nv != cur.v[pi])
                    {
                        cur.v[pi] = nv;
                        changed = true;
                    }
                }
                else
                    panel.gDragPoint = -1;
            }
        }

        // The hue ring: a spoke per hue, as long as that hue's saturation gain (the dotted circle is no
        // change), turned by its hue shift; around it the hues themselves; the three wheels as markers.
        const float rx = std::round((ax1 + x1 - pad) * 0.5f + U(6)), ry = std::round(y + scopeH * 0.5f + U(6));
        const float R = std::min((x1 - pad - ax1) * 0.5f - U(16), scopeH * 0.5f - U(26)), r0 = U(8);
        Text(ax1 + U(26), y + U(10), L"色相环", kLabel, kReadout);
        auto at = [&](float deg, float rad, float *px, float *py) {
            *px = rx + std::cos(deg * kPi / 180.0f) * rad;
            *py = ry - std::sin(deg * kPi / 180.0f) * rad;
        };
        for (int j = 0; j < 72; ++j)
        {
            float ax, ay, bx2, by2, cx2, cy2, dx2, dy2;
            at(j * 5.0f, R + U(5), &ax, &ay);
            at((j + 1) * 5.0f, R + U(5), &bx2, &by2);
            at((j + 1) * 5.0f, R + U(8), &cx2, &cy2);
            at(j * 5.0f, R + U(8), &dx2, &dy2);
            const uint32_t c = HueInk(j * 5.0f + 2.5f, 0.72f, 0.13f);
            Tri(ax, ay, bx2, by2, cx2, cy2, c);
            Tri(ax, ay, cx2, cy2, dx2, dy2, c);
        }
        for (int j = 0; j < 36; ++j)
        {
            float px, py;
            at(j * 10.0f, r0 + (R - r0) * 0.5f, &px, &py);
            Fill(px - t, py - t, px + t, py + t, kQuiet);
        }
        const float sat = 1.0f + cur.v[prism::kGSat] / 100.0f;
        for (int j = 0; j < 72; ++j)
        {
            const float h = j * 5.0f + 2.5f;
            const float gain = std::max(0.0f, sat * (1.0f + ByHue(cur, prism::kGHueSat, h) / 100.0f));
            const float turn = ByHue(cur, prism::kGHueShift, h), light = ByHue(cur, prism::kGHueLight, h) / 100.0f;
            const float len = (R - r0) * std::clamp(gain / 2.0f, 0.0f, 1.0f);
            float ax, ay, bx2, by2;
            at(h + turn, r0, &ax, &ay);
            at(h + turn, r0 + len, &bx2, &by2);
            LineSeg(ax, ay, bx2, by2, std::max(2.0f, U(2)), HueInk(h + turn, std::clamp(0.74f * (1.0f + 0.3f * light), 0.3f, 0.95f), 0.14f));
        }
        // The wheels: shadows a hollow square, midtones a cross, highlights a filled square.
        const int hueOf[3] = {prism::kGShadowHue, prism::kGMidHue, prism::kGHiHue},
                  amtOf[3] = {prism::kGShadowAmt, prism::kGMidAmt, prism::kGHiAmt};
        int focusedWheel = -1;
        if (panel.gPage == 3 && panel.gFocus >= 2 && panel.gFocus < 2 + int(rows.size()))
            focusedWheel = (panel.gFocus - 2) / 2;
        for (int w = 0; w < 3; ++w)
        {
            float px, py;
            at(cur.v[hueOf[w]], r0 + (R - r0) * cur.v[amtOf[w]] / 100.0f, &px, &py);
            const bool hot = (keys && focusedWheel == w) || panel.gDragWheel == w;
            const float r = U(hot ? 6 : 4.5f), lw = std::max(2.0f, U(2));
            Fill(px - r - t, py - r - t, px + r + t, py + r + t, Rgba(0x000000, w == 1 ? 0.0f : 1.0f));
            if (w == 0)
                Frame(px - r, py - r, px + r, py + r, lw, kWhite);
            else if (w == 2)
                Fill(px - r, py - r, px + r, py + r, kWhite);
            else
            {
                Fill(px - r - t, py - lw * 0.5f - t, px + r + t, py + lw * 0.5f + t, kBlack);
                Fill(px - lw * 0.5f - t, py - r - t, px + lw * 0.5f + t, py + r + t, kBlack);
                Fill(px - r, py - lw * 0.5f, px + r, py + lw * 0.5f, kWhite);
                Fill(px - lw * 0.5f, py - r, px + lw * 0.5f, py + r, kWhite);
            }
            if (in.pressed && !in.pad && std::fabs(in.mx - px) < U(10) && std::fabs(in.my - py) < U(10)) panel.gDragWheel = w;
        }
        // On the wheels' page a press anywhere in the ring moves the focused wheel there.
        const float dxm = in.mx - rx, dym = in.my - ry;
        if (in.pressed && !in.pad && panel.gDragWheel < 0 && panel.gPage == 3 &&
            dxm * dxm + dym * dym < (R + U(8)) * (R + U(8)))
            panel.gDragWheel = focusedWheel >= 0 ? focusedWheel : 1;
        if (panel.gDragWheel >= 0)
        {
            if (in.down)
            {
                const int w = panel.gDragWheel;
                float deg = std::atan2(-(in.my - ry), in.mx - rx) * 180.0f / kPi;
                if (deg < 0) deg += 360.0f;
                const float amt = std::clamp((std::sqrt(dxm * dxm + dym * dym) - r0) / (R - r0) * 100.0f, 0.0f, 100.0f);
                if (std::round(deg) != cur.v[hueOf[w]] || std::round(amt) != cur.v[amtOf[w]])
                {
                    cur.v[hueOf[w]] = std::round(deg);
                    cur.v[amtOf[w]] = std::round(amt);
                    changed = true;
                }
                if (panel.gPage == 3) panel.gFocus = 2 + w * 2 + 1;
                panel.side = 1;
            }
            else
                panel.gDragWheel = -1;
        }
        y += scopeH;
        Fill(x0, y - t, x1, y, kHair);
    }

    // Pages.
    {
        const float sh = U(28), sy = y + std::round((tabsH - sh) * 0.5f);
        if (keys && panel.gFocus == 1) focusMark(y, tabsH);
        int want = Segments(kRows + 1, x0 + pad, sy, x1 - pad, sh, std::vector<std::wstring>(pageNames, pageNames + 6),
                            panel.gPage, 63, 0, &hover, dt);
        if (keys && panel.gFocus == 1 && step) want = (panel.gPage + step + 6) % 6;
        if (want >= 0 && want != panel.gPage) panel.gPage = want;
        y += tabsH;
        Fill(x0 + pad, y - t, x1 - pad, y, kHairFaint);
    }

    // The page's rows.
    const float cx0 = x0 + pad + labelW, cx1 = x1 - pad;
    y += U(3);
    for (int r = 0; r < int(rows.size()); ++r)
    {
        const float ry = y + r * rowH;
        const int item = 2 + r, param = rows[size_t(r)];
        const bool focused = keys && panel.gFocus == item;
        const bool overRow = in.mx >= x0 && in.mx < x1 && in.my >= ry && in.my < ry + rowH && !in.pad;
        if (overRow && in.mouseMoved && panel.drag == Panel::kNone && panel.gDragWheel < 0 && panel.gDragPoint < 0)
        {
            panel.gFocus = item;
            panel.side = 1;
        }
        if (focused) focusMark(ry, rowH);
        const float ty = ry + std::round((rowH - LineHeight(kBody)) * 0.5f), sh = U(24),
                    sy = ry + std::round((rowH - sh) * 0.5f);
        if (param < 0)
        {
            const bool channel = param == -1;
            Text(x0 + pad, ty, channel ? L"通道" : L"调整", kBody, focused ? kWhite : kReadout);
            const int chosen = channel ? panel.gChannel : panel.gAttr;
            int want = channel ? Segments(kRows + 2, cx0, sy, cx1, sh, {L"亮度", L"红", L"绿", L"蓝"}, chosen, 15, 0, &hover, dt)
                               : Segments(kRows + 3, cx0, sy, cx1, sh, {L"色相", L"饱和", L"明度"}, chosen, 7, 0, &hover, dt);
            if (focused && step) want = std::clamp(chosen + step, 0, channel ? 3 : 2);
            if (want >= 0) (channel ? panel.gChannel : panel.gAttr) = want;
        }
        else
        {
            const prism::GradeSpec &p = prism::GradeInfo(param);
            // A hue's or a channel's mark beside its name.
            float lx = x0 + pad;
            uint32_t mark = 0;
            if (param >= prism::kGHueShift && param < prism::kGCurve)
                mark = HueInk(kHueCentres[(param - prism::kGHueShift) % 6], 0.72f, 0.14f);
            else if (param >= prism::kGCurve && param < prism::kGCurve + 20)
                mark = ChannelInk((param - prism::kGCurve) / prism::kCurvePoints);
            if (mark)
            {
                Fill(lx, ry + U(11), lx + std::max(2.0f, U(2)), ry + rowH - U(11), mark);
                lx += U(10);
            }
            const bool neutral = std::fabs(cur.v[param] - p.def) < 1e-4f;
            Text(lx, ty, p.label, kBody, focused ? kWhite : neutral ? kReadout : Rgba(0xd9d9d9));
            const float vw = U(56);
            const Bars b = GradeSlider(cx0, sy + U(4), cx1 - vw - U(8), sh - U(10), param, cur.v[param], focused);
            TextRight(cx1, ty, GradeText(param, cur.v[param]), kValue, focused ? kWhite : neutral ? kQuiet : kReadout);
            float nv = cur.v[param];
            const bool overBar = in.mx >= b.x0 - U(4) && in.mx < b.x1 + U(4) && overRow;
            if (overRow && in.pressed)
            {
                if (panel.gClickParam == param && now - panel.gClickAt < 0.35)
                {
                    nv = p.def;
                    panel.gClickAt = -1;
                }
                else
                {
                    panel.gClickParam = param;
                    panel.gClickAt = now;
                    if (overBar)
                    {
                        panel.drag = Panel::kSlide;
                        panel.dragRow = 1000 + param;
                    }
                }
            }
            if (panel.drag == Panel::kSlide && panel.dragRow == 1000 + param && in.down)
            {
                const float f = std::clamp((in.mx - b.x0) / (b.x1 - b.x0), 0.0f, 1.0f);
                nv = GradeNudge(param, p.min + f * (p.max - p.min), 0.0f);
                if (p.unit == prism::kUHue) nv = std::round(p.min + f * (p.max - p.min));
            }
            if (overRow && in.wheelSteps) nv = GradeNudge(param, nv, in.wheelSteps * GradeStep(param, in.fine));
            if (focused && step) nv = GradeNudge(param, nv, step * GradeStep(param, in.fine));
            if (focused && (in.enter || in.reset)) nv = p.def;
            if (std::fabs(nv - cur.v[param]) > 1e-4f)
            {
                cur.v[param] = nv;
                changed = true;
            }
        }
        if (r + 1 < int(rows.size())) Fill(x0 + pad, ry + rowH - t, x1 - pad, ry + rowH, kHairFaint);
    }
    y += rowsH - U(3);
    Fill(x0, y - t, x1, y, kHair);

    // A preset to start from (it replaces the slot's controls); the one the slot matches is inverted.
    {
        const bool focused = keys && panel.gFocus == presetItem;
        if (focused) focusMark(y, presetH);
        const float ty = y + std::round((presetH - LineHeight(kBody)) * 0.5f), sh = U(26),
                    sy = y + std::round((presetH - sh) * 0.5f);
        Text(x0 + pad, ty, L"起点", kBody, focused ? kWhite : kReadout);
        int match = -1;
        for (int i = 0; i < 4; ++i)
            if (prism::GradeFromPreset(i) == cur) match = i;
        int want = Segments(kRows + 4, cx0, sy, cx1, sh, {L"中性", L"高饱和", L"低饱和", L"浓烈"}, match, 15, 0, &hover, dt);
        if (focused && step) want = std::clamp((match < 0 ? (step > 0 ? -1 : 4) : match) + step, 0, 3);
        if (want >= 0 && want != match)
        {
            cur = prism::GradeFromPreset(want);
            changed = true;
        }
        y += presetH;
        Fill(x0, y - t, x1, y, kHair);
    }

    // Help for the focused item.
    {
        std::wstring help;
        const int f = panel.gFocus;
        if (f == 0)
            help = L"三个方案各存一套调色，切换立即生效，方便来回对比。全部保存在游戏目录的调色文件里。";
        else if (f == 1)
            help = L"六页参数：影调、曲线、色彩、三向色轮、色相、胶片。手柄 LB / RB 翻页。";
        else if (f == presetItem)
            help = L"把一个预设载入当前方案作为起点，会替换这个方案的全部参数。和预设完全一致时那一项反白。";
        else
        {
            const int param = rows[size_t(f - 2)];
            if (param == -1)
                help = L"要调的曲线：亮度只改明暗，红、绿、蓝分别改那个颜色在各档亮度上的多少。也可以在上方曲线上直接拖动点。";
            else if (param == -2)
                help = L"六种颜色各自要调的属性：色相、饱和或明度。上方色相环会跟着变：辐条长短是饱和，偏转是色相。";
            else
            {
                help = prism::GradeInfo(param).help;
                if (param >= prism::kGShadowHue && param <= prism::kGHiAmt) help += L" 也可以在色相环上直接拖动它的标记。";
            }
        }
        float hy = y + U(10);
        for (const std::wstring &line : Wrap(help, kSmall, W - pad * 2))
        {
            Text(x0 + pad, hy, line, kSmall, kReadout);
            hy += LineHeight(kSmall);
        }
        y += helpH;
        Fill(x0, y - t, x1, y, kHair);
    }

    // Footer: the keys.
    {
        const float fy = y + std::round((footH - LineHeight(kLabel)) * 0.5f);
        const std::wstring k =
            in.pad ? L"LB RB 翻页　十字键 选择和调整　X 复位　Y 回到面板" : L"Tab 回到面板　←→ 调整　Shift 微调　Enter 或双击 复位";
        Text(x0 + pad, fy, k, kLabel, kQuiet);
    }
    clip = {-1e9f, -1e9f, 1e9f, 1e9f};
    if (changed)
    {
        ApplySet(n);
        panel.changedAt = now;
    }
}

// The split's labels, and its handle while the panel is open (drag it to move the split).
void DrawSplit(UINT bw, UINT bh, bool interactive)
{
    OvSet s;
    ReadSet(s);
    if (!s.split || !s.enabled) return;
    const float sx = std::round(s.splitPos * bw);
    const float ly = U(24);
    auto tag = [&](const std::wstring &text, bool leftOfLine) {
        const float tw = Measure(text, kLabel), bx0 = leftOfLine ? sx - U(10) - tw - U(16) : sx + U(10),
                    bx1 = bx0 + tw + U(16);
        Fill(bx0, ly, bx1, ly + LineHeight(kLabel) + U(8), Rgba(0x000000, 0.8f));
        Text(bx0 + U(8), ly + U(4), text, kLabel, kWhite);
    };
    tag(L"原画", true);
    tag(L"神经", false);
    Fill(sx, 0, sx + Hair(), float(bh), Rgba(0x000000, 0.5f));
    if (!interactive) return;
    const float hy = std::round(bh * 0.5f), hw = U(7), hh = U(28);
    const bool over = std::fabs(in.mx - sx) < U(14) && std::fabs(in.my - hy) < U(40) && !in.pad;
    Fill(sx - hw, hy - hh, sx + hw, hy + hh, over || panel.drag == Panel::kSplitLine ? kWhite : Rgba(0xe6e6e6));
    for (int i = -1; i <= 1; ++i)
        Fill(sx - U(3), hy + i * U(5) - Hair() * 0.5f, sx + U(3), hy + i * U(5) + Hair() * 0.5f, kBlack);
    if (over && in.pressed && panel.drag == Panel::kNone) panel.drag = Panel::kSplitLine;
    if (panel.drag == Panel::kSplitLine)
    {
        if (in.down)
        {
            mgpu::gpu1::ui_set_split_pos(std::clamp(in.mx / float(bw), 0.0f, 1.0f));
            panel.changedAt = Now();
        }
        else
        {
            panel.drag = Panel::kNone;
            mgpu::gpu1::ui_persist_split_pos(s.splitPos);
        }
    }
}

void DrawToast(double now)
{
    const float age = float(now - panel.toastAt);
    if (panel.toast.empty() || age > panel.toastFor + 0.3f) return;
    const float a = age < panel.toastFor ? 1.0f : 1.0f - (age - float(panel.toastFor)) / 0.3f;
    const float x0 = U(24), y0 = U(24);
    const float tw = Measure(panel.toast, kBody), w = tw + U(40), h = LineHeight(kBody) + U(20);
    const float wipe = cfg.motion ? EaseOutExpo(age / 0.15f) : 1.0f;
    clip = {x0, y0, x0 + w * wipe, y0 + h};
    Fill(x0, y0, x0 + w, y0 + h, Fade(Rgba(0x000000, 0.9f), a));
    Fill(x0 + U(12), y0 + U(12), x0 + U(12) + std::max(2.0f, U(2)), y0 + h - U(12), Fade(panel.toastMark, a));
    Text(x0 + U(24), y0 + U(10), panel.toast, kBody, Fade(kWhite, a));
    clip = {-1e9f, -1e9f, 1e9f, 1e9f};
}

bool started = false;
bool g_dead = false;  // fonts failed at start: the overlay can never draw, stop early
// Binds the overlay to a presenting (queue, swap chain) pair. One Chain per
// swap chain; each points at the Gpu context of the chain's own device. A
// chain that is not D3D12, whose device failed, or that has no queue is
// marked unsupported and skipped from then on - but it never blocks another
// chain from binding.
Chain *Bind(IDXGISwapChain *sc, ID3D12CommandQueue *q)
{
    Chain *c = ChainFor(sc);
    if (c->unsupported && !c->queue) return nullptr;
    if (c->queue && c->gpu)
    {
        // Late input hook: on a D3D11 title the panel can bind pre-arm, when the
        // bridge has not recorded the game's window yet (dcomp_game_hwnd null).
        // Hook it as soon as it exists - Bind's body never runs again for this chain.
        if (gameWnd == nullptr &&
            mgpu::gpu1::dcomp_overlay_mode() && mgpu::gpu1::present_chain_native() == (void *)sc &&
            mgpu::gpu1::dcomp_game_hwnd() != nullptr)
        {
            gameWnd = (HWND)mgpu::gpu1::dcomp_game_hwnd();
            gameProc = (WNDPROC)SetWindowLongPtrW(gameWnd, GWLP_WNDPROC, (LONG_PTR)&HkWndProc);
        }
        return c;
    }
    ID3D12Device *d = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void **)&d)) || !d)
    {  // D3D11 or another API: not ours
        c->unsupported = true;
        LOG("[MGPU][POV] panel: swap chain %p is not D3D12: not drawing on it", (void *)sc);
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC desc;
    const bool have_desc = SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow != nullptr;
    bool dcomp_host = false;
    if (!have_desc || !IsWindowVisible(desc.OutputWindow))
    {
        // DComp mode on a D3D11 title: the bridge's own present chain has no
        // visible window - DirectComposition shows it inside the game's window.
        // It is the only D3D12 chain that reaches the display there, so the
        // panel draws into it and appears composed, one copy. Probe and ghost
        // chains stay refused: drawing there puts a second, compositor-jittered
        // copy of the panel on screen.
        dcomp_host = mgpu::gpu1::dcomp_overlay_mode() &&
                     mgpu::gpu1::present_chain_native() == (void *)sc;
        if (!dcomp_host)
        {
            d->Release();
            // NOT permanent: on a D3D11 title the composition chain presents long
            // before the bridge arms, and present_chain_native() is only nominated
            // at arm time. A pre-arm refusal must be retried on a later present,
            // otherwise the panel never binds on the one chain that reaches the
            // display. Log once per chain; the probe costs one GetDevice per present.
            if (!c->said_no_window)
            {
                c->said_no_window = true;
                LOG("[MGPU][POV] panel: chain %p has no visible window: not drawing on it (retryable - "
                    "re-probed on every present until the bridge arms)", (void *)sc);
            }
            if ((++c->probes % 300) == 0)
                LOG("[MGPU][POV] panel: chain %p still unbound after %u probes | dcomp=%d native=%p",
                    (void *)sc, c->probes, (int)mgpu::gpu1::dcomp_overlay_mode(),
                    mgpu::gpu1::present_chain_native());
            return nullptr;
        }
    }
    Gpu *g = GpuFor(d);
    d->Release();
    if (!g || q == nullptr)
    {
        c->unsupported = true;
        return nullptr;
    }
    q->AddRef();
    c->queue = q;
    c->gpu = g;
    LOG("[MGPU][POV] panel: chain %p %ux%u fmt %d buffers %u -> device %p queue %p%s", (void *)sc,
        desc.BufferDesc.Width, desc.BufferDesc.Height, int(desc.BufferDesc.Format), desc.BufferCount, (void *)d, (void *)q,
        dcomp_host ? " (DComp host: composed into the game's window)" : "");
    // Input hooks the window the player actually touches: on a DComp host that
    // is the GAME's window (the chain's own window is hidden), elsewhere the
    // chain's own output window.
    HWND input_wnd = dcomp_host ? (HWND)mgpu::gpu1::dcomp_game_hwnd()
                                : (have_desc ? desc.OutputWindow : nullptr);
    if (input_wnd != nullptr && input_wnd != gameWnd)
    {
        gameWnd = input_wnd;
        gameProc = (WNDPROC)SetWindowLongPtrW(gameWnd, GWLP_WNDPROC, (LONG_PTR)&HkWndProc);
    }
    return c;
}

}  // namespace

bool running()
{
    for (const Chain &c : chains)
        if (c.queue && c.gpu && c.gpu->ok) return true;
    return false;
}

bool panel_open()
{
    return panel.open.load(std::memory_order_relaxed);
}

void on_present(void *queue_native, void *sc_native)
{
    static std::mutex presentMutex;
    std::lock_guard<std::mutex> presentLock(presentMutex);
    if (g_dead) return;
    // Last-resort input watchdog, placed before every chain-binding early
    // return.  A panel that has not produced a successful draw for 500 ms is
    // not allowed to keep Raw Input, cursor or keyboard captured.
    const double guardNow = Now();
    if (panel.open && guardNow - panel.openedAt > 0.5 && guardNow - panelDrawnAt > 0.5)
    {
        panel.open = false;
        SetBlocking(false);
        if (!inputGuardSaid)
        {
            inputGuardSaid = true;
            LOG("[MGPU][POV][INPUT-GUARD] no successful visible panel draw for 500 ms; panel closed and game input released");
        }
    }
    IDXGISwapChain *sc = static_cast<IDXGISwapChain *>(sc_native);
    ID3D12CommandQueue *queue = static_cast<ID3D12CommandQueue *>(queue_native);
    if (sc == nullptr || queue == nullptr) return;
    if (!started)
    {
        started = true;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&on_present, &self);
        if (self)
        {
            const auto *dos = (const IMAGE_DOS_HEADER *)self;
            const auto *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)self + dos->e_lfanew);
            selfBegin = (const uint8_t *)self;
            selfEnd = selfBegin + nt->OptionalHeader.SizeOfImage;
        }
        LoadPanelConfig();
        if (!InitFonts())
        {
            LOG("[MGPU][POV] panel off: no fonts");
            g_dead = true;
            return;
        }
        if (cfg.block) HookInput();
        HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
        if (xi) padRead = (PadStateFn)(void *)GetProcAddress(xi, "XInputGetState");
        panel.startedAt = Now();
        LOG("[MGPU][POV] panel ready (%ls opens it, View + Y on a gamepad)", KeyName(cfg.key));
    }
    Chain *c = Bind(sc, queue);
    if (!c || c->unsupported || !c->queue || !c->gpu || c->sc != sc) return;

    static double last = Now();
    const double now = Now();
    const float dt = float(std::min(0.1, now - last));
    last = now;
    static uint32_t presents = 0;
    if (++presents % 120 == 0)
    {
        LoadPanelConfig();
        if (cfg.block) HookPads();
    }

    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(sc->GetDesc(&d)) || !d.BufferDesc.Width || !d.BufferDesc.Height) return;
    const UINT bw = d.BufferDesc.Width, bh = d.BufferDesc.Height;
    ui = std::clamp(std::round(float(bh) / 1080.0f * 4.0f) / 4.0f, 1.0f, 3.0f) * cfg.scale;

    // While the panel is visible, only the chain that actually displays it may
    // advance the shared input edge state. A background game chain polling first
    // otherwise consumes lWas/pressSeq and the visible chain never sees the click.
    if (panel.open)
    {
        void *displayChain = mgpu::gpu1::present_chain_native();
        if (displayChain != nullptr && displayChain != (void *)sc) return;
    }
    PollInput(bw, bh, panel.open);

    if (in.toggle || (panel.open && in.back))
    {
        panel.open = in.toggle ? !panel.open : false;
        if (panel.open)
            panel.openedAt = now;
        else
            panel.closedAt = now;
        panel.drag = Panel::kNone;
        panel.hinted = true;
    }
    // After closing, the game gets its input back only once the keys that closed the panel are up
    // (otherwise it sees Esc, Home or B go down and opens its own menu).
    const bool closingKeysDown = Key(VK_ESCAPE) || (cfg.key && Key(cfg.key)) || Key(VK_RETURN) || Key(VK_LBUTTON) ||
                                 (in.padHeld & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_BACK));
    const bool panelDrawFresh = panel.open && now - panelDrawnAt <= 0.5;
    SetBlocking(cfg.block && (panelDrawFresh || (!panel.open && blocking.load() && closingKeysDown)));
    if (!panel.hinted && cfg.hint && now - panel.startedAt > 1.0)
    {
        panel.hinted = true;
        Toast(in.padConnected ? std::wstring(L"按 ") + KeyName(cfg.key) + L"，或在手柄上按住 View 再按 Y，打开 MGPU 面板"
                              : std::wstring(L"按 ") + KeyName(cfg.key) + L" 打开 MGPU 面板",
              kInfo, 6.0);
    }

    // An armed dual-GPU session must draw the panel on the visible neural chain only.
    // Previously the same panel was submitted to the game chain and two bridge chains;
    // their independent present cadences made it flash and starved the draw watchdog.
    if (panel.open)
    {
        void *displayChain = mgpu::gpu1::present_chain_native();
        if (displayChain != nullptr && displayChain != (void *)sc) return;
    }

    NoteStats();
    verts.clear();
    DrawSplit(bw, bh, panel.open);
    if (panel.open)
    {
        DrawPanel(bw, bh, dt);
        OvSet s;
        ReadSet(s);
        if (s.look == 5) DrawGrade(bw, bh, dt);
        CURSORINFO ci = {};
        ci.cbSize = sizeof ci;
        const bool systemCursor = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
        const bool inside = in.mx >= 0 && in.my >= 0 && in.mx < float(bw) && in.my < float(bh);
        if (!in.pad && !systemCursor && inside && Foreground()) Cursor(in.mx, in.my);
    }
    else
    {
        DrawToast(now);
    }
    if (verts.empty())
    {
        if (panel.open) SetBlocking(false);
        return;
    }
    const bool submitted = Submit(*c, bw, bh, d.BufferDesc.Format);
    if (panel.open)
    {
        if (submitted)
        {
            panelDrawnAt = Now();
            inputGuardSaid = false;
            static unsigned visibleDraws = 0;
            if (++visibleDraws <= 3)
                LOG("[MGPU][POV][INPUT-GUARD] visible panel submit #%u succeeded on chain %p", visibleDraws, (void *)sc);
        }
        else
        {
            SetBlocking(false);
        }
    }
}

}  // namespace mgpu::poverlay
