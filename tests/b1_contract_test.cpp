#include "../src/b1_contract.hpp"
#include <cstdio>

using mgpu::b1::answer_status;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main()
{
    // Every-frame B1: all frames are due, including the first frame.
    CHECK(mgpu::b1::nr_due(1, 1));
    CHECK(mgpu::b1::nr_due(2, 1));
    CHECK(mgpu::b1::nr_due(30, 1));
    CHECK(!mgpu::b1::nr_due(0, 1));
    CHECK(!mgpu::b1::nr_due(1, 0));

    // Future cadence remains explicit, but is not enabled by the B1 build.
    CHECK(mgpu::b1::nr_due(1, 4));
    CHECK(!mgpu::b1::nr_due(2, 4));
    CHECK(!mgpu::b1::nr_due(3, 4));
    CHECK(!mgpu::b1::nr_due(4, 4));
    CHECK(mgpu::b1::nr_due(5, 4));

    // Slot F % RING may be written only after the consumer retired F-RING.
    CHECK(mgpu::b1::ring_release_target(0, 6) == 0);
    CHECK(mgpu::b1::ring_release_target(1, 6) == 0);
    CHECK(mgpu::b1::ring_release_target(6, 6) == 0);
    CHECK(mgpu::b1::ring_release_target(7, 6) == 1);
    CHECK(mgpu::b1::ring_release_target(120, 6) == 114);
    CHECK(mgpu::b1::ring_release_target(7, 0) == 0); // invalid storage never fabricates a wait

    // P1/P2/P3 own independent, rounded-even work extents.
    const auto p1 = mgpu::b1::pass_extent(3314, 1820, 0.48f);
    const auto p2 = mgpu::b1::pass_extent(3314, 1820, 0.62f);
    const auto p3 = mgpu::b1::pass_extent(3314, 1820, 1.00f);
    CHECK(p1.width == 1590 && p1.height == 874);
    CHECK(p2.width == 2054 && p2.height == 1128);
    CHECK(p3.width == 3314 && p3.height == 1820);
    CHECK(mgpu::b1::pass_extent(3314, 1820, 0.01f).width == 166); // clamped to 5%
    CHECK(mgpu::b1::pass_extent(3314, 1820, 1.50f).width == 3314); // capped at native

    // A smaller older answer remains valid after the live final pass grows.
    CHECK(mgpu::b1::valid_answer_extent(p1.width, p1.height, p3.width, p3.height));
    CHECK(mgpu::b1::valid_answer_extent(p2.width, p2.height, p3.width, p3.height));
    CHECK(!mgpu::b1::valid_answer_extent(0, p1.height, p3.width, p3.height));
    CHECK(!mgpu::b1::valid_answer_extent(1591, p1.height, p3.width, p3.height));
    CHECK(!mgpu::b1::valid_answer_extent(p3.width + 2, p3.height, p3.width, p3.height));
    CHECK(mgpu::b1::valid_answer_pass(0, 1));
    CHECK(mgpu::b1::valid_answer_pass(1, 2));
    CHECK(mgpu::b1::valid_answer_pass(2, 3));
    CHECK(!mgpu::b1::valid_answer_pass(2, 2));
    CHECK(!mgpu::b1::valid_answer_pass(3, 3));

    constexpr unsigned long long bytes = 4096;
    using mgpu::b1::classify_answer;
    CHECK(classify_answer(bytes, 1, 1, 1, 1) == answer_status::accept);
    CHECK(classify_answer(0, 1, 1, 1, 1) == answer_status::no_storage);

    // First frame not ready / Evaluate failure: producer publishes tag zero.
    CHECK(classify_answer(bytes, 0, 1, 0, 1) == answer_status::missing);
    CHECK(classify_answer(bytes, 0, 30, 0, 1) == answer_status::missing);

    // Wrong and stale frame numbers are rejected, never temporally retained.
    CHECK(classify_answer(bytes, 31, 30, 1, 1) == answer_status::future);
    CHECK(classify_answer(bytes, 29, 30, 1, 1) == answer_status::stale);

    // The same frame number from an earlier stream generation is still stale data.
    CHECK(classify_answer(bytes, 30, 30, 0, 1) == answer_status::old_generation);
    CHECK(classify_answer(bytes, 30, 30, 1, 2) == answer_status::old_generation);

    // Shutdown invalidation uses empty tags and a generation change.
    CHECK(classify_answer(bytes, 0, 31, 0, 2) == answer_status::missing);
    CHECK(classify_answer(bytes, 30, 30, 1, 2) == answer_status::old_generation);

    std::printf(fails ? "b1_contract_test: %d FAILED\n" : "b1_contract_test: all passed\n", fails);
    return fails ? 1 : 0;
}
