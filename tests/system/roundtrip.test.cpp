// The seam test: real producer, real files, real reader. Everything before
// this file proved the halves against the wire contract; this proves them
// against each other, including across a hard kill (R3.1) and across
// processes (R5).

#include <sub0log/log.hpp>
#include <sub0log/merge.hpp>
#include <sub0log/reader.hpp>

#include "support/fixtures.hpp"

#include "support/test_framework.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {



constexpr sub0log::SubsystemId cStorage{3};

} // namespace

TEST_CASE("a logged record round-trips typed through the file")
{
    const auto directory = sub0log::test::freshDirectory("basic");

    std::uint64_t correlationSeen = 0;
    {
        auto logger = sub0log::Logger::create({.directory_ = directory.string()});
        REQUIRE(logger.valid());
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope correlation{};
        correlationSeen = correlation.id();

        sub0log_debug(cStorage, "read {} at {} for {} bytes",
                      std::uint64_t{42}, std::int32_t{-7},
                      std::string_view{"blob.bin"});
        sub0log_error(cStorage, "no args here");
    }

    const auto image = sub0log::test::slurp(sub0log::test::onlySegmentIn(directory));
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());

    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(reader.unreadableBytes() > 0); // the unfilled remainder is reported
    CHECK(decoder.undecodableRecords() == 0);
    REQUIRE(records.size() == 2);

    // R2.1/R2.3: assert on fields, typed, never on rendered substrings.
    const auto& first = records[0];
    REQUIRE(first.site_ != nullptr);
    CHECK(first.site_->severity_ == sub0log::Severity::Debug);
    CHECK(first.site_->subsystem_ == cStorage);
    CHECK(first.site_->format_ == "read {} at {} for {} bytes");
    CHECK(first.correlationId_ == correlationSeen);
    REQUIRE(first.args_.size() == 3);
    CHECK(std::get<std::uint64_t>(first.args_[0]) == 42u);
    CHECK(std::get<std::int64_t>(first.args_[1]) == -7);
    CHECK(std::get<std::string_view>(first.args_[2]) == "blob.bin");

    CHECK(records[1].site_->severity_ == sub0log::Severity::Error);
    CHECK(records[1].args_.empty());

    // Text is a view produced on request, nowhere else (R1.1).
    CHECK(sub0log::Decoder::format(first) == "read 42 at -7 for blob.bin bytes");

    std::filesystem::remove_all(directory);
}

#ifndef _WIN32
TEST_CASE("with no scope active, records carry the environment's root correlation (R5.4)")
{
    const auto directory = sub0log::test::freshDirectory("rootcorr");

    sub0log::test::setEnvVar("SUB0LOG_CORRELATION", "7777");
    {
        auto logger = sub0log::Logger::create({.directory_ = directory.string()});
        REQUIRE(logger.valid());
        CHECK(logger.rootCorrelation() == 7777u);
        sub0log::Logger::ScopedBind bind{logger};

        // No CorrelationScope on this thread: the inherited root applies.
        sub0log_info(cStorage, "inherited activity");

        // An explicit scope overrides the root for its duration.
        {
            sub0log::CorrelationScope scoped{42u};
            sub0log_info(cStorage, "scoped activity");
        }
    }
    sub0log::test::unsetEnvVar("SUB0LOG_CORRELATION");

    const auto image = sub0log::test::slurp(sub0log::test::onlySegmentIn(directory));
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    REQUIRE(records.size() == 2);
    CHECK(records[0].correlationId_ == 7777u);
    CHECK(records[1].correlationId_ == 42u);

    std::filesystem::remove_all(directory);
}

TEST_CASE("committed records survive a hard kill of the producer (R3.1)")
{
    const auto directory = sub0log::test::freshDirectory("kill");

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // Producer process: log, then die with no shutdown path of any kind
        // (R3.2) -- no destructors, no atexit, no flush.
        auto logger = sub0log::Logger::create({.directory_ = directory.string()});
        if (!logger.valid()) { ::_exit(2); }
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cStorage, "before the end: {}", std::uint32_t{123});
        ::_exit(0);
    }

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    const auto image = sub0log::test::slurp(sub0log::test::onlySegmentIn(directory));
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    REQUIRE(records.size() == 1);
    CHECK(std::get<std::uint64_t>(records[0].args_[0]) == 123u);
    CHECK(records[0].site_->format_ == "before the end: {}");

    std::filesystem::remove_all(directory);
}

TEST_CASE("two processes, one merged stream, ordered by anchored time (R5)")
{
    const auto directory = sub0log::test::freshDirectory("merge");

    // Parent logs one record, then a forked child (its own process, its own
    // segment, its own anchor) logs one, then the parent logs a third. The
    // merge must interleave them in that order without any coordination.
    auto logger = sub0log::Logger::create({.directory_ = directory.string(),
                                           .stem_ = "parent"});
    REQUIRE(logger.valid());
    std::uint64_t correlationSeen = 0;
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope correlation{};
        correlationSeen = correlation.id();
        sub0log_info(cStorage, "parent first");

        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            // R5.4: the id crosses via the environment, as it would exec.
            ::setenv("SUB0LOG_CORRELATION",
                     std::to_string(correlation.id()).c_str(), 1);
            auto childLogger =
                sub0log::Logger::create({.directory_ = directory.string(),
                                         .stem_ = "child"});
            if (!childLogger.valid()) { ::_exit(2); }
            sub0log::Logger::ScopedBind childBind{childLogger};
            sub0log::CorrelationScope inherited{
                sub0log::detail::correlationFromEnvironment()};
            sub0log_info(cStorage, "child speaks");
            ::_exit(0);
        }
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);

        sub0log_info(cStorage, "parent last");
    }

    std::vector<std::vector<std::byte>> images;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().extension() == ".s0l") {
            images.push_back(sub0log::test::slurp(entry.path()));
        }
    }
    REQUIRE(images.size() == 2);

    sub0log::Merger merger;
    for (const auto& image : images) {
        REQUIRE(merger.addSegment(image) == sub0log::SegmentError::Ok);
    }
    const auto merged = merger.merged();
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].record_.site_->format_ == "parent first");
    CHECK(merged[1].record_.site_->format_ == "child speaks");
    CHECK(merged[2].record_.site_->format_ == "parent last");

    // R6.2/R5.4: joining the child's work to the parent's activity is an
    // equality test on the correlation field.
    CHECK(merged[1].record_.correlationId_ == correlationSeen);
    CHECK(merged[0].processId_ != merged[1].processId_);

    std::filesystem::remove_all(directory);
}
#endif // !_WIN32
