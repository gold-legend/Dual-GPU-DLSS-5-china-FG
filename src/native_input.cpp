#include "native_input.hpp"

#include <cstdio>
#include <cstring>
#include <utility>
#include "diag.hpp"

namespace mgpu::native_input
{
namespace
{
const mgpu_native_input_api_v1 *g_api = nullptr;
bool g_tried = false;
uint64_t g_generation = 0;
uint64_t g_reports = 0;
char g_reason[192] = "native input provider not found; real frames only";

void update_reason(const char *text)
{
    if (text == nullptr || *text == '\0') text = "native input unavailable";
    if (std::strcmp(g_reason, text) == 0) return;
    std::snprintf(g_reason, sizeof(g_reason), "%s", text);
    if ((g_reports++ % 60u) == 0u)
    {
        char line[320];
        std::snprintf(line, sizeof(line), "[MGPU][NATIVE] %s; no interpolation is enabled.", g_reason);
        mgpu::diag::info(line);
    }
}

bool discover()
{
    if (g_api != nullptr) return true;
    if (g_tried) return false;
    g_tried = true;
    HMODULE provider = GetModuleHandleW(L"dinput8.dll");
    if (provider == nullptr)
    {
        update_reason("REFramework dinput8.dll is not loaded");
        return false;
    }
    auto entry = reinterpret_cast<mgpu_get_native_input_v1_fn>(
        GetProcAddress(provider, "MGPU_GetNativeInputV1"));
    if (entry == nullptr)
    {
        update_reason("the loaded REFramework has no MGPU_GetNativeInputV1 export");
        return false;
    }
    const auto *api = entry();
    if (api == nullptr || api->magic != MGPU_NATIVE_INPUT_MAGIC ||
        api->struct_size != sizeof(mgpu_native_input_api_v1) ||
        api->abi_version != MGPU_NATIVE_INPUT_ABI || api->slot_count != 3u ||
        api->acquire_v1 == nullptr || api->release_v1 == nullptr ||
        api->generation_v1 == nullptr || api->last_reason_v1 == nullptr || api->sample_v1 == nullptr ||
        api->acquire_for_color_v1 == nullptr || api->bind_color_v1 == nullptr)
    {
        update_reason("native input ABI is incompatible");
        return false;
    }
    g_api = api;
    g_generation = api->generation_v1();
    update_reason("native input provider found; waiting for matched frames and pixel verification");
    return true;
}
}

lease::lease(lease &&other) noexcept
{
    *this = std::move(other);
}

lease &lease::operator=(lease &&other) noexcept
{
    if (this == &other) return *this;
    reset();
    api = other.api;
    data = other.data;
    held = other.held;
    other.api = nullptr;
    other.data = {};
    other.held = false;
    return *this;
}

lease::~lease() { reset(); }

void lease::reset()
{
    if (held && api != nullptr && api->release_v1 != nullptr) api->release_v1(&data);
    held = false;
    api = nullptr;
    data = {};
}

header_report inspect_header(const mgpu_native_input_v1 &s, uint64_t expected_game_luid,
                             uint64_t expected_generation, int require_pixels)
{
    header_report r{};
    if (!mgpu_native_input_bounds_valid(&s)) { r.reason = "invalid ABI, resource bounds, frame or cookie"; return r; }
    if (expected_game_luid == 0u || s.game_luid != expected_game_luid)
    { r.reason = "provider and confirmed game adapter LUID differ"; return r; }
    if (expected_generation == 0u || s.generation != expected_generation)
    { r.reason = "native input generation changed; history must reset"; return r; }
    if (s.process_id != GetCurrentProcessId())
    { r.reason = "resource belongs to another process"; return r; }
    if (s.mv_format != DXGI_FORMAT_R16G16_FLOAT)
    { r.reason = "motion vectors are not native R16G16_FLOAT"; return r; }
    for (const float *m : {s.view_to_clip, s.clip_to_view, s.clip_to_prev_clip, s.prev_clip_to_clip})
        if (!mgpu_native_input_finite_matrix(m))
        { r.reason = "non-finite camera matrix"; return r; }
    for (const float *v : {s.camera_position, s.camera_right, s.camera_up, s.camera_forward})
        for (unsigned i = 0; i < 3u; ++i)
            if (!(v[i] >= -1.0e30f && v[i] <= 1.0e30f))
            { r.reason = "non-finite camera basis"; return r; }
    if (!(s.camera_near > 0.0f) || !(s.camera_far > s.camera_near) ||
        !(s.camera_fov_y > 0.0f) || !(s.camera_fov_y < 3.14f) ||
        !(s.camera_aspect > 0.0f))
    { r.reason = "invalid clipping planes, FOV or aspect ratio"; return r; }
    r.missing = mgpu_native_input_missing(s.flags, 1);
    if (require_pixels && r.missing != 0u) { r.reason = "unverified pixels, depth convention or camera history"; return r; }
    const uint64_t diagnostic = MGPU_NI_F_DX12 | MGPU_NI_F_CAMERA | MGPU_NI_F_CAMERA_HISTORY |
        MGPU_NI_F_MOTION_VECTORS | MGPU_NI_F_DEPTH | MGPU_NI_F_GAMEPLAY_KNOWN |
        MGPU_NI_F_GAMEPLAY | MGPU_NI_F_MATCHED_FRAME | MGPU_NI_F_ENGINE_COPY_DONE |
        MGPU_NI_F_RESOURCE_BOUNDS | MGPU_NI_F_TRACKED_COPY | MGPU_NI_F_CROSS_ADAPTER | MGPU_NI_F_COLOR_MATCHED;
    if (s.game_frame == 0 || !mgpu_native_input_transfer_valid(&s))
    { r.reason = "no exact color frame or valid cross-adapter transfer descriptor"; return r; }
    if ((s.flags & diagnostic) != diagnostic || (s.flags & MGPU_NI_F_HISTORY_RESET) != 0u)
    { r.reason = "diagnostic frame is not contiguous, playable and fully matched"; return r; }
    r.compatible = true;
    r.reason = (r.missing == 0u) ? "native pixels and camera verified" : "diagnostic only; pixels are not yet verified";
    return r;
}

bool take(mgpu_native_input_v1 &candidate, uint64_t expected_game_luid, lease &out)
{
    const uint64_t gen = g_api->generation_v1();
    if (gen == 0u) { update_reason("provider returned generation zero"); return false; }
    if (gen != g_generation)
    {
        g_generation = gen;
        update_reason("provider history generation changed; discard the old input");
    }
    lease next;
    next.api = g_api;
    next.data = candidate;
    next.held = true; // Every acquired frame is released, including rejected headers.
    const auto r = inspect_header(candidate, expected_game_luid, gen, 0);
    if (!r.compatible)
    {
        update_reason(r.reason);
        return false;
    }
    out = std::move(next);
    return true;
}

bool acquire(uint64_t minimum_frame, uint64_t expected_game_luid, lease &out)
{
    out.reset();
    if (!discover()) return false;
    mgpu_native_input_v1 candidate{};
    if (g_api->acquire_v1(minimum_frame, &candidate, sizeof(candidate)) == 0u)
    {
        update_reason(g_api->last_reason_v1());
        return false;
    }
    return take(candidate, expected_game_luid, out);
}

bool acquire_for_color(uint64_t game_frame, uint64_t expected_game_luid, lease &out)
{
    out.reset();
    if (game_frame == 0 || !discover()) return false;
    mgpu_native_input_v1 candidate{};
    if (g_api->acquire_for_color_v1(game_frame, &candidate, sizeof(candidate)) == 0u)
    {
        update_reason(g_api->last_reason_v1());
        return false;
    }
    if (candidate.game_frame != game_frame)
    {
        lease reject;
        reject.api = g_api; reject.data = candidate; reject.held = true;
        update_reason("provider returned a different color frame");
        return false;
    }
    return take(candidate, expected_game_luid, out);
}

void bind_color_frame(uint64_t game_frame, uint64_t previous_game_frame, uint64_t seal_qpc)
{
    if (!discover()) return;
    g_api->bind_color_v1(game_frame, previous_game_frame, seal_qpc);
}

bool sample(const lease &frame, mgpu_native_input_sample_v1 &out)
{
    out = {};
    if (!frame.held || frame.api == nullptr || frame.api != g_api || frame.api->sample_v1 == nullptr)
    { update_reason("sample lease is not held by the current provider"); return false; }
    if (frame.data.generation != g_generation || frame.api->generation_v1() != g_generation)
    { update_reason("native generation changed before sampling"); return false; }
    const auto header = inspect_header(frame.data, frame.data.game_luid, g_generation, 0);
    if (!header.compatible) { update_reason(header.reason); return false; }
    if (frame.api->sample_v1(&frame.data, &out, sizeof(out)) == 0)
    { update_reason(frame.api->last_reason_v1()); out = {}; return false; }
    const uint32_t depth = out.flags & (MGPU_NI_SAMPLE_DEPTH_F32 | MGPU_NI_SAMPLE_DEPTH_U24 | MGPU_NI_SAMPLE_DEPTH_U16);
    const uint32_t known = MGPU_NI_SAMPLE_MV_HALF | MGPU_NI_SAMPLE_TRACKED_STATES |
        MGPU_NI_SAMPLE_DEPTH_F32 | MGPU_NI_SAMPLE_DEPTH_U24 | MGPU_NI_SAMPLE_DEPTH_U16;
    const bool depth_ok = depth == MGPU_NI_SAMPLE_DEPTH_F32 || depth == MGPU_NI_SAMPLE_DEPTH_U24 ||
        depth == MGPU_NI_SAMPLE_DEPTH_U16;
    const bool encoding_matches =
        (frame.data.depth_format == DXGI_FORMAT_R32_TYPELESS || frame.data.depth_format == DXGI_FORMAT_D32_FLOAT ||
         frame.data.depth_format == DXGI_FORMAT_R32_FLOAT) ? depth == MGPU_NI_SAMPLE_DEPTH_F32 :
        (frame.data.depth_format == DXGI_FORMAT_R24G8_TYPELESS || frame.data.depth_format == DXGI_FORMAT_D24_UNORM_S8_UINT) ?
            depth == MGPU_NI_SAMPLE_DEPTH_U24 :
        (frame.data.depth_format == DXGI_FORMAT_R16_TYPELESS || frame.data.depth_format == DXGI_FORMAT_D16_UNORM) ?
            depth == MGPU_NI_SAMPLE_DEPTH_U16 : false;
    if (!encoding_matches)
    {
        out = {};
        update_reason("sample depth encoding contradicts the native resource format");
        return false;
    }
    if (out.struct_size != sizeof(out) || out.abi_version != MGPU_NATIVE_INPUT_ABI ||
        out.count != MGPU_NATIVE_INPUT_SAMPLE_COUNT || out.frame_id != frame.data.frame_id ||
        out.generation != frame.data.generation || (out.flags & known) != known ||
        (out.flags & ~known) != 0u || !depth_ok)
    {
        out = {};
        update_reason("sample ABI, frame, generation or format is invalid");
        return false;
    }
    for (unsigned i = 0; i < out.count; ++i)
    {
        if (out.x[i] >= frame.data.mv_width || out.y[i] >= frame.data.mv_height)
        {
            out = {};
            update_reason("sample pixel coordinates exceed the native resources");
            return false;
        }
    }
    return true;
}

bool current(const lease &frame)
{
    if (!frame.held || frame.api == nullptr || frame.api != g_api || g_generation == 0 ||
        frame.data.generation != g_generation || frame.api->generation_v1() != g_generation)
    {
        update_reason("native history changed before presentation; generated colour withheld");
        return false;
    }
    return true;
}

const char *last_reason() { return g_reason; }

void note_reason(const char *reason) { update_reason(reason); }

#if defined(MGPU_NATIVE_INPUT_TEST)
void use_api_for_test(const mgpu_native_input_api_v1 *api)
{
    g_api = api;
    g_tried = api != nullptr;
    g_generation = api != nullptr ? api->generation_v1() : 0u;
}
#endif

void reset()
{
    // FG releases every set lease before calling this on the bridge thread.
    if (g_api != nullptr && g_api->bind_color_v1 != nullptr) g_api->bind_color_v1(0, 0, 0);
    g_generation = 0;
    g_tried = false;
    g_api = nullptr;
    update_reason("native input reader reset");
}
}
