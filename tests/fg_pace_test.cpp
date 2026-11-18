#include "../src/fg_pace.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
bool check_relative(unsigned n_gen)
{
    std::vector<unsigned> presented;
    std::vector<double> waits;
    auto wait = [&](double at) { waits.push_back(at); };
    auto present = [&](unsigned index) {
        presented.push_back(index);
        return true;
    };
    if (!mgpu::fgpace::pair_relative(100.0, 4.0, n_gen, wait, present)) return false;
    if (presented.size() != n_gen + 1u || waits.size() != n_gen) return false;
    for (unsigned i = 0; i <= n_gen; ++i)
    {
        if (presented[i] != i) return false; // Distinct scene-order indices; no repeated fill frame.
        if (i > 0 && std::fabs(waits[i - 1] - (100.0 + 4.0 * i)) > 1.0e-9) return false;
    }
    return true;
}

bool check_fill()
{
    return mgpu::fgpace::fill_count(1.0f, 3u) == 0u &&
           mgpu::fgpace::fill_count(2.0f, 3u) == 1u &&
           mgpu::fgpace::fill_count(3.0f, 3u) == 2u &&
           mgpu::fgpace::fill_count(4.0f, 3u) == 3u &&
           mgpu::fgpace::fill_count(8.0f, 3u) == 3u;
}
}

int main()
{
    const bool ok = check_relative(0) && check_relative(1) && check_relative(3) && check_fill();
    std::puts(ok ? "fg_pace_test: ordered indices and skipped-game-frame fill counts pass" :
                   "fg_pace_test: FAILED");
    return ok ? 0 : 1;
}
