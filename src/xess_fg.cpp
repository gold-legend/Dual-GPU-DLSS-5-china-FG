// Optional XeSS-FG output stage. Runtime libraries are resolved dynamically so the
// ordinary MGPU path has no Intel import dependency and remains byte-for-byte off.
#include "xess_fg.hpp"
#include "diag.hpp"

#include <windows.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <mutex>

namespace mgpu::xessfg
{
namespace
{
    struct state
    {
        HMODULE fg_dll = nullptr;
        HMODULE xell_dll = nullptr;
        bool load_tried = false, loaded = false, ready = false;

        xefg_swapchain_handle_t fg = nullptr;
        xell_context_handle_t xell = nullptr;

        decltype(&xefgSwapChainGetVersion) fg_version = nullptr;
        decltype(&xefgSwapChainD3D12CreateContext) fg_create = nullptr;
        decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) fg_init_desc = nullptr;
        decltype(&xefgSwapChainD3D12GetSwapChainPtr) fg_get_chain = nullptr;
        decltype(&xefgSwapChainD3D12TagFrameResource) fg_tag_resource = nullptr;
        decltype(&xefgSwapChainTagFrameConstants) fg_tag_constants = nullptr;
        decltype(&xefgSwapChainSetEnabled) fg_set_enabled = nullptr;
        decltype(&xefgSwapChainGetProperties) fg_get_properties = nullptr;
        using set_num_interpolated_fn = xefg_swapchain_result_t (*)(xefg_swapchain_handle_t, uint32_t);
        set_num_interpolated_fn fg_set_num_interpolated = nullptr;
        decltype(&xefgSwapChainSetPresentId) fg_set_present_id = nullptr;
        decltype(&xefgSwapChainGetLastPresentStatus) fg_last_status = nullptr;
        decltype(&xefgSwapChainSetLatencyReduction) fg_set_xell = nullptr;
        decltype(&xefgSwapChainSetLoggingCallback) fg_set_log = nullptr;
        decltype(&xefgSwapChainDestroy) fg_destroy = nullptr;

        decltype(&xellGetVersion) xell_version = nullptr;
        decltype(&xellD3D12CreateContext) xell_create = nullptr;
        decltype(&xellSetSleepMode) xell_set_sleep = nullptr;
        decltype(&xellSleep) xell_sleep = nullptr;
        decltype(&xellAddMarkerData) xell_marker = nullptr;
        decltype(&xellDestroyContext) xell_destroy = nullptr;

        uint32_t present_id = 0, current_id = 0;
        unsigned max_interpolated = 1, num_interpolated = 1;
        bool enabled = false, have_source = false;
        unsigned long long last_source = 0;
        unsigned last_dw = 0, last_dh = 0, last_mw = 0, last_mh = 0;

        // XeSS-FG requires a depth input even when motion comes from AMD optical
        // flow. A constant placeholder satisfies the API without game depth.
        ID3D12Device *device = nullptr;
        ID3D12Resource *dummy_depth = nullptr;
        ID3D12DescriptorHeap *dummy_dsv = nullptr;
        unsigned dummy_w = 0, dummy_h = 0;

        std::mutex stats_cs;
        stats current;
        LARGE_INTEGER fps_window_start{};
        unsigned long long fps_window_frames = 0;
    };

    state &st()
    {
        static state s;
        return s;
    }

    void log_result(const char *call, int result, bool error)
    {
        char line[320];
        snprintf(line, sizeof line, "[MGPU][XESS-FG] %s returned %d%s", call, result,
                 error ? " - frame generation disabled for this frame" : "");
        if (error) mgpu::diag::error(line);
        else       mgpu::diag::warn(line);
    }

    bool bad_fg(const char *call, xefg_swapchain_result_t result)
    {
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) return false;
        log_result(call, (int)result, result < XEFG_SWAPCHAIN_RESULT_SUCCESS);
        return result < XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }

    void fg_log_callback(const char *message, xefg_swapchain_logging_level_t level, void *)
    {
        if (message == nullptr) return;
        char line[900];
        snprintf(line, sizeof line, "[MGPU][XESS-FG][Intel] %s", message);
        if (level == XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR) mgpu::diag::error(line);
        else if (level == XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING) mgpu::diag::warn(line);
        else mgpu::diag::info(line);
    }

    bool module_path(const wchar_t *name, wchar_t *out, size_t count)
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&module_path), &self) == FALSE)
            return false;
        wchar_t path[1024]{};
        const DWORD n = GetModuleFileNameW(self, path, 1024);
        if (n == 0 || n >= 1024) return false;
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash == nullptr) return false;
        slash[1] = L'\0';
        const int written = swprintf(out, count, L"%ls%ls", path, name);
        return written > 0 && (size_t)written < count;
    }

    HMODULE load_local(const wchar_t *name)
    {
        wchar_t path[1024]{};
        if (!module_path(name, path, 1024))
        {
            mgpu::diag::error("[MGPU][XESS-FG] could not derive the add-on directory");
            return nullptr;
        }
        HMODULE dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (dll == nullptr)
        {
            char line[1200];
            snprintf(line, sizeof line,
                     "[MGPU][XESS-FG] LoadLibrary failed for %ls (Win32=%lu); the native MGPU chain will be used",
                     name, (unsigned long)GetLastError());
            mgpu::diag::error(line);
        }
        return dll;
    }

    template <typename T>
    bool resolve_one(HMODULE dll, const char *name, T &out)
    {
        out = reinterpret_cast<T>(GetProcAddress(dll, name));
        if (out != nullptr) return true;
        char line[240];
        snprintf(line, sizeof line, "[MGPU][XESS-FG] missing export: %s", name);
        mgpu::diag::error(line);
        return false;
    }

    bool load_api()
    {
        auto &S = st();
        if (S.load_tried) return S.loaded;
        S.load_tried = true;

        // XeLL must be loaded first: XeSS-FG requires its context and markers.
        S.xell_dll = load_local(L"libxell.dll");
        S.fg_dll = load_local(L"libxess_fg.dll");
        if (S.xell_dll == nullptr || S.fg_dll == nullptr) return false;

        bool ok = true;
#define RESOLVE(dll, name, member) ok = resolve_one(dll, #name, member) && ok
        RESOLVE(S.fg_dll, xefgSwapChainGetVersion, S.fg_version);
        RESOLVE(S.fg_dll, xefgSwapChainD3D12CreateContext, S.fg_create);
        RESOLVE(S.fg_dll, xefgSwapChainD3D12InitFromSwapChainDesc, S.fg_init_desc);
        RESOLVE(S.fg_dll, xefgSwapChainD3D12GetSwapChainPtr, S.fg_get_chain);
        RESOLVE(S.fg_dll, xefgSwapChainD3D12TagFrameResource, S.fg_tag_resource);
        RESOLVE(S.fg_dll, xefgSwapChainTagFrameConstants, S.fg_tag_constants);
        RESOLVE(S.fg_dll, xefgSwapChainSetEnabled, S.fg_set_enabled);
        RESOLVE(S.fg_dll, xefgSwapChainGetProperties, S.fg_get_properties);
        S.fg_set_num_interpolated = reinterpret_cast<state::set_num_interpolated_fn>(
            GetProcAddress(S.fg_dll, "xefgSwapChainSetNumInterpolatedFrames"));
        if (S.fg_set_num_interpolated == nullptr)
            mgpu::diag::warn("[MGPU][XESS-FG] SetNumInterpolatedFrames unavailable; only X2 can be used");
        RESOLVE(S.fg_dll, xefgSwapChainSetPresentId, S.fg_set_present_id);
        RESOLVE(S.fg_dll, xefgSwapChainGetLastPresentStatus, S.fg_last_status);
        RESOLVE(S.fg_dll, xefgSwapChainSetLatencyReduction, S.fg_set_xell);
        RESOLVE(S.fg_dll, xefgSwapChainSetLoggingCallback, S.fg_set_log);
        RESOLVE(S.fg_dll, xefgSwapChainDestroy, S.fg_destroy);
        RESOLVE(S.xell_dll, xellGetVersion, S.xell_version);
        RESOLVE(S.xell_dll, xellD3D12CreateContext, S.xell_create);
        RESOLVE(S.xell_dll, xellSetSleepMode, S.xell_set_sleep);
        RESOLVE(S.xell_dll, xellSleep, S.xell_sleep);
        RESOLVE(S.xell_dll, xellAddMarkerData, S.xell_marker);
        RESOLVE(S.xell_dll, xellDestroyContext, S.xell_destroy);
#undef RESOLVE
        S.loaded = ok;
        return ok;
    }

    bool create_dummy_depth(ID3D12Device *device, unsigned width, unsigned height)
    {
        auto &S = st();
        if (device == nullptr || width == 0 || height == 0) return false;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = width;
        texture.Height = height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        clear.DepthStencil.Depth = 1.0f;
        HRESULT hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear, IID_PPV_ARGS(&S.dummy_depth));
        if (FAILED(hr))
        {
            log_result("CreateCommittedResource(constant FG depth)", (int)hr, true);
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&S.dummy_dsv));
        if (FAILED(hr))
        {
            log_result("CreateDescriptorHeap(constant FG depth)", (int)hr, true);
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        view.Flags = D3D12_DSV_FLAG_NONE;
        view.Texture2D.MipSlice = 0;
        device->CreateDepthStencilView(
            S.dummy_depth, &view, S.dummy_dsv->GetCPUDescriptorHandleForHeapStart());
        S.dummy_w = width;
        S.dummy_h = height;
        return true;
    }

    void destroy_contexts()
    {
        auto &S = st();
        S.ready = false;
        if (S.fg != nullptr && S.fg_destroy != nullptr)
        {
            const auto r = S.fg_destroy(S.fg);
            if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) log_result("xefgSwapChainDestroy", (int)r, true);
        }
        S.fg = nullptr;
        if (S.xell != nullptr && S.xell_destroy != nullptr)
        {
            const auto r = S.xell_destroy(S.xell);
            if (r != XELL_RESULT_SUCCESS) log_result("xellDestroyContext", (int)r, true);
        }
        S.xell = nullptr;
        if (S.dummy_dsv != nullptr) S.dummy_dsv->Release();
        if (S.dummy_depth != nullptr) S.dummy_depth->Release();
        S.dummy_dsv = nullptr;
        S.dummy_depth = nullptr;
        S.dummy_w = S.dummy_h = 0;
        if (S.device != nullptr) S.device->Release();
        S.device = nullptr;
        S.enabled = false;
        S.have_source = false;
        S.last_source = 0;
        S.present_id = S.current_id = 0;
        S.last_dw = S.last_dh = S.last_mw = S.last_mh = 0;
    }

    bool set_enabled(bool on)
    {
        auto &S = st();
        if (S.enabled == on) return true;
        const auto r = S.fg_set_enabled(S.fg, on ? 1u : 0u);
        if (bad_fg("xefgSwapChainSetEnabled", r)) return false;
        S.enabled = on;
        return true;
    }

    void marker(xell_latency_marker_type_t which)
    {
        auto &S = st();
        const auto r = S.xell_marker(S.xell, S.current_id, which);
        if (r != XELL_RESULT_SUCCESS)
        {
            static unsigned errors = 0;
            if (errors++ < 8u) log_result("xellAddMarkerData", (int)r, true);
        }
    }
}

bool create_swapchain(ID3D12Device *device, ID3D12CommandQueue *queue,
                      IDXGIFactory2 *factory, HWND hwnd,
                      const DXGI_SWAP_CHAIN_DESC1 &desc, bool inverted_depth,
                      unsigned interpolated_frames, IDXGISwapChain3 **out_swapchain)
{
    if (out_swapchain == nullptr) return false;
    *out_swapchain = nullptr;
    auto &S = st();
    if (S.ready || device == nullptr || queue == nullptr || factory == nullptr || hwnd == nullptr)
        return false;
    if (!load_api()) return false;

    xefg_swapchain_version_t fv{};
    xell_version_t lv{};
    const auto fvr = S.fg_version(&fv);
    const auto lvr = S.xell_version(&lv);
    if (fvr == XEFG_SWAPCHAIN_RESULT_SUCCESS && lvr == XELL_RESULT_SUCCESS)
    {
        char line[240];
        snprintf(line, sizeof line, "[MGPU][XESS-FG] runtime API: FG %u.%u.%u, XeLL %u.%u.%u",
                 fv.major, fv.minor, fv.patch, lv.major, lv.minor, lv.patch);
        mgpu::diag::info(line);
    }

    auto r = S.xell_create(device, &S.xell);
    if (r != XELL_RESULT_SUCCESS)
    {
        log_result("xellD3D12CreateContext", (int)r, true);
        destroy_contexts();
        return false;
    }
    xell_sleep_params_t sleep{};
    sleep.bLowLatencyMode = 1;
    r = S.xell_set_sleep(S.xell, &sleep);
    if (r != XELL_RESULT_SUCCESS)
    {
        log_result("xellSetSleepMode", (int)r, true);
        destroy_contexts();
        return false;
    }

    auto fr = S.fg_create(device, &S.fg);
    if (fr != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        log_result("xefgSwapChainD3D12CreateContext", (int)fr, true);
        destroy_contexts();
        return false;
    }
    (void)S.fg_set_log(S.fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, fg_log_callback, nullptr);
    fr = S.fg_set_xell(S.fg, S.xell);
    if (fr != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        log_result("xefgSwapChainSetLatencyReduction", (int)fr, true);
        destroy_contexts();
        return false;
    }

    // Query the runtime rather than inferring X3 support from headers. Creating
    // with the supported maximum allows live X2/X3 changes without reinit.
    S.max_interpolated = 1;
    xefg_swapchain_properties_t properties{};
    const auto properties_result = S.fg_get_properties(S.fg, &properties);
    if (properties_result == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
        S.fg_set_num_interpolated != nullptr && properties.maxSupportedInterpolations >= 2u)
        S.max_interpolated = 2;
    const unsigned requested = interpolated_frames == 0u ? 1u : interpolated_frames;
    S.num_interpolated = requested > S.max_interpolated ? S.max_interpolated : requested;
    {
        char line[320];
        snprintf(line, sizeof line,
                 "[MGPU][XESS-FG] runtime interpolation capacity: maxSupported=%u, SetNum=%s; requested X%u, initial X%u",
                 properties.maxSupportedInterpolations,
                 S.fg_set_num_interpolated != nullptr ? "yes" : "no",
                 requested + 1u, S.num_interpolated + 1u);
        mgpu::diag::info(line);
    }
    if (requested > S.max_interpolated)
        mgpu::diag::warn("[MGPU][XESS-FG] X3 is unsupported by this runtime/device; using X2 instead of pretending X3 is active");

    (void)inverted_depth; // Constant placeholder depth has no scene convention.
    xefg_swapchain_d3d12_init_params_t params{};
    params.pApplicationSwapChain = nullptr;
    params.maxInterpolatedFrames = S.max_interpolated;
    params.creationNodeMask = 1;
    params.visibleNodeMask = 1;
    params.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE; // final colour already contains the HUD
    // No INVERTED_DEPTH flag: the placeholder is constant, not game depth.

    fr = S.fg_init_desc(S.fg, hwnd, &desc, nullptr, queue, factory, &params);
    if (fr != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        log_result("xefgSwapChainD3D12InitFromSwapChainDesc", (int)fr, true);
        destroy_contexts();
        return false;
    }
    fr = S.fg_get_chain(S.fg, __uuidof(IDXGISwapChain3), reinterpret_cast<void **>(out_swapchain));
    if (fr != XEFG_SWAPCHAIN_RESULT_SUCCESS || *out_swapchain == nullptr)
    {
        log_result("xefgSwapChainD3D12GetSwapChainPtr", (int)fr, true);
        if (*out_swapchain != nullptr)
        {
            (*out_swapchain)->Release();
            *out_swapchain = nullptr;
        }
        destroy_contexts();
        return false;
    }
    if (S.max_interpolated > 1u)
    {
        fr = S.fg_set_num_interpolated(S.fg, S.num_interpolated);
        if (fr != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            log_result("xefgSwapChainSetNumInterpolatedFrames(initial)", (int)fr, true);
            (*out_swapchain)->Release();
            *out_swapchain = nullptr;
            destroy_contexts();
            return false;
        }
    }
    S.device = device;
    S.device->AddRef();
    // Stay native until the first valid tag has created matching depth. Enabling
    // FG here would let Intel's worker run with no input resources at startup.
    S.enabled = false;

    S.ready = true;
    QueryPerformanceCounter(&S.fps_window_start);
    {
        std::lock_guard<std::mutex> lk(S.stats_cs);
        S.current = stats{};
        S.current.active = true;
    }
    {
        char line[240];
        snprintf(line, sizeof line,
                 "[MGPU][XESS-FG] proxy swap chain active: initial X%u, maximum X%u, native until first valid input, UI_MODE_NONE",
                 S.num_interpolated + 1u, S.max_interpolated + 1u);
        mgpu::diag::info(line);
    }
    mgpu::diag::warn("[MGPU][XESS-FG] AMD optical-flow motion + constant placeholder depth; no real game depth");
    return true;
}

bool active()
{
    return st().ready;
}

void begin_frame()
{
    auto &S = st();
    if (!S.ready) return;
    S.current_id = ++S.present_id;
    if (S.current_id == 0) S.current_id = ++S.present_id; // zero is reserved by the API warning
    const auto r = S.xell_sleep(S.xell, S.current_id);
    if (r != XELL_RESULT_SUCCESS) log_result("xellSleep", (int)r, true);
    marker(XELL_SIMULATION_START);
    marker(XELL_SIMULATION_END);
    marker(XELL_RENDERSUBMIT_START);
}

bool tag_frame(ID3D12GraphicsCommandList *command_list, const frame_inputs &inputs)
{
    auto &S = st();
    if (!S.ready || command_list == nullptr) return false;

    const bool dimensions = inputs.motion_width != 0 && inputs.motion_height != 0;
    if (S.dummy_depth == nullptr && S.device != nullptr && inputs.motion != nullptr && dimensions)
    {
        if (create_dummy_depth(S.device, inputs.motion_width, inputs.motion_height))
        {
            char line[240];
            snprintf(line, sizeof line,
                     "[MGPU][XESS-FG] constant placeholder depth created at motion size %ux%u",
                     S.dummy_w, S.dummy_h);
            mgpu::diag::info(line);
        }
    }
    const bool fresh = inputs.source_frame != 0 && (!S.have_source || inputs.source_frame != S.last_source);
    const bool valid = inputs.generation_requested && inputs.color != nullptr && inputs.motion != nullptr &&
                       S.dummy_depth != nullptr && S.dummy_dsv != nullptr &&
                       S.dummy_w == inputs.motion_width && S.dummy_h == inputs.motion_height &&
                       dimensions && fresh && std::isfinite(inputs.render_ms) && inputs.render_ms > 0.0f;
    if (!valid)
    {
        // Keep the existing proxy alive, but make OFF a real native-present
        // path. Dropping the source continuity also resets FG history on ON.
        if (!inputs.generation_requested) S.have_source = false;
        static unsigned skipped = 0;
        ++skipped;
        if (skipped == 1u || (skipped % 120u) == 0u)
        {
            char line[520];
            snprintf(line, sizeof line,
                     "[MGPU][XESS-FG] no interpolation this present: colour=%p motion=%p motion=%ux%u depth=%ux%u fresh=%d skipped=%u",
                     (void *)inputs.color, (void *)inputs.motion,
                     inputs.motion_width, inputs.motion_height, S.dummy_w, S.dummy_h,
                     fresh ? 1 : 0, skipped);
            mgpu::diag::warn(line);
        }
        (void)set_enabled(false);
        (void)S.fg_set_present_id(S.fg, S.current_id);
        marker(XELL_RENDERSUBMIT_END);
        marker(XELL_PRESENT_START);
        return false;
    }

    const unsigned requested = inputs.interpolated_frames == 0u ? 1u : inputs.interpolated_frames;
    const unsigned wanted = requested > S.max_interpolated ? S.max_interpolated : requested;
    if (wanted != S.num_interpolated)
    {
        const auto change_result = S.fg_set_num_interpolated == nullptr
            ? XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED
            : S.fg_set_num_interpolated(S.fg, wanted);
        if (bad_fg("xefgSwapChainSetNumInterpolatedFrames(live)", change_result))
        {
            (void)set_enabled(false);
            marker(XELL_RENDERSUBMIT_END);
            marker(XELL_PRESENT_START);
            return false;
        }
        S.num_interpolated = wanted;
        S.have_source = false; // never reuse temporal history across a rate change
        char line[240];
        snprintf(line, sizeof line,
                 "[MGPU][XESS-FG] live multiplier applied: X%u (interpolated=%u); history reset",
                 S.num_interpolated + 1u, S.num_interpolated);
        mgpu::diag::info(line);
    }

    if (!set_enabled(true))
    {
        marker(XELL_RENDERSUBMIT_END);
        marker(XELL_PRESENT_START);
        return false;
    }

    xefg_swapchain_d3d12_resource_data_t motion{};
    motion.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    // Match the official FG sample: make Intel record its own motion copy into
    // this command list, which is executed before Present. This is an A/B of
    // resource delivery, not a change to AMD's optical-flow vectors. Our present
    // fence still protects the source texture until all submitted work retires.
    motion.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
    motion.resourceBase = { 0, 0 };
    motion.resourceSize = { inputs.motion_width, inputs.motion_height };
    motion.pResource = inputs.motion;
    motion.incomingState = inputs.motion_state;
    const auto motion_result = S.fg_tag_resource(S.fg, command_list, S.current_id, &motion);
    static unsigned motion_samples = 0;
    ++motion_samples;
    if (motion_samples <= 5u || motion_samples % 120u == 0u || motion_result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        const D3D12_RESOURCE_DESC actual = inputs.motion->GetDesc();
        char line[600];
        snprintf(line, sizeof line,
                 "[MGPU][XESS-TAG] sample=%u present_id=%u source=%llu slot=%u motion=%p "
                 "extent=%ux%u actual=%llux%u format=%u state=0x%X validity=ONLY_NOW result=%d; constant placeholder depth supplied",
                 motion_samples, S.current_id, inputs.source_frame, inputs.input_slot,
                 (void *)inputs.motion, inputs.motion_width, inputs.motion_height,
                 (unsigned long long)actual.Width, actual.Height, (unsigned)actual.Format,
                 (unsigned)inputs.motion_state, (int)motion_result);
        mgpu::diag::info(line);
    }
    if (bad_fg("tag AMD motion vectors", motion_result))
    {
        (void)set_enabled(false);
        marker(XELL_RENDERSUBMIT_END); marker(XELL_PRESENT_START);
        return false;
    }

    // Match the validated official A/B: constant depth is supplied every frame,
    // while motion still comes from AMD optical flow. No game depth is read.
    command_list->ClearDepthStencilView(
        S.dummy_dsv->GetCPUDescriptorHandleForHeapStart(),
        D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    xefg_swapchain_d3d12_resource_data_t depth{};
    depth.type = XEFG_SWAPCHAIN_RES_DEPTH;
    depth.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
    depth.resourceBase = { 0, 0 };
    depth.resourceSize = { S.dummy_w, S.dummy_h };
    depth.pResource = S.dummy_depth;
    depth.incomingState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    const auto depth_result = S.fg_tag_resource(S.fg, command_list, S.current_id, &depth);
    if (bad_fg("tag constant placeholder depth", depth_result))
    {
        (void)set_enabled(false);
        marker(XELL_RENDERSUBMIT_END); marker(XELL_PRESENT_START);
        return false;
    }

    xefg_swapchain_frame_constant_data_t constants{};
    for (unsigned i = 0; i < 16u; ++i)
        constants.viewMatrix[i] = constants.projectionMatrix[i] = (i % 5u == 0u) ? 1.0f : 0.0f;
    constants.motionVectorScaleX = inputs.motion_scale_x;
    constants.motionVectorScaleY = inputs.motion_scale_y;
    const bool geometry_changed = S.have_source &&
        (S.last_mw != inputs.motion_width || S.last_mh != inputs.motion_height);
    const bool discontinuous = S.have_source && inputs.source_frame != S.last_source + 1ull;
    constants.resetHistory = (!S.have_source || inputs.reset_history || geometry_changed || discontinuous) ? 1u : 0u;
    constants.frameRenderTime = inputs.render_ms;
    if (bad_fg("xefgSwapChainTagFrameConstants", S.fg_tag_constants(S.fg, S.current_id, &constants)) ||
        bad_fg("xefgSwapChainSetPresentId", S.fg_set_present_id(S.fg, S.current_id)))
    {
        (void)set_enabled(false);
        marker(XELL_RENDERSUBMIT_END); marker(XELL_PRESENT_START);
        return false;
    }

    S.have_source = true;
    S.last_source = inputs.source_frame;
    S.last_mw = inputs.motion_width; S.last_mh = inputs.motion_height;
    marker(XELL_RENDERSUBMIT_END);
    marker(XELL_PRESENT_START);
    return true;
}

void end_frame(HRESULT present_result)
{
    auto &S = st();
    if (!S.ready) return;
    marker(XELL_PRESENT_END);
    xefg_swapchain_present_status_t ps{};
    const auto r = S.fg_last_status(S.fg, &ps);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS && r != XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS)
        log_result("xefgSwapChainGetLastPresentStatus", (int)r, r < XEFG_SWAPCHAIN_RESULT_SUCCESS);
    static unsigned status_samples = 0;
    ++status_samples;
    if (status_samples <= 3u || (status_samples % 120u) == 0u)
    {
        char line[320];
        snprintf(line, sizeof line,
                 "[MGPU][XESS-STATUS] sample=%u present_id=%u present_hr=0x%08lX status=%d "
                 "framesPresented=%u isFrameGenEnabled=%u frameGenResult=%d",
                 status_samples, S.current_id, (unsigned long)present_result, (int)r,
                 ps.framesPresented, ps.isFrameGenEnabled, (int)ps.frameGenResult);
        mgpu::diag::info(line);
    }

    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (S.fps_window_start.QuadPart == 0) S.fps_window_start = now;
    S.fps_window_frames += ps.framesPresented;
    const double elapsed = (freq.QuadPart != 0)
        ? double(now.QuadPart - S.fps_window_start.QuadPart) / double(freq.QuadPart) : 0.0;

    std::lock_guard<std::mutex> lk(S.stats_cs);
    ++S.current.application_presents;
    S.current.last_frames_presented = ps.framesPresented;
    S.current.last_frame_result = (int)ps.frameGenResult;
    S.current.generating = ps.isFrameGenEnabled != 0 && ps.framesPresented > 1u;
    if (ps.framesPresented > 1u) S.current.generated_frames += ps.framesPresented - 1u;
    if (ps.frameGenResult < XEFG_SWAPCHAIN_RESULT_SUCCESS) S.current.last_error = (int)ps.frameGenResult;
    if (elapsed >= 0.5)
    {
        S.current.output_fps = double(S.fps_window_frames) / elapsed;
        S.fps_window_frames = 0;
        S.fps_window_start = now;
    }
    if (FAILED(present_result) && S.current.last_error == 0) S.current.last_error = (int)present_result;
}

void read_stats(stats &out)
{
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.stats_cs);
    out = S.current;
    out.active = S.ready;
}

void shutdown()
{
    auto &S = st();
    if (S.fg == nullptr && S.xell == nullptr) return;
    destroy_contexts();
    std::lock_guard<std::mutex> lk(S.stats_cs);
    S.current.active = false;
    S.current.generating = false;
    S.current.last_frames_presented = 0;
    // Keep the library handles loaded for process lifetime: vendor callbacks and
    // worker threads must never be left pointing into an unloaded DLL.
}

} // namespace mgpu::xessfg
