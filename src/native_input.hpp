#pragma once

// Experimental RE2 native-input reader. This module cannot enable frame generation.
// The provider is already loaded in this process; never load an arbitrary DLL here.
#include <cstdint>
#include <d3d12.h>
#include "mgpu_native_input_v1.h"

namespace mgpu::native_input
{
struct lease
{
    const mgpu_native_input_api_v1 *api = nullptr;
    mgpu_native_input_v1 data{};
    bool held = false;

    lease() = default;
    lease(const lease &) = delete;
    lease &operator=(const lease &) = delete;
    lease(lease &&other) noexcept;
    lease &operator=(lease &&other) noexcept;
    ~lease();
    void reset();
};

struct header_report
{
    bool compatible = false;
    const char *reason = "not checked";
    uint64_t missing = 0;
};

// Sufficient for the diagnostic copy, never for interpolation: GPU_VERIFIED,
// history and a proven reverse-Z convention must be supplied by later checks.
header_report inspect_header(const mgpu_native_input_v1 &s, uint64_t expected_game_luid,
                             uint64_t expected_generation, int require_pixels);

// Called only on the bridge thread. Looks up dinput8.dll without loading it.
bool acquire(uint64_t minimum_frame, uint64_t expected_game_luid, lease &out);
// Exact neural-color correspondence. The old latest-frame API is diagnostics only.
bool acquire_for_color(uint64_t game_frame, uint64_t expected_game_luid, lease &out);
// The producer reports a color seal and the last SUCCESSFUL neural output.
void bind_color_frame(uint64_t game_frame, uint64_t previous_game_frame, uint64_t seal_qpc);
// GPU0 readback is performed by the provider using its observed queue/states.
// Success proves transfer, NOT motion-vector correctness or permission for FG.
bool sample(const lease &frame, mgpu_native_input_sample_v1 &out);
// Recheck before presentation, after GPU work, not only at acquisition.
bool current(const lease &frame);
const char *last_reason();
void note_reason(const char *reason);
void reset();
#if defined(MGPU_NATIVE_INPUT_TEST)
void use_api_for_test(const mgpu_native_input_api_v1 *api);
#endif
}
