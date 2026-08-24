#pragma once

/** @file child.hpp
 *  @brief Spawning a third-party child and capturing its stdout/stderr as
 *         records attributed to it (R5.5), with per-line interception (R5.6).
 *
 *  This is the cold path by construction: capture runs on dedicated reader
 *  threads in the parent, at pipe-buffer pace, so allocation is acceptable
 *  here (docs/record-model.md, the blob argument). Nothing in this header may
 *  be reachable from the hot emit path.
 *
 *  The child links nothing and is asked for nothing: its output streams are
 *  the whole interface (docs/multi-process.md, "Why not inherit a handle").
 *
 *  POSIX only in v1: pipe2/fork/execvp, one capture thread per stream. The
 *  Windows arm is v2 per docs/architecture.md's phasing; spawn() on Windows
 *  returns an invalid ChildProcess whose error() says so.
 */

#include "context.hpp"
#include "detail/platform.hpp"
#include "instance.hpp"
#include "severity.hpp"
#include "wire.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#  include <cerrno>
#  include <csignal>
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace sub0log {

/// What an interceptor decides for one captured line (R5.6).
enum class InterceptAction : std::uint8_t {
    Log,      ///< Write the ChildOutput record as usual.
    Suppress, ///< Do not write it; the suppression is counted, never silent.
};

/// One captured line as the interceptor sees it, before it is written.
/// The text view is valid only for the duration of the callback.
struct ChildLine {
    std::uint64_t childId_{};
    wire::ChildStream stream_{};
    std::string_view text_{}; ///< Newline stripped; truncation already applied.
    bool truncated_{};
};

/// Called on the capture thread, synchronously, once per captured line and
/// before that line's record is written. Exceptions escaping it are treated
/// as InterceptAction::Log (capture must not die because a matcher did).
using LineInterceptor = std::function<InterceptAction(const ChildLine&)>;

struct ChildOptions {
    /// argv[0] is the executable; resolved via PATH like execvp.
    std::vector<std::string> argv_{};
    std::string workingDirectory_{}; ///< Empty: inherit the parent's.
    bool captureStdout_{true};
    bool captureStderr_{true};
    /// R5.4: stamp SUB0LOG_CORRELATION into the child's environment so a
    /// cooperating descendant joins the activity; a non-cooperating child
    /// simply ignores it.
    bool propagateCorrelation_{true};
    /// Optional per-line hook (R5.6); empty means log everything.
    LineInterceptor onLine_{};
};

/// Cap on the ChildStart command-line text: the same role cInlineBytesCap
/// plays for other inline string payloads (R9.2: truncation flagged, never
/// silent), reused here rather than duplicated with a different value.
inline constexpr std::uint16_t cCommandLineCap = wire::cInlineBytesCap;

/// Cap on one captured line before it is split and flagged truncated (R5.5).
/// A child's output line is commonly longer than a single logged argument,
/// hence a multiple of cInlineBytesCap rather than that cap itself.
inline constexpr std::size_t cLineCap = static_cast<std::size_t>(wire::cInlineBytesCap) * 8u;

namespace detail {

/** Heap-allocated so its address outlives a ChildProcess move: capture
 *  threads hold a raw pointer to this, never to the ChildProcess object
 *  itself, which is what makes spawn() returning ChildProcess by value safe
 *  even without guaranteed copy elision on the named return.
 */
struct ChildCaptureState {
    Logger* logger_{nullptr};
    LineInterceptor onLine_{};
    std::uint64_t childId_{0};
    std::atomic<std::uint64_t> capturedLines_{0};
    std::atomic<std::uint64_t> suppressedLines_{0};
    std::atomic<std::uint64_t> unloggedLines_{0};
    std::atomic<std::uint64_t> truncatedLines_{0};
};

} // namespace detail

/** Spawns the child with its stdout/stderr piped, emits a ChildStart record
 *  (command line, pid, the correlation id in scope) into the active Logger,
 *  and starts one capture thread per captured stream. Each complete line
 *  becomes a ChildOutput record unless the interceptor suppresses it; lines
 *  longer than the line cap are flagged truncated. wait() reaps the child,
 *  joins the capture threads (pipes drain to EOF first, so no tail is
 *  lost), and emits ChildExit.
 *
 *  The Logger active at spawn is captured and must outlive this object.
 *  If no Logger is bound at spawn, the child still runs; its output is
 *  drained and counted unlogged in stats (a drop is never silent, R9.1).
 */
class ChildProcess {
public:
    ChildProcess(ChildProcess&&) noexcept;
    ChildProcess& operator=(ChildProcess&&) = delete;
    ChildProcess(const ChildProcess&) = delete;
    /// Waits (and drains) if still running: losing the tail of a child's
    /// output to a forgotten wait() would defeat the point of capturing it.
    ~ChildProcess();

    [[nodiscard]] static ChildProcess spawn(const ChildOptions& options) noexcept;

    [[nodiscard]] bool valid() const noexcept { return childId_ != 0; }
    [[nodiscard]] detail::PlatformError error() const noexcept { return error_; }

    /// Parent-side identity the records carry; not the OS pid (pids recycle).
    [[nodiscard]] std::uint64_t childId() const noexcept { return childId_; }
    [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

    struct ExitStatus {
        std::int32_t exitCode_{};
        std::int32_t signal_{}; ///< 0 when the child exited normally.
    };

    /// Blocks until exit and capture drain; idempotent after the first call.
    ExitStatus wait() noexcept;

    struct CaptureStats {
        std::uint64_t capturedLines_{};
        std::uint64_t suppressedLines_{}; ///< By the interceptor (R5.6) ...
        std::uint64_t unloggedLines_{};   ///< ... or because no Logger was bound.
        std::uint64_t truncatedLines_{};
    };
    [[nodiscard]] CaptureStats stats() const noexcept;

private:
    ChildProcess() noexcept = default;

    /// Writes lines from `fd` as ChildOutput records until EOF, splitting on
    /// '\n' (a trailing '\r' stripped too) and flagging an over-cLineCap
    /// line truncated while discarding its remainder up to the next '\n'.
    /// POSIX only -- never instantiated on the Windows arm (spawn() never
    /// starts a thread there).
    static void captureLoop(detail::ChildCaptureState& state, int fd, wire::ChildStream stream);

    /// One captured line: counts it, runs the interceptor (Log on an
    /// escaping exception, per the LineInterceptor contract), then writes
    /// the ChildOutput record unless suppressed or no Logger was captured.
    static void writeChildOutputLine(detail::ChildCaptureState& state, wire::ChildStream stream,
                                      std::string_view text, bool truncated);

    /// wire::ChildStartPayload then u16 commandLen + the (already-capped)
    /// command text. No-op with no Logger.
    static void writeChildStart(Logger* logger, std::uint64_t childId, std::uint64_t pid,
                                 std::uint64_t correlationId, const std::string& command,
                                 bool commandTruncated);

    /// wire::ChildExitPayload. No-op with no Logger.
    static void writeChildExit(Logger* logger, std::uint64_t childId, const ExitStatus& status);

    /// Fit-retry-drop, exactly the pattern in detail/emit.hpp: try the
    /// thread's current writer, refill once on failure, count a drop on a
    /// second failure. Shared by all three Child* record writers above.
    [[nodiscard]] static detail::ChunkWriter::Reservation
    reserveRecord(Logger& logger, std::uint32_t payloadBytes, detail::ChunkWriter*& writer) noexcept;

    std::uint64_t childId_{0};
    std::uint64_t pid_{0};
    detail::PlatformError error_{};

    /// Null until a successful spawn(); the capture threads' only handle
    /// onto shared state (see detail::ChildCaptureState's own comment).
    std::unique_ptr<detail::ChildCaptureState> state_{};
    std::vector<std::jthread> captureThreads_{};
    bool waited_{false};
    ExitStatus exitStatus_{};

    /// Process-wide; starts at 1 so 0 stays reserved for "invalid" (valid()).
    static inline std::atomic<std::uint64_t> sNextChildId_{1};
};

// ---------------------------------------------------------------------------
// Implementation shared by both platform arms

[[nodiscard]] inline detail::ChunkWriter::Reservation
ChildProcess::reserveRecord(Logger& logger, const std::uint32_t payloadBytes,
                            detail::ChunkWriter*& writer) noexcept
{
    writer = logger.currentWriter();
    detail::ChunkWriter::Reservation slot =
        (writer != nullptr) ? writer->reserve(payloadBytes) : detail::ChunkWriter::Reservation{};
    if (!slot.valid()) {
        writer = logger.refillWriter();
        slot = (writer != nullptr) ? writer->reserve(payloadBytes) : detail::ChunkWriter::Reservation{};
        if (!slot.valid()) {
            logger.countDrop();
        }
    }
    return slot;
}

inline void ChildProcess::writeChildStart(Logger* const logger, const std::uint64_t childId,
                                          const std::uint64_t pid, const std::uint64_t correlationId,
                                          const std::string& command, bool commandTruncated)
{
    if (logger == nullptr) {
        return;
    }

    // Defensive re-cap: spawn() already truncates `command` to
    // cCommandLineCap before this is called, but this function does not
    // trust that discipline blindly -- a payload that overflows the head
    // word's u16 length must never be attempted.
    std::size_t commandLen = command.size();
    if (commandLen > cCommandLineCap) {
        commandLen = cCommandLineCap;
        commandTruncated = true;
    }

    const auto payloadBytes = static_cast<std::uint32_t>(sizeof(wire::ChildStartPayload) + 2u + commandLen);

    detail::ChunkWriter* writer = nullptr;
    const detail::ChunkWriter::Reservation slot = reserveRecord(*logger, payloadBytes, writer);
    if (!slot.valid()) {
        return; // reserveRecord() already counted the drop.
    }

    wire::ChildStartPayload payload{};
    payload.childId_ = childId;
    payload.childPid_ = pid;
    payload.correlationId_ = correlationId;
    payload.monoNs_ = detail::monotonicNowNs();

    std::byte* p = slot.payload_;
    wire::storeUnaligned(p, payload);
    p += sizeof(payload);
    wire::storeUnaligned(p, static_cast<std::uint16_t>(commandLen));
    p += sizeof(std::uint16_t);
    if (commandLen > 0u) {
        std::memcpy(p, command.data(), commandLen);
    }

    wire::RecordHead head{};
    head.payloadBytes_ = static_cast<std::uint16_t>(payloadBytes);
    head.kind_ = wire::RecordKind::ChildStart;
    head.flags_ = commandTruncated ? static_cast<std::uint8_t>(wire::cFlagTruncated) : std::uint8_t{0};
    writer->commit(slot, head);
}

inline void ChildProcess::writeChildExit(Logger* const logger, const std::uint64_t childId,
                                         const ExitStatus& status)
{
    if (logger == nullptr) {
        return;
    }

    const auto payloadBytes = static_cast<std::uint32_t>(sizeof(wire::ChildExitPayload));
    detail::ChunkWriter* writer = nullptr;
    const detail::ChunkWriter::Reservation slot = reserveRecord(*logger, payloadBytes, writer);
    if (!slot.valid()) {
        return;
    }

    wire::ChildExitPayload payload{};
    payload.childId_ = childId;
    payload.monoNs_ = detail::monotonicNowNs();
    payload.exitCode_ = status.exitCode_;
    payload.signal_ = status.signal_;
    wire::storeUnaligned(slot.payload_, payload);

    wire::RecordHead head{};
    head.payloadBytes_ = static_cast<std::uint16_t>(payloadBytes);
    head.kind_ = wire::RecordKind::ChildExit;
    writer->commit(slot, head);
}

inline void ChildProcess::writeChildOutputLine(detail::ChildCaptureState& state,
                                               const wire::ChildStream stream,
                                               const std::string_view text, const bool truncated)
{
    // Every complete line is counted here, before interception can suppress
    // it or the absence of a Logger can swallow it (R5.6, R9.1).
    state.capturedLines_.fetch_add(1u, std::memory_order_relaxed);
    if (truncated) {
        state.truncatedLines_.fetch_add(1u, std::memory_order_relaxed);
    }

    InterceptAction action = InterceptAction::Log;
    if (state.onLine_) {
        const ChildLine line{state.childId_, stream, text, truncated};
        try {
            action = state.onLine_(line);
        } catch (...) {
            // A matcher's exception must not kill capture: the contract on
            // LineInterceptor treats an escaping exception as Log.
            action = InterceptAction::Log;
        }
    }
    if (action == InterceptAction::Suppress) {
        state.suppressedLines_.fetch_add(1u, std::memory_order_relaxed);
        return;
    }

    Logger* const logger = state.logger_;
    if (logger == nullptr) {
        state.unloggedLines_.fetch_add(1u, std::memory_order_relaxed);
        return;
    }

    const auto textBytes = static_cast<std::uint32_t>(text.size());
    const auto payloadBytes = static_cast<std::uint32_t>(sizeof(wire::ChildOutputPayload)) + textBytes;

    detail::ChunkWriter* writer = nullptr;
    const detail::ChunkWriter::Reservation slot = reserveRecord(*logger, payloadBytes, writer);
    if (!slot.valid()) {
        return;
    }

    wire::ChildOutputPayload payload{};
    payload.childId_ = state.childId_;
    payload.monoNs_ = detail::monotonicNowNs();
    payload.stream_ = static_cast<std::uint8_t>(stream);

    std::byte* p = slot.payload_;
    wire::storeUnaligned(p, payload);
    p += sizeof(payload);
    if (textBytes > 0u) {
        std::memcpy(p, text.data(), textBytes);
    }

    wire::RecordHead head{};
    head.payloadBytes_ = static_cast<std::uint16_t>(payloadBytes);
    head.kind_ = wire::RecordKind::ChildOutput;
    head.flags_ = truncated ? static_cast<std::uint8_t>(wire::cFlagTruncated) : std::uint8_t{0};
    writer->commit(slot, head);
}

inline ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : childId_{other.childId_}, pid_{other.pid_}, error_{other.error_},
      state_{std::move(other.state_)}, captureThreads_{std::move(other.captureThreads_)},
      waited_{other.waited_}, exitStatus_{other.exitStatus_}
{
    other.childId_ = 0;
    other.pid_ = 0;
    other.waited_ = true; // moved-from: nothing left to wait for.
}

inline ChildProcess::~ChildProcess()
{
    if (childId_ != 0u && !waited_) {
        wait();
    }
}

[[nodiscard]] inline ChildProcess::CaptureStats ChildProcess::stats() const noexcept
{
    if (!state_) {
        return CaptureStats{};
    }
    return CaptureStats{
        state_->capturedLines_.load(std::memory_order_relaxed),
        state_->suppressedLines_.load(std::memory_order_relaxed),
        state_->unloggedLines_.load(std::memory_order_relaxed),
        state_->truncatedLines_.load(std::memory_order_relaxed),
    };
}

inline ChildProcess::ExitStatus ChildProcess::wait() noexcept
{
    if (waited_) {
        return exitStatus_;
    }

    // Capture threads first: they exit at EOF, which the child's death
    // guarantees once its stdout/stderr fds close -- joining before reaping
    // can never deadlock on a child that is still writing.
    for (std::jthread& t : captureThreads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    captureThreads_.clear();

    ExitStatus result{};
#if !defined(_WIN32)
    if (pid_ != 0u) {
        int status = 0;
        pid_t reaped = 0;
        do {
            reaped = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
        } while (reaped < 0 && errno == EINTR);
        if (reaped == static_cast<pid_t>(pid_)) {
            if (WIFEXITED(status)) {
                result.exitCode_ = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.signal_ = WTERMSIG(status);
            }
        }
    }
#endif

    waited_ = true;
    exitStatus_ = result;

    if (state_) {
        writeChildExit(state_->logger_, state_->childId_, result);
    }
    return result;
}

// ---------------------------------------------------------------------------
// POSIX arm: spawn() and the capture loop.

#if !defined(_WIN32)

namespace detail {

/// pipe2(O_CLOEXEC) where available; __APPLE__ lacks pipe2, so pipe() then
/// fcntl(FD_CLOEXEC) on each end there. Either way the parent's and the
/// child's own copy of the *other* end never survives an execvp (both are
/// closed explicitly besides), which is the point of O_CLOEXEC here.
[[nodiscard]] inline bool createCloExecPipe(int fds[2]) noexcept
{
#  if defined(__APPLE__)
    if (::pipe(fds) != 0) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        const int flags = ::fcntl(fds[i], F_GETFD);
        if (flags == -1 || ::fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC) == -1) {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
    }
    return true;
#  else
    return ::pipe2(fds, O_CLOEXEC) == 0;
#  endif
}

} // namespace detail

inline void ChildProcess::captureLoop(detail::ChildCaptureState& state, const int fd,
                                      const wire::ChildStream stream)
{
    // Guards the low-probability bad_alloc from the buffer/carry below: an
    // uncaught exception on a detached-from-the-caller thread would call
    // std::terminate, and "this one stream stops being captured" is a far
    // better failure than taking the whole process down with it.
    try {
        std::vector<char> buffer(65536); // read(2) scratch; cold path, allocation is fine.
        std::string carry;
        bool discardingOverlong = false; // mid-discard of an over-cLineCap line's remainder.

        for (;;) {
            const ssize_t n = ::read(fd, buffer.data(), buffer.size());
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break; // Any other read() error ends capture like EOF would.
            }
            if (n == 0) {
                break; // EOF: every writer of the pipe (the child) has closed it.
            }

            for (ssize_t i = 0; i < n; ++i) {
                const char c = buffer[static_cast<std::size_t>(i)];
                if (c == '\n') {
                    if (discardingOverlong) {
                        discardingOverlong = false;
                        carry.clear();
                        continue;
                    }
                    if (!carry.empty() && carry.back() == '\r') {
                        carry.pop_back();
                    }
                    writeChildOutputLine(state, stream, carry, false);
                    carry.clear();
                    continue;
                }
                if (discardingOverlong) {
                    continue;
                }
                carry.push_back(c);
                if (carry.size() > cLineCap) {
                    // Longer than the cap: emit the capped prefix, flagged,
                    // and discard the rest of this logical line up to '\n'.
                    writeChildOutputLine(state, stream, std::string_view{carry}.substr(0, cLineCap), true);
                    discardingOverlong = true;
                    carry.clear();
                }
            }
        }

        if (!discardingOverlong && !carry.empty()) {
            // EOF mid-line: the final partial line is still a line (no '\r'
            // stripping here -- that rule is about a CRLF pair, and there
            // is no '\n' at all in this case).
            writeChildOutputLine(state, stream, carry, false);
        }
    } catch (...) {
        // Nothing to recover into: the stream's remaining output is lost,
        // which is a strictly rarer and smaller failure than terminating
        // the process (this is not counted -- there is no counter for
        // "this capture thread itself failed", only for what it captured).
    }

    ::close(fd);
}

[[nodiscard]] inline ChildProcess ChildProcess::spawn(const ChildOptions& options) noexcept
{
    ChildProcess result{};

    if (options.argv_.empty()) {
        result.error_ = detail::PlatformError{EINVAL, "ChildProcess::spawn: empty argv"};
        return result;
    }

    // Tracked at function scope so the catch handler below can reclaim
    // whatever this call already owns -- pipe ends, and (past fork()) the
    // child itself -- rather than leaking them if a cold-path allocation
    // throws (spawn() is noexcept end to end).
    int outRead = -1, outWrite = -1, errRead = -1, errWrite = -1;
    pid_t pid = -1;

    try {
        // Read before fork(): both are thread-local state on the calling
        // thread, and the child inherits this exact value as a copy of the
        // stack rather than re-reading its own (single-threaded, post-fork)
        // thread-local storage.
        Logger* const logger = Logger::active();
        // Same fallback as the emit path (detail/emit.hpp): no CorrelationScope
        // on this thread still means the process's own inherited activity
        // (R5.4), not "no correlation" -- a spawned child should join that
        // chain exactly as a Message record would.
        const std::uint64_t scoped = currentCorrelation();
        const std::uint64_t correlationId =
            scoped != 0u ? scoped : (logger != nullptr ? logger->rootCorrelation() : 0u);
        const bool setCorrelationEnv = options.propagateCorrelation_ && correlationId != 0u;
        const std::string correlationEnvValue =
            setCorrelationEnv ? std::to_string(correlationId) : std::string{};

        // The ChildStart command text, built and capped now: every
        // allocation this function can do happens before fork(), so a
        // failure here never leaves an orphaned child behind.
        std::string command;
        for (std::size_t i = 0; i < options.argv_.size(); ++i) {
            if (i != 0u) {
                command.push_back(' ');
            }
            command += options.argv_[i];
        }
        bool commandTruncated = false;
        if (command.size() > cCommandLineCap) {
            command.resize(cCommandLineCap);
            commandTruncated = true;
        }

        auto state = std::make_unique<detail::ChildCaptureState>();
        state->logger_ = logger;
        state->onLine_ = options.onLine_;

        std::vector<char*> execArgv;
        execArgv.reserve(options.argv_.size() + 1u);
        for (const std::string& arg : options.argv_) {
            execArgv.push_back(const_cast<char*>(arg.c_str()));
        }
        execArgv.push_back(nullptr);

        if (options.captureStdout_) {
            int fds[2];
            if (!detail::createCloExecPipe(fds)) {
                result.error_ = detail::PlatformError{errno, "pipe2(stdout)"};
                return result;
            }
            outRead = fds[0];
            outWrite = fds[1];
        }
        if (options.captureStderr_) {
            int fds[2];
            if (!detail::createCloExecPipe(fds)) {
                result.error_ = detail::PlatformError{errno, "pipe2(stderr)"};
                if (outRead != -1) {
                    ::close(outRead);
                    ::close(outWrite);
                }
                return result;
            }
            errRead = fds[0];
            errWrite = fds[1];
        }

        pid = ::fork();
        if (pid < 0) {
            result.error_ = detail::PlatformError{errno, "fork"};
            if (outRead != -1) {
                ::close(outRead);
                ::close(outWrite);
            }
            if (errRead != -1) {
                ::close(errRead);
                ::close(errWrite);
            }
            pid = -1;
            return result;
        }

        if (pid == 0) {
            // Child: no path back to the parent except the exit code, so
            // every failure here ends in _exit(127) -- exactly what a
            // failed execvp itself produces, per the contract.
            if (outWrite != -1) {
                ::dup2(outWrite, STDOUT_FILENO);
                ::close(outRead);
                ::close(outWrite);
            }
            if (errWrite != -1) {
                ::dup2(errWrite, STDERR_FILENO);
                ::close(errRead);
                ::close(errWrite);
            }
            if (!options.workingDirectory_.empty() && ::chdir(options.workingDirectory_.c_str()) != 0) {
                ::_exit(127);
            }
            if (setCorrelationEnv) {
                // Formally, setenv between fork and exec is not
                // async-signal-safe when the parent has threads (it may
                // allocate). glibc keeps malloc usable in the child via its
                // atfork handlers, and the value was built pre-fork to keep
                // this window minimal; replacing this with a pre-built envp
                // + execve would need PATH resolution by hand and is the v2
                // shape if a non-glibc target ever makes this real.
                ::setenv(detail::cCorrelationEnvVar, correlationEnvValue.c_str(), 1);
            }
            ::execvp(execArgv[0], execArgv.data());
            ::_exit(127); // execvp only returns on failure.
        }

        // Parent: the write ends belong to the child now; holding them open
        // here would mean our own read() never sees EOF (R5.5's capture
        // relies on the pipe closing when the child does).
        if (outWrite != -1) {
            ::close(outWrite);
            outWrite = -1;
        }
        if (errWrite != -1) {
            ::close(errWrite);
            errWrite = -1;
        }

        result.pid_ = static_cast<std::uint64_t>(pid);
        result.childId_ = sNextChildId_.fetch_add(1u, std::memory_order_relaxed);
        state->childId_ = result.childId_;

        writeChildStart(logger, result.childId_, result.pid_, correlationId, command, commandTruncated);

        result.state_ = std::move(state);

        if (outRead != -1) {
            detail::ChildCaptureState* const st = result.state_.get();
            result.captureThreads_.emplace_back(
                [st, fd = outRead] { captureLoop(*st, fd, wire::cChildStdout); });
            outRead = -1; // ownership moved to the capture thread.
        }
        if (errRead != -1) {
            detail::ChildCaptureState* const st = result.state_.get();
            result.captureThreads_.emplace_back(
                [st, fd = errRead] { captureLoop(*st, fd, wire::cChildStderr); });
            errRead = -1;
        }

        return result;
    } catch (...) {
        // spawn() is noexcept: an allocation on this cold path (argv/env
        // strings, the capture state, or -- past fork() -- a jthread's own
        // thread creation) may throw, and that must become an invalid
        // ChildProcess (R9.2: visible, not a crash), with whatever this call
        // already owns reclaimed rather than leaked -- a half-started child
        // killed and reaped, any still-open pipe end closed.
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
        if (outRead != -1) {
            ::close(outRead);
        }
        if (outWrite != -1) {
            ::close(outWrite);
        }
        if (errRead != -1) {
            ::close(errRead);
        }
        if (errWrite != -1) {
            ::close(errWrite);
        }

        ChildProcess failed{};
        failed.error_ = detail::PlatformError{0, "ChildProcess::spawn: allocation failed"};
        return failed;
    }
}

#else // _WIN32

[[nodiscard]] inline ChildProcess ChildProcess::spawn(const ChildOptions& /*options*/) noexcept
{
    // v2 per docs/architecture.md's phasing (CreatePipe/CreateProcessW
    // behind this same seam); v1 ships the format and the POSIX arm only.
    ChildProcess result{};
    result.error_ = detail::PlatformError{0, "ChildProcess::spawn: Windows arm not implemented yet (v2)"};
    return result;
}

inline void ChildProcess::captureLoop(detail::ChildCaptureState&, int, wire::ChildStream)
{
    // Never called: spawn() above never starts a capture thread on Windows.
}

#endif // platform switch

} // namespace sub0log
