#pragma once

// Compile-time PIX_COLOR encoding required by the AMD D3D12 backend.
// No Microsoft PIX header, Magpie source, or PIX runtime is included.
#ifndef PIX_COLOR
#define PIX_COLOR(red, green, blue) \
    (static_cast<UINT64>(0xff000000u) | \
     (static_cast<UINT64>(red) << 16) | \
     (static_cast<UINT64>(green) << 8) | \
     static_cast<UINT64>(blue))
#endif
