// R5.5/R5.6: capturing a non-cooperating child's stdout/stderr as records
// attributed to it, with per-line interception. Decoder::decodeAll ignores
// child record kinds in v1 (its site table only knows Message/SiteDefinition),
// so these tests read at the RAW level -- SegmentReader::visit -- and check
// kind/payload bytes by hand against wire.hpp, exactly as R2.3 asks for.
//
// Most cases run on every platform: the child command is chosen per platform
// (cmd.exe /c ... on Windows, sh -c ... elsewhere) but the assertions against
// the recorded wire bytes are identical, because R5.5/R5.6 promise the same
// thing regardless of what OS the child ran on. A few cases are genuinely
// platform-specific -- POSIX signals have no Windows equivalent, and a failed
// exec surfaces completely differently -- and stay gated, each with its own
// comment explaining why.

#include <sub0log/child.hpp>
#include <sub0log/context.hpp>
#include <sub0log/instance.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include "support/fixtures.hpp"

#include "support/test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#  include <csignal> // SIGTERM, for the POSIX-only terminating-signal case.
#endif

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

/// argv for "run this one-line shell script and exit": cmd.exe /c on
/// Windows (which has no /bin/sh), sh -c elsewhere. The script text itself
/// is *not* portable between the two shells (";" vs "&", "$VAR" vs "%VAR%"),
/// so callers that need a script write it per-platform and pass it here --
/// this only picks the interpreter.
std::vector<std::string> shellArgv(const std::string& script)
{
#if defined(_WIN32)
    return {"cmd.exe", "/c", script};
#else
    return {"sh", "-c", script};
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Windows command-line quoting (child.hpp's quoteWindowsCommandLineArgument
// and buildWindowsCommandLine): pure string logic with no Win32 dependency,
// so -- unlike the rest of this file -- these run and are meaningful on
// every platform, including the POSIX CI that is the only CI this task's
// author can actually run. There is no CommandLineToArgvW to call outside
// Windows, so the round-trip case below replicates the algorithm it
// documents (the inverse of the quoting function under test) rather than
// skip verifying the split.

namespace {

/// Mirrors the documented CommandLineToArgvW / MSVC CRT command-line
/// splitting rules: a run of N backslashes followed by a '"' contributes
/// N/2 literal backslashes, and either escapes a literal '"' (N odd) or
/// toggles quoting (N even, consumed); a run of backslashes not followed by
/// '"' is all literal; a bare '"' toggles quoting; unquoted whitespace ends
/// an argument. Written only so the quoting logic can be round-trip tested
/// here, without a Windows box to run CommandLineToArgvW itself on.
std::vector<std::string> splitWindowsCommandLineForTest(const std::string& commandLine)
{
    std::vector<std::string> args;
    std::string current;
    bool inQuotes = false;
    bool haveArg = false;
    std::size_t i = 0;
    const std::size_t n = commandLine.size();

    while (i < n) {
        const char c = commandLine[i];
        if (!inQuotes && (c == ' ' || c == '\t')) {
            if (haveArg) {
                args.push_back(current);
                current.clear();
                haveArg = false;
            }
            ++i;
            continue;
        }
        haveArg = true;
        if (c == '\\') {
            std::size_t backslashes = 0;
            while (i < n && commandLine[i] == '\\') {
                ++backslashes;
                ++i;
            }
            if (i < n && commandLine[i] == '"') {
                current.append(backslashes / 2u, '\\');
                if (backslashes % 2u == 1u) {
                    current.push_back('"'); // Escaped quote: literal, does not toggle.
                    ++i;
                } else {
                    inQuotes = !inQuotes; // Unescaped: toggles quoting, and is consumed.
                    ++i;
                }
            } else {
                current.append(backslashes, '\\'); // Not before a quote: every one is literal.
            }
            continue;
        }
        if (c == '"') {
            inQuotes = !inQuotes;
            ++i;
            continue;
        }
        current.push_back(c);
        ++i;
    }
    if (haveArg) {
        args.push_back(current);
    }
    return args;
}

} // namespace

TEST_CASE("Windows command-line quoting round-trips through the documented split rules")
{
    // Each vector is a whole argv; buildWindowsCommandLine joins it into the
    // one string CreateProcessW would take, and splitting that back apart
    // must reproduce the original argv exactly -- the property that matters
    // (spawn() launches the child CreateProcessW's own parser will re-split
    // the same way), not any particular quoted-string shape.
    const std::vector<std::vector<std::string>> vectors = {
        {"simple"},
        {"has space"},
        {"trailing\\backslash\\in\\the\\middle"},
        {"has space and a trailing backslash\\"},
        {"embedded\"quote"},
        {"back\\\"slash-then-quote"},
        {""},
        {"C:\\Program Files\\App\\app.exe", "--flag", "value with spaces", "trailing\\\\", ""},
        {"a\\\\\\"},
        {"tab\ttab"},
        {"quote\"and\\backslash\\\"combo"},
        {"\"\""},
        {"C:\\Program Files (x86)\\Vendor\\tool.exe", "--config", "C:\\Users\\a b\\config.json"},
    };

    for (const auto& argv : vectors) {
        const std::string commandLine = sub0log::detail::buildWindowsCommandLine(argv);
        const std::vector<std::string> roundTripped = splitWindowsCommandLineForTest(commandLine);
        CHECK(roundTripped == argv);
    }
}

TEST_CASE("Windows command-line quoting matches the documented rules on known shapes")
{
    using sub0log::detail::quoteWindowsCommandLineArgument;

    // No special characters: quoting would only add noise.
    CHECK(quoteWindowsCommandLineArgument("plain") == "plain");
    // A space forces quoting, with nothing else changed.
    CHECK(quoteWindowsCommandLineArgument("a b") == "\"a b\"");
    // An embedded quote forces quoting and is itself escaped with one backslash.
    CHECK(quoteWindowsCommandLineArgument("a\"b") == "\"a\\\"b\"");
    // Backslashes nowhere near a quote pass through literally, undoubled, unquoted.
    CHECK(quoteWindowsCommandLineArgument("a\\b") == "a\\b");
    CHECK(quoteWindowsCommandLineArgument("a\\") == "a\\");
    // Once a space forces quoting, a trailing backslash must be doubled --
    // otherwise it would escape the closing quote this function adds rather
    // than terminate the argument.
    CHECK(quoteWindowsCommandLineArgument("a \\") == "\"a \\\\\"");
    // An empty argument still needs an explicit "" -- CommandLineToArgvW has
    // no other way to produce a zero-length argv entry.
    CHECK(quoteWindowsCommandLineArgument("") == "\"\"");
}

// ---------------------------------------------------------------------------
// Process spawn/capture: most cases run on every platform (R5.5/R5.6), a few
// stay POSIX-only where Windows has no equivalent to assert against.

TEST_CASE("a captured echo produces ChildStart, ChildOutput and a clean ChildExit (R5.5)")
{
    const auto directory = sub0log::test::freshDirectory("echo");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    std::uint64_t correlationSeen = 0;
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope scope{};
        correlationSeen = scope.id();

        // POSIX execs /bin/echo directly (no shell); Windows has no such
        // binary, so cmd.exe's builtin is the closest equivalent -- its own
        // /c parsing takes the rest of the line literally here, no
        // sub0log-side quoting concerns for a plain phrase like this one.
#if defined(_WIN32)
        auto child = sub0log::ChildProcess::spawn({.argv_ = {"cmd.exe", "/c", "echo hello from the child"}});
#else
        auto child = sub0log::ChildProcess::spawn({.argv_ = {"/bin/echo", "hello from the child"}});
#endif
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
#if defined(_WIN32)
        // The redirection leads each stderr line rather than trailing it,
        // and that is not style. `echo err1 1>&2` in cmd emits "err1 " --
        // cmd takes everything between `echo ` and the redirection token as
        // the text to print, so the space in front of the operator is part
        // of the output. A shell tokenises the redirection out first and
        // prints "err1". Identical-looking syntax, different parse, and the
        // difference is one trailing byte that only Windows CI can see.
        const std::string script = "echo out1&echo out2&>&2 echo err1&echo out3&>&2 echo err2";
#else
        const std::string script = "echo out1; echo out2; echo err1 1>&2; echo out3; echo err2 1>&2";
#endif
        auto child = sub0log::ChildProcess::spawn({.argv_ = shellArgv(script)});
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

TEST_CASE("an exit code reaches ChildExit and wait()'s return")
{
    const auto directory = sub0log::test::freshDirectory("exit");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    {
        sub0log::Logger::ScopedBind bind{logger};

        auto exited = sub0log::ChildProcess::spawn({.argv_ = shellArgv("exit 7")});
        REQUIRE(exited.valid());
        const auto exitedStatus = exited.wait();
        CHECK(exitedStatus.exitCode_ == 7);
        CHECK(exitedStatus.signal_ == 0);
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto exits = recordsOfKind(all, sub0log::wire::RecordKind::ChildExit);
    REQUIRE(exits.size() == 1u);
    CHECK(asChildExit(exits[0]).exitCode_ == 7);
    CHECK(asChildExit(exits[0]).signal_ == 0);

    std::filesystem::remove_all(directory);
}

#if !defined(_WIN32)
// Windows has no signal concept at all (ExitStatus::signal_'s own comment):
// there is nothing portable to send a child that terminates it the way
// SIGTERM does, and nothing for signal_ to hold there but 0. This case is
// therefore POSIX-only rather than approximated with e.g. TerminateProcess,
// which is a different, unsignaled kind of kill with no WTERMSIG analogue.
TEST_CASE("a terminating signal reaches ChildExit and wait()'s return as signal_ (POSIX only)")
{
    const auto directory = sub0log::test::freshDirectory("signal");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    {
        sub0log::Logger::ScopedBind bind{logger};

        auto killed = sub0log::ChildProcess::spawn({.argv_ = {"sh", "-c", "kill -TERM $$"}});
        REQUIRE(killed.valid());
        const auto killedStatus = killed.wait();
        CHECK(killedStatus.signal_ == SIGTERM);
    }

    const auto all = readAllRecords(sub0log::test::slurp(sub0log::test::onlySegmentIn(directory)));
    const auto exits = recordsOfKind(all, sub0log::wire::RecordKind::ChildExit);
    REQUIRE(exits.size() == 1u);
    CHECK(asChildExit(exits[0]).signal_ == SIGTERM);

    std::filesystem::remove_all(directory);
}
#endif // !_WIN32

TEST_CASE("an interceptor can suppress lines, counted, and survives throwing (R5.6)")
{
    const auto directory = sub0log::test::freshDirectory("intercept");
    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    {
        sub0log::Logger::ScopedBind bind{logger};

#if defined(_WIN32)
        const std::string script = "echo keep-this&echo NOISE-drop-me&echo also-keep&echo NOISE-again";
#else
        const std::string script = "echo keep-this; echo NOISE-drop-me; echo also-keep; echo NOISE-again";
#endif
        sub0log::ChildOptions options{.argv_ = shellArgv(script)};
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

#if defined(_WIN32)
        const std::string script = "echo starting up&echo ready on port 4242&echo steady state";
#else
        const std::string script = "echo starting up; echo ready on port 4242; echo steady state";
#endif
        sub0log::ChildOptions options{.argv_ = shellArgv(script)};
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
#if defined(_WIN32)
        auto child = sub0log::ChildProcess::spawn({.argv_ = shellArgv("echo %SUB0LOG_CORRELATION%")});
#else
        auto child = sub0log::ChildProcess::spawn({.argv_ = shellArgv("echo $SUB0LOG_CORRELATION")});
#endif
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
#if defined(_WIN32)
    const std::string script = "echo %LONGLINE%&echo after";
#else
    const std::string script = "printf '%s\\n' \"$LONGLINE\"; echo after";
#endif

    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::test::setEnvVar("LONGLINE", longLine.c_str());
        auto child = sub0log::ChildProcess::spawn({.argv_ = shellArgv(script)});
        sub0log::test::unsetEnvVar("LONGLINE");
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
#if defined(_WIN32)
    const std::string script = "echo one&echo two&echo three";
#else
    const std::string script = "echo one; echo two; echo three";
#endif
    auto child = sub0log::ChildProcess::spawn({.argv_ = shellArgv(script)});
    REQUIRE(child.valid());
    const auto status = child.wait();
    CHECK(status.exitCode_ == 0);

    const auto stats = child.stats();
    CHECK(stats.capturedLines_ == 3u);
    CHECK(stats.unloggedLines_ == 3u);
    CHECK(stats.suppressedLines_ == 0u);
}

#if !defined(_WIN32)
// spawn()'s contract for a bad executable is platform-specific by
// construction, not just untested on Windows: POSIX's spawn() only forks
// and execvp()s, so a failed exec is the CHILD's failure, reported through
// its exit status (127) with spawn() itself having already returned a valid
// ChildProcess -- there is no channel back to the parent from inside the
// forked-but-not-yet-exec'd child other than that exit code. CreateProcessW
// has no such split: resolving the executable happens synchronously in the
// PARENT before any child exists, so a bad path fails spawn() itself
// (invalid ChildProcess, error() set) rather than producing exit code 127.
// Both are R9.2-honest -- visible, not silent -- but they are different
// shapes of visible, and asserting POSIX's shape on Windows would be wrong.
TEST_CASE("a nonexistent executable spawns successfully but exits 127 (R9.2: visible, not silent)")
{
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
