/** @file atomics.bench.cpp
 *  @brief KPI group "atomics": the 64-bit and split 32-bit protocols
 *         (detail/atomics.hpp) side by side, in one binary, on resident
 *         memory.
 *
 *  The split path is what every Cortex-M runs. It can also be selected on
 *  a host (SUB0LOG_SPLIT_ATOMICS=1), and these numbers are what that
 *  choice costs where it is not needed -- and, more usefully, what the two
 *  protocols cost relative to each other with everything else held equal.
 *  Both are templates on the path, so neither needs a separate build.
 *
 *  Unlike the `claim` group, nothing here touches a freshly mapped page:
 *  the cursor and slots are small, resident arrays, so this isolates the
 *  atomic operations themselves -- the part the two protocols differ in.
 *  `claim` measures the first-touch cost a real segment adds on top.
 */

#include "support/groups.hpp"

#include <sub0log/detail/atomics.hpp>
#include <sub0log/wire.hpp>

#include <nanobench.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace sub0log::bench {

namespace {

template <bool Split>
const char* pathName()
{
    return Split ? "split32" : "u64";
}

template <bool Split>
void commitAndLoad(ankerl::nanobench::Bench& bench)
{
    // 64 slots cycled, so the store stream is not one hot cache line.
    alignas(64) static std::array<std::byte, 64 * 8> slots{};
    wire::RecordHead head{};
    head.payloadBytes_ = 24;
    head.kind_ = wire::RecordKind::Message;
    std::uint32_t i = 0;
    bench.run(std::string{"atomics.commit."} + pathName<Split>(), [&] {
        head.sequence_ = static_cast<std::uint16_t>(i);
        detail::storeHeadWord<Split>(slots.data() + (i & 63u) * 8u, head.pack());
        ++i;
    });
    ankerl::nanobench::doNotOptimizeAway(slots);

    std::uint64_t sink = 0;
    i = 0;
    bench.run(std::string{"atomics.loadHeadWord."} + pathName<Split>(), [&] {
        sink += detail::loadHeadWord<Split>(slots.data() + (i & 63u) * 8u);
        ++i;
    });
    ankerl::nanobench::doNotOptimizeAway(sink);
}

template <bool Split>
void uncontendedClaim(ankerl::nanobench::Bench& bench)
{
    // A count no run can reach, so every claim succeeds: the steady-state
    // cost of the claim itself, with no refusal path mixed in.
    alignas(64) static std::array<std::byte, 8> cursor{};
    cursor = {};
    bench.run(std::string{"atomics.claim."} + pathName<Split>(), [&] {
        ankerl::nanobench::doNotOptimizeAway(
            detail::claimChunkIndex<Split>(cursor.data(), 0xFFFFFFF0u));
    });
}

template <bool Split>
void contendedClaims(ankerl::nanobench::Bench& bench, const int threads)
{
    // The split claim is a compare-exchange loop where the 64-bit one is a
    // single fetch_add; contention is where that difference would show.
    // Every thread claims flat out against one cursor; reported per claim
    // across all threads.
    constexpr std::uint32_t cClaimsPerThread = 200'000;
    const std::uint32_t total = cClaimsPerThread * static_cast<std::uint32_t>(threads);
    alignas(64) static std::array<std::byte, 8> cursor{};
    bench.batch(total).run(
        std::string{"atomics.claim.contended."} + pathName<Split>() + ".t" + std::to_string(threads),
        [&] {
            cursor = {};
            std::vector<std::thread> pool;
            pool.reserve(static_cast<std::size_t>(threads));
            for (int t = 0; t < threads; ++t) {
                pool.emplace_back([&] {
                    for (std::uint32_t n = 0; n < cClaimsPerThread; ++n) {
                        ankerl::nanobench::doNotOptimizeAway(
                            detail::claimChunkIndex<Split>(cursor.data(), 0xFFFFFFF0u));
                    }
                });
            }
            for (std::thread& thread : pool) {
                thread.join();
            }
        });
    bench.batch(1);
}

} // namespace

void runAtomicsGroup(std::vector<ankerl::nanobench::Result>& allResults)
{
    ankerl::nanobench::Bench ops;
    ops.title("atomics").unit("op").performanceCounters(false).minEpochIterations(5'000'000);
    commitAndLoad<false>(ops);
    commitAndLoad<true>(ops);
    uncontendedClaim<false>(ops);
    uncontendedClaim<true>(ops);
    allResults.insert(allResults.end(), ops.results().begin(), ops.results().end());

    ankerl::nanobench::Bench contended;
    contended.title("atomics (contended claims)").unit("claim").performanceCounters(false)
        .epochs(7).minEpochIterations(5);
    for (const int threads : {1, 2, 4, 8}) {
        contendedClaims<false>(contended, threads);
        contendedClaims<true>(contended, threads);
    }
    allResults.insert(allResults.end(), contended.results().begin(), contended.results().end());
}

} // namespace sub0log::bench
