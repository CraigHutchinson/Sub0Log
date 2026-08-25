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
    CHECK(reader.unwrittenBytes() > 0);   // the unfilled remainder is reported
    CHECK(reader.unreadableBytes() == 0); // and none of it is damage
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

// A forked child inherits three things at once: the shared mapping (it
// survives fork), the parent's bound Logger, and this thread's writer cache
// -- pointing at the same chunk, at the same offset, as the parent. Without
// a fork handler both processes then write the same bytes to the same
// addresses and the last writer wins. Measured before the handler existed:
// 41 records emitted across a fork, 21 decodable afterwards, with drops,
// damage and undecodable all reporting zero -- the invisible loss R9.1
// exists to forbid. The child is therefore detached, and this pins both
// halves: the child finds nothing bound, and every one of the parent's
// records survives.
TEST_CASE("a forked child is detached rather than overwriting the parent's chunk")
{
    const auto directory = sub0log::test::freshDirectory("forkdetach");

    constexpr int cParentRecords = 20;
    constexpr int cChildAttempts = 20;

    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());
    {
        sub0log::Logger::ScopedBind bind{logger};
        REQUIRE_FALSE(sub0log::Logger::detachedByFork());

        for (int i = 0; i < cParentRecords; ++i) {
            sub0log_info(cStorage, "parent {}", static_cast<std::uint64_t>(i));
        }

        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            // Nothing the child does here may reach the parent's segment.
            if (!sub0log::Logger::detachedByFork()) { ::_exit(3); }
            if (sub0log::Logger::active() != nullptr) { ::_exit(4); }
            for (int i = 0; i < cChildAttempts; ++i) {
                sub0log_info(cStorage, "child {}", static_cast<std::uint64_t>(i));
            }
            ::_exit(0);
        }
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);

        // The parent is untouched by its child's detachment: still bound,
        // still writing into the same chunk it held before the fork.
        CHECK(sub0log::Logger::active() == &logger);
        CHECK_FALSE(sub0log::Logger::detachedByFork());
        sub0log_info(cStorage, "parent after fork");
    }

    const auto image = sub0log::test::slurp(logger.segmentPath());
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0u);
    CHECK(reader.unreadableBytes() == 0u);
    CHECK(logger.stats().droppedRecords_ == 0u);

    REQUIRE(records.size() == static_cast<std::size_t>(cParentRecords) + 1u);
    for (std::size_t i = 0; i < static_cast<std::size_t>(cParentRecords); ++i) {
        CAPTURE(i);
        REQUIRE(records[i].site_ != nullptr);
        CHECK(records[i].site_->format_ == "parent {}");
        CHECK(std::get<std::uint64_t>(records[i].args_[0]) == i);
    }
    REQUIRE(records.back().site_ != nullptr);
    CHECK(records.back().site_->format_ == "parent after fork");

    std::filesystem::remove_all(directory);
}

#endif // !_WIN32

// A call site is not announced once per process: it is announced once per
// segment. Two Loggers in one process, one shared logging helper -- the
// ordinary shape of a test suite binding its own instance per case, or an
// app that recreates its Logger -- and both segments must decode on their
// own. Before this was fixed the second segment held a Message whose site
// had been announced only into the first, so the record was undecodable and
// silently absent, which R7.1's scoped binding promise cannot survive.
namespace {

constexpr sub0log::SubsystemId cShared{9};

/// Deliberately a shared helper: one call site reached under both Loggers.
void logSharedSite(const int step)
{
    sub0log_info(cShared, "shared helper step {}", step);
}

} // namespace

TEST_CASE("a call site reached under a second Logger is announced into that segment too")
{
    const auto directory = sub0log::test::freshDirectory("announce");

    std::filesystem::path firstPath;
    std::filesystem::path secondPath;
    {
        auto first = sub0log::Logger::create(
            {.directory_ = directory.string(), .stem_ = "first"});
        REQUIRE(first.valid());
        sub0log::Logger::ScopedBind bind{first};
        logSharedSite(1);
        firstPath = first.segmentPath();
    }
    {
        auto second = sub0log::Logger::create(
            {.directory_ = directory.string(), .stem_ = "second"});
        REQUIRE(second.valid());
        sub0log::Logger::ScopedBind bind{second};
        logSharedSite(2);
        secondPath = second.segmentPath();
    }

    // Each segment is decodable entirely on its own.
    for (const auto& [label, path] : {std::pair{"first", firstPath},
                                      std::pair{"second", secondPath}}) {
        CAPTURE(label);
        const auto image = sub0log::test::slurp(path);
        auto reader = sub0log::SegmentReader::open(image);
        REQUIRE(reader.valid());
        sub0log::Decoder decoder;
        const auto records = decoder.decodeAll(reader);
        CHECK(decoder.undecodableRecords() == 0);
        REQUIRE(records.size() == 1);
        REQUIRE(records[0].site_ != nullptr);
        CHECK(records[0].site_->format_ == "shared helper step {}");
    }

    std::filesystem::remove_all(directory);
}


// Filling a segment must never leave a Message behind whose definition was
// dropped: such a record is intact on the wire and undecodable forever.
//
// The emit path now refuses to mark a site announced when its definition
// write failed, so the next attempt retries. Worth recording what that fix
// is and is not: I could not construct a case where the old code actually
// produced an undecodable record, because the definition's truncated floor
// (32 + one byte per argument) is never larger than the message that
// follows it (32 + the argument bytes, each at least one), so a definition
// that does not fit is always followed by a message that does not either.
// The fix is therefore defence in depth -- the invariant should not rest on
// that size coincidence holding as the format grows -- and this test pins
// the invariant itself rather than a reproduction of the bug.
TEST_CASE("filling a segment leaves no record without its definition")
{
    const auto directoryPath = sub0log::test::freshDirectory("dropdef");
    const auto directory = directoryPath.string();

    // A segment with exactly one chunk, filled until nothing more fits, so
    // the definition write is forced to fail.
    sub0log::Logger::Options options{};
    options.directory_ = directory;
    options.segment_.segmentBytes_ = sub0log::wire::cSegmentHeaderBytes + 4096u;
    options.segment_.chunkBytes_ = 4096u;

    auto logger = sub0log::Logger::create(options);
    REQUIRE(logger.valid());
    sub0log::Logger::ScopedBind bind{logger};

    // Fill the segment from one call site.
    for (int i = 0; i < 4000; ++i) {
        sub0log_info(cStorage, "filler {}", static_cast<std::uint64_t>(i));
    }
    REQUIRE(logger.stats().droppedRecords_ > 0u); // the segment is full

    // A *different*, previously unused call site now cannot write its
    // definition. Every one of these must be dropped rather than written as
    // a message the reader cannot interpret.
    for (int i = 0; i < 5; ++i) {
        sub0log_warning(cStorage, "never fits {}", static_cast<std::uint64_t>(i));
    }

    const auto image = sub0log::test::slurp(logger.segmentPath());
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    (void)records;

    // The whole point: no record in this segment lacks its definition.
    CHECK(decoder.undecodableRecords() == 0u);

    std::filesystem::remove_all(directoryPath);
}
