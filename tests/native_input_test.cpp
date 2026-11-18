#include "native_input.hpp"
#include <cassert>
#include <cstdio>
#include <limits>

namespace mgpu::diag { void info(const char *) {} void warn(const char *) {} void error(const char *) {} }

static mgpu_native_input_v1 valid()
{
    mgpu_native_input_v1 s{};
    s.struct_size = sizeof(s);
    s.abi_version = MGPU_NATIVE_INPUT_ABI;
    s.cookie = 1;
    s.frame_id = 7;
    s.game_frame = 7;
    s.previous_game_frame = 3;
    s.transfer.struct_size = sizeof(s.transfer);
    s.transfer.abi_version = MGPU_NATIVE_INPUT_ABI;
    s.transfer.heap_handle = 1;
    s.transfer.fence_handle = 2;
    s.transfer.fence_value = 1;
    s.transfer.slot_bytes = 4 * 1024 * 1024;
    s.transfer.heap_bytes = 3 * s.transfer.slot_bytes;
    s.transfer.mv_bytes = s.transfer.depth_bytes = 1024 * 1024;
    s.transfer.depth_offset = s.transfer.mv_bytes;
    s.transfer.mv_row_pitch = s.transfer.depth_row_pitch = 1920 * 4;
    s.transfer.mv_rows = s.transfer.depth_rows = 1080;
    s.transfer.mv_copy_format = DXGI_FORMAT_R16G16_FLOAT;
    s.transfer.depth_copy_format = DXGI_FORMAT_R16_UNORM;
    s.generation = 3;
    s.game_luid = 0x12345678;
    s.process_id = GetCurrentProcessId();
    s.mv_width = s.depth_width = s.color_width = 1920;
    s.mv_height = s.depth_height = s.color_height = 1080;
    s.mv_format = DXGI_FORMAT_R16G16_FLOAT;
    s.depth_format = DXGI_FORMAT_D16_UNORM;
    s.mv_resource = reinterpret_cast<void *>(1);
    s.depth_resource = reinterpret_cast<void *>(2);
    s.flags = MGPU_NI_F_DX12 | MGPU_NI_F_CAMERA | MGPU_NI_F_CAMERA_HISTORY |
        MGPU_NI_F_MOTION_VECTORS | MGPU_NI_F_DEPTH | MGPU_NI_F_GAMEPLAY_KNOWN |
        MGPU_NI_F_GAMEPLAY | MGPU_NI_F_MATCHED_FRAME | MGPU_NI_F_ENGINE_COPY_DONE |
        MGPU_NI_F_RESOURCE_BOUNDS | MGPU_NI_F_TRACKED_COPY | MGPU_NI_F_CLIP_SPACE_MV |
        MGPU_NI_F_CROSS_ADAPTER | MGPU_NI_F_COLOR_MATCHED;
    for (unsigned k = 0; k < 16; k += 5) {
        s.view_to_clip[k] = s.clip_to_view[k] = s.clip_to_prev_clip[k] = s.prev_clip_to_clip[k] = 1.0f;
    }
    s.camera_forward[2] = -1.0f;
    s.camera_right[0] = s.camera_up[1] = 1.0f;
    s.camera_near = 0.1f;
    s.camera_far = 10000.0f;
    s.camera_fov_y = 1.0f;
    s.camera_aspect = 16.0f / 9.0f;
    return s;
}

int main()
{
    auto s = valid();
    auto r = mgpu::native_input::inspect_header(s, s.game_luid, s.generation, 0);
    assert(r.compatible);             // Diagnostics, not FG permission.
    r = mgpu::native_input::inspect_header(s, s.game_luid, s.generation, 1);
    assert(!r.compatible && r.missing != 0);
    s.flags |= MGPU_NI_F_REVERSE_Z | MGPU_NI_F_GPU_VERIFIED;
    r = mgpu::native_input::inspect_header(s, s.game_luid, s.generation, 1);
    assert(r.compatible && r.missing == 0);
    s.game_luid++;
    assert(!mgpu::native_input::inspect_header(s, 0x12345678, s.generation, 0).compatible);
    s = valid(); s.generation++;
    assert(!mgpu::native_input::inspect_header(s, s.game_luid, 3, 0).compatible);
    s = valid(); s.flags &= ~MGPU_NI_F_CAMERA_HISTORY;
    assert(!mgpu::native_input::inspect_header(s, s.game_luid, s.generation, 0).compatible);
    s = valid(); s.view_to_clip[1] = std::numeric_limits<float>::quiet_NaN();
    assert(!mgpu::native_input::inspect_header(s, s.game_luid, s.generation, 0).compatible);
    s = valid(); s.mv_width = 1280;
    assert(!mgpu::native_input::inspect_header(s, s.game_luid, s.generation, 0).compatible);
    s = valid(); s.flags &= ~MGPU_NI_F_GAMEPLAY;
    assert(!mgpu::native_input::inspect_header(s, s.game_luid, s.generation, 0).compatible);

    mgpu::native_input::lease empty;
    empty.reset();                    // Never call a null release function.

    static int acquired = 0, released = 0, mode = 0;
    static mgpu_native_input_v1 offered;
    const auto fake_acquire = [](uint64_t minimum, mgpu_native_input_v1 *out, uint32_t size) -> uint64_t {
        if (out == nullptr || size != sizeof(*out) || offered.frame_id < minimum) return 0;
        *out = offered;
        ++acquired;
        return out->frame_id;
    };
    const auto fake_release = [](const mgpu_native_input_v1 *frame) {
        assert(frame != nullptr && frame->cookie == 1 && frame->frame_id == offered.frame_id);
        ++released;
    };
    const auto fake_generation = []() -> uint64_t { return 3u; };
    const auto fake_reason = []() -> const char * { return "fake sample rejected"; };
    const auto fake_sample = [](const mgpu_native_input_v1 *frame, mgpu_native_input_sample_v1 *out, uint32_t size) -> int {
        if (frame == nullptr || out == nullptr || size != sizeof(*out) || mode == 0) return 0;
        *out = {};
        out->struct_size = sizeof(*out);
        out->abi_version = MGPU_NATIVE_INPUT_ABI;
        out->count = MGPU_NATIVE_INPUT_SAMPLE_COUNT;
        out->flags = MGPU_NI_SAMPLE_MV_HALF | MGPU_NI_SAMPLE_DEPTH_U16 | MGPU_NI_SAMPLE_TRACKED_STATES;
        out->frame_id = mode == 2 ? frame->frame_id + 1 : frame->frame_id;
        out->generation = frame->generation;
        if (mode == 3) out->x[0] = (uint16_t)frame->mv_width;
        return 1;
    };
    const auto fake_color = [](uint64_t frame, mgpu_native_input_v1 *out, uint32_t size) -> uint64_t {
        if (out == nullptr || size != sizeof(*out) || offered.game_frame != frame) return 0;
        *out = offered;
        ++acquired;
        return out->frame_id;
    };
    const auto fake_bind = [](uint64_t, uint64_t, uint64_t) -> uint64_t { return 1u; };
    mgpu_native_input_api_v1 api{MGPU_NATIVE_INPUT_MAGIC, sizeof(api), MGPU_NATIVE_INPUT_ABI, 3,
        fake_acquire, fake_release, fake_generation, fake_reason, fake_sample, fake_color, fake_bind};
    offered = valid();
    mgpu::native_input::use_api_for_test(&api);
    mgpu::native_input::lease held;
    assert(mgpu::native_input::acquire_for_color(7, offered.game_luid, held));
    held.reset();
    assert(acquired == 1 && released == 1);
    assert(mgpu::native_input::acquire_for_color(8, offered.game_luid, held) == false);
    assert(acquired == 1 && released == 1);
    assert(mgpu::native_input::acquire(7, offered.game_luid, held));
    assert(acquired == 2 && released == 1 && held.held);
    assert(mgpu::native_input::current(held));
    ++held.data.generation;
    assert(!mgpu::native_input::current(held));
    --held.data.generation;
    assert(mgpu::native_input::current(held));
    mgpu_native_input_sample_v1 pixels{};
    assert(!mgpu::native_input::sample(held, pixels)); // Provider failure cannot lose the lease.
    held.reset();
    assert(released == 2 && !held.held);

    mode = 2;
    assert(mgpu::native_input::acquire(7, offered.game_luid, held));
    assert(!mgpu::native_input::sample(held, pixels)); // Mismatched frame is rejected.
    held.reset();
    assert(released == 3);

    mode = 3;
    assert(mgpu::native_input::acquire(7, offered.game_luid, held));
    assert(!mgpu::native_input::sample(held, pixels)); // Out-of-bounds sample is rejected.
    held.reset();
    assert(released == 4);

    mode = 1;
    assert(mgpu::native_input::acquire(7, offered.game_luid, held));
    assert(mgpu::native_input::sample(held, pixels)); // Transfer success is NOT pixel verification.
    held.reset();
    assert(released == 5);

    assert(mgpu::native_input::acquire(7, offered.game_luid, held));
    offered.depth_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    assert(!mgpu::native_input::sample(held, pixels)); // The sample encoding must match the resource.
    offered.depth_format = DXGI_FORMAT_D16_UNORM;
    assert(mgpu::native_input::sample(held, pixels));
    held.reset();
    assert(released == 6);

    offered.game_luid ^= 1u;
    assert(!mgpu::native_input::acquire(7, s.game_luid, held)); // Rejected headers release too.
    assert(acquired == 7 && released == 7 && !held.held);
    mgpu::native_input::reset();
    std::puts("native input header, sampling and acquire/release failure tests passed (not a GPU or FG test)");
    return 0;
}
