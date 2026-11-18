#pragma once

// Low-overhead CPU timing for the game's render/present thread. Enabled only
// with ProducerDiag=1. No fences, GPU queries, sleeps, or per-frame logging.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include "diag.hpp"

namespace mgpu::gtdiag
{
enum stage : int
{
    OVERLAY = 0,
    FRAME_CAP,
    STREAM_PRESENT,
    PRESENT_CALLBACK,
    PRESENT_INTERVAL,
    PRESENT_ROUNDTRIP,
    FINISH_CALLBACK,
    FX_TOTAL,
    FX_PRE,
    FX_TAP,
    FX_STREAM,
    STAGE_COUNT
};

struct bucket
{
    double sum_ms = 0.0;
    double max_ms = 0.0;
    std::uint64_t n = 0;
    void add(double ms)
    {
        sum_ms += ms;
        if (ms > max_ms) max_ms = ms;
        ++n;
    }
    double mean() const { return n != 0 ? sum_ms / double(n) : 0.0; }
    void clear() { *this = bucket{}; }
};

static bool g_enabled = false;
static bucket g_b[STAGE_COUNT];
static std::uint64_t g_presents = 0;
static long long g_last_present = 0;
static thread_local long long t_present_enter = 0;

inline long long qpc()
{
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

inline double elapsed_ms(long long start, long long end)
{
    static const double freq = []
    {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return (start > 0 && end >= start) ? double(end - start) * 1000.0 / freq : 0.0;
}

inline void set_enabled(bool on) { g_enabled = on; }
inline long long stamp(bool game) { return (game && g_enabled) ? qpc() : 0; }

inline void mark(stage s, long long start)
{
    if (start == 0 || !g_enabled) return;
    g_b[s].add(elapsed_ms(start, qpc()));
}

class scope
{
public:
    explicit scope(stage s, bool active = false) : m_stage(s) { if (active) activate(true); }
    void activate(bool active)
    {
        if (active && g_enabled && m_start == 0) m_start = qpc();
    }
    void stop()
    {
        if (m_start == 0) return;
        mark(m_stage, m_start);
        m_start = 0;
    }
    ~scope() { stop(); }
    long long entry() const { return m_start; }
private:
    stage m_stage;
    long long m_start = 0;
};

class present_scope
{
public:
    explicit present_scope(bool game)
    {
        if (!game || !g_enabled) return;
        m_start = qpc();
        t_present_enter = m_start;
    }
    ~present_scope()
    {
        if (m_start == 0) return;
        g_b[PRESENT_CALLBACK].add(elapsed_ms(m_start, qpc()));
    }
    long long entry() const { return m_start; }
private:
    long long m_start = 0;
};

inline void report()
{
    const std::uint64_t n = g_b[PRESENT_CALLBACK].n;
    if (n == 0) return;
    const double interval = g_b[PRESENT_INTERVAL].mean();
    const double outside = g_b[PRESENT_ROUNDTRIP].mean() - g_b[PRESENT_CALLBACK].mean() - g_b[FINISH_CALLBACK].mean();
    char line[1100];
    std::snprintf(line, sizeof line,
        "[MGPU][GTDIAG] 600 game presents | interval %.3f ms (%.2f FPS) | Present roundtrip %.3f/%.3f ms mean/max | "
        "outside our callbacks %.3f ms | our present callback %.3f/%.3f | finish callback %.3f/%.3f",
        interval, interval > 0.0 ? 1000.0 / interval : 0.0,
        g_b[PRESENT_ROUNDTRIP].mean(), g_b[PRESENT_ROUNDTRIP].max_ms, outside,
        g_b[PRESENT_CALLBACK].mean(), g_b[PRESENT_CALLBACK].max_ms,
        g_b[FINISH_CALLBACK].mean(), g_b[FINISH_CALLBACK].max_ms);
    mgpu::diag::info(line);
    std::snprintf(line, sizeof line,
        "[MGPU][GTDIAG] stages mean/max ms | Prism %.3f/%.3f | FrameCap %.3f/%.3f | stream-on-present %.3f/%.3f | "
        "finish-effects total %.3f/%.3f | effects-before-capture %.3f/%.3f | motion/depth tap %.3f/%.3f | stream-record %.3f/%.3f",
        g_b[OVERLAY].mean(), g_b[OVERLAY].max_ms,
        g_b[FRAME_CAP].mean(), g_b[FRAME_CAP].max_ms,
        g_b[STREAM_PRESENT].mean(), g_b[STREAM_PRESENT].max_ms,
        g_b[FX_TOTAL].mean(), g_b[FX_TOTAL].max_ms,
        g_b[FX_PRE].mean(), g_b[FX_PRE].max_ms,
        g_b[FX_TAP].mean(), g_b[FX_TAP].max_ms,
        g_b[FX_STREAM].mean(), g_b[FX_STREAM].max_ms);
    mgpu::diag::info(line);
    for (bucket &b : g_b) b.clear();
}

class finish_scope
{
public:
    explicit finish_scope(bool game)
    {
        if (!game || !g_enabled) return;
        m_start = qpc();
    }
    ~finish_scope()
    {
        if (m_start == 0) return;
        const long long end = qpc();
        g_b[FINISH_CALLBACK].add(elapsed_ms(m_start, end));
        if (t_present_enter != 0)
        {
            if (g_last_present != 0)
                g_b[PRESENT_INTERVAL].add(elapsed_ms(g_last_present, t_present_enter));
            g_b[PRESENT_ROUNDTRIP].add(elapsed_ms(t_present_enter, end));
            g_last_present = t_present_enter;
            t_present_enter = 0;
        }
        if (++g_presents % 600 == 0) report();
    }
private:
    long long m_start = 0;
};

} // namespace mgpu::gtdiag
