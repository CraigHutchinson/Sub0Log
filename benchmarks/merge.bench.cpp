/** @file merge.bench.cpp
 *  @brief KPI group "merge": Merger::addSegment() x4 + merged() over four
 *         pre-built segment images -- the read-time cross-process ordering
 *         cost (R5.2).
 */

#include "support/groups.hpp"
#include "support/mixed_records.hpp"

#include <sub0log/merge.hpp>

#include <nanobench.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace sub0log::bench {

void runMergeGroup(std::vector<ankerl::nanobench::Result>& allResults)
{
    constexpr std::uint64_t cRecordsPerSegment = 25'000;

    // Setup: four independent segments (a distinct Tag per build, so each
    // gets its own SiteDescriptors and thus its own SiteDefinition records
    // -- see support/mixed_records.hpp -- exactly as four real producer
    // processes would each write their own), unmeasured, slurped once.
    const std::array<std::vector<std::byte>, 4> images{
        buildMixedSegmentImage<10>("merge0", cRecordsPerSegment),
        buildMixedSegmentImage<11>("merge1", cRecordsPerSegment),
        buildMixedSegmentImage<12>("merge2", cRecordsPerSegment),
        buildMixedSegmentImage<13>("merge3", cRecordsPerSegment),
    };
    for (const auto& image : images) {
        if (image.empty()) {
            std::cerr << "sub0log-bench: merge group: failed to build a fixture segment, skipping\n";
            return;
        }
    }

    const std::uint64_t totalRecords = cRecordsPerSegment * images.size();

    ankerl::nanobench::Bench bench;
    bench.title("merge").unit("record").batch(totalRecords)
        .minEpochIterations(10).performanceCounters(false);

    // A fresh Merger every iteration: addSegment() accretes into it, so
    // reusing one across iterations would merge the same four segments'
    // worth of records over and over into one ever-growing Merger.
    bench.run("merge.addAndMerge", [&] {
        sub0log::Merger merger;
        for (const auto& image : images) {
            const sub0log::SegmentError err = merger.addSegment(image);
            ankerl::nanobench::doNotOptimizeAway(err);
        }
        const std::vector<sub0log::MergedRecord> merged = merger.merged();
        ankerl::nanobench::doNotOptimizeAway(merged);
    });

    const auto& results = bench.results();
    allResults.insert(allResults.end(), results.begin(), results.end());
}

} // namespace sub0log::bench
