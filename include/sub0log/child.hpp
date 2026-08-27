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
 *  Two platform arms, one capture thread per stream either way: POSIX uses
 *  pipe2/fork/execvp; Windows uses CreatePipe/CreateProcessW (v2 per
 *  docs/architecture.md's phasing). Only the OS read call and the process
 *  spawn/reap differ -- the line-splitting core that turns raw bytes into
 *  ChildOutput records (captureLoop's inner loop) is one copy, shared by
 *  both arms as processCapturedChunk, and the argv -> single-string
 *  quoting CreateProcessW needs (quoteWindowsCommandLineArgument) is pure
 *  string logic with no Win32 dependency, compiled and testable on every
 *  platform even though only the Windows spawn() path calls it.
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
#else
#  include <algorithm>
#  include <cwctype>
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

/// Quotes one argument for CreateProcessW's single command-line string, per
/// the rules CommandLineToArgvW documents. Pure string logic -- no Win32
/// dependency -- so unlike the rest of the Windows arm it compiles and can
/// be round-trip tested on every platform (tests/system/child.test.cpp);
/// getting it wrong silently mis-splits any argument containing a space,
/// which on Windows is most paths, so it is worth pinning here rather than
/// trusting a naive join(" ").
[[nodiscard]] inline std::string quoteWindowsCommandLineArgument(const std::string_view arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string_view::npos) {
        return std::string{arg}; // No special characters: quoting would only add noise.
    }

    std::string out{"\""};
    std::size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // Every pending backslash is doubled, plus one more to escape
            // the quote itself: 2n+1 backslashes ahead of a literal `"`.
            out.append(backslashes * 2u + 1u, '\\');
            backslashes = 0;
            out.push_back('"');
            continue;
        }
        out.append(backslashes, '\\'); // Not before a quote: literal, undoubled.
        backslashes = 0;
        out.push_back(c);
    }
    // A run of backslashes immediately before the closing quote this
    // function is about to append must be doubled too, or
    // CommandLineToArgvW reads them as escaping *that* quote instead of
    // terminating the argument.
    out.append(backslashes * 2u, '\\');
    out.push_back('"');
    return out;
}

/// Joins a whole argv into the one string CreateProcessW takes, each
/// argument quoted independently by quoteWindowsCommandLineArgument and
/// separated by a single space (CommandLineToArgvW splits on runs of
/// unquoted whitespace, so one space between arguments is enough).
[[nodiscard]] inline std::string buildWindowsCommandLine(const std::vector<std::string>& argv)
{
    std::string result;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0u) {
            result.push_back(' ');
        }
        result += quoteWindowsCommandLineArgument(argv[i]);
    }
    return result;
}

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
        /// POSIX: the terminating signal (WTERMSIG), 0 when the child
        /// exited normally. Windows has no signal concept at all, so this
        /// field is meaningless there and is always 0; an abnormal Windows
        /// exit (TerminateProcess, an unhandled structured exception) shows
        /// up honestly in exitCode_ instead -- e.g. as
        /// STATUS_ACCESS_VIOLATION -- rather than a signal number invented
        /// to fill this field.
        std::int32_t signal_{};
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

#if !defined(_WIN32)
    /// Reads `fd` in a loop until EOF, handing each chunk to
    /// processCapturedChunk() below. POSIX only -- read(2) over an int fd.
    static void captureLoop(detail::ChildCaptureState& state, int fd, wire::ChildStream stream);
#else
    /// Reads `handle` in a loop until EOF (ERROR_BROKEN_PIPE / a
    /// zero-byte successful read), handing each chunk to
    /// processCapturedChunk() below. Windows only -- ReadFile over a HANDLE.
    static void captureLoop(detail::ChildCaptureState& state, HANDLE handle, wire::ChildStream stream);
#endif

    /// The platform-independent heart of captureLoop: splits one freshly-read
    /// chunk into complete lines -- '\n'-terminated, a trailing '\r' of a
    /// CRLF pair stripped, an over-cLineCap line flagged truncated with its
    /// remainder discarded up to the next '\n' -- and writes each one via
    /// writeChildOutputLine. `carry` (the not-yet-terminated tail) and
    /// `discardingOverlong` (mid-discard of an overlong line's remainder)
    /// are the state a capture loop threads from one chunk to the next; the
    /// caller also flushes a final non-empty `carry` at EOF itself (no '\n'
    /// arrived at all there, so the '\r'-stripping rule -- which is about a
    /// CRLF pair -- does not apply).
    static void processCapturedChunk(detail::ChildCaptureState& state, wire::ChildStream stream,
                                     std::string_view chunk, std::string& carry,
                                     bool& discardingOverlong);

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
#if defined(_WIN32)
    /// HANDLE from CreateProcessW; wait() needs it (WaitForSingleObject /
    /// GetExitCodeProcess take the handle, not pid_, which this class
    /// otherwise treats as the cross-platform identity). Null once wait()
    /// has closed it.
    HANDLE processHandle_{nullptr};
#endif
    detail::PlatformError error_{};

    /// Null until a successful spawn(); the capture threads' only handle
    /// onto shared state (see detail::ChildCaptureState's own comment).
    std::unique_ptr<detail::ChildCaptureState> state_{};
    /// std::thread rather than std::jthread: Apple's libc++ ships no
    /// jthread, and macOS is first-class (R8.1). Nothing here wanted
    /// jthread's stop token -- capture ends at EOF, which the child's death
    /// delivers -- and wait() joins explicitly, so the only property lost is
    /// the auto-join this class already implements.
    std::vector<std::thread> captureThreads_{};
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
    // The shared implementation (instance.hpp). Kept as a member so the
    // three Child* writers below read the same as they did, but there is
    // now one copy of the fit-retry-drop rule for the whole library.
    return detail::reserveRecord(logger, payloadBytes, writer);
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

inline void ChildProcess::processCapturedChunk(detail::ChildCaptureState& state,
                                               const wire::ChildStream stream,
                                               const std::string_view chunk, std::string& carry,
                                               bool& discardingOverlong)
{
    for (const char c : chunk) {
        if (c == '\n') {
            if (discardingOverlong) {
                discardingOverlong = false;
                carry.clear();
                continue;
            }
            // A Windows child's CRLF line ending is folded to the same
            // record a POSIX child's bare '\n' would produce: R5.5 says
            // nothing about the child's platform, and a reader matching on
            // captured text (R5.6-style, or after the fact) should not have
            // to special-case a trailing '\r' depending on where the child
            // ran.
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
            // Longer than the cap: emit the capped prefix, flagged, and
            // discard the rest of this logical line up to '\n'.
            writeChildOutputLine(state, stream, std::string_view{carry}.substr(0, cLineCap), true);
            discardingOverlong = true;
            carry.clear();
        }
    }
}

inline ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : childId_{other.childId_}, pid_{other.pid_},
#if defined(_WIN32)
      processHandle_{other.processHandle_},
#endif
      error_{other.error_},
      state_{std::move(other.state_)}, captureThreads_{std::move(other.captureThreads_)},
      waited_{other.waited_}, exitStatus_{other.exitStatus_}
{
    other.childId_ = 0;
    other.pid_ = 0;
#if defined(_WIN32)
    other.processHandle_ = nullptr;
#endif
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

    // Capture threads first: they exit at EOF, and draining before reaping
    // is what keeps a dying child's last lines.
    //
    // The limit, stated because the earlier comment here claimed this could
    // never deadlock and that is not true: EOF arrives when the LAST holder
    // of the pipe's write end closes it, which is not necessarily the child.
    // A child that backgrounds a grandchild inheriting the pipe
    // (`sh -c "(sleep 600 &); echo hi"`) leaves the write end open after the
    // child exits, and this join -- and therefore ~ChildProcess -- waits for
    // the grandchild. Bounding that needs a deadline on the drain, which
    // would trade a hang for lost output; until this library has an opinion
    // on that trade, the behaviour is documented rather than hidden.
    for (std::thread& t : captureThreads_) {
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
#else
    if (processHandle_ != nullptr) {
        ::WaitForSingleObject(processHandle_, INFINITE);
        DWORD exitCode = 0;
        if (::GetExitCodeProcess(processHandle_, &exitCode)) {
            // signal_ stays 0 -- see ExitStatus's own comment: Windows has
            // no signal concept, so whatever ended the child (a normal
            // return, TerminateProcess, an unhandled structured exception)
            // surfaces only as this one DWORD, reinterpreted as the 32-bit
            // exit code POSIX's WEXITSTATUS would report for a normal exit.
            result.exitCode_ = static_cast<std::int32_t>(exitCode);
        }
        ::CloseHandle(processHandle_);
        processHandle_ = nullptr;
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

            processCapturedChunk(state, stream, std::string_view{buffer.data(), static_cast<std::size_t>(n)},
                                 carry, discardingOverlong);
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
        // strings, the capture state, or -- past fork() -- a thread's own
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
// ---------------------------------------------------------------------------
// Windows arm: spawn() and the capture loop.

namespace detail {

/// A CreateProcessW(..., CREATE_UNICODE_ENVIRONMENT, ...) environment block:
/// back-to-back NUL-terminated "Name=Value" strings, double-NUL terminated.
/// Copies the parent's own environment (GetEnvironmentStringsW) rather than
/// mutating it with SetEnvironmentVariableW -- unlike POSIX's setenv
/// between fork() and execvp, Windows has no post-fork child-only window to
/// do that in, and mutating the *parent* process's live environment to
/// spawn one child would race every other thread that reads it (R8.1: this
/// library is used from multi-threaded processes).
[[nodiscard]] inline std::vector<wchar_t> buildChildEnvironmentBlock(
    const std::wstring& overrideName, const std::wstring& overrideValue)
{
    std::vector<wchar_t> block;
    if (LPWCH env = ::GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p != L'\0';) {
            const std::wstring_view entry{p};
            const auto eq = entry.find(L'=');
            // Case-insensitive name match (Windows environment variable
            // names are case-insensitive), and never a bare "=..." pseudo
            // variable (Windows' own per-drive current-directory entries,
            // e.g. "=C:=C:\foo", legitimately start with '=' at position 0
            // and must pass through untouched).
            const bool isOverride =
                eq != std::wstring_view::npos && eq == overrideName.size() &&
                std::equal(entry.begin(), entry.begin() + static_cast<std::ptrdiff_t>(eq), overrideName.begin(),
                          [](const wchar_t a, const wchar_t b) {
                              return ::towupper(static_cast<wint_t>(a)) == ::towupper(static_cast<wint_t>(b));
                          });
            if (!isOverride) {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
            p += entry.size() + 1u;
        }
        ::FreeEnvironmentStringsW(env);
    }
    block.insert(block.end(), overrideName.begin(), overrideName.end());
    block.push_back(L'=');
    block.insert(block.end(), overrideValue.begin(), overrideValue.end());
    block.push_back(L'\0');
    block.push_back(L'\0'); // Block terminator: an empty "name" ends the list.
    return block;
}

} // namespace detail

inline void ChildProcess::captureLoop(detail::ChildCaptureState& state, const HANDLE handle,
                                      const wire::ChildStream stream)
{
    // Mirrors the POSIX captureLoop's own comment: a capture thread must
    // not std::terminate the whole process over one lost stream.
    try {
        std::vector<char> buffer(65536); // ReadFile scratch; cold path, allocation is fine.
        std::string carry;
        bool discardingOverlong = false;

        for (;;) {
            DWORD bytesRead = 0;
            const BOOL ok =
                ::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr);
            if (!ok) {
                // ERROR_BROKEN_PIPE is this platform's EOF: every writer of
                // the pipe (the child) has closed it. Any other ReadFile
                // error ends capture the same way a POSIX read() error does.
                break;
            }
            if (bytesRead == 0u) {
                break; // A successful zero-byte read is EOF too.
            }
            processCapturedChunk(state, stream, std::string_view{buffer.data(), bytesRead}, carry,
                                 discardingOverlong);
        }

        if (!discardingOverlong && !carry.empty()) {
            // EOF mid-line: see the POSIX captureLoop's identical comment.
            writeChildOutputLine(state, stream, carry, false);
        }
    } catch (...) {
        // Nothing to recover into -- see the POSIX captureLoop's identical
        // comment; this stream's remaining output is lost, uncounted.
    }

    ::CloseHandle(handle);
}

[[nodiscard]] inline ChildProcess ChildProcess::spawn(const ChildOptions& options) noexcept
{
    ChildProcess result{};

    if (options.argv_.empty()) {
        result.error_ =
            detail::PlatformError{static_cast<int>(ERROR_INVALID_PARAMETER), "ChildProcess::spawn: empty argv"};
        return result;
    }

    // Tracked at function scope, mirroring the POSIX arm, so the catch
    // handler below can close whatever this call already opened rather than
    // leaking it if a cold-path allocation throws (spawn() is noexcept end
    // to end). nullptr is "not open" here the way -1 is for a POSIX fd.
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    PROCESS_INFORMATION processInfo{};

    // One place that closes whatever of the four pipe handles is currently
    // open, used on every early-return failure path below as well as the
    // catch handler -- four sites that each need it, and a leaked HANDLE
    // here is exactly the kind of mistake this task called out by name.
    const auto closePipes = [&] {
        if (outRead != nullptr) {
            ::CloseHandle(outRead);
            outRead = nullptr;
        }
        if (outWrite != nullptr) {
            ::CloseHandle(outWrite);
            outWrite = nullptr;
        }
        if (errRead != nullptr) {
            ::CloseHandle(errRead);
            errRead = nullptr;
        }
        if (errWrite != nullptr) {
            ::CloseHandle(errWrite);
            errWrite = nullptr;
        }
    };

    try {
        Logger* const logger = Logger::active();
        // Same fallback as the emit path and the POSIX arm above: no
        // CorrelationScope on this thread still means the process's own
        // inherited activity (R5.4), not "no correlation".
        const std::uint64_t scoped = currentCorrelation();
        const std::uint64_t correlationId =
            scoped != 0u ? scoped : (logger != nullptr ? logger->rootCorrelation() : 0u);
        const bool setCorrelationEnv = options.propagateCorrelation_ && correlationId != 0u;

        // The string CreateProcessW actually launches with (untruncated --
        // truncating this would mis-launch the child) and a separate,
        // possibly-capped copy for the ChildStart record. writeChildStart()
        // below re-caps defensively regardless, same as the POSIX arm.
        const std::string execCommandUtf8 = detail::buildWindowsCommandLine(options.argv_);
        std::string logCommand = execCommandUtf8;
        bool commandTruncated = false;
        if (logCommand.size() > cCommandLineCap) {
            logCommand.resize(cCommandLineCap);
            commandTruncated = true;
        }

        const detail::WidePath wideCommand = detail::toWidePath(execCommandUtf8);
        if (wideCommand.error_) {
            result.error_ = wideCommand.error_;
            return result;
        }
        // CreateProcessW's lpCommandLine is documented as a buffer the
        // function *may write into* -- a std::wstring's data() would
        // technically do, but a plain mutable buffer says so without
        // relying on the reader remembering that. Passing a string literal
        // or a c_str() here is the mistake this guards against.
        std::vector<wchar_t> commandLineBuffer(wideCommand.value_.begin(), wideCommand.value_.end());
        commandLineBuffer.push_back(L'\0');

        detail::WidePath wideDirectory{};
        const wchar_t* workingDirectory = nullptr;
        if (!options.workingDirectory_.empty()) {
            wideDirectory = detail::toWidePath(options.workingDirectory_);
            if (wideDirectory.error_) {
                result.error_ = wideDirectory.error_;
                return result;
            }
            workingDirectory = wideDirectory.value_.c_str();
        }

        std::vector<wchar_t> envBlock;
        LPVOID environment = nullptr;
        DWORD environmentFlag = 0;
        if (setCorrelationEnv) {
            const detail::WidePath wideName = detail::toWidePath(detail::cCorrelationEnvVar);
            const detail::WidePath wideValue = detail::toWidePath(std::to_string(correlationId));
            // Both inputs are ASCII (the variable name, a decimal number),
            // so this cannot fail in practice, but every toWidePath() call
            // elsewhere in this function is checked and this one is not an
            // exception to that discipline.
            if (wideName.error_ || wideValue.error_) {
                result.error_ = wideName.error_ ? wideName.error_ : wideValue.error_;
                return result;
            }
            envBlock = detail::buildChildEnvironmentBlock(wideName.value_, wideValue.value_);
            environment = envBlock.data();
            environmentFlag = CREATE_UNICODE_ENVIRONMENT;
        }

        auto state = std::make_unique<detail::ChildCaptureState>();
        state->logger_ = logger;
        state->onLine_ = options.onLine_;

        // bInheritHandle = TRUE on the pipe's SECURITY_ATTRIBUTES makes
        // *both* ends inheritable at creation time; SetHandleInformation
        // right after clears it on the end the parent keeps (the read end),
        // so only the write end -- the one the child needs -- actually
        // crosses into it. Getting this backwards leaks the parent's read
        // end into every child this process ever spawns.
        SECURITY_ATTRIBUTES inheritable{};
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;

        if (options.captureStdout_) {
            if (!::CreatePipe(&outRead, &outWrite, &inheritable, 0) ||
                !::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0)) {
                result.error_ = detail::PlatformError{static_cast<int>(::GetLastError()), "CreatePipe(stdout)"};
                closePipes();
                return result;
            }
        }
        if (options.captureStderr_) {
            if (!::CreatePipe(&errRead, &errWrite, &inheritable, 0) ||
                !::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0)) {
                result.error_ = detail::PlatformError{static_cast<int>(::GetLastError()), "CreatePipe(stderr)"};
                closePipes();
                return result;
            }
        }

        // A stream this call was not asked to capture passes through to
        // wherever the parent's own standard handle already goes -- the
        // same as leaving a fd untouched across POSIX's fork() -- rather
        // than being redirected to nothing.
        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        startupInfo.hStdOutput = options.captureStdout_ ? outWrite : ::GetStdHandle(STD_OUTPUT_HANDLE);
        startupInfo.hStdError = options.captureStderr_ ? errWrite : ::GetStdHandle(STD_ERROR_HANDLE);

        const BOOL created = ::CreateProcessW(
            nullptr, // lpApplicationName: resolved from argv[0] via lpCommandLine + PATH, like execvp.
            commandLineBuffer.data(), nullptr, nullptr,
            /* bInheritHandles */ TRUE, environmentFlag, environment, workingDirectory, &startupInfo, &processInfo);
        if (!created) {
            result.error_ = detail::PlatformError{static_cast<int>(::GetLastError()), "CreateProcessW"};
            closePipes();
            return result;
        }
        ::CloseHandle(processInfo.hThread); // Never used past spawn().
        processInfo.hThread = nullptr;

        // Parent: the write ends belong to the child now (inherited into
        // it); holding them open here would mean our own ReadFile never
        // sees EOF -- the classic hung-pipe mistake, identical in cause to
        // the POSIX arm's own comment on this same step.
        if (outWrite != nullptr) {
            ::CloseHandle(outWrite);
            outWrite = nullptr;
        }
        if (errWrite != nullptr) {
            ::CloseHandle(errWrite);
            errWrite = nullptr;
        }

        result.pid_ = static_cast<std::uint64_t>(processInfo.dwProcessId);
        // Ownership of the process handle transfers to `result` here (the
        // local is cleared) exactly like the pipe read ends below -- so the
        // catch handler's cleanup and this object's own wait()/destructor
        // never both touch it, which would be a double CloseHandle.
        result.processHandle_ = processInfo.hProcess;
        processInfo.hProcess = nullptr;
        result.childId_ = sNextChildId_.fetch_add(1u, std::memory_order_relaxed);
        state->childId_ = result.childId_;

        writeChildStart(logger, result.childId_, result.pid_, correlationId, logCommand, commandTruncated);

        result.state_ = std::move(state);

        if (outRead != nullptr) {
            detail::ChildCaptureState* const st = result.state_.get();
            result.captureThreads_.emplace_back(
                [st, h = outRead] { captureLoop(*st, h, wire::cChildStdout); });
            outRead = nullptr; // ownership moved to the capture thread.
        }
        if (errRead != nullptr) {
            detail::ChildCaptureState* const st = result.state_.get();
            result.captureThreads_.emplace_back(
                [st, h = errRead] { captureLoop(*st, h, wire::cChildStderr); });
            errRead = nullptr;
        }

        return result;
    } catch (...) {
        // spawn() is noexcept: an allocation on this cold path may throw,
        // and that must become an invalid ChildProcess (R9.2: visible, not
        // a crash), with whatever this call already owns reclaimed rather
        // than leaked -- a half-started child killed and reaped, any still
        // open handle closed. Exactly the POSIX arm's catch handler, HANDLE
        // for fd.
        if (processInfo.hProcess != nullptr) {
            ::TerminateProcess(processInfo.hProcess, 1);
            ::WaitForSingleObject(processInfo.hProcess, INFINITE);
            ::CloseHandle(processInfo.hProcess);
        }
        if (processInfo.hThread != nullptr) {
            ::CloseHandle(processInfo.hThread);
        }
        closePipes();

        ChildProcess failed{};
        failed.error_ = detail::PlatformError{0, "ChildProcess::spawn: allocation failed"};
        return failed;
    }
}

#endif // platform switch

} // namespace sub0log
