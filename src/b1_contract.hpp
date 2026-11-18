#pragma once

// CPU-only contract shared by the D3D12 path and its off-device fault tests.
// A displayed frame may consume only the answer produced for that exact frame
// and the current stream generation. No rule here permits retaining old NR.
namespace mgpu::b1
{
    enum class answer_status
    {
        accept = 0,
        no_storage,
        missing,
        stale,
        future,
        old_generation
    };

    constexpr bool nr_due(unsigned long long frame, unsigned cadence)
    {
        return frame != 0ull && cadence != 0u && ((frame - 1ull) % cadence) == 0ull;
    }

    // Before frame F reuses a ring slot, the consumer must have retired F-RING.
    // Zero means that the first ring of frames needs no backward wait.
    constexpr unsigned long long ring_release_target(unsigned long long frame, unsigned ring)
    {
        return (ring != 0u && frame > (unsigned long long)ring)
            ? frame - (unsigned long long)ring : 0ull;
    }

    struct extent
    {
        unsigned width, height;
    };

    // Every pass owns its work extent. The feature itself is created at the
    // native cap; these dimensions are the per-evaluate subrect and the exact
    // answer extent carried by that frame's seal.
    constexpr extent pass_extent(unsigned native_w, unsigned native_h, float scale)
    {
        if (!(scale > 0.05f)) scale = 0.05f;
        if (scale > 1.0f) scale = 1.0f;
        unsigned w = (unsigned)((double)native_w * (double)scale + 0.5);
        unsigned h = (unsigned)((double)native_h * (double)scale + 0.5);
        if (w < 128u) w = 128u;
        if (h < 128u) h = 128u;
        w &= ~1u; h &= ~1u;
        if (w > native_w) w = native_w & ~1u;
        if (h > native_h) h = native_h & ~1u;
        return {w, h};
    }

    // Ring storage is allocated at the maximum extent, while each seal names
    // the actual final-pass extent. This permits live pass/scale changes without
    // reinterpretating an older frame using the newest slider values.
    constexpr bool valid_answer_extent(unsigned w, unsigned h,
                                       unsigned max_w, unsigned max_h)
    {
        return w >= 128u && h >= 128u && w <= max_w && h <= max_h &&
               (w & 1u) == 0u && (h & 1u) == 0u;
    }

    constexpr bool valid_answer_pass(unsigned pass, unsigned active_passes)
    {
        return pass < 3u && pass < active_passes && active_passes <= 3u;
    }

    constexpr answer_status classify_answer(
        unsigned long long storage_bytes,
        unsigned long long answer_frame,
        unsigned long long display_frame,
        unsigned long long answer_generation,
        unsigned long long current_generation)
    {
        if (storage_bytes == 0ull) return answer_status::no_storage;
        if (answer_frame == 0ull) return answer_status::missing;
        if (answer_frame < display_frame) return answer_status::stale;
        if (answer_frame > display_frame) return answer_status::future;
        if (answer_generation != current_generation) return answer_status::old_generation;
        return answer_status::accept;
    }

    constexpr const char *status_name(answer_status s)
    {
        switch (s)
        {
            case answer_status::accept: return "accept";
            case answer_status::no_storage: return "no-storage";
            case answer_status::missing: return "missing";
            case answer_status::stale: return "stale";
            case answer_status::future: return "future";
            case answer_status::old_generation: return "old-generation";
        }
        return "unknown";
    }
}
