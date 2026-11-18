#include "mvec_admission.hpp"

#include <cstdio>
#include <cstdlib>

namespace
{
    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "mvec admission regression: %s\n", message);
            std::exit(1);
        }
    }
}

int main()
{
    using mgpu::mvec_extract::admit_velocity_resource;
    using mgpu::mvec_extract::exact_scene_size;

    // The original scene band is unchanged before and after source selection.
    require(admit_velocity_resource(10, 1920, 1080, 1920, 1080, false),
            "scene-sized RGBA16F must be admitted");
    require(admit_velocity_resource(11, 960, 540, 1920, 1080, false),
            "half-resolution RGBA16 UNORM must be admitted");
    require(!admit_velocity_resource(13, 959, 540, 1920, 1080, true),
            "the lower scene-band bound must remain enforced");

    // RE2 launch order: a 4K G-buffer can exist while the chain is still 1080p.
    require(!admit_velocity_resource(10, 3840, 2160, 1920, 1080, false),
            "an oversized texture must stay rejected after source selection");
    require(admit_velocity_resource(10, 3840, 2160, 1920, 1080, true),
            "an early final-resolution candidate must not be lost permanently");
    require(exact_scene_size(3840, 2160, 3840, 2160),
            "the early candidate must match after the chain resize");
    require(!exact_scene_size(1920, 1080, 3840, 2160) &&
            !exact_scene_size(3839, 2160, 3840, 2160),
            "only the exact resized dimensions may enter source decision");

    // Widening admission must not widen formats or accept unreasonable sizes.
    require(!admit_velocity_resource(12, 3840, 2160, 1920, 1080, true) &&
            !admit_velocity_resource(16, 3840, 2160, 1920, 1080, true) &&
            !admit_velocity_resource(2, 3840, 2160, 1920, 1080, true),
            "only RGBA16 float/unorm/snorm formats are eligible");
    require(!admit_velocity_resource(10, 8192, 4320, 1920, 1080, true) &&
            !admit_velocity_resource(10, 3840, 8192, 1920, 1080, true),
            "startup upscale remains bounded to four times each dimension");
    require(!admit_velocity_resource(10, 7681, 4320, 1920, 1080, true) &&
            !admit_velocity_resource(10, 3840, 2160, 0, 1080, true),
            "overflow and empty-scene guards must reject invalid candidates");

    std::puts("mvec admission tests passed");
    return 0;
}
