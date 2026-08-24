// R5.5/R5.6: capturing a non-cooperating child's stdout/stderr as records
// attributed to it, with per-line interception. Decoder::decodeAll ignores
// child record kinds in v1 (its site table only knows Message/SiteDefinition),
// so these tests read at the RAW level -- SegmentReader::visit -- and check
// kind/payload bytes by hand against wire.hpp, exactly as R2.3 asks for.

#ifndef _WIN32

#include <sub0log/child.hpp>
#include <sub0log/context.hpp>
#include <sub0log/instance.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include "support/fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <cstdlib> // ::setenv/::unsetenv

namespace {



// A committed record, payload bytes copied out so it outlives the visit()
// call (RecordView::payload_ is only a view into the segment image).
struct RawRecord {
    sub0log::wire::RecordHead head_{};
    std::vector<std::byte> payload_{};
};

std::vector<RawRecord> readAllRecords(const std::vector<std::byte>& image)
{
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());
    std::vector<RawRecord> records;
    reader.visit([&records](const sub0log::RecordView& v) {
        records.push_back(RawRecord{v.head_, std::vector<std::byte>(v.payload_.begin(), v.payload_.end())});
    });
    return records;
}

std::vector<RawRecord> recordsOfKind(const std::vector<RawRecord>& all, const sub0log::wire::RecordKind kind)
{
    std::vector<RawRecord> out;
    for (const auto& r : all) {
        if (r.head_.kind_ == kind) {
            out.push_back(r);
        }
    }
    return out;
}

std::string_view payloadText(const RawRecord& r, const std::size_t offset)
{
    REQUIRE(r.payload_.size() >= offset);
    return {reinterpret_cast<const char*>(r.payload_.data() + offset), r.payload_.size() - offset};
}

/// Parses a ChildStart record's tail: u16 commandLen + command bytes.
std::string childStartCommand(const RawRecord& r)
{
    REQUIRE(r.payload_.size() >= sizeof(sub0log::wire::ChildStartPayload) + 2u);
    const std::uint16_t len = sub0log::wire::loadUnaligned<std::uint16_t>(
        r.payload_.data() + sizeof(sub0log::wire::ChildStartPayload));
    const auto text = payloadText(r, sizeof(sub0log::wire::ChildStartPayload) + 2u);
    REQUIRE(text.size() >= len);
    return std::string{text.substr(0, len)};
}

sub0log::wire::ChildStartPayload asChildStart(const RawRecord& r)
{
    REQUIRE(r.payload_.size() >= sizeof(sub0log::wire::ChildStartPayload));
    return sub0log::wire::loadUnaligned<sub0log::wire::ChildStartPayload>(r.payload_.data());
}

sub0log::wire::ChildOutputPayload asChildOutput(const RawRecord& r)
{
    REQUIRE(r.payload_.size() >= sizeof(sub0log::wire::ChildOutputPayload));
    return sub0log::wire::loadUnaligned<sub0log::wire::ChildOutputPayload>(r.payload_.data());
}

std::string_view childOutputText(const RawRecord& r)
{
    return payloadText(r, sizeof(sub0log::wire::ChildOutputPayload));
}

sub0log::wire::ChildExitPayload asChildExit(const RawRecord& r)
{
    REQUIRE(r.payload_.size() >= sizeof(sub0log::wire::ChildExitPayload));
    return sub0log::wire::loadUnaligned<sub0log::wire::ChildExitPayload>(r.payload_.data());
}

} // namespace

TEST_CASE("a captured /bin/echo produces ChildStart, ChildOutput and a clean ChildExit (R5.5)")
{
    const auto directory = sub0log::test::freshDirectory("echo");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    std::uint64_t correlationSeen = 0;
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope scope{};
        correlationSeen = scope.id();

        auto child = sub0log::ChildProcess::spawn({.argv_ = {"/bin/echo", "hello from the child"}});
        REQUIRE(child.valid());
        const auto status = child.wait();
        CHECK(status.exitCode_ == 0);
        CHECK(status.signal_ == 0);

        const auto stats = child.stats();
        CHECK(stats.capturedLines_ == 1u);
        CHECK(stats.suppressedLines_ == 0u);
        CHECK(stats.unloggedLines_ == 0u);
        CHECK(stats.truncatedLines_ == 0u);
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto starts = recordsOfKind(all, sub0log::wire::RecordKind::ChildStart);
    const auto outputs = recordsOfKind(all, sub0log::wire::RecordKind::ChildOutput);
    const auto exits = recordsOfKind(all, sub0log::wire::RecordKind::ChildExit);
    REQUIRE(starts.size() == 1u);
    REQUIRE(outputs.size() == 1u);
    REQUIRE(exits.size() == 1u);

    const auto startPayload = asChildStart(starts[0]);
    CHECK(startPayload.correlationId_ == correlationSeen);
    CHECK(childStartCommand(starts[0]).find("echo") != std::string::npos);

    const auto outPayload = asChildOutput(outputs[0]);
    CHECK(outPayload.childId_ == startPayload.childId_);
    CHECK(outPayload.stream_ == sub0log::wire::cChildStdout);
    CHECK(childOutputText(outputs[0]) == "hello from the child");
    CHECK((outputs[0].head_.flags_ & sub0log::wire::cFlagTruncated) == 0u);

    const auto exitPayload = asChildExit(exits[0]);
    CHECK(exitPayload.childId_ == startPayload.childId_);
    CHECK(exitPayload.exitCode_ == 0);
    CHECK(exitPayload.signal_ == 0);

    std::filesystem::remove_all(directory);
}

TEST_CASE("stdout and stderr are captured on separate streams, per-line (R5.5)")
{
    const auto directory = sub0log::test::freshDirectory("streams");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    {
        sub0log::Logger::ScopedBind bind{logger};
        auto child = sub0log::ChildProcess::spawn(
            {.argv_ = {"sh", "-c", "echo out1; echo out2; echo err1 1>&2; echo out3; echo err2 1>&2"}});
        REQUIRE(child.valid());
        const auto status = child.wait();
        CHECK(status.exitCode_ == 0);
        CHECK(child.stats().capturedLines_ == 5u);
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto outputs = recordsOfKind(all, sub0log::wire::RecordKind::ChildOutput);
    REQUIRE(outputs.size() == 5u);

    std::size_t stdoutCount = 0, stderrCount = 0;
    std::vector<std::string> stdoutLines, stderrLines;
    for (const auto& r : outputs) {
        const auto p = asChildOutput(r);
        const auto text = std::string{childOutputText(r)};
        if (p.stream_ == sub0log::wire::cChildStdout) {
            ++stdoutCount;
            stdoutLines.push_back(text);
        } else {
            REQUIRE(p.stream_ == sub0log::wire::cChildStderr);
            ++stderrCount;
            stderrLines.push_back(text);
        }
    }
    CHECK(stdoutCount == 3u);
    CHECK(stderrCount == 2u);
    // Per-stream order is preserved (one capture thread per stream, no
    // interleaving within a stream).
    CHECK(stdoutLines == std::vector<std::string>{"out1", "out2", "out3"});
    CHECK(stderrLines == std::vector<std::string>{"err1", "err2"});

    std::filesystem::remove_all(directory);
}

TEST_CASE("exit code and terminating signal both reach ChildExit and wait()'s return")
{
    const auto directory = sub0log::test::freshDirectory("exit");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    {
        sub0log::Logger::ScopedBind bind{logger};

        auto exited = sub0log::ChildProcess::spawn({.argv_ = {"sh", "-c", "exit 7"}});
        REQUIRE(exited.valid());
        const auto exitedStatus = exited.wait();
        CHECK(exitedStatus.exitCode_ == 7);
        CHECK(exitedStatus.signal_ == 0);

        auto killed = sub0log::ChildProcess::spawn({.argv_ = {"sh", "-c", "kill -TERM $$"}});
        REQUIRE(killed.valid());
        const auto killedStatus = killed.wait();
        CHECK(killedStatus.signal_ == SIGTERM);
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto exits = recordsOfKind(all, sub0log::wire::RecordKind::ChildExit);
    REQUIRE(exits.size() == 2u);
    CHECK(asChildExit(exits[0]).exitCode_ == 7);
    CHECK(asChildExit(exits[0]).signal_ == 0);
    CHECK(asChildExit(exits[1]).signal_ == SIGTERM);

    std::filesystem::remove_all(directory);
}

TEST_CASE("an interceptor can suppress lines, counted, and survives throwing (R5.6)")
{
    const auto directory = sub0log::test::freshDirectory("intercept");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    {
        sub0log::Logger::ScopedBind bind{logger};

        sub0log::ChildOptions options{
            .argv_ = {"sh", "-c", "echo keep-this; echo NOISE-drop-me; echo also-keep; echo NOISE-again"},
        };
        options.onLine_ = [](const sub0log::ChildLine& line) {
            if (line.text_.find("NOISE") != std::string_view::npos) {
                if (line.text_ == "NOISE-again") {
                    throw std::runtime_error{"a matcher that misbehaves"};
                }
                return sub0log::InterceptAction::Suppress;
            }
            return sub0log::InterceptAction::Log;
        };

        auto child = sub0log::ChildProcess::spawn(options);
        REQUIRE(child.valid());
        child.wait();

        const auto stats = child.stats();
        CHECK(stats.capturedLines_ == 4u);
        // One suppressed by the interceptor's own verdict, one that would
        // have been suppressed but the interceptor threw first -- an
        // escaping exception is treated as Log, so it is NOT counted here.
        CHECK(stats.suppressedLines_ == 1u);
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto outputs = recordsOfKind(all, sub0log::wire::RecordKind::ChildOutput);
    REQUIRE(outputs.size() == 3u); // keep-this, also-keep, and the one that threw (logged anyway)

    std::vector<std::string> lines;
    for (const auto& r : outputs) {
        lines.push_back(std::string{childOutputText(r)});
    }
    CHECK(std::find(lines.begin(), lines.end(), "keep-this") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "also-keep") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "NOISE-again") != lines.end()); // logged: exception -> Log
    CHECK(std::find(lines.begin(), lines.end(), "NOISE-drop-me") == lines.end()); // actually suppressed

    std::filesystem::remove_all(directory);
}

TEST_CASE("an interceptor can harvest a value from output as it arrives (R5.6)")
{
    const auto directory = sub0log::test::freshDirectory("harvest");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    std::mutex portMutex;
    std::string harvestedPort;

    {
        sub0log::Logger::ScopedBind bind{logger};

        sub0log::ChildOptions options{
            .argv_ = {"sh", "-c", "echo starting up; echo ready on port 4242; echo steady state"},
        };
        options.onLine_ = [&](const sub0log::ChildLine& line) {
            constexpr std::string_view marker = "ready on port ";
            const auto pos = line.text_.find(marker);
            if (pos != std::string_view::npos) {
                std::lock_guard<std::mutex> lock{portMutex};
                harvestedPort = std::string{line.text_.substr(pos + marker.size())};
            }
            return sub0log::InterceptAction::Log;
        };

        auto child = sub0log::ChildProcess::spawn(options);
        REQUIRE(child.valid());
        child.wait();
    }

    CHECK(harvestedPort == "4242");

    std::filesystem::remove_all(directory);
}

TEST_CASE("SUB0LOG_CORRELATION propagates into the child's environment (R5.4)")
{
    const auto directory = sub0log::test::freshDirectory("correlation");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    std::uint64_t correlationSeen = 0;
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope scope{};
        correlationSeen = scope.id();

        // A shell is not a sub0log-linked process; it cannot join the stream
        // itself, but it can print the variable, proving the parent set it.
        auto child = sub0log::ChildProcess::spawn({.argv_ = {"sh", "-c", "echo $SUB0LOG_CORRELATION"}});
        REQUIRE(child.valid());
        child.wait();
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto outputs = recordsOfKind(all, sub0log::wire::RecordKind::ChildOutput);
    REQUIRE(outputs.size() == 1u);
    CHECK(childOutputText(outputs[0]) == std::to_string(correlationSeen));

    std::filesystem::remove_all(directory);
}

TEST_CASE("a line longer than the line cap is flagged truncated and counted (R9.2)")
{
    const auto directory = sub0log::test::freshDirectory("longline");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    // sub0log::cLineCap is 4096; this line is comfortably longer.
    const std::string longLine(sub0log::cLineCap + 500u, 'x');
    const std::string script = "printf '%s\\n' \"$LONGLINE\"; echo after";

    {
        sub0log::Logger::ScopedBind bind{logger};
        ::setenv("LONGLINE", longLine.c_str(), 1);
        auto child = sub0log::ChildProcess::spawn({.argv_ = {"sh", "-c", script}});
        ::unsetenv("LONGLINE");
        REQUIRE(child.valid());
        child.wait();

        const auto stats = child.stats();
        CHECK(stats.capturedLines_ == 2u);
        CHECK(stats.truncatedLines_ == 1u);
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto outputs = recordsOfKind(all, sub0log::wire::RecordKind::ChildOutput);
    REQUIRE(outputs.size() == 2u);

    CHECK((outputs[0].head_.flags_ & sub0log::wire::cFlagTruncated) != 0u);
    CHECK(childOutputText(outputs[0]).size() == sub0log::cLineCap);
    CHECK(childOutputText(outputs[0]) == std::string_view{longLine}.substr(0, sub0log::cLineCap));

    CHECK((outputs[1].head_.flags_ & sub0log::wire::cFlagTruncated) == 0u);
    CHECK(childOutputText(outputs[1]) == "after");

    std::filesystem::remove_all(directory);
}

TEST_CASE("with no Logger bound at spawn, the child still runs and output is counted unlogged (R9.1)")
{
    // No Logger::create()/ScopedBind at all: Logger::active() is nullptr.
    auto child = sub0log::ChildProcess::spawn({.argv_ = {"sh", "-c", "echo one; echo two; echo three"}});
    REQUIRE(child.valid());
    const auto status = child.wait();
    CHECK(status.exitCode_ == 0);

    const auto stats = child.stats();
    CHECK(stats.capturedLines_ == 3u);
    CHECK(stats.unloggedLines_ == 3u);
    CHECK(stats.suppressedLines_ == 0u);
}

TEST_CASE("a nonexistent executable spawns successfully but exits 127 (R9.2: visible, not silent)")
{
    // spawn() itself only forks and execvp()s; a failed exec is the CHILD's
    // failure, reported through its exit status -- exactly what a shell
    // reports for "command not found" -- not a spawn() failure. There is no
    // channel back to the parent from inside the forked-but-not-yet-exec'd
    // child other than that exit code (documented in the header's contract).
    const auto directory = sub0log::test::freshDirectory("noexec");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    {
        sub0log::Logger::ScopedBind bind{logger};
        auto child = sub0log::ChildProcess::spawn({.argv_ = {"sub0log-definitely-not-a-real-binary-xyz"}});
        REQUIRE(child.valid());
        const auto status = child.wait();
        CHECK(status.exitCode_ == 127);
        CHECK(status.signal_ == 0);
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto exits = recordsOfKind(all, sub0log::wire::RecordKind::ChildExit);
    REQUIRE(exits.size() == 1u);
    CHECK(asChildExit(exits[0]).exitCode_ == 127);

    std::filesystem::remove_all(directory);
}

#endif // !_WIN32
