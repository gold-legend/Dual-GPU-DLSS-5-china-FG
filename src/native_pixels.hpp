#pragma once

#include <cstdint>
#include "native_input.hpp"

namespace mgpu::native_input
{
struct sample_report
{
    unsigned usable = 0;
    unsigned camera_motion = 0;
    unsigned nonzero_vectors = 0;
    unsigned matched = 0;
    double median_error_px = 0.0;
    double p90_error_px = 0.0;
    double match_ratio = 0.0;
    bool reverse_z = false;
    bool passed = false;
    const char *reason = "not evaluated";
};

float half_to_float(uint16_t bits);
bool decode_depth(uint32_t raw, uint32_t sample_flags, float &depth);

// Compare previous-current pixel motion, in PIXELS, against the real matrices.
// No matrix or depth convention is inferred from an all-zero vector buffer.
sample_report evaluate_pixels(const mgpu_native_input_v1 &frame,
                              const mgpu_native_input_sample_v1 &samples);

class pixel_validator
{
public:
    pixel_validator();
    void reset();
    bool observe(const mgpu_native_input_v1 &frame,
                 const mgpu_native_input_sample_v1 &samples, uint64_t qpc_frequency);
    // Only this bridge-side method may mark a frame GPU_VERIFIED / REVERSE_Z.
    bool authorize(mgpu_native_input_v1 &frame) const;
    const char *reason() const { return reason_; }
    unsigned consecutive_frames() const { return good_frames_; }
    const sample_report &last_report() const { return report_; }

private:
    bool fail(const char *reason);
    uint64_t generation_ = 0;
    uint64_t last_frame_ = 0;        // Last validated NEURAL color frame.
    uint64_t last_native_frame_ = 0; // The provider's native snapshot may skip frames.
    uint64_t last_qpc_ = 0;
    uint32_t mv_format_ = 0;
    uint32_t depth_format_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    unsigned good_frames_ = 0;
    bool verified_ = false;
    sample_report report_{};
    char reason_[192]{};
};
}
