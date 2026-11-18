#include "native_pixels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace mgpu::native_input
{
namespace
{
constexpr unsigned MIN_FRAMES = 6;
constexpr unsigned MIN_MOTION_SAMPLES = 16;
constexpr unsigned MIN_USABLE_SAMPLES = 128;
constexpr double MIN_MOTION_PX = 0.15;
constexpr double MATCH_ERROR_PX = 0.35;
constexpr double MEDIAN_LIMIT_PX = 0.35;
constexpr double P90_LIMIT_PX = 1.5;
constexpr double MATCH_RATIO_LIMIT = 0.85;

bool multiply(const float *matrix, const double *v, double *out)
{
    for (unsigned row = 0; row < 4; ++row)
    {
        out[row] = 0.0;
        for (unsigned col = 0; col < 4; ++col) out[row] += (double)matrix[col * 4 + row] * v[col];
        if (!std::isfinite(out[row])) return false;
    }
    return true;
}

bool reverse_z_projection(const mgpu_native_input_v1 &f)
{
    // Candidate encoding only. Actual pixels must still reproject correctly.
    const float a = f.view_to_clip[10], w = f.view_to_clip[11], b = f.view_to_clip[14];
    return std::isfinite(a) && std::isfinite(w) && std::isfinite(b) &&
        a >= -1.0e-5f && a < 0.05f && w < -0.5f && b > 0.0f;
}
}

float half_to_float(uint16_t h)
{
    const unsigned exponent = (h >> 10u) & 31u;
    const unsigned fraction = h & 1023u;
    const float sign = (h & 0x8000u) != 0u ? -1.0f : 1.0f;
    if (exponent == 0u) return sign * std::ldexp((float)fraction, -24);
    if (exponent == 31u)
        return fraction == 0u ? sign * std::numeric_limits<float>::infinity()
                              : std::numeric_limits<float>::quiet_NaN();
    return sign * std::ldexp((float)(fraction + 1024u), (int)exponent - 25);
}

bool decode_depth(uint32_t raw, uint32_t flags, float &depth)
{
    if ((flags & MGPU_NI_SAMPLE_DEPTH_F32) != 0u)
    {
        static_assert(sizeof(raw) == sizeof(depth), "F32 depth storage");
        std::memcpy(&depth, &raw, sizeof(depth));
    }
    else if ((flags & MGPU_NI_SAMPLE_DEPTH_U24) != 0u)
        depth = (float)(raw & 0x00ffffffu) / 16777215.0f;
    else if ((flags & MGPU_NI_SAMPLE_DEPTH_U16) != 0u)
        depth = (float)(raw & 0xffffu) / 65535.0f;
    else return false;
    return std::isfinite(depth);
}

sample_report evaluate_pixels(const mgpu_native_input_v1 &frame,
                              const mgpu_native_input_sample_v1 &samples)
{
    sample_report result{};
    if (samples.struct_size != sizeof(samples) || samples.abi_version != MGPU_NATIVE_INPUT_ABI ||
        samples.count != MGPU_NATIVE_INPUT_SAMPLE_COUNT || samples.frame_id != frame.frame_id ||
        samples.generation != frame.generation || frame.mv_width == 0 || frame.mv_height == 0 ||
        frame.mv_width != frame.depth_width || frame.mv_height != frame.depth_height ||
        (samples.flags & MGPU_NI_SAMPLE_MV_HALF) == 0u ||
        (samples.flags & MGPU_NI_SAMPLE_TRACKED_STATES) == 0u)
    {
        result.reason = "sample identity, dimensions or tracked format is invalid";
        return result;
    }
    const uint32_t depth_kind = samples.flags & (MGPU_NI_SAMPLE_DEPTH_F32 |
        MGPU_NI_SAMPLE_DEPTH_U24 | MGPU_NI_SAMPLE_DEPTH_U16);
    if (depth_kind != MGPU_NI_SAMPLE_DEPTH_F32 && depth_kind != MGPU_NI_SAMPLE_DEPTH_U24 &&
        depth_kind != MGPU_NI_SAMPLE_DEPTH_U16)
    {
        result.reason = "depth is not F32, U24 or U16 storage";
        return result;
    }
    result.reverse_z = reverse_z_projection(frame);
    std::vector<double> errors;
    errors.reserve(samples.count);
    const double width = frame.mv_width, height = frame.mv_height;
    for (unsigned i = 0; i < samples.count; ++i)
    {
        if (samples.x[i] >= frame.mv_width || samples.y[i] >= frame.mv_height)
        {
            result.reason = "sample lies outside the texture";
            return result;
        }
        float depth = 0.0f;
        if (!decode_depth(samples.depth_raw[i], samples.flags, depth) ||
            !(depth > 1.0e-7f && depth < 1.0f - 1.0e-7f)) continue;
        const double current_x = (double)samples.x[i] + 0.5;
        const double current_y = (double)samples.y[i] + 0.5;
        const double current_clip[4] = {current_x * 2.0 / width - 1.0,
            1.0 - current_y * 2.0 / height, (double)depth, 1.0};
        double view[4], reconstructed[4], previous_clip[4];
        if (!multiply(frame.clip_to_view, current_clip, view) || std::abs(view[3]) < 1.0e-10 ||
            !multiply(frame.view_to_clip, view, reconstructed) || std::abs(reconstructed[3]) < 1.0e-10 ||
            !multiply(frame.clip_to_prev_clip, current_clip, previous_clip) ||
            std::abs(previous_clip[3]) < 1.0e-10) continue;
        bool round_trip = true;
        for (unsigned k = 0; k < 3; ++k)
        {
            if (!std::isfinite(reconstructed[k] / reconstructed[3]) ||
                std::abs(reconstructed[k] / reconstructed[3] - current_clip[k]) > 2.0e-3)
                round_trip = false;
        }
        if (!round_trip) continue;
        ++result.usable;
        const double previous_x = (previous_clip[0] / previous_clip[3] * 0.5 + 0.5) * width - 0.5;
        const double previous_y = (0.5 - previous_clip[1] / previous_clip[3] * 0.5) * height - 0.5;
        if (!std::isfinite(previous_x) || !std::isfinite(previous_y) ||
            std::abs(previous_x) > width * 16.0 || std::abs(previous_y) > height * 16.0) continue;
        const double expected_x = previous_x - current_x;
        const double expected_y = previous_y - current_y;
        const float observed_x = half_to_float(samples.mv_raw[i * 2u]);
        const float observed_y = half_to_float(samples.mv_raw[i * 2u + 1u]);
        if (!std::isfinite(observed_x) || !std::isfinite(observed_y) ||
            std::abs(observed_x) > width * 4.0 || std::abs(observed_y) > height * 4.0) continue;
        if (std::abs(observed_x) + std::abs(observed_y) > 1.0e-4) ++result.nonzero_vectors;
        if (std::hypot(expected_x, expected_y) < MIN_MOTION_PX) continue;
        ++result.camera_motion;
        const double error = std::hypot((double)observed_x - expected_x, (double)observed_y - expected_y);
        errors.push_back(error);
        if (error <= MATCH_ERROR_PX) ++result.matched;
    }
    if (errors.empty())
    {
        result.reason = "no finite depth samples with actual camera motion";
        return result;
    }
    std::sort(errors.begin(), errors.end());
    const auto percentile = [&errors](double p) {
        const double at = p * (double)(errors.size() - 1u);
        const auto low = (size_t)std::floor(at), high = (size_t)std::ceil(at);
        const double f = at - (double)low;
        return errors[low] * (1.0 - f) + errors[high] * f;
    };
    result.median_error_px = percentile(0.50);
    result.p90_error_px = percentile(0.90);
    result.match_ratio = (double)result.matched / (double)errors.size();
    result.passed = result.reverse_z && result.usable >= MIN_USABLE_SAMPLES &&
        result.camera_motion >= MIN_MOTION_SAMPLES && result.nonzero_vectors >= MIN_MOTION_SAMPLES &&
        result.median_error_px <= MEDIAN_LIMIT_PX && result.p90_error_px <= P90_LIMIT_PX &&
        result.match_ratio >= MATCH_RATIO_LIMIT;
    result.reason = !result.reverse_z ? "projection is not a supported reverse-Z candidate" :
        result.usable < MIN_USABLE_SAMPLES ? "too few finite depth samples" :
        result.camera_motion < MIN_MOTION_SAMPLES ? "camera is stationary; this frame cannot prove vectors" :
        result.nonzero_vectors < MIN_MOTION_SAMPLES ? "motion-vector buffer is zero or nearly empty" :
        !result.passed ? "motion-vector pixel errors exceed the fixed limits" : "pixel reprojection passed";
    return result;
}

pixel_validator::pixel_validator() { reset(); }

void pixel_validator::reset()
{
    generation_ = last_frame_ = last_native_frame_ = last_qpc_ = 0;
    mv_format_ = depth_format_ = width_ = height_ = 0;
    good_frames_ = 0;
    verified_ = false;
    report_ = {};
    std::snprintf(reason_, sizeof(reason_), "native pixels not verified");
}

bool pixel_validator::fail(const char *reason)
{
    last_frame_ = last_native_frame_ = last_qpc_ = 0;
    good_frames_ = 0;
    verified_ = false;
    std::snprintf(reason_, sizeof(reason_), "%s", reason);
    return false;
}

bool pixel_validator::observe(const mgpu_native_input_v1 &frame,
                              const mgpu_native_input_sample_v1 &samples, uint64_t qpc_frequency)
{
    const auto header = inspect_header(frame, frame.game_luid, frame.generation, 0);
    if (!header.compatible) return fail(header.reason);
    if ((frame.flags & (MGPU_NI_F_REVERSE_Z | MGPU_NI_F_GPU_VERIFIED)) != 0u)
        return fail("provider tried to self-authorize pixel verification");
    if (generation_ != frame.generation)
    {
        reset();
        generation_ = frame.generation;
    }
    if (qpc_frequency == 0 || frame.qpc == 0) return fail("native frame timestamp is unavailable");
    report_ = evaluate_pixels(frame, samples);
    if (!report_.passed) return fail(report_.reason);
    if (frame.game_frame == 0 || frame.previous_game_frame == 0 ||
        frame.previous_game_frame >= frame.game_frame)
        return fail("native snapshot has no exact previous neural-color anchor");
    if (last_frame_ != 0 && (frame.previous_game_frame != last_frame_ ||
        frame.game_frame <= last_frame_ || last_qpc_ >= frame.qpc ||
        (frame.qpc - last_qpc_) > qpc_frequency / 10u))
        return fail("pixel-verified neural frames are not contiguous in a valid time interval");
    if (last_frame_ != 0 && (mv_format_ != frame.mv_format || depth_format_ != frame.depth_format ||
        width_ != frame.mv_width || height_ != frame.mv_height))
        return fail("native resource dimensions or formats changed");
    mv_format_ = frame.mv_format;
    depth_format_ = frame.depth_format;
    width_ = frame.mv_width;
    height_ = frame.mv_height;
    last_frame_ = frame.game_frame;
    last_native_frame_ = frame.frame_id;
    last_qpc_ = frame.qpc;
    if (good_frames_ < MIN_FRAMES) ++good_frames_;
    verified_ = good_frames_ >= MIN_FRAMES;
    std::snprintf(reason_, sizeof(reason_),
        "pixel window %u/%u: motion=%u, matched=%.1f%%, median=%.3f px, P90=%.3f px%s",
        good_frames_, MIN_FRAMES, report_.camera_motion, report_.match_ratio * 100.0,
        report_.median_error_px, report_.p90_error_px, verified_ ? "; VERIFIED" : "; waiting");
    return verified_;
}

bool pixel_validator::authorize(mgpu_native_input_v1 &frame) const
{
    if (!verified_ || generation_ == 0 || frame.generation != generation_ ||
        frame.game_frame != last_frame_ || frame.frame_id != last_native_frame_ ||
        frame.mv_width != width_ || frame.mv_height != height_ ||
        frame.mv_format != mv_format_ || frame.depth_format != depth_format_ ||
        !mgpu_native_input_transfer_valid(&frame)) return false;
    frame.flags |= MGPU_NI_F_REVERSE_Z | MGPU_NI_F_GPU_VERIFIED | MGPU_NI_F_PIXEL_HISTORY;
    frame.mv_scale_x = 0.5f * (float)frame.mv_width;
    frame.mv_scale_y = -0.5f * (float)frame.mv_height;
    return true;
}
}
