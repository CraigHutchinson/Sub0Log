/** @file decode.bench.cpp
 *  @brief KPI group "decode": the reader-side cost of turning a mapped
 *         segment image into typed records (R2) -- SegmentReader::open()
 *         plus Decoder::decodeAll(), measured against an image built once,
 *         unmeasured, up front.
 */

#include "support/groups.hpp"
#include "support/mixed_records.hpp"

#include <sub0log/reader.hpp>

#include <nanobench.h>

#include <cstdint>
#include <iostream>
#include <vector>

namespace sub0log::bench {

void runDecodeGroup(std::vector<ankerl::nanobench::Result>& allResults)
{
    constexpr std::uint64_t cRecordCount = 100'000;

    // Setup: never measured. One segment with ~100k mixed records, slurped
    // into memory once; every decode below reads that same image.
    const std::vector<std::byte> image = buildMixedSegmentImage("decode", cRecordCount);
    if (image.empty()) {
        std::cerr << "sub0log-bench: decode group: failed to build the fixture segment, skipping\n";
        return;
    }

    // Two Bench objects, not one: nanobench clears a Bench's accumulated
    // Results whenever unit() is called with a *different* string than it
    // already holds, so the "record" and "byte" views of the same decode
    // cost need separate Bench objects -- alternating unit() on one would
    // silently drop the first measurement when the second unit() call runs.
    ankerl::nanobench::Bench benchRecords;
    benchRecords.title("decode").unit("record").batch(cRecordCount)
        .performanceCounters(false);
    ankerl::nanobench::Bench benchBytes;
    benchBytes.title("decode").unit("byte").batch(image.size())
        .performanceCounters(false);

    // A fresh SegmentReader and Decoder every iteration: Decoder accretes a
    // site table and an undecodable-record count across calls, so reusing
    // one across iterations would both grow unboundedly and, from the
    // second iteration on, measure a decode into an already-populated
    // table rather than the cold-start decode a reader actually performs.
    benchRecords.minEpochIterations(10)
        .run("decode.recordsPerSecond", [&] {
            sub0log::SegmentReader reader = sub0log::SegmentReader::open(image);
            sub0log::Decoder decoder;
            std::vector<sub0log::DecodedRecord> records = decoder.decodeAll(reader);
            ankerl::nanobench::doNotOptimizeAway(records);
            ankerl::nanobench::doNotOptimizeAway(reader);
        });

    benchBytes.minEpochIterations(10)
        .run("decode.bytesPerSecond", [&] {
            sub0log::SegmentReader reader = sub0log::SegmentReader::open(image);
            sub0log::Decoder decoder;
            std::vector<sub0log::DecodedRecord> records = decoder.decodeAll(reader);
            ankerl::nanobench::doNotOptimizeAway(records);
            ankerl::nanobench::doNotOptimizeAway(reader);
        });

    allResults.insert(allResults.end(), benchRecords.results().begin(), benchRecords.results().end());
    allResults.insert(allResults.end(), benchBytes.results().begin(), benchBytes.results().end());
}

} // namespace sub0log::bench
