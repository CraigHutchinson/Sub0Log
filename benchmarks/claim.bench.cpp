/** @file claim.bench.cpp
 *  @brief KPI group "claim": Segment::claimChunk() cost -- the single
 *         cross-thread synchronisation the producer path has (R1.3).
 */

#include "support/groups.hpp"
#include "support/temp_dir.hpp"

#include <sub0log/segment.hpp>

#include <nanobench.h>

#include <cstdint>
#include <iostream>

namespace sub0log::bench {

void runClaimGroup(std::vector<ankerl::nanobench::Result>& allResults)
{
    ankerl::nanobench::Bench bench;
    bench.title("claim").unit("chunk").batch(1).performanceCounters(false);

    // claimChunk() is one fetch_add: nanobench's usual auto-scaling would
    // happily ask for millions of iterations to fill its target epoch
    // time, but every call here permanently advances the segment's claim
    // cursor and cannot be "redone" -- once a segment is exhausted, a claim
    // returns instantly invalid, which would silently mix a cheap failure
    // path into what is supposed to be a steady-state claim-cost average.
    // So the iteration count is fixed exactly via epochIterations(), not
    // left to calibration, and the segment is pre-created once, outside
    // the measured lambda, sized with deliberate headroom rather than
    // recreated mid-run:
    //   epochIterations * epochs = 300 * 11 = 3,300 claims measured
    //   segmentBytes = 256 MiB, default 64 KiB chunks -> 4,095 chunks
    // 3,300 of 4,095 chunks (~81%) are consumed by the measured claims --
    // close to exhaustion (representative of a nearly-full segment's claim
    // cost) without the run ever actually failing a claim.
    //
    // Caveat worth reading before trusting this number (see
    // benchmarks/README.md): every chunk claimed here is one this process
    // has never touched before, so the timed op is the atomic fetch_add
    // *plus* the first-touch page fault for the ChunkHeader it then
    // stamps -- on a tmpfs-backed temp directory that fault is cheap, but
    // std::filesystem::temp_directory_path() may resolve to real block
    // storage (it does in this suite's own CI sandbox, ext4 on a virtual
    // disk), where first-touch can mean on-demand block allocation and
    // dominates the reported ns/chunk by two to three orders of magnitude
    // versus the bare atomic op. That is a real cost a process pays for
    // its *first* pass through a freshly created segment, not a claim-cost
    // regression; steady-state claims against already-resident chunks (the
    // common case once a segment has been written once) are not what this
    // KPI isolates.
    constexpr std::uint64_t cEpochIterations = 300;
    constexpr std::size_t cEpochs = 11;

    TempDir dir{"claim"};
    sub0log::detail::Segment segment = sub0log::detail::Segment::create(
        dir.path().string(), "claim", {.segmentBytes_ = 256ull * 1024 * 1024});
    if (!segment.valid()) {
        std::cerr << "sub0log-bench: claim group: Segment::create failed, skipping\n";
        return;
    }

    bench.epochs(cEpochs).epochIterations(cEpochIterations)
        .run("claim.claimChunk", [&] {
            sub0log::detail::ChunkWriter writer = segment.claimChunk();
            ankerl::nanobench::doNotOptimizeAway(writer);
        });

    const auto& results = bench.results();
    allResults.insert(allResults.end(), results.begin(), results.end());
}

} // namespace sub0log::bench
