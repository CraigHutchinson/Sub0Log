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
 */

#include "context.hpp"
#include "detail/platform.hpp"
#include "instance.hpp"
#include "severity.hpp"
#include "wire.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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

/** Spawns the child with its stdout/stderr piped, emits a ChildStart record
 *  (command line, pid, the correlation id in scope) into the active Logger,
 *  and starts one capture thread per captured stream. Each complete line
 *  becomes a ChildOutput record unless the interceptor suppresses it; lines
 *  longer than the record cap are split, with continuation slices flagged
 *  truncated. wait() reaps the child, joins the capture threads (pipes
 *  drain to EOF first, so no tail is lost), and emits ChildExit.
 *
 *  The Logger active at spawn is captured and must outlive this object.
 *  If no Logger is bound at spawn, the child still runs; its output is
 *  drained and counted as suppressed-by-absence in stats (a drop is never
 *  silent, R9.1).
 *
 *  TODO(impl:child): POSIX first -- pipe2/fork/execvp (or posix_spawn with
 *  file actions), capture threads on read(2) with a bounded line buffer;
 *  Windows behind the platform seam with CreatePipe/CreateProcessW. The
 *  interceptor runs before the record write, wrapped in try/catch per the
 *  LineInterceptor contract.
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

    std::uint64_t childId_{0};
    std::uint64_t pid_{0};
    detail::PlatformError error_{};
    // TODO(impl:child): pipe fds, capture threads, Logger*, options, stats.
};

} // namespace sub0log
