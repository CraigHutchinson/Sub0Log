// 04_crash_handler.cpp -- this is the example that shows why Sub0Log exists.
//
// Most loggers answer "what if the process dies before I flush?" with "make
// sure you flush" -- a background thread, an atexit hook, a signal handler
// that tries to drain a queue. Every one of those answers assumes a
// well-behaved shutdown gets to run. R3.1/R3.2 refuse that assumption:
// a record that has been *committed* must survive SIGKILL, and nothing
// about that survival is allowed to depend on a graceful-shutdown path,
// because a graceful-shutdown path is exactly what a hard kill skips.
//
// Sub0Log can make that promise because there is nothing left to flush. A
// producer's call site copies its argument bytes straight into a page the
// kernel already owns (a MAP_SHARED mapping over a real file) and the
// commit is a single release-store of one word (docs/architecture.md,
// "Record framing"). By the time sub0log_info() returns, the record is
// already exactly as durable as it will ever be.
//
// This example proves it two ways, both without a single line of shutdown
// code:
//   1. a child process installs a signal handler that itself calls
//      sub0log_fatal() -- logging *from inside the handler* -- then lets
//      the signal kill the process for real (no cleanup, no atexit);
//   2. a second child installs no handler at all and is hard-killed with
//      SIGKILL, which cannot be caught or blocked by anything.
// The parent then opens both children's segment files and shows every
// record -- ordinary work and the in-handler fatal alike -- decodes intact.
//
// POSIX only: signal delivery and fork() are POSIX concepts: the Windows
// arm of this story (structured exception handling) is out of scope for
// v1 (docs/architecture.md's phasing) and for this example.
//
// Requirements demonstrated: R3.1 (committed records survive a hard kill),
// R3.2 (no graceful shutdown is required, or exists, on either path).

#ifndef _WIN32

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr sub0log::SubsystemId cWorker{1};

std::filesystem::path makeScratchDir(const char* const tag)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-" + std::string{tag} + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

std::filesystem::path segmentWithStem(const std::filesystem::path& dir, const std::string& stem)
{
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.path().extension() == ".s0l" && entry.path().filename().string().starts_with(stem + "-")) {
            return entry.path();
        }
    }
    return {};
}

/// Everything a signal handler is allowed to touch: async-signal-safe by
/// construction, because Sub0Log's producer path is already just an atomic
/// fetch_add and some unsynchronised memcpys into memory this thread
/// already owns (R1.3) -- there is no lock to deadlock on and no allocator
/// to re-enter. This is the property that makes logging from inside a
/// signal handler viable at all; most logging libraries cannot make this
/// claim.
extern "C" void onFatalSignal(int signalNumber)
{
    sub0log_fatal(cWorker, "caught signal {}, process is going down with no cleanup", signalNumber);

    // Restore the default disposition and let the signal actually kill the
    // process for real -- returning from this handler and continuing
    // execution would defeat the point (we want a genuine, uncontrolled
    // death, not a caught-and-carried-on). RLIMIT_CORE was set to 0 before
    // this signal was ever raised, so no core file is left behind.
    ::signal(signalNumber, SIG_DFL);
    ::raise(signalNumber);
}

/// Child A: installs the handler above, logs some ordinary work, then
/// actually crashes (a real null-pointer write) so the handler fires for
/// real -- not a staged/simulated call to it.
[[noreturn]] void runHandledChild(const std::filesystem::path& dir)
{
    rlimit noCores{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCores);

    struct sigaction action{};
    action.sa_handler = &onFatalSignal;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(SIGSEGV, &action, nullptr);
    ::sigaction(SIGABRT, &action, nullptr);

    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "handled"});
    if (!logger.valid()) { ::_exit(2); }
    sub0log::Logger::ScopedBind bind{logger};

    sub0log_info(cWorker, "handled-child: doing step {}", std::uint32_t{1});
    sub0log_info(cWorker, "handled-child: doing step {}", std::uint32_t{2});

    // The actual crash. Volatile so no optimiser reasons this away.
    int* volatile trap = nullptr;
    *trap = 42; // -> SIGSEGV -> onFatalSignal() -> sub0log_fatal() -> real death.

    ::_exit(3); // unreachable if the crash and handler behaved as described.
}

/// Child B: no handler anywhere. Logs some work, then sends itself
/// SIGKILL -- a signal no handler can intercept and no cleanup path can
/// run ahead of (this is the same shape as the library's own
/// hard-kill system test, tests/system/roundtrip.test.cpp).
[[noreturn]] void runKilledChild(const std::filesystem::path& dir)
{
    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "killed"});
    if (!logger.valid()) { ::_exit(2); }
    sub0log::Logger::ScopedBind bind{logger};

    sub0log_info(cWorker, "killed-child: doing step {}", std::uint32_t{1});
    sub0log_info(cWorker, "killed-child: doing step {}", std::uint32_t{2});
    sub0log_info(cWorker, "killed-child: about to be SIGKILLed, no destructor will run");

    ::kill(::getpid(), SIGKILL); // no handler exists for this; nothing runs after it.
    ::_exit(4); // unreachable.
}

/// Forks `body`, waits for it, and reports how it died.
struct DeathReport {
    bool signaled_{};
    int signal_{};
};

DeathReport forkAndWait(void (*body)(const std::filesystem::path&), const std::filesystem::path& dir)
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        body(dir);
        ::_exit(5); // body() is [[noreturn]]; unreachable.
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    DeathReport report{};
    report.signaled_ = WIFSIGNALED(status) != 0;
    report.signal_ = report.signaled_ ? WTERMSIG(status) : 0;
    return report;
}

int reportSegment(const std::filesystem::path& dir, const std::string& stem, const std::size_t expectedAtLeast)
{
    const auto path = segmentWithStem(dir, stem);
    if (path.empty()) {
        std::fprintf(stderr, "no segment found for stem '%s'\n", stem.c_str());
        return -1;
    }
    const auto image = slurp(path);
    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "'%s' segment did not open\n", stem.c_str());
        return -1;
    }
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);

    std::printf("-- %s.s0l: %zu record(s) recovered from a process that never shut down --\n",
                stem.c_str(), records.size());
    bool sawFatal = false;
    for (const auto& record : records) {
        std::printf("    %s\n", sub0log::Decoder::format(record).c_str());
        sawFatal = sawFatal || (record.site_->severity_ == sub0log::Severity::Fatal);
    }

    if (records.size() < expectedAtLeast) {
        std::fprintf(stderr, "'%s': expected at least %zu records, found %zu\n",
                    stem.c_str(), expectedAtLeast, records.size());
        return -1;
    }
    return sawFatal ? 1 : 0;
}

} // namespace

int main()
{
    const auto dir = makeScratchDir("crash");

    std::printf("== forking a child that crashes with a handler installed ==\n");
    const DeathReport handled = forkAndWait(&runHandledChild, dir);
    std::printf("handled child died: signaled=%s signal=%s\n\n",
                handled.signaled_ ? "yes" : "no",
                handled.signaled_ ? ::strsignal(handled.signal_) : "n/a");

    std::printf("== forking a child that is hard-killed with SIGKILL, no handler ==\n");
    const DeathReport killed = forkAndWait(&runKilledChild, dir);
    std::printf("killed child died: signaled=%s signal=%s\n\n",
                killed.signaled_ ? "yes" : "no",
                killed.signaled_ ? ::strsignal(killed.signal_) : "n/a");

    // Both children are gone. Neither ran a destructor, an atexit handler,
    // or anything resembling a flush. Everything printed below came from
    // reading the bytes they left behind (R3.1).
    const int handledResult = reportSegment(dir, "handled", 3); // 2 work + 1 fatal-from-handler
    std::printf("\n");
    const int killedResult = reportSegment(dir, "killed", 3);

    bool ok = true;
    ok = ok && handled.signaled_ && handled.signal_ == SIGSEGV;
    ok = ok && killed.signaled_ && killed.signal_ == SIGKILL;
    ok = ok && (handledResult == 1); // the fatal record written from inside the handler is there
    ok = ok && (killedResult == 0);  // ordinary records, no handler ever ran

    std::filesystem::remove_all(dir);
    if (!ok) {
        std::fprintf(stderr, "one or more expectations about the crash/kill paths did not hold\n");
        return 1;
    }
    return 0;
}

#else // _WIN32

#include <cstdio>

int main()
{
    // Structured exception handling is the Windows equivalent of this
    // story and is not implemented in v1 (docs/architecture.md's phasing:
    // "v2 ... Windows CI"). This arm exists so the example still builds and
    // passes on every first-class platform (R8.1), rather than being
    // excluded from the ladder there.
    std::puts("04_crash_handler: POSIX-only demo (signals, fork); this is a v2 story on Windows. Skipping.");
    return 0;
}

#endif // !_WIN32
