/** @file child_flood.cpp
 *  @brief Scenario "child_flood" (POSIX only, R5.5/R5.6): spawns
 *         `/bin/sh -c` printing many thousands of lines fast across both
 *         stdout and stderr, and checks that capture accounts for every
 *         one of them -- captured, suppressed, or unlogged, never simply
 *         gone (R9.1 applies to capture exactly as it does to the producer
 *         path's drop counter) -- and that the persisted ChildExit record
 *         (not just the in-process wait() return) shows the exit code the
 *         shell actually used.
 */

#include "support/check.hpp"
#include "support/scenario.hpp"
#include "support/temp_dir.hpp"

#ifndef _WIN32

#include <sub0log/child.hpp>
#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sub0log::stress {

ScenarioResult runChildFlood(const RunOptions& options)
{
    Checker checker{"child_flood"};

    const std::uint64_t linesPerStream = options.quick_ ? 800u : 5'000u;

    TempDir dir{"child_flood"};
    auto logger = Logger::create({.directory_ = dir.path().string(), .stem_ = "child_flood"});
    if (!checker.check(logger.valid(), "Logger::create succeeds", true, logger.valid())) {
        return finish(checker, {});
    }

    ChildProcess::ExitStatus exitStatus{};
    ChildProcess::CaptureStats captureStats{};
    {
        Logger::ScopedBind bind{logger};

        // A tight, deterministic shell loop: N lines to stdout and N to
        // stderr, interleaved, then a clean exit(0). "seq" is the only
        // external command; the rest are shell builtins, which is what
        // makes this fast enough for --quick.
        std::string script = "for i in $(seq 1 " + std::to_string(linesPerStream) +
                             "); do echo \"out $i\"; echo \"err $i\" 1>&2; done; exit 0";

        ChildOptions options_{};
        options_.argv_ = {"/bin/sh", "-c", script};

        ChildProcess child = ChildProcess::spawn(options_);
        if (!checker.check(child.valid(), "ChildProcess::spawn succeeds", true, child.valid())) {
            return finish(checker, {});
        }

        exitStatus = child.wait();
        captureStats = child.stats();
    }

    const std::uint64_t expectedLines = linesPerStream * 2u;
    checker.check(captureStats.capturedLines_ == expectedLines,
                 "capturedLines_ == exact line count the shell was told to print", expectedLines,
                 captureStats.capturedLines_);

    // Re-open the segment and count the ChildOutput records actually
    // written -- Decoder::decodeAll() skips non-Message/SiteDefinition
    // kinds, so this walks raw RecordViews directly.
    const auto image = slurpFile(logger.segmentPath());
    auto reader = SegmentReader::open(image);
    if (!checker.check(reader.valid(), "segment opens", true, reader.valid())) {
        return finish(checker, {});
    }

    std::uint64_t loggedChildOutputLines = 0;
    std::optional<wire::ChildExitPayload> childExit;
    reader.visit([&](const RecordView& view) {
        if (view.head_.kind_ == wire::RecordKind::ChildOutput) {
            ++loggedChildOutputLines;
        } else if (view.head_.kind_ == wire::RecordKind::ChildExit) {
            if (view.payload_.size() >= sizeof(wire::ChildExitPayload)) {
                childExit = wire::loadUnaligned<wire::ChildExitPayload>(view.payload_.data());
            }
        }
    });

    const std::uint64_t accountedLines =
        loggedChildOutputLines + captureStats.suppressedLines_ + captureStats.unloggedLines_;
    checker.check(accountedLines == captureStats.capturedLines_,
                 "logged + suppressedLines_ + unloggedLines_ == capturedLines_ (R9.1: never silently gone)",
                 captureStats.capturedLines_, accountedLines);

    checker.check(childExit.has_value(), "a ChildExit record was written", true, childExit.has_value());
    if (childExit.has_value()) {
        checker.check(childExit->signal_ == 0, "ChildExit record: signal_ == 0 (exited normally)", 0,
                     childExit->signal_);
        checker.check(childExit->exitCode_ == 0, "ChildExit record: exitCode_ == 0", 0,
                     childExit->exitCode_);
    }
    checker.check(exitStatus.signal_ == 0 && exitStatus.exitCode_ == 0,
                 "ChildProcess::wait() also reports a clean exit(0)", std::string_view{"exit 0, no signal"},
                 std::string("exit ") + std::to_string(exitStatus.exitCode_) + ", signal " +
                     std::to_string(exitStatus.signal_));

    return finish(checker, {
        {"linesPerStream", std::to_string(linesPerStream)},
        {"capturedLines", std::to_string(captureStats.capturedLines_)},
        {"loggedChildOutputLines", std::to_string(loggedChildOutputLines)},
        {"suppressedLines", std::to_string(captureStats.suppressedLines_)},
        {"unloggedLines", std::to_string(captureStats.unloggedLines_)},
    });
}

} // namespace sub0log::stress

#else // _WIN32

namespace sub0log::stress {

ScenarioResult runChildFlood(const RunOptions&)
{
    return skip("child_flood", "POSIX only (child capture's Windows arm is v2, docs/architecture.md)");
}

} // namespace sub0log::stress

#endif // _WIN32
