// Pure admission policy for R280's D3D12 velocity-source census.
// Kept free of ReShade/D3D12 types so the resize policy has CPU regression tests.
#pragma once

namespace mgpu::mvec_extract
{
    inline bool four_channel_16(unsigned format) noexcept
    {
        // DXGI_FORMAT_R16G16B16A16_FLOAT / UNORM / SNORM.
        return format == 10u || format == 11u || format == 13u;
    }

    inline bool exact_scene_size(unsigned texture_w, unsigned texture_h,
                                 unsigned scene_w, unsigned scene_h) noexcept
    {
        return texture_w == scene_w && texture_h == scene_h;
    }

    inline bool admit_velocity_resource(unsigned format,
                                        unsigned texture_w, unsigned texture_h,
                                        unsigned scene_w, unsigned scene_h,
                                        bool allow_startup_upscale) noexcept
    {
        if (!four_channel_16(format) || texture_w == 0u || texture_h == 0u ||
            scene_w == 0u || scene_h == 0u)
            return false;

        using u64 = unsigned long long;
        const u64 tw = texture_w, th = texture_h, sw = scene_w, sh = scene_h;

        // Keep the existing half-to-full scene band and its lower bound.
        if (tw * 2u < sw || th * 2u < sh)
            return false;
        if (tw <= sw + sw / 50u && th <= sh + sh / 50u)
            return true;

        // A game can allocate its final-resolution G-buffer before resizing the
        // swap chain. Admit it for the census only while the first source is
        // still undecided; decide_source still requires the exact scene size.
        return allow_startup_upscale && tw <= 8192u && th <= 8192u &&
               tw <= sw * 4u && th <= sh * 4u;
    }
}
