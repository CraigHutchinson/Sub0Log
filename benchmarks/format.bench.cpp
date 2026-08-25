/** @file format.bench.cpp
 *  @brief KPI group "format": Decoder::format() of a 4-arg record -- text
 *         rendering is the one consumer cost paid per line viewed, since
 *         decoding otherwise never touches text (R1.1, docs/architecture.md).
 */

#include "support/groups.hpp"
#include "support/mixed_records.hpp"

#include <sub0log/reader.hpp>

#include <nanobench.h>

#include <iostream>
#include <string>
#include <vector>

namespace sub0log::bench {

void runFormatGroup(std::vector<ankerl::nanobench::Result>& allResults)
{
    // Setup: a handful of mixed records -- the round-robin in
    // support/mixed_records.hpp puts a 4-arg ("quad") record at every 5th
    // index, so 40 records guarantee several -- decoded once, unmeasured,
    // to hand format() one real DecodedRecord to render repeatedly.
    const std::vector<std::byte> image = buildMixedSegmentImage("format", 40);
    if (image.empty()) {
        std::cerr << "sub0log-bench: format group: failed to build the fixture segment, skipping\n";
        return;
    }

    sub0log::SegmentReader reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::cerr << "sub0log-bench: format group: fixture segment failed to open, skipping\n";
        return;
    }
    sub0log::Decoder decoder;
    const std::vector<sub0log::DecodedRecord> records = decoder.decodeAll(reader);

    const sub0log::DecodedRecord* fourArgRecord = nullptr;
    for (const auto& record : records) {
        if (record.args_.size() == 4) {
            fourArgRecord = &record;
            break;
        }
    }
    if (fourArgRecord == nullptr) {
        std::cerr << "sub0log-bench: format group: no 4-arg record in the fixture, skipping\n";
        return;
    }

    ankerl::nanobench::Bench bench;
    bench.title("format").unit("record").batch(1)
        .minEpochIterations(50'000).performanceCounters(false);

    bench.run("format.fourArgs", [&] {
        const std::string text = sub0log::Decoder::format(*fourArgRecord);
        ankerl::nanobench::doNotOptimizeAway(text);
    });

    const auto& results = bench.results();
    allResults.insert(allResults.end(), results.begin(), results.end());
}

} // namespace sub0log::bench
