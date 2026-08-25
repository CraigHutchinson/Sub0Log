/** @file many_segments.cpp
 *  @brief Scenario "many_segments": M Loggers, distinct stems, one shared
 *         directory, merged by sub0log::Merger -- the R5.2 case: several
 *         processes' segments read back as one ordered stream, modelled
 *         here as several Loggers (Logger::active() is one process-wide
 *         slot, so each Logger's threads run while it alone is bound; the
 *         merge afterwards is what actually exercises R5.2/R5.3).
 *
 *  Each Logger gets its OWN call site (see the Tag trick below): the
 *  SiteDescriptor announced-once latch is process-wide
 *  (benchmarks/support/mixed_records.hpp explains why), and this scenario
 *  runs M Loggers in one process, so sharing a call site across them would
 *  leave every Logger after the first with no SiteDefinition in its
 *  segment -- exactly the undecodable-record case this scenario's third
 *  invariant checks for.
 */

#include "support/check.hpp"
#include "support/scenario.hpp"
#include "support/temp_dir.hpp"

#include <sub0log/log.hpp>
#include <sub0log/merge.hpp>
#include <sub0log/reader.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sub0log::stress {

namespace {

constexpr sub0log::SubsystemId cManySegmentsSubsystem{104};
constexpr int cMaxLoggers = 8; // compile-time ceiling on the Tag table below

template <int Tag>
void emitManySegmentsRecord(std::uint64_t counter) noexcept
{
    sub0log_info(cManySegmentsSubsystem, "many_segments {}", counter);
}

using EmitFn = void (*)(std::uint64_t);

template <int... Tags>
constexpr std::array<EmitFn, sizeof...(Tags)> makeEmitTable(std::integer_sequence<int, Tags...>)
{
    return {&emitManySegmentsRecord<Tags>...};
}

constexpr auto cEmitTable = makeEmitTable(std::make_integer_sequence<int, cMaxLoggers>{});

} // namespace

ScenarioResult runManySegments(const RunOptions& options)
{
    Checker checker{"many_segments"};

    const int loggerCount = options.quick_ ? 3 : 6;
    const unsigned threadsPerLogger = std::min(4u, std::max(1u, options.threads_));
    const std::uint64_t perThread = options.quick_ ? 100u : 1'000u;

    if (!checker.check(loggerCount <= cMaxLoggers, "loggerCount fits the compile-time Tag table",
                       cMaxLoggers, loggerCount)) {
        return finish(checker, {});
    }

    TempDir dir{"many_segments"};

    std::vector<std::string> segmentPaths;
    std::vector<std::uint64_t> expectedPerLogger;
    std::vector<std::uint64_t> droppedPerLogger;
    bool everyLoggerAccounted = true;

    for (int idx = 0; idx < loggerCount; ++idx) {
        Logger::Options loggerOptions{};
        loggerOptions.directory_ = dir.path().string();
        loggerOptions.stem_ = "many-" + std::to_string(idx);

        auto logger = Logger::create(loggerOptions);
        if (!checker.check(logger.valid(), "Logger::create succeeds for this index", true,
                           logger.valid())) {
            everyLoggerAccounted = false;
            continue;
        }

        const EmitFn emitFn = cEmitTable[static_cast<std::size_t>(idx)];
        std::uint64_t emitted = 0;
        {
            Logger::ScopedBind bind{logger};
            emitFn(0u); // pre-warm this Logger's own call site (see saturate.cpp)
            ++emitted;

            std::vector<std::thread> workers;
            workers.reserve(threadsPerLogger);
            for (unsigned t = 0; t < threadsPerLogger; ++t) {
                workers.emplace_back([emitFn, perThread] {
                    for (std::uint64_t i = 1; i <= perThread; ++i) { // 0 taken by the pre-warm
                        emitFn(i);
                    }
                });
            }
            for (std::thread& worker : workers) {
                worker.join();
            }
            emitted += static_cast<std::uint64_t>(threadsPerLogger) * perThread;
        }

        segmentPaths.push_back(logger.segmentPath());
        expectedPerLogger.push_back(emitted);
        droppedPerLogger.push_back(logger.stats().droppedRecords_);
    }

    checker.check(segmentPaths.size() == static_cast<std::size_t>(loggerCount),
                 "every Logger produced a segment", loggerCount, segmentPaths.size());

    std::vector<std::vector<std::byte>> images;
    images.reserve(segmentPaths.size());
    for (const std::string& path : segmentPaths) {
        images.push_back(slurpFile(path));
    }

    // Independently re-decode each segment (not through Merger) so the
    // "merged size == sum of decoded per segment" check below is a real
    // cross-check of Merger's bookkeeping, not a restatement of it.
    std::uint64_t independentDecodedTotal = 0;
    for (std::size_t i = 0; i < images.size(); ++i) {
        auto segReader = SegmentReader::open(images[i]);
        if (!checker.check(segReader.valid(), "each segment opens independently", true,
                           segReader.valid())) {
            continue;
        }
        Decoder decoder;
        const auto records = decoder.decodeAll(segReader);
        independentDecodedTotal += records.size();

        if (i < expectedPerLogger.size()) {
            const std::uint64_t expected = expectedPerLogger[i];
            const std::uint64_t dropped = droppedPerLogger[i];
            if (expected != records.size() + dropped || decoder.undecodableRecords() != 0u) {
                everyLoggerAccounted = false;
            }
        }
    }
    checker.check(everyLoggerAccounted,
                 "per-logger emitted == decoded + dropped, and undecodableRecords() == 0",
                 std::string_view{"true"}, everyLoggerAccounted ? std::string_view{"true"} : std::string_view{"false"});

    Merger merger;
    for (const auto& image : images) {
        const SegmentError addResult = merger.addSegment(image);
        checker.check(addResult == SegmentError::Ok, "Merger::addSegment accepts the image", 0,
                     static_cast<int>(addResult));
    }
    const auto merged = merger.merged();
    const auto totals = merger.totals();

    checker.check(merged.size() == independentDecodedTotal, "merged size == sum of decoded per segment",
                 independentDecodedTotal, merged.size());

    bool alignedNsNonDecreasing = true;
    for (std::size_t i = 1; i < merged.size(); ++i) {
        if (merged[i].alignedNs_ < merged[i - 1].alignedNs_) {
            alignedNsNonDecreasing = false;
            break;
        }
    }
    checker.check(alignedNsNonDecreasing, "merged alignedNs_ is non-decreasing across the whole stream",
                 std::string_view{"true"}, alignedNsNonDecreasing ? std::string_view{"true"} : std::string_view{"false"});

    checker.check(totals.undecodableRecords_ == 0u, "Merger::totals().undecodableRecords_ == 0", 0u,
                 totals.undecodableRecords_);

    return finish(checker, {
        {"loggers", std::to_string(loggerCount)},
        {"threadsPerLogger", std::to_string(threadsPerLogger)},
        {"perThread", std::to_string(perThread)},
        {"mergedRecords", std::to_string(merged.size())},
        {"unreadableBytes", std::to_string(totals.unreadableBytes_)},
    });
}

} // namespace sub0log::stress
