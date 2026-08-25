/** @file saturate.cpp
 *  @brief Scenario "saturate": N threads emit into a deliberately small
 *         segment so exhaustion (R1.3's claim path under contention, and
 *         R9.1's drop counter) is reached, not merely approached.
 *
 *  The invariant that matters: emitted == decoded + dropped. Every record a
 *  call site attempted is either readable afterwards or counted as a drop
 *  (R9.1) -- nothing vanishes, and nothing the decoder reports was invented.
 */

#include "support/check.hpp"
#include "support/scenario.hpp"
#include "support/temp_dir.hpp"
#include "support/verify.hpp"

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace sub0log::stress {

namespace {

constexpr sub0log::SubsystemId cSaturateSubsystem{100};

/// The one call site every producer thread (and the pre-warm call below)
/// shares. A single, distinct function -- never reused by another
/// scenario -- so its SiteDescriptor::announced_ latch belongs to this
/// scenario alone (docs/architecture.md, "the site's identity is its
/// descriptor address"; benchmarks/support/mixed_records.hpp explains why a
/// shared call site across independent producers is the wrong shape).
void emitSaturateRecord(std::uint64_t counter) noexcept
{
    sub0log_info(cSaturateSubsystem, "saturate {}", counter);
}

} // namespace

ScenarioResult runSaturate(const RunOptions& options)
{
    Checker checker{"saturate"};

    const unsigned threadCount = std::max(1u, options.threads_);
    const std::uint64_t perThread = options.quick_ ? 600u : 20'000u;

    // Small enough, relative to threadCount * perThread, that the segment is
    // certain to run out mid-run: a handful of tiny chunks against tens of
    // thousands of attempted records.
    constexpr std::uint32_t cChunkBytes = 256u;
    const std::uint32_t chunkCount = options.quick_ ? 40u : 100u;

    TempDir dir{"saturate"};
    Logger::Options loggerOptions{};
    loggerOptions.directory_ = dir.path().string();
    loggerOptions.stem_ = "saturate";
    loggerOptions.segment_.chunkBytes_ = cChunkBytes;
    loggerOptions.segment_.segmentBytes_ =
        wire::cSegmentHeaderBytes + static_cast<std::uint64_t>(chunkCount) * cChunkBytes;

    auto logger = Logger::create(loggerOptions);
    if (!checker.check(logger.valid(), "Logger::create succeeds", true, logger.valid())) {
        return finish(checker, {});
    }

    std::uint64_t totalEmitted = 0;
    {
        Logger::ScopedBind bind{logger};

        // Pre-warm the shared call site on the calling thread, with the
        // segment still completely empty, so the SiteDefinition write is
        // guaranteed to succeed before any producer thread starts. Without
        // this, several threads could race the "announced_ == 0" check
        // simultaneously; the loser(s) would each pay a second, spurious
        // drop for a definition nobody needed after the first succeeded,
        // which would desynchronise "one emitted call, at most one drop"
        // from reality under no fault of the library's -- purely an
        // artifact of how this harness would have shaped the race. Emitting
        // once, single-threaded, first, makes the accounting exact instead
        // of merely probable.
        emitSaturateRecord(0u);
        ++totalEmitted;

        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (unsigned t = 0; t < threadCount; ++t) {
            workers.emplace_back([perThread] {
                for (std::uint64_t i = 0; i < perThread; ++i) {
                    emitSaturateRecord(i);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        totalEmitted += static_cast<std::uint64_t>(threadCount) * perThread;
    } // unbind; the Logger and its mapping are still alive below.

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

    // The pre-warm call used counter 0 on the harness's own thread, which is
    // never one of the spawned producer threads, so it lands in its own
    // group and does not collide with any worker's [0, perThread) range.
    const std::uint64_t validCounters = checkPerThreadCounters(
        checker, records, 0, perThread, "decoded counters are unique per thread and in range");

    return finish(checker, {
        {"threads", std::to_string(threadCount)},
        {"perThread", std::to_string(perThread)},
        {"emitted", std::to_string(totalEmitted)},
        {"decoded", std::to_string(decoded)},
        {"dropped", std::to_string(stats.droppedRecords_)},
        {"validCounters", std::to_string(validCounters)},
        {"unreadableBytes", std::to_string(reader.unreadableBytes())},
    });
}

} // namespace sub0log::stress
