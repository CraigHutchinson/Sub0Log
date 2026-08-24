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
 *  A Logger must reach its final address before ScopedBind binds it:
 *  ScopedBind stores a Logger&, and the emit path holds onto the Logger*
 *  returned by active() for the duration of one emit() call, so moving a
 *  Logger while it is bound (or while another thread might be mid-emit)
 *  invalidates a reference/pointer someone may be using. Construct with
 *  create(), move it into its final storage if needed, *then* bind.
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

    /// This thread's current writer: the same chunk across calls as long as
    /// it is still bound to this Logger and this segment's generation.
    /// Refills from the segment (claimChunk()) the first time this thread
    /// asks, or after this Logger was recreated at the same address with a
    /// new segment (generation mismatch). Returns nullptr when the segment
    /// has never had a chunk to give this thread (caller counts a drop).
    [[nodiscard]] detail::ChunkWriter* currentWriter() noexcept;

    /// Unconditionally claims a fresh chunk for this thread, replacing the
    /// cache currentWriter() would otherwise keep returning. This is the
    /// "spent" case: the emit path already tried reserve() on
    /// currentWriter() and it did not fit, which is information only the
    /// emit path has (currentWriter() cannot tell a chunk with 3 bytes left
    /// from one with 3000 -- both are "valid"). Returns nullptr when the
    /// segment is exhausted (caller counts a drop, R9.1).
    [[nodiscard]] detail::ChunkWriter* refillWriter() noexcept;

    void countDrop() noexcept;
    void countTruncation() noexcept;
    [[nodiscard]] Stats stats() const noexcept;

    [[nodiscard]] const std::string& segmentPath() const noexcept
    {
        return segment_.path();
    }

private:
    Logger() noexcept = default;

    /// Per-thread cache behind currentWriter()/refillWriter(), keyed on
    /// {owning Logger*, segment generation} so a thread that logs through
    /// two different Loggers (or through a Logger recreated at the same
    /// address) never reuses a chunk that is not both.
    struct WriterCache {
        Logger* owner_{nullptr};
        std::uint64_t generation_{0};
        detail::ChunkWriter writer_{};
    };

    [[nodiscard]] static WriterCache& cacheForThisThread() noexcept
    {
        thread_local WriterCache cache{};
        return cache;
    }

    // constinit-friendly: no dynamic initialisation, no latched local static.
    static inline std::atomic<Logger*> sActive_{nullptr};

    detail::Segment segment_{};
    std::atomic<Severity> threshold_{Severity::Trace};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> truncated_{0};
};

// ---------------------------------------------------------------------------
// Implementation

[[nodiscard]] inline Logger Logger::create(const Options& options) noexcept
{
    Logger result{};
    result.segment_ = detail::Segment::create(options.directory_, options.stem_, options.segment_);
    result.threshold_.store(options.threshold_, std::memory_order_relaxed);
    return result;
}

inline Logger::Logger(Logger&& other) noexcept
    : segment_{std::move(other.segment_)},
      threshold_{other.threshold_.load(std::memory_order_relaxed)},
      dropped_{other.dropped_.load(std::memory_order_relaxed)},
      truncated_{other.truncated_.load(std::memory_order_relaxed)}
{
}

inline Logger::~Logger() = default;

[[nodiscard]] inline detail::ChunkWriter* Logger::currentWriter() noexcept
{
    WriterCache& cache = cacheForThisThread();
    const std::uint64_t generation = segment_.generation();
    if (cache.owner_ != this || cache.generation_ != generation) {
        cache.writer_ = segment_.claimChunk();
        cache.owner_ = this;
        cache.generation_ = generation;
    }
    return cache.writer_.valid() ? &cache.writer_ : nullptr;
}

[[nodiscard]] inline detail::ChunkWriter* Logger::refillWriter() noexcept
{
    WriterCache& cache = cacheForThisThread();
    cache.writer_ = segment_.claimChunk();
    cache.owner_ = this;
    cache.generation_ = segment_.generation();
    return cache.writer_.valid() ? &cache.writer_ : nullptr;
}

inline void Logger::countDrop() noexcept
{
    dropped_.fetch_add(1u, std::memory_order_relaxed);
}

inline void Logger::countTruncation() noexcept
{
    truncated_.fetch_add(1u, std::memory_order_relaxed);
}

[[nodiscard]] inline Stats Logger::stats() const noexcept
{
    return Stats{dropped_.load(std::memory_order_relaxed),
                truncated_.load(std::memory_order_relaxed)};
}

} // namespace sub0log
