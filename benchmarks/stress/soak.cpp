/** @file soak.cpp
 *  @brief Scenario "soak": a time-bounded (--seconds) mixed workload --
 *         producers, a live reader, and periodic Logger recreation -- run
 *         together for the duration, one generation at a time.
 *
 *  Each generation gets its own Logger and reuses the SAME call site,
 *  which is the point: a site announces itself into each new segment, so a
 *  recreated Logger decodes on its own. That did not hold when this
 *  scenario was written -- announcing was once per process, so every
 *  generation after the first produced undecodable records, and this file
 *  worked around it with a per-generation call site and an arbitrary
 *  eight-generation ceiling. The library is fixed; the ceiling is gone.
 *  At the end of each generation's life this scenario
 *  checks the same accounting invariant as "saturate" (emitted == decoded +
 *  dropped, undecodableRecords() == 0); across the whole run it checks that
 *  the count of undecodable records per generation never grows above zero.
 */

#include "support/check.hpp"
#include "support/live_reader.hpp"
#include "support/scenario.hpp"
#include "support/temp_dir.hpp"
#include "support/verify.hpp"

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sub0log::stress {

namespace {

constexpr sub0log::SubsystemId cSoakSubsystem{105};

void emitSoakRecord(std::uint64_t counter) noexcept
{
    sub0log_info(cSoakSubsystem, "soak {}", counter);
}

/// One generation's outcome, folded into the scenario's overall counters
/// and its two invariants after the generation's Logger is retired.
struct GenerationOutcome {
    std::uint64_t emitted_{};
    std::uint64_t decoded_{};
    std::uint64_t dropped_{};
    std::uint64_t undecodable_{};
    std::uint64_t readPasses_{};
    bool readerMonotonic_{true};
    bool readerHealthy_{true};
};

GenerationOutcome runOneGeneration(int generationIndex, unsigned writerCount,
                                   std::chrono::milliseconds generationDuration,
                                   const std::string& directory)
{
    GenerationOutcome outcome{};

    Logger::Options loggerOptions{};
    loggerOptions.directory_ = directory;
    loggerOptions.stem_ = "soak-gen" + std::to_string(generationIndex);
    auto logger = Logger::create(loggerOptions);
    if (!logger.valid()) {
        return outcome; // reported as a decode-open failure below (segmentPath empty)
    }
    const std::string segmentPath = logger.segmentPath();

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> lastSeenCount{0};

    {
        Logger::ScopedBind bind{logger};
        emitSoakRecord(0u); // pre-warm: announce into this generation's segment
        outcome.emitted_ = 1;

        std::thread reader([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const LivePassResult pass = readOneLivePass(segmentPath);
                if (pass.outcome_ == LivePassOutcome::MappingUnavailable) {
                    continue;
                }
                if (pass.outcome_ == LivePassOutcome::ReaderInvalid) {
                    outcome.readerHealthy_ = false;
                    continue;
                }
                if (pass.undecodableFound_) {
                    outcome.readerHealthy_ = false;
                }
                const std::uint64_t previous =
                    lastSeenCount.exchange(pass.decodedCount_, std::memory_order_relaxed);
                if (pass.decodedCount_ < previous) {
                    outcome.readerMonotonic_ = false;
                }
                ++outcome.readPasses_;
            }
        });

        std::vector<std::thread> writers;
        writers.reserve(writerCount);
        std::vector<std::atomic<std::uint64_t>> perWriterEmitted(writerCount);
        for (unsigned t = 0; t < writerCount; ++t) {
            writers.emplace_back([&stop, &perWriterEmitted, t] {
                std::uint64_t i = 1; // 0 taken by the pre-warm call
                while (!stop.load(std::memory_order_relaxed)) {
                    emitSoakRecord(i);
                    ++i;
                }
                perWriterEmitted[t].store(i - 1, std::memory_order_relaxed);
            });
        }

        std::this_thread::sleep_for(generationDuration);
        stop.store(true, std::memory_order_relaxed);

        for (std::thread& writer : writers) {
            writer.join();
        }
        for (const auto& counted : perWriterEmitted) {
            outcome.emitted_ += counted.load(std::memory_order_relaxed);
        }
        reader.join();
    } // unbind; logger and its mapping are still alive below.

    outcome.dropped_ = logger.stats().droppedRecords_;

    const auto image = slurpFile(logger.segmentPath());
    auto segReader = SegmentReader::open(image);
    if (!segReader.valid()) {
        return outcome;
    }
    Decoder decoder;
    const auto records = decoder.decodeAll(segReader);
    outcome.decoded_ = records.size();
    outcome.undecodable_ = decoder.undecodableRecords();

    return outcome;
}

} // namespace

ScenarioResult runSoak(const RunOptions& options)
{
    Checker checker{"soak"};

    const unsigned totalSeconds = std::max(1u, options.effectiveSeconds());
    const int generations = options.quick_ ? 2 : 4;
    const unsigned writerCount = std::min(4u, std::max(1u, options.threads_));
    const auto generationDuration =
        std::chrono::milliseconds((totalSeconds * 1000u) / static_cast<unsigned>(generations));

    TempDir dir{"soak"};

    std::uint64_t totalEmitted = 0, totalDecoded = 0, totalDropped = 0, totalUndecodable = 0,
                 totalReadPasses = 0;
    bool everyGenerationAccounted = true;
    bool undecodableEverGrewAboveZero = false;
    bool everyReaderHealthy = true;
    bool everyReaderMonotonic = true;

    for (int gen = 0; gen < generations; ++gen) {
        const GenerationOutcome outcome =
            runOneGeneration(gen, writerCount, generationDuration, dir.path().string());

        totalEmitted += outcome.emitted_;
        totalDecoded += outcome.decoded_;
        totalDropped += outcome.dropped_;
        totalUndecodable += outcome.undecodable_;
        totalReadPasses += outcome.readPasses_;

        if (outcome.emitted_ != outcome.decoded_ + outcome.dropped_) {
            everyGenerationAccounted = false;
        }
        if (outcome.undecodable_ != 0u) {
            undecodableEverGrewAboveZero = true;
        }
        if (!outcome.readerHealthy_) {
            everyReaderHealthy = false;
        }
        if (!outcome.readerMonotonic_) {
            everyReaderMonotonic = false;
        }
    }

    checker.check(everyGenerationAccounted,
                 "per generation: emitted == decoded + dropped (the saturate invariant, at the end of each Logger's life)",
                 std::string_view{"true"}, everyGenerationAccounted ? std::string_view{"true"} : std::string_view{"false"});
    checker.check(!undecodableEverGrewAboveZero, "no growth in undecodable records across generations",
                 0u, totalUndecodable);
    checker.check(everyReaderHealthy, "the live reader stayed healthy (Ok, undecodableRecords()==0) in every generation",
                 std::string_view{"true"}, everyReaderHealthy ? std::string_view{"true"} : std::string_view{"false"});
    checker.check(everyReaderMonotonic, "the live reader's decoded count was non-decreasing within every generation",
                 std::string_view{"true"}, everyReaderMonotonic ? std::string_view{"true"} : std::string_view{"false"});

    return finish(checker, {
        {"generations", std::to_string(generations)},
        {"writers", std::to_string(writerCount)},
        {"totalSeconds", std::to_string(totalSeconds)},
        {"emitted", std::to_string(totalEmitted)},
        {"decoded", std::to_string(totalDecoded)},
        {"dropped", std::to_string(totalDropped)},
        {"undecodable", std::to_string(totalUndecodable)},
        {"readPasses", std::to_string(totalReadPasses)},
    });
}

} // namespace sub0log::stress
