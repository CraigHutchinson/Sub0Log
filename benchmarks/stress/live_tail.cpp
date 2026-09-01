/** @file live_tail.cpp
 *  @brief Scenario "live_tail": writers hammer a segment while a reader
 *         thread repeatedly opens and decodes the SAME file, live -- the
 *         property tests/integration/producer.test.cpp states once
 *         ("a segment is readable while its producer still holds it open")
 *         exercised under sustained concurrent load instead of a single
 *         snapshot.
 *
 *  Invariants: the reader never crashes and its SegmentError stays Ok on
 *  every pass; the count of records it decodes is monotonically
 *  non-decreasing across passes (nothing a previous pass saw ever
 *  disappears); and undecodableRecords() stays 0 throughout -- a
 *  partially-written tail must show up as unreadable bytes (R3.3), never as
 *  a record the decoder claims to understand but got wrong.
 */

#include "support/check.hpp"
#include "support/live_reader.hpp"
#include "support/scenario.hpp"
#include "support/temp_dir.hpp"

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sub0log::stress {

namespace {

constexpr sub0log::SubsystemId cLiveTailSubsystem{103};

void emitLiveTailRecord(std::uint64_t counter) noexcept
{
    sub0log_info(cLiveTailSubsystem, "live_tail {}", counter);
}

} // namespace

ScenarioResult runLiveTail(const RunOptions& options)
{
    Checker checker{"live_tail"};

    const unsigned writerCount = std::min(4u, std::max(1u, options.threads_));
    const unsigned durationSeconds = std::max(1u, options.effectiveSeconds());

    TempDir dir{"live_tail"};
    auto logger = Logger::create({.directory_ = dir.path().string(), .stem_ = "live_tail"});
    if (!checker.check(logger.valid(), "Logger::create succeeds", true, logger.valid())) {
        return finish(checker, {});
    }
    const std::string segmentPath = logger.segmentPath();

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> readPasses{0};
    std::atomic<std::uint64_t> lastSeenCount{0};
    std::atomic<bool> monotonic{true};
    std::atomic<bool> readerHealthy{true};
    std::atomic<bool> readerCrashed{false};
    std::string crashDetail;

    {
        Logger::ScopedBind bind{logger};

        // Pre-warm on the calling thread (see saturate.cpp): the invariant
        // under test here is "undecodableRecords stays 0 while writing
        // continues", so the SiteDefinition write must not itself be a race
        // between writer threads.
        emitLiveTailRecord(0u);

        std::thread reader([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                try {
                    const LivePassResult pass = readOneLivePass(segmentPath);
                    if (pass.outcome_ == LivePassOutcome::MappingUnavailable) {
                        continue; // a transient open race is not itself a failure here
                    }
                    if (pass.outcome_ == LivePassOutcome::ReaderInvalid) {
                        readerHealthy.store(false, std::memory_order_relaxed);
                        continue;
                    }
                    if (pass.undecodableFound_) {
                        readerHealthy.store(false, std::memory_order_relaxed);
                    }
                    const std::uint64_t previous =
                        lastSeenCount.exchange(pass.decodedCount_, std::memory_order_relaxed);
                    if (pass.decodedCount_ < previous) {
                        monotonic.store(false, std::memory_order_relaxed);
                    }
                    readPasses.fetch_add(1u, std::memory_order_relaxed);
                } catch (const std::exception& ex) {
                    readerCrashed.store(true, std::memory_order_relaxed);
                    crashDetail = ex.what();
                    return;
                } catch (...) {
                    readerCrashed.store(true, std::memory_order_relaxed);
                    crashDetail = "non-std::exception thrown";
                    return;
                }
            }
        });

        std::vector<std::thread> writers;
        writers.reserve(writerCount);
        for (unsigned t = 0; t < writerCount; ++t) {
            writers.emplace_back([&stop] {
                std::uint64_t i = 1; // 0 already used by the pre-warm call
                while (!stop.load(std::memory_order_relaxed)) {
                    emitLiveTailRecord(i);
                    ++i;
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::seconds(durationSeconds));
        stop.store(true, std::memory_order_relaxed);

        for (std::thread& writer : writers) {
            writer.join();
        }
        reader.join();
    }

    checker.check(!readerCrashed.load(), "reader never throws/crashes", std::string_view{"no exception"},
                 readerCrashed.load() ? crashDetail : std::string("no exception"));
    checker.check(readerHealthy.load(), "reader.error() == Ok and undecodableRecords() == 0 on every pass",
                 true, readerHealthy.load());
    checker.check(monotonic.load(), "decoded record count is monotonically non-decreasing across passes",
                 true, monotonic.load());
    checker.check(readPasses.load() > 0u, "the reader completed at least one pass", true,
                 readPasses.load());

    return finish(checker, {
        {"writers", std::to_string(writerCount)},
        {"durationSeconds", std::to_string(durationSeconds)},
        {"readPasses", std::to_string(readPasses.load())},
        {"finalDecodedCount", std::to_string(lastSeenCount.load())},
    });
}

} // namespace sub0log::stress
