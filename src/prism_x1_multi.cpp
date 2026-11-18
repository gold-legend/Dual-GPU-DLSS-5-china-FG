// Compile the same current-frame PX1 implementation into three isolated
// namespaces. All mutable state in prism_x1.cpp is namespace- or TU-local, so
// these are independent instances rather than three views of one history.

#define PRISM_X1_NAMESPACE prism_x1_p1
#define PRISM_X1_PASS_ID "1"
#include "prism_x1.cpp"
#undef PRISM_X1_PASS_ID
#undef PRISM_X1_NAMESPACE

#define PRISM_X1_NAMESPACE prism_x1_p2
#define PRISM_X1_PASS_ID "2"
#include "prism_x1.cpp"
#undef PRISM_X1_PASS_ID
#undef PRISM_X1_NAMESPACE

#define PRISM_X1_NAMESPACE prism_x1_p3
#define PRISM_X1_PASS_ID "3"
#include "prism_x1.cpp"
#undef PRISM_X1_PASS_ID
#undef PRISM_X1_NAMESPACE

#include "prism_x1_multi.hpp"

namespace mgpu::prism_x1m
{
bool arm(unsigned pass, ID3D12Device *dev, unsigned width, unsigned height,
         DXGI_FORMAT fmt, unsigned low_width, unsigned low_height)
{
    switch (pass)
    {
    case 0: return prism_x1_p1::arm(dev, width, height, fmt, low_width, low_height);
    case 1: return prism_x1_p2::arm(dev, width, height, fmt, low_width, low_height);
    case 2: return prism_x1_p3::arm(dev, width, height, fmt, low_width, low_height);
    default: return false;
    }
}

bool active(unsigned pass)
{
    switch (pass)
    {
    case 0: return prism_x1_p1::active();
    case 1: return prism_x1_p2::active();
    case 2: return prism_x1_p3::active();
    default: return false;
    }
}

bool ab_enabled(unsigned pass)
{
    switch (pass)
    {
    case 0: return prism_x1_p1::ab_enabled();
    case 1: return prism_x1_p2::ab_enabled();
    case 2: return prism_x1_p3::ab_enabled();
    default: return false;
    }
}

bool down(unsigned pass, ID3D12GraphicsCommandList *cl, unsigned parity,
          ID3D12Resource *base, ID3D12Resource *low_proxy,
          unsigned low_width, unsigned low_height)
{
    switch (pass)
    {
    case 0: return prism_x1_p1::down(cl, parity, base, low_proxy, low_width, low_height);
    case 1: return prism_x1_p2::down(cl, parity, base, low_proxy, low_width, low_height);
    case 2: return prism_x1_p3::down(cl, parity, base, low_proxy, low_width, low_height);
    default: return false;
    }
}

bool compose(unsigned pass, ID3D12GraphicsCommandList *cl, unsigned parity,
             ID3D12Resource *base, ID3D12Resource *mask_base,
             ID3D12Resource *low_proxy, ID3D12Resource *low_neural, ID3D12Resource *out,
             unsigned low_width, unsigned low_height,
             ID3D12Resource *depth, bool limit_residual)
{
    switch (pass)
    {
    case 0: return prism_x1_p1::compose(cl, parity, base, mask_base, low_proxy, low_neural, out,
                                        low_width, low_height, depth, limit_residual);
    case 1: return prism_x1_p2::compose(cl, parity, base, mask_base, low_proxy, low_neural, out,
                                        low_width, low_height, depth, limit_residual);
    case 2: return prism_x1_p3::compose(cl, parity, base, mask_base, low_proxy, low_neural, out,
                                        low_width, low_height, depth, limit_residual);
    default: return false;
    }
}

void set_intensity(unsigned pass, float value)
{
    switch (pass)
    {
    case 0: prism_x1_p1::set_intensity(value); break;
    case 1: prism_x1_p2::set_intensity(value); break;
    case 2: prism_x1_p3::set_intensity(value); break;
    default: break;
    }
}

float intensity(unsigned pass)
{
    switch (pass)
    {
    case 0: return prism_x1_p1::intensity();
    case 1: return prism_x1_p2::intensity();
    case 2: return prism_x1_p3::intensity();
    default: return 1.0f;
    }
}

void set_guard(unsigned pass, float value)
{
    switch (pass)
    {
    case 0: prism_x1_p1::set_guard(value); break;
    case 1: prism_x1_p2::set_guard(value); break;
    case 2: prism_x1_p3::set_guard(value); break;
    default: break;
    }
}

float guard(unsigned pass)
{
    switch (pass)
    {
    case 0: return prism_x1_p1::guard();
    case 1: return prism_x1_p2::guard();
    case 2: return prism_x1_p3::guard();
    default: return 1.0f;
    }
}

void reset_history(unsigned pass)
{
    switch (pass)
    {
    case 0: prism_x1_p1::reset_history(); break;
    case 1: prism_x1_p2::reset_history(); break;
    case 2: prism_x1_p3::reset_history(); break;
    default: break;
    }
}

void release(unsigned pass)
{
    switch (pass)
    {
    case 0: prism_x1_p1::release(); break;
    case 1: prism_x1_p2::release(); break;
    case 2: prism_x1_p3::release(); break;
    default: break;
    }
}

void release_all()
{
    prism_x1_p3::release();
    prism_x1_p2::release();
    prism_x1_p1::release();
}
}
