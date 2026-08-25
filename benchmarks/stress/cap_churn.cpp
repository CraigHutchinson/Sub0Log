/** @file cap_churn.cpp
 *  @brief Scenario "cap_churn": payloads straddling wire::cInlineBytesCap --
 *         some under, some exactly at, some far over. Truncation must be
 *         visible on the wire, never silent (R9.2): every over-cap record
 *         carries cFlagTruncated, no under-cap record does,
 *         Logger::stats().truncatedRecords_ matches the over-cap count
 *         exactly, and a truncated argument's decoded length is exactly the
 *         cap.
 */

#include "support/check.hpp"
#include "support/scenario.hpp"
#include "support/temp_dir.hpp"

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sub0log::stress {

namespace {

constexpr sub0log::SubsystemId cCapChurnSubsystem{102};

/// Sizes deliberately covering all three classes relative to
/// wire::cInlineBytesCap (512): well under, exactly at, and far over --
/// listed once and cycled, rather than drawn from an RNG, because the
/// classes themselves (not their variety within a class) are what the
/// invariants check.
constexpr std::size_t cSizes[] = {0, 1, 50, 100, 300, 400, 511, 512, 513, 600, 800, 1024, 2000, 5000, 10000};

void emitCapChurnRecord(std::string_view payload) noexcept
{
    sub0log_info(cCapChurnSubsystem, "cap_churn {}", payload);
}

} // namespace

ScenarioResult runCapChurn(const RunOptions& options)
{
    Checker checker{"cap_churn"};

    const unsigned repeatCount = options.quick_ ? 4u : 40u;

    constexpr std::size_t cMaxSize = 10'000;
    static const std::string cSourceBuffer(cMaxSize, 'x'); // one buffer, sliced by length below

    TempDir dir{"cap_churn"};
    auto logger = Logger::create({.directory_ = dir.path().string(), .stem_ = "cap_churn"});
    if (!checker.check(logger.valid(), "Logger::create succeeds", true, logger.valid())) {
        return finish(checker, {});
    }

    std::vector<std::size_t> expectedSizes;
    expectedSizes.reserve(repeatCount * (sizeof(cSizes) / sizeof(cSizes[0])));
    {
        Logger::ScopedBind bind{logger};
        for (unsigned rep = 0; rep < repeatCount; ++rep) {
            for (const std::size_t size : cSizes) {
                emitCapChurnRecord(std::string_view(cSourceBuffer.data(), size));
                expectedSizes.push_back(size);
            }
        }
    }

    const Stats stats = logger.stats();
    checker.check(stats.droppedRecords_ == 0u, "no drops (segment sized generously)",
                 0u, stats.droppedRecords_);

    const auto image = slurpFile(logger.segmentPath());
    auto reader = SegmentReader::open(image);
    if (!checker.check(reader.valid(), "segment opens", true, reader.valid())) {
        return finish(checker, {});
    }

    Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    checker.check(decoder.undecodableRecords() == 0u, "Decoder::undecodableRecords() == 0",
                 0u, decoder.undecodableRecords());
    checker.check(records.size() == expectedSizes.size(), "decoded record count == emitted count",
                 expectedSizes.size(), records.size());

    std::uint64_t observedTruncated = 0;
    bool everyRecordMatched = true;
    const std::size_t compareCount = std::min(records.size(), expectedSizes.size());
    for (std::size_t i = 0; i < compareCount; ++i) {
        const std::size_t emittedSize = expectedSizes[i];
        const bool expectedTruncated = emittedSize > wire::cInlineBytesCap;
        const bool actualTruncated = records[i].truncated_;
        if (actualTruncated != expectedTruncated) {
            everyRecordMatched = false;
        }
        if (actualTruncated) {
            ++observedTruncated;
        }

        if (records[i].args_.empty() || !std::holds_alternative<std::string_view>(records[i].args_[0])) {
            everyRecordMatched = false;
            continue;
        }
        const std::string_view decodedArg = std::get<std::string_view>(records[i].args_[0]);
        const std::size_t expectedDecodedLen =
            expectedTruncated ? static_cast<std::size_t>(wire::cInlineBytesCap) : emittedSize;
        if (decodedArg.size() != expectedDecodedLen) {
            everyRecordMatched = false;
        }
    }

    checker.check(everyRecordMatched,
                 "cFlagTruncated set iff over cap, and truncated length == cap exactly",
                 std::string_view{"true"}, everyRecordMatched ? std::string_view{"true"} : std::string_view{"false"});

    std::uint64_t expectedTruncatedCount = 0;
    for (const std::size_t size : expectedSizes) {
        if (size > wire::cInlineBytesCap) {
            ++expectedTruncatedCount;
        }
    }
    checker.check(stats.truncatedRecords_ == expectedTruncatedCount,
                 "Logger::stats().truncatedRecords_ == over-cap emission count",
                 expectedTruncatedCount, stats.truncatedRecords_);
    checker.check(observedTruncated == expectedTruncatedCount,
                 "decoded truncated-flag count == over-cap emission count",
                 expectedTruncatedCount, observedTruncated);

    return finish(checker, {
        {"emitted", std::to_string(expectedSizes.size())},
        {"decoded", std::to_string(records.size())},
        {"truncated", std::to_string(stats.truncatedRecords_)},
        {"cap", std::to_string(wire::cInlineBytesCap)},
    });
}

} // namespace sub0log::stress
