/** @file oversubscribe.cpp
 *  @brief Scenario "oversubscribe": 4x hardware_concurrency threads racing
 *         the single fetch_add chunk claim (R1.3) on one large segment.
 *         Same accounting invariant as "saturate"; the point here is
 *         contention on the claim itself, not exhaustion, so the segment is
 *         sized generously and throughput is reported (never asserted --
 *         wall-clock numbers are noisy in CI, R1.3's correctness is not).
 */

#include "support/check.hpp"
#include "support/scenario.hpp"
#include "support/temp_dir.hpp"
#include "support/verify.hpp"

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sub0log::stress {

namespace {

constexpr sub0log::SubsystemId cOversubscribeSubsystem{101};

/// Distinct call site, not shared with any other scenario (see saturate.cpp
/// for why that matters to the pre-warm below).
void emitOversubscribeRecord(std::uint64_t counter) noexcept
{
    sub0log_info(cOversubscribeSubsystem, "oversubscribe {}", counter);
}

} // namespace

ScenarioResult runOversubscribe(const RunOptions& options)
{
    Checker checker{"oversubscribe"};

    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threadCount = cores * 4u;
    const std::uint64_t perThread = options.quick_ ? 400u : 5'000u;

    // Large relative to threadCount * perThread: this scenario is about
    // contention on the claim, not about running out of room.
    const std::uint64_t totalMessages = static_cast<std::uint64_t>(threadCount) * perThread;
    const std::uint64_t segmentBytes =
        std::max<std::uint64_t>(32ull * 1024 * 1024, totalMessages * 128ull + wire::cSegmentHeaderBytes);

    TempDir dir{"oversubscribe"};
    Logger::Options loggerOptions{};
    loggerOptions.directory_ = dir.path().string();
    loggerOptions.stem_ = "oversubscribe";
    loggerOptions.segment_.segmentBytes_ = segmentBytes;

    auto logger = Logger::create(loggerOptions);
    if (!checker.check(logger.valid(), "Logger::create succeeds", true, logger.valid())) {
        return finish(checker, {});
    }

    std::uint64_t totalEmitted = 0;
    std::chrono::steady_clock::duration elapsed{};
    {
        Logger::ScopedBind bind{logger};

        // Pre-warm on the calling thread first (see saturate.cpp for why):
        // makes the SiteDefinition write race-free even though this
        // segment has ample room for every racer to succeed anyway.
        emitOversubscribeRecord(0u);
        ++totalEmitted;

        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        const auto start = std::chrono::steady_clock::now();
        for (unsigned t = 0; t < threadCount; ++t) {
            workers.emplace_back([perThread] {
                for (std::uint64_t i = 0; i < perThread; ++i) {
                    emitOversubscribeRecord(i);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        elapsed = std::chrono::steady_clock::now() - start;
        totalEmitted += totalMessages;
    }

    const Stats stats = logger.stats();

    const auto image = slurpFile(logger.segmentPath());
    auto reader = SegmentReader::open(image);
    if (!checker.check(reader.valid(), "segment opens", true, reader.valid())) {
        return finish(checker, {{"emitted", std::to_string(totalEmitted)}});
    }

    Decoder decoder;
    const auto records = decoder.decodeAll(reader);

    checker.check(decoder.undecodableRecords() == 0u, "Decoder::undecodableRecords() == 0",
                 0u, decoder.undecodableRecords());

    const std::uint64_t decoded = records.size();
    checker.check(totalEmitted == decoded + stats.droppedRecords_,
                 "emitted == decoded + dropped", totalEmitted, decoded + stats.droppedRecords_);

    const std::uint64_t validCounters = checkPerThreadCounters(
        checker, records, 0, perThread, "decoded counters are unique per thread and in range");

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double throughput = seconds > 0.0 ? static_cast<double>(totalMessages) / seconds : 0.0;
    std::ostringstream throughputStr;
    throughputStr.precision(0);
    throughputStr << std::fixed << throughput;

    return finish(checker, {
        {"threads", std::to_string(threadCount)},
        {"perThread", std::to_string(perThread)},
        {"emitted", std::to_string(totalEmitted)},
        {"decoded", std::to_string(decoded)},
        {"dropped", std::to_string(stats.droppedRecords_)},
        {"validCounters", std::to_string(validCounters)},
        {"claimThroughputRecordsPerSec", throughputStr.str()},
    });
}

} // namespace sub0log::stress
