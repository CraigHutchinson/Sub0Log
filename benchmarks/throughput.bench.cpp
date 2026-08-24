/** @file throughput.bench.cpp
 *  @brief KPI group "throughput": sustained multi-threaded emit rate.
 *         Chunk claiming (R1.3) is the only cross-thread synchronisation on
 *         the producer path, so contention on it would show up here.
 */

#include "support/groups.hpp"
#include "support/temp_dir.hpp"

#include <sub0log/log.hpp>

#include <nanobench.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace sub0log::bench {

namespace {

constexpr sub0log::SubsystemId cThroughputSubsystem{2};
constexpr std::uint64_t cRecordsPerThread = 20'000;
constexpr std::size_t cEpochs = 11; // nanobench's own default epoch count

/// One producer thread's tight loop: fixed2-shaped records, the same
/// argument shape as the "emit" group's fixed2 KPI.
void emitFixed2Loop()
{
    const std::int32_t i32Arg = -7;
    for (std::uint64_t i = 0; i < cRecordsPerThread; ++i) {
        sub0log_info(cThroughputSubsystem, "throughput {} {}", i, i32Arg);
    }
}

} // namespace

void runThroughputGroup(std::vector<ankerl::nanobench::Result>& allResults)
{
    ankerl::nanobench::Bench bench;
    bench.title("throughput").unit("record").performanceCounters(false);

    for (const unsigned threadCount : {1u, 2u, 4u}) {
        const std::uint64_t totalRecords =
            static_cast<std::uint64_t>(threadCount) * cRecordsPerThread;

        // A fresh Logger per thread-count configuration -- not shared with
        // the other two -- each with its own segment, sized for every
        // epoch this configuration will run: cEpochs * totalRecords
        // fixed2-shaped records (~48 bytes each) plus generous headroom.
        TempDir dir{"throughput-" + std::to_string(threadCount)};
        auto logger = sub0log::Logger::create(
            {.directory_ = dir.path().string(),
             .segment_ = {.segmentBytes_ = 256ull * 1024 * 1024}});
        if (!logger.valid()) {
            std::cerr << "sub0log-bench: throughput group: Logger::create failed for N="
                      << threadCount << ", skipping\n";
            continue;
        }
        sub0log::Logger::ScopedBind bind{logger};

        const std::string name = "throughput.threads" + std::to_string(threadCount);
        bench.epochs(cEpochs).minEpochIterations(1).batch(totalRecords)
            .run(name, [&] {
                std::vector<std::thread> threads;
                threads.reserve(threadCount);
                for (unsigned t = 0; t < threadCount; ++t) {
                    threads.emplace_back(emitFixed2Loop);
                }
                for (auto& worker : threads) {
                    worker.join();
                }
                ankerl::nanobench::doNotOptimizeAway(logger);
            });
    }

    const auto& results = bench.results();
    allResults.insert(allResults.end(), results.begin(), results.end());
}

} // namespace sub0log::bench
