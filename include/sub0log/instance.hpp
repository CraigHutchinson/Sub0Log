#pragma once

/** @file instance.hpp
 *  @brief The Logger instance, the scoped-active binding (R7), and the
 *         mechanism's own counters (R9).
 */

#include "segment.hpp"
#include "severity.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace sub0log {

/// R9.1: a drop is never silent. Snapshot type returned by Logger::stats().
struct Stats {
    std::uint64_t droppedRecords_{};   ///< No chunk available; record not written.
    std::uint64_t truncatedRecords_{}; ///< Written, but a payload was capped.
};

/** Owns one segment and the producer-side state. No latched global anywhere:
 *  the active instance is a constant-initialised atomic pointer, and binding
 *  is the same seam in production and in tests (R7.2).
 *
 *  Threshold checks are one relaxed load (R1.4); with no instance bound a
 *  call site costs that load and nothing else.
 *
 *  TODO(impl:producer): implement create(), the thread-local ChunkWriter
 *  cache with refill via segment_.claimChunk(), and the counters.
 */
class Logger {
public:
    struct Options {
        std::string directory_{"."};
        std::string stem_{"sub0log"};
        detail::SegmentOptions segment_{};
        Severity threshold_{Severity::Trace};
    };

    /// On failure the result has valid() false and error() set; binding an
    /// invalid Logger is harmless (its segment drops everything, counted).
    [[nodiscard]] static Logger create(const Options& options) noexcept;

    [[nodiscard]] bool valid() const noexcept { return segment_.valid(); }
    [[nodiscard]] detail::PlatformError error() const noexcept
    {
        return segment_.error();
    }

    Logger(Logger&&) noexcept;
    Logger& operator=(Logger&&) = delete;
    Logger(const Logger&) = delete;
    ~Logger();

    // -- the scoped-active pattern (R7.1) ---------------------------------

    /// The bound instance, or nullptr. Relaxed: the emit path re-checks
    /// nothing else before using it, so binding while producer threads are
    /// live is the caller's race to avoid -- bind before spawning, as
    /// Sub0Pipeline instances do.
    [[nodiscard]] static Logger* active() noexcept
    {
        return sActive_.load(std::memory_order_relaxed);
    }

    /// Binds an instance for a scope and restores the previous binding on
    /// exit, so a test can substitute its own instance and drain it
    /// synchronously (R7.1).
    class [[nodiscard]] ScopedBind {
    public:
        explicit ScopedBind(Logger& logger) noexcept
            : previous_{sActive_.exchange(&logger, std::memory_order_relaxed)} {}
        ~ScopedBind() { sActive_.store(previous_, std::memory_order_relaxed); }
        ScopedBind(const ScopedBind&) = delete;
        ScopedBind& operator=(const ScopedBind&) = delete;

    private:
        Logger* previous_;
    };

    // -- producer-path services (called from detail::emit) ----------------

    [[nodiscard]] Severity threshold() const noexcept
    {
        return threshold_.load(std::memory_order_relaxed);
    }
    void setThreshold(const Severity severity) noexcept
    {
        threshold_.store(severity, std::memory_order_relaxed);
    }

    /// This thread's current writer, refilled from the segment when spent.
    /// Returns nullptr when the segment is exhausted (caller counts a drop).
    [[nodiscard]] detail::ChunkWriter* currentWriter() noexcept;

    void countDrop() noexcept;
    void countTruncation() noexcept;
    [[nodiscard]] Stats stats() const noexcept;

    [[nodiscard]] const std::string& segmentPath() const noexcept
    {
        return segment_.path();
    }

private:
    Logger() noexcept = default;

    // constinit-friendly: no dynamic initialisation, no latched local static.
    static inline std::atomic<Logger*> sActive_{nullptr};

    detail::Segment segment_{};
    std::atomic<Severity> threshold_{Severity::Trace};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> truncated_{0};
};

} // namespace sub0log
