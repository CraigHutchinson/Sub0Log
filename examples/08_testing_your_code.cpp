// 08_testing_your_code.cpp -- testing your own code's logging, no test
// framework required.
//
// What this teaches: R7.1's "scoped-active pattern" exists so *your* tests
// can bind a private Logger over a temp directory for the duration of one
// test, run the code under test, and drain the result synchronously --
// exactly the seam production code uses at startup, with no `#if
// TEST_BUILD` branch anywhere (R7.2). This file has no dependency on
// doctest, Catch2, or anything else: it is plain `if (...) { report; return
// 1; }` checks, because the point being made is that Sub0Log does not
// require a test framework to be testable, only the scoped bind and a
// decoder.
//
// The other half of the point is R2.3: every check below asserts on a
// *field* -- severity, subsystem, correlation id, an argument's typed value
// -- never on a substring of rendered text. A rendered-text assertion
// breaks the moment someone rewords a message; a field assertion does not
// care what the format string says.
//
// Requirements demonstrated: R7.1 (scoped bind + synchronous drain), R7.2
// (no test-only branch), R2.3 (assert on fields), R9.1 (the drop counter).

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::_getpid())
#else
#  include <unistd.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::getpid())
#endif

namespace {

constexpr sub0log::SubsystemId cAccount{7};

// --- the "application code" under test -------------------------------
//
// An ordinary function that happens to log. It has no idea it is being
// tested: it does not take a Logger, does not check for a test mode, and
// calls the same macros production code would. That is the point of
// binding a Logger via ScopedBind rather than passing one around --
// application code stays unaware of which Logger, if any, is active.
void applyWithdrawal(const std::uint64_t accountId, const std::int64_t amountCents,
                     const std::int64_t balanceCents)
{
    if (amountCents > balanceCents) {
        sub0log_warning(cAccount, "withdrawal {} on account {} exceeds balance {}",
                        amountCents, accountId, balanceCents);
        return;
    }
    sub0log_info(cAccount, "withdrew {} from account {}, balance now {}",
                amountCents, accountId, balanceCents - amountCents);
}

std::filesystem::path makeScratchDir(const char* const tag)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-" + std::string{tag} + "-" + std::to_string(SUB0LOG_EXAMPLE_PID()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path onlySegment(const std::filesystem::path& dir)
{
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.path().extension() == ".s0l") {
            return entry.path();
        }
    }
    return {};
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

int failuresSeen = 0;

// A minimal stand-in for a test framework's CHECK(): print, keep going, and
// remember that something failed so main() can report a nonzero exit. Real
// test frameworks add more (source locations, pretty diffs); the mechanism
// this example is demonstrating does not need any of that.
void expect(const bool condition, const char* const description)
{
    if (condition) {
        std::printf("  ok   %s\n", description);
    } else {
        std::printf("  FAIL %s\n", description);
        ++failuresSeen;
    }
}

} // namespace

int main()
{
    const auto dir = makeScratchDir("testingyourcode");

    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "test"});
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        return 1;
    }

    // The scoped bind IS the test fixture: it makes `logger` the active
    // instance for exactly the lifetime of this block, and restores
    // whatever was active before on exit (nullptr here, since nothing else
    // in this program binds one). applyWithdrawal() below needs no
    // awareness that this is happening.
    {
        sub0log::Logger::ScopedBind bind{logger};

        applyWithdrawal(/*accountId=*/1001, /*amountCents=*/2500, /*balanceCents=*/10000);
        applyWithdrawal(/*accountId=*/1001, /*amountCents=*/50000, /*balanceCents=*/7500);
    }
    // Unbound here: `logger` itself is untouched and still readable through
    // its own stats()/segmentPath() -- draining does not require the bind
    // to still be active.

    std::printf("-- Logger::stats(): the drop counter, checked like any other field (R9.1) --\n");
    const auto stats = logger.stats();
    std::printf("dropped=%llu truncated=%llu\n\n", static_cast<unsigned long long>(stats.droppedRecords_),
                static_cast<unsigned long long>(stats.truncatedRecords_));
    expect(stats.droppedRecords_ == 0, "no records were dropped (the segment was never exhausted)");

    const auto image = slurp(onlySegment(dir));
    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "segment did not open\n");
        return 1;
    }
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);

    std::printf("-- asserting on fields, never on rendered text (R2.3) --\n");
    expect(records.size() == 2, "applyWithdrawal() produced exactly 2 records");
    if (records.size() == 2) {
        const auto& approved = records[0];
        const auto& rejected = records[1];

        // Field checks: severity, subsystem, and the *typed* argument
        // values -- not "the message contains the word 'withdrew'". If
        // someone rewords applyWithdrawal()'s format strings tomorrow,
        // every check below still passes unchanged.
        expect(approved.site_->severity_ == sub0log::Severity::Info, "approved withdrawal logged at Info");
        expect(approved.site_->subsystem_ == cAccount, "approved withdrawal logged under cAccount");
        expect(approved.args_.size() == 3, "approved withdrawal carries 3 typed arguments");
        if (approved.args_.size() == 3) {
            expect(std::get<std::int64_t>(approved.args_[0]) == 2500, "withdrawal amount arrived as int64_t 2500");
            expect(std::get<std::uint64_t>(approved.args_[1]) == 1001, "account id arrived as uint64_t 1001");
            expect(std::get<std::int64_t>(approved.args_[2]) == 7500,
                  "resulting balance arrived as int64_t 7500 (10000 - 2500)");
        }

        expect(rejected.site_->severity_ == sub0log::Severity::Warning,
              "over-limit withdrawal logged at Warning, not Info");
        expect(rejected.args_.size() == 3, "rejected withdrawal also carries 3 typed arguments");
        if (rejected.args_.size() == 3) {
            expect(std::get<std::int64_t>(rejected.args_[0]) == 50000, "rejected amount arrived as int64_t 50000");
        }

        // Both calls happened with no CorrelationScope active and no
        // SUB0LOG_CORRELATION in the environment, so both records carry the
        // Logger's root correlation of 0 -- itself a field worth asserting
        // on, the same way 03_correlation.cpp asserts on a nonzero one.
        expect(approved.correlationId_ == 0, "no scope was active: correlation id is the unset value 0");
        expect(rejected.correlationId_ == approved.correlationId_,
              "both records share the same (absent) correlation");
    }

    std::filesystem::remove_all(dir);

    if (failuresSeen > 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failuresSeen);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
