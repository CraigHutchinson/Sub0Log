// 06_child_capture.cpp -- capturing a third-party tool that links nothing.
//
// What this teaches: a real subprocess -- a shell script here, but it could
// be `ffmpeg`, `curl`, or any tool you did not write -- cannot be asked to
// emit Sub0Log records, because it does not link the library and never
// will. R5.5 answers that by attributing its stdout/stderr to it instead:
// ChildProcess::spawn() writes a ChildStart record (command line, pid, the
// correlation in scope), one ChildOutput record per captured line, and a
// ChildExit record when it ends -- the child itself does nothing special
// and needs nothing special, and its own diagnostics are never simply
// lost.
//
// R5.6 rides the same capture threads: a LineInterceptor sees each line
// before its record is written and can suppress noise or harvest a value
// out of the child's output as it streams in -- both used below.
// 10_child_capture_git.cpp deepens this same story against a real tool
// (`git`, driving an actual throwaway repository) rather than a synthetic
// script; this file stays focused on the mechanism.
//
// A wrinkle worth being explicit about: sub0log::Decoder (reader.hpp) has
// grown since v1 -- it now also reassembles Continuation chains into whole
// arguments (11_continuation_chains.cpp) and tracks SubsystemDefinition
// records for name lookups (02_subsystems.cpp) -- but it still has no shape
// for the three Child* kinds themselves. That is not an oversight so much
// as a boundary: Decoder builds its site table from *your* call sites, and
// a captured child obviously has none, so ChildStart/ChildOutput/ChildExit
// are counted by Decoder::skippedRecords() rather than decoded (R9.2) --
// intact on the wire, just not typed-call-site shaped. So this example,
// like tests/system/child.test.cpp, reads them at the raw
// SegmentReader::visit() level and decodes their payloads by hand against
// the structs in wire.hpp.
//
// This example runs on every first-class platform (R8.1): ChildProcess's
// Windows arm (CreateProcessW) shipped in v2, and `windows-msvc` CI runs
// seven of the nine process-spawning tests in tests/system/child.test.cpp
// (docs/architecture.md's phasing) -- the two exceptions are a signal-
// specific case and a bad-executable-path case, neither of which this
// example depends on. What differs below is only the *script text*: `sh`
// and `cmd.exe` do not speak the same syntax (`;` vs `&`, and `cmd.exe` has
// no `/bin/sh`), so the interpreter and the script are chosen together,
// per platform, the same way tests/system/child.test.cpp's own shellArgv()
// helper does. Everything past that line -- ChildProcess::spawn(), the
// interceptor, the raw record decode -- is one code path on every platform.
//
// Requirements demonstrated: R5.5 (attribution of a non-cooperating
// child), R5.6 (per-line interception: suppress and harvest).

#include <sub0log/child.hpp>
#include <sub0log/context.hpp>
#include <sub0log/instance.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include <cstdio>
#include <cstring>
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

// A committed record with its payload copied out, so it outlives the
// visit() call (RecordView::payload_ only views the segment image) --
// exactly the pattern tests/system/child.test.cpp uses to read Child*
// records at the raw level.
struct RawRecord {
    sub0log::wire::RecordHead head_{};
    std::vector<std::byte> payload_{};
};

std::vector<RawRecord> readAllRaw(const std::vector<std::byte>& image)
{
    auto reader = sub0log::SegmentReader::open(image);
    std::vector<RawRecord> records;
    if (!reader.valid()) {
        return records;
    }
    reader.visit([&records](const sub0log::RecordView& v) {
        records.push_back(RawRecord{v.head_, std::vector<std::byte>(v.payload_.begin(), v.payload_.end())});
    });
    return records;
}

std::string_view payloadText(const RawRecord& r, const std::size_t offset)
{
    return {reinterpret_cast<const char*>(r.payload_.data() + offset), r.payload_.size() - offset};
}

// argv for "run this one-line shell script and exit": cmd.exe /c on
// Windows (which has no /bin/sh), sh -c elsewhere -- the interpreter is the
// only part of this that is platform-specific; ChildProcess::spawn() itself
// is not (see the file comment above). Mirrors
// tests/system/child.test.cpp's own shellArgv() helper.
std::vector<std::string> shellArgv(const std::string& script)
{
#if defined(_WIN32)
    return {"cmd.exe", "/c", script};
#else
    return {"sh", "-c", script};
#endif
}

} // namespace

int main()
{
    const auto dir = makeScratchDir("childcapture");

    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "childcapture"});
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        return 1;
    }

    std::string harvestedPort;
    sub0log::ChildProcess::CaptureStats stats{};
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope scope{};

        // A stand-in "third-party tool": a shell script printing a mix of
        // useful lines, deliberate noise, and a readiness message with a
        // value embedded in it -- the kind of thing a real server prints
        // once its listening socket is up. `;` (sh) vs `&` (cmd.exe) is the
        // only reason this text differs by platform; what it says does not.
#if defined(_WIN32)
        const std::string script = "echo starting up&"
                                    "echo NOISE-debug-spam&"
                                    "echo ready on port 4242&"
                                    "echo NOISE-more-spam 1>&2&"
                                    "echo steady state";
#else
        const std::string script = "echo starting up; "
                                    "echo NOISE-debug-spam; "
                                    "echo ready on port 4242; "
                                    "echo NOISE-more-spam 1>&2; "
                                    "echo steady state";
#endif
        sub0log::ChildOptions options{.argv_ = shellArgv(script)};

        // R5.6: the interceptor runs on the capture thread (a cold path --
        // it drains a pipe at the child's pace, not the producer's), so it
        // is allowed to do things the hot emit path never could: string
        // searches, writing into a captured local variable.
        options.onLine_ = [&harvestedPort](const sub0log::ChildLine& line) {
            constexpr std::string_view noise = "NOISE";
            if (line.text_.find(noise) != std::string_view::npos) {
                return sub0log::InterceptAction::Suppress; // dropped, but counted (R9.1)
            }
            constexpr std::string_view marker = "ready on port ";
            const auto pos = line.text_.find(marker);
            if (pos != std::string_view::npos) {
                harvestedPort = std::string{line.text_.substr(pos + marker.size())};
            }
            return sub0log::InterceptAction::Log;
        };

        auto child = sub0log::ChildProcess::spawn(options);
        if (!child.valid()) {
            std::fprintf(stderr, "spawn failed\n");
            return 1;
        }
        const auto status = child.wait();
        stats = child.stats();

        std::printf("child exited: code=%d signal=%d\n", status.exitCode_, status.signal_);
    }

    std::printf("harvested from output while it was still streaming: port=%s\n",
                harvestedPort.empty() ? "(none)" : harvestedPort.c_str());
    std::printf("capture stats: captured=%llu suppressed=%llu unlogged=%llu truncated=%llu\n\n",
                static_cast<unsigned long long>(stats.capturedLines_),
                static_cast<unsigned long long>(stats.suppressedLines_),
                static_cast<unsigned long long>(stats.unloggedLines_),
                static_cast<unsigned long long>(stats.truncatedLines_));

    // --- the raw layer: ChildStart / ChildOutput / ChildExit by hand ----
    const auto image = slurp(onlySegment(dir));
    const auto all = readAllRaw(image);

    std::printf("-- raw Child* records (Decoder counts these skipped, not undecodable -- R9.2) --\n");
    std::size_t starts = 0, outputs = 0, exits = 0;
    for (const auto& r : all) {
        using sub0log::wire::RecordKind;
        if (r.head_.kind_ == RecordKind::ChildStart) {
            const auto p = sub0log::wire::loadUnaligned<sub0log::wire::ChildStartPayload>(r.payload_.data());
            const auto len = sub0log::wire::loadUnaligned<std::uint16_t>(
                r.payload_.data() + sizeof(sub0log::wire::ChildStartPayload));
            const auto command = payloadText(r, sizeof(sub0log::wire::ChildStartPayload) + 2u).substr(0, len);
            std::printf("ChildStart  childId=%llu pid=%llu correlation=%llu command=\"%.*s\"\n",
                        static_cast<unsigned long long>(p.childId_), static_cast<unsigned long long>(p.childPid_),
                        static_cast<unsigned long long>(p.correlationId_), static_cast<int>(command.size()),
                        command.data());
            ++starts;
        } else if (r.head_.kind_ == RecordKind::ChildOutput) {
            const auto p = sub0log::wire::loadUnaligned<sub0log::wire::ChildOutputPayload>(r.payload_.data());
            const auto text = payloadText(r, sizeof(sub0log::wire::ChildOutputPayload));
            const char* streamName = (p.stream_ == sub0log::wire::cChildStdout) ? "stdout" : "stderr";
            const bool truncated = (r.head_.flags_ & sub0log::wire::cFlagTruncated) != 0u;
            std::printf("ChildOutput childId=%llu stream=%-6s truncated=%-5s text=\"%.*s\"\n",
                        static_cast<unsigned long long>(p.childId_), streamName, truncated ? "yes" : "no",
                        static_cast<int>(text.size()), text.data());
            ++outputs;
        } else if (r.head_.kind_ == RecordKind::ChildExit) {
            const auto p = sub0log::wire::loadUnaligned<sub0log::wire::ChildExitPayload>(r.payload_.data());
            std::printf("ChildExit   childId=%llu exitCode=%d signal=%d\n",
                        static_cast<unsigned long long>(p.childId_), p.exitCode_, p.signal_);
            ++exits;
        }
    }

    bool ok = true;
    ok = ok && (harvestedPort == "4242");
    ok = ok && (stats.capturedLines_ == 5u);
    ok = ok && (stats.suppressedLines_ == 2u); // the two NOISE- lines
    ok = ok && (starts == 1 && exits == 1);
    ok = ok && (outputs == 3); // 5 captured minus the 2 suppressed

    std::filesystem::remove_all(dir);
    if (!ok) {
        std::fprintf(stderr, "one or more expectations about the capture did not hold\n");
        return 1;
    }
    return 0;
}
