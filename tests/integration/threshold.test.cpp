// Per-subsystem thresholds: turning one component up without inviting the
// traffic of every other one.
//
// A single process-wide threshold meant that watching storage at Debug also
// admitted Debug from everything else -- which, at 72 ns a record, is the
// difference between watching one component and drowning. These assert
// through the segment rather than through the threshold accessors, because
// what a consumer cares about is which records exist afterwards.

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include "support/fixtures.hpp"

#include "support/test_framework.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr sub0log::SubsystemId cStorage{3};
constexpr sub0log::SubsystemId cNetwork{4};

/// The one id in these tests deliberately above the table's ceiling.
constexpr sub0log::SubsystemId cBeyondTable{sub0log::cSubsystemLevels + 7u};

[[nodiscard]] std::vector<std::string> formatsIn(const std::filesystem::path& path)
{
    const auto image = sub0log::test::slurp(path);
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0);

    std::vector<std::string> formats;
    formats.reserve(records.size());
    for (const auto& record : records) {
        REQUIRE(record.site_ != nullptr);
        formats.emplace_back(record.site_->format_);
    }
    return formats;
}

[[nodiscard]] bool contains(const std::vector<std::string>& formats, const std::string_view what)
{
    return std::ranges::find(formats, what) != formats.end();
}

} // namespace

TEST_CASE("one subsystem can be turned up without turning the rest up")
{
    const auto directory = sub0log::test::freshDirectory("subthreshold");

    auto logger = sub0log::Logger::create({.directory_ = directory.string(),
                                           .threshold_ = sub0log::Severity::Warning});
    REQUIRE(logger.valid());
    {
        sub0log::Logger::ScopedBind bind{logger};
        logger.setThreshold(cStorage, sub0log::Severity::Debug);

        sub0log_debug(cStorage, "storage detail");   // admitted: steered
        sub0log_debug(cNetwork, "network detail");   // suppressed: still Warning
        sub0log_warning(cNetwork, "network warning"); // admitted: meets Warning
    }

    const auto formats = formatsIn(logger.segmentPath());
    CHECK(contains(formats, "storage detail"));
    CHECK_FALSE(contains(formats, "network detail"));
    CHECK(contains(formats, "network warning"));

    std::filesystem::remove_all(directory);
}

TEST_CASE("a global threshold overrides subsystems already steered")
{
    const auto directory = sub0log::test::freshDirectory("globaloverride");

    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());
    {
        sub0log::Logger::ScopedBind bind{logger};
        logger.setThreshold(cStorage, sub0log::Severity::Trace);

        // "Turn everything down to Error" has to mean everything, including
        // the subsystem somebody turned up an hour ago, or it is not a
        // control -- and the operator reaching for it during an incident is
        // the one person who cannot be asked to enumerate the exceptions.
        logger.setThreshold(sub0log::Severity::Error);
        sub0log_debug(cStorage, "steered but overridden");
        sub0log_error(cStorage, "still loud enough");
    }

    const auto formats = formatsIn(logger.segmentPath());
    CHECK_FALSE(contains(formats, "steered but overridden"));
    CHECK(contains(formats, "still loud enough"));

    std::filesystem::remove_all(directory);
}

TEST_CASE("a subsystem id above the table follows the process-wide threshold")
{
    const auto directory = sub0log::test::freshDirectory("beyondtable");

    auto logger = sub0log::Logger::create({.directory_ = directory.string(),
                                           .threshold_ = sub0log::Severity::Warning});
    REQUIRE(logger.valid());
    {
        sub0log::Logger::ScopedBind bind{logger};

        // Steering it is a no-op rather than an error: the table is a fixed
        // size so the index stays a constant offset, and an id above the
        // ceiling keeps whatever the process-wide threshold says.
        logger.setThreshold(cBeyondTable, sub0log::Severity::Trace);
        CHECK(logger.threshold(cBeyondTable) == sub0log::Severity::Warning);
        sub0log_debug(cBeyondTable, "above the table, below the threshold");
        sub0log_warning(cBeyondTable, "above the table, loud enough");

        logger.setThreshold(sub0log::Severity::Trace);
        CHECK(logger.threshold(cBeyondTable) == sub0log::Severity::Trace);
        sub0log_debug(cBeyondTable, "now the global lets it through");
    }

    const auto formats = formatsIn(logger.segmentPath());
    CHECK_FALSE(contains(formats, "above the table, below the threshold"));
    CHECK(contains(formats, "above the table, loud enough"));
    CHECK(contains(formats, "now the global lets it through"));

    std::filesystem::remove_all(directory);
}
