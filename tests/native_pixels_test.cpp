#include "native_pixels.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace mgpu::diag { void info(const char *) {} void warn(const char *) {} void error(const char *) {} }

static void mul(const float *a, const float *b, float *out)
{
    double t[16]{};
    for (unsigned c = 0; c < 4; ++c) for (unsigned r = 0; r < 4; ++r)
        for (unsigned k = 0; k < 4; ++k) t[c * 4 + r] += (double)a[k * 4 + r] * b[c * 4 + k];
    for (unsigned i = 0; i < 16; ++i) out[i] = (float)t[i];
}

static bool inverse(const float *m, float *out)
{
    double a[4][8]{};
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
    {
        a[r][c] = m[c * 4 + r];
        a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
    for (unsigned c = 0; c < 4; ++c)
    {
        unsigned pivot = c;
        for (unsigned r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (std::abs(a[pivot][c]) < 1e-12) return false;
        for (unsigned k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double d = a[c][c];
        for (double &v : a[c]) v /= d;
        for (unsigned r = 0; r < 4; ++r) if (r != c)
        {
            const double f = a[r][c];
            for (unsigned k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) out[c * 4 + r] = (float)a[r][c + 4];
    return true;
}

static void project(const float *m, const double *v, double *out)
{
    for (unsigned r = 0; r < 4; ++r)
    {
        out[r] = 0;
        for (unsigned c = 0; c < 4; ++c) out[r] += (double)m[c * 4 + r] * v[c];
    }
}

static uint16_t half(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const unsigned sign = bits >> 31;
    int exponent = (int)((bits >> 23) & 255u) - 127 + 15;
    unsigned fraction = bits & 0x7fffffu;
    if (exponent >= 31) return (uint16_t)((sign << 15) | 0x7c00u);
    if (exponent <= 0)
    {
        if (exponent < -10) return (uint16_t)(sign << 15);
        fraction |= 0x800000u;
        const unsigned shift = (unsigned)(14 - exponent);
        return (uint16_t)((sign << 15) | (fraction >> shift));
    }
    return (uint16_t)((sign << 15) | ((unsigned)exponent << 10) | (fraction >> 13));
}

static mgpu_native_input_v1 make_frame()
{
    mgpu_native_input_v1 f{};
    f.struct_size = sizeof(f);
    f.abi_version = MGPU_NATIVE_INPUT_ABI;
    f.frame_id = 1;
    f.game_frame = 1;
    f.previous_game_frame = 0;
    f.transfer.struct_size = sizeof(f.transfer);
    f.transfer.abi_version = MGPU_NATIVE_INPUT_ABI;
    f.transfer.heap_handle = 1;
    f.transfer.fence_handle = 2;
    f.transfer.fence_value = 1;
    f.transfer.slot_bytes = 3 * 1024 * 1024;
    f.transfer.heap_bytes = 3 * f.transfer.slot_bytes;
    f.transfer.mv_bytes = f.transfer.depth_bytes = 1024 * 1024;
    f.transfer.depth_offset = f.transfer.mv_bytes;
    f.transfer.mv_row_pitch = f.transfer.depth_row_pitch = 640 * 4;
    f.transfer.mv_rows = f.transfer.depth_rows = 360;
    f.transfer.mv_copy_format = DXGI_FORMAT_R16G16_FLOAT;
    f.transfer.depth_copy_format = DXGI_FORMAT_R32_FLOAT;
    f.generation = 9;
    f.qpc = 10000000;
    f.cookie = 0;
    f.game_luid = 0x1234;
    f.process_id = GetCurrentProcessId();
    f.mv_width = f.depth_width = f.color_width = 640;
    f.mv_height = f.depth_height = f.color_height = 360;
    f.mv_format = DXGI_FORMAT_R16G16_FLOAT;
    f.depth_format = DXGI_FORMAT_R32_FLOAT;
    f.mv_resource = (void *)1;
    f.depth_resource = (void *)2;
    f.flags = MGPU_NI_F_DX12 | MGPU_NI_F_CAMERA | MGPU_NI_F_CAMERA_HISTORY |
        MGPU_NI_F_MOTION_VECTORS | MGPU_NI_F_DEPTH | MGPU_NI_F_GAMEPLAY_KNOWN |
        MGPU_NI_F_GAMEPLAY | MGPU_NI_F_MATCHED_FRAME | MGPU_NI_F_ENGINE_COPY_DONE |
        MGPU_NI_F_RESOURCE_BOUNDS | MGPU_NI_F_TRACKED_COPY | MGPU_NI_F_CLIP_SPACE_MV |
        MGPU_NI_F_CROSS_ADAPTER | MGPU_NI_F_COLOR_MATCHED;
    f.camera_near = 0.1f;
    f.camera_far = 1000.0f;
    f.camera_fov_y = 1.0f;
    f.camera_aspect = 640.0f / 360.0f;
    f.camera_right[0] = f.camera_up[1] = 1.0f;
    f.camera_forward[2] = -1.0f;
    const float ys = 1.0f / std::tan(0.5f), xs = ys / f.camera_aspect;
    const float n = f.camera_near, far_z = f.camera_far;
    const float a = n / (far_z - n), b = far_z * n / (far_z - n);
    const float p[16] = {xs,0,0,0, 0,ys,0,0, 0,0,a,-1, 0,0,b,0};
    const float v[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, -0.02f,0.0f,-0.4f,1};
    float pv[16], ip[16], ipv[16], ivp[16], ipv_full[16], iprev[16];
    mul(p, v, pv);
    assert(inverse(p, ip));
    assert(inverse(v, ipv));
    mul(ipv, ip, ipv_full);
    assert(inverse(pv, ivp));
    assert(inverse(p, iprev));
    std::memcpy(f.view_to_clip, p, sizeof(p));
    std::memcpy(f.clip_to_view, ip, sizeof(ip));
    mul(p, ipv_full, f.clip_to_prev_clip); // Previous view is identity.
    mul(pv, iprev, f.prev_clip_to_clip);
    (void)ivp;
    return f;
}

static mgpu_native_input_sample_v1 make_samples(const mgpu_native_input_v1 &f)
{
    mgpu_native_input_sample_v1 s{};
    s.struct_size = sizeof(s);
    s.abi_version = MGPU_NATIVE_INPUT_ABI;
    s.count = MGPU_NATIVE_INPUT_SAMPLE_COUNT;
    s.flags = MGPU_NI_SAMPLE_MV_HALF | MGPU_NI_SAMPLE_DEPTH_F32 | MGPU_NI_SAMPLE_TRACKED_STATES;
    s.frame_id = f.frame_id;
    s.generation = f.generation;
    for (unsigned i = 0; i < s.count; ++i)
    {
        const float depth = 0.7f + 0.2f * (float)((i * 37u) % 100u) / 100.0f;
        const double x = (i % 40u) * 16.0 + 0.5, y = (i / 40u) * 9.0 + 0.5;
        const double current[4] = {x * 2.0 / f.mv_width - 1.0, 1.0 - y * 2.0 / f.mv_height, depth, 1.0};
        double previous[4];
        project(f.clip_to_prev_clip, current, previous);
        const double px = (previous[0] / previous[3] * 0.5 + 0.5) * f.mv_width - 0.5;
        const double py = (0.5 - previous[1] / previous[3] * 0.5) * f.mv_height - 0.5;
        s.x[i] = (uint16_t)(i % 40u * 16u);
        s.y[i] = (uint16_t)(i / 40u * 9u);
        s.mv_raw[i * 2u] = half((float)(px - x));
        s.mv_raw[i * 2u + 1u] = half((float)(py - y));
        std::memcpy(&s.depth_raw[i], &depth, sizeof(depth));
    }
    return s;
}

int main()
{
    assert(mgpu::native_input::half_to_float(0x3c00) == 1.0f);
    assert(mgpu::native_input::half_to_float(0xbc00) == -1.0f);
    assert(std::abs(mgpu::native_input::half_to_float(0x3555) - 0.33325195f) < 1e-7f);
    assert(!std::isfinite(mgpu::native_input::half_to_float(0x7c01)));
    float depth = 0.0f;
    assert(mgpu::native_input::decode_depth(0x00800000u, MGPU_NI_SAMPLE_DEPTH_U24, depth));
    assert(std::abs(depth - 0.5f) < 1e-6f);
    assert(mgpu::native_input::decode_depth(0x8000u, MGPU_NI_SAMPLE_DEPTH_U16, depth));
    assert(std::abs(depth - 0.5f) < 1e-4f);
    assert(!mgpu::native_input::decode_depth(0x3c00u, 0u, depth)); // Never interpret U16 as FP16.

    auto f = make_frame();
    auto s = make_samples(f);
    auto report = mgpu::native_input::evaluate_pixels(f, s);
    assert(report.passed && report.reverse_z && report.camera_motion >= 16);
    assert(report.median_error_px <= 0.35 && report.p90_error_px <= 1.5);
    auto zeros = s;
    std::memset(zeros.mv_raw, 0, sizeof(zeros.mv_raw));
    assert(!mgpu::native_input::evaluate_pixels(f, zeros).passed);
    auto bad = s;
    bad.mv_raw[20] = 0x7c01;
    (void)mgpu::native_input::evaluate_pixels(f, bad); // NaN is rejected sample by sample.
    auto outliers = s;
    for (unsigned i = 0; i < 200; ++i) outliers.mv_raw[i * 2u] = half(40.0f);
    assert(!mgpu::native_input::evaluate_pixels(f, outliers).passed);
    auto depth16 = s;
    depth16.flags = MGPU_NI_SAMPLE_MV_HALF | MGPU_NI_SAMPLE_DEPTH_U16 | MGPU_NI_SAMPLE_TRACKED_STATES;
    for (unsigned i = 0; i < depth16.count; ++i)
    {
        float d;
        std::memcpy(&d, &s.depth_raw[i], sizeof(d));
        depth16.depth_raw[i] = (uint32_t)std::lround(d * 65535.0f);
    }
    assert(mgpu::native_input::evaluate_pixels(f, depth16).passed);

    mgpu::native_input::pixel_validator validator;
    constexpr uint64_t frequency = 10000000;
    for (unsigned i = 0; i < 6; ++i)
    {
        f.frame_id = i * 2u + 1u; // Native snapshots may skip game frames.
        f.game_frame = i + 2;
        f.previous_game_frame = i + 1; // Exact previous NEURAL output, not native frame_id - 1.
        f.qpc = 10000000 + i * frequency / 60;
        s = make_samples(f);
        const bool ready = validator.observe(f, s, frequency);
        assert(ready == (i == 5));
        auto authorized = f;
        assert(validator.authorize(authorized) == (i == 5));
        if (i == 5)
        {
            assert((authorized.flags & MGPU_NI_F_GPU_VERIFIED) != 0);
            assert((authorized.flags & MGPU_NI_F_REVERSE_Z) != 0);
            assert((f.flags & MGPU_NI_F_GPU_VERIFIED) == 0); // Provider data is never self-approved.
        }
    }
    f.frame_id = 7;
    f.qpc += frequency; // A one-second gap cannot use the previous verification window.
    s = make_samples(f);
    assert(!validator.observe(f, s, frequency));
    assert(!validator.authorize(f));
    validator.reset();
    f.frame_id = 7;
    f.qpc = frequency;
    s = make_samples(f);
    assert(!validator.observe(f, s, frequency));
    f.frame_id = 8;
    f.qpc += frequency / 60;
    s = make_samples(f);
    s.frame_id = 9;
    assert(!validator.observe(f, s, frequency));
    std::puts("FP16 decode, F32/U24/U16 depth, reprojection, zero-vector rejection and six-frame gate: PASS");
}
