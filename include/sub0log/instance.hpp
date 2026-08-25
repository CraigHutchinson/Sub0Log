#pragma once

/** @file instance.hpp
 *  @brief The Logger instance, the scoped-active binding (R7), and the
 *         mechanism's own counters (R9).
 */

#include "context.hpp"
#include "segment.hpp"
#include "severity.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

/// Keeps a cold branch's body out of the function that tests for it. There
/// is no standard spelling for "do not inline this" -- `[[unlikely]]` says
/// which way to predict, not where to put the code -- so this is one of the
/// isolated, documented extensions R8.2 allows, and countUnboundEmit()
/// carries the measurement that made it necessary.
#if defined(__GNUC__) || defined(__clang__)
#  define SUB0LOG_COLD_PATH [[gnu::noinline, gnu::cold]]
#elif defined(_MSC_VER)
#  define SUB0LOG_COLD_PATH __declspec(noinline)
#else
#  define SUB0LOG_COLD_PATH
#endif

namespace sub0log {

/// How many subsystem ids get their own threshold slot. Sized to one cache
/// line's worth of levels: large enough that a consumer's named subsystems
/// fit below it, small enough that the table is not a page. See
/// Logger::threshold(SubsystemId) for what happens above it.
inline constexpr std::uint32_t cSubsystemLevels = 64u;

/// R9.1: a drop is never silent. Snapshot type returned by Logger::stats().
struct Stats {
    std::uint64_t droppedRecords_{};   ///< No chunk available; record not written.
    std::uint64_t truncatedRecords_{}; ///< Written, but a payload was capped.
};

/// Ceiling on the unbound-emit count (R9.3). Counting is exact below it and
/// stops there, which is the whole of what makes the counter affordable --
/// see unboundEmits() for the measurements that set it.
inline constexpr std::uint64_t cUnboundEmitCap = 1024u;

namespace detail {

/// Counts emit attempts that found no bound instance. See unboundEmits().
inline std::atomic<std::uint64_t> sUnboundEmits{0};

/** Counts one unbound emit and answers enabled()'s check for it. Always
 *  false: there is nothing to emit into, which is the whole point.
 *
 *  Out of line and cold by attribute, and that is not a micro-optimisation
 *  -- it is the difference between this diagnostic being free and it being
 *  the most expensive thing on R1.4's path. Inlined, the counter's load,
 *  compare and atomic add sit in the middle of every threshold check and
 *  cost 1.08 ns per disabled call site against 0.57 ns without them,
 *  because the branchless compare the check used to compile to becomes a
 *  real branch around real work. Moved out of line the number goes back to
 *  0.56 ns: identical, with the counter present.
 *
 *  `[[unlikely]]` alone was tried first, since it is the standard spelling
 *  and STYLE_GUIDE's rule is to prefer one -- it is what the call site
 *  already carries, and it is what produced the 1.08 ns. The branch hint
 *  says which way to predict; only `noinline` keeps the work out of the
 *  caller, and no standard attribute says that.
 */
[[nodiscard]] SUB0LOG_COLD_PATH inline bool countUnboundEmit() noexcept
{
    // Read before write, and stop at the cap. Both halves are about the
    // cache line rather than the arithmetic: an unconditional fetch_add
    // measured 6.9 ns on one thread and 104 ns per call on four, because
    // every increment invalidates the line in every other core. Loading a
    // line nobody writes any more costs a fraction of that and scales, and
    // by then the count has already said what it had to say.
    if (sUnboundEmits.load(std::memory_order_relaxed) < cUnboundEmitCap) {
        sUnboundEmits.fetch_add(1u, std::memory_order_relaxed);
    }
    return false;
}

} // namespace detail

/** R9.3: how many times a call site emitted nothing because no instance was
 *  reachable from it.
 *
 *  R9.1 makes a drop visible, and the emit path honours it exactly -- but
 *  only once a Logger has been found to count on. Everything that fails
 *  *before* that point was invisible: a forked child, a call site in a
 *  shared library built `-fvisibility=hidden` (which gets its own copy of
 *  the active-instance pointer, so nothing the host bound is reachable from
 *  it), and a process that simply logs before it binds all produce the same
 *  observation -- records gone, drops zero, undecodable zero, segment
 *  intact. Silence about not-logging is R9.1's failure one layer up, and
 *  this is the counter that ends it.
 *
 *  A count rather than a flag because the two situations it distinguishes
 *  need different responses: a handful is the startup window before
 *  binding, and a call site that will never reach a segment at all runs the
 *  number up at once. Telling those apart is the whole job, so the count is
 *  exact below cUnboundEmitCap and saturates there: a returned
 *  cUnboundEmitCap reads as "at least this many", and means the second one.
 *
 *  Module-scoped, deliberately and unavoidably: a plugin that cannot see
 *  the host's instance cannot see the host's counter either, so each module
 *  reports its own -- which is exactly the granularity that makes the
 *  duplicated-instance case legible from inside the module suffering it.
 *
 *  Cost: nothing at all when an instance is bound. The counter is reached
 *  only on the branch where there is no Logger, so R1.4's disabled-site
 *  cost is untouched -- 0.62 ns before this existed and after it. Once
 *  saturated, an unbound call site pays one relaxed load of a line nobody
 *  writes any more.
 */
[[nodiscard]] inline std::uint64_t unboundEmits() noexcept
{
    return detail::sUnboundEmits.load(std::memory_order_relaxed);
}

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

        /// Subsystem names to declare into the fresh segment before anything
        /// else can be written to it, so the segment carries its own
        /// vocabulary the way it already carries its own site definitions
        /// (docs/record-model.md, "strict self-description" extended to the
        /// subsystem axis). Optional: an empty span is the default, costs
        /// nothing here beyond an empty loop, and touches nothing on the
        /// emit path. The span only needs to outlive create() -- the names
        /// themselves are copied into the segment, not retained.
        std::span<const std::pair<SubsystemId, std::string_view>> subsystemNames_{};
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

    /// True when a `fork()` detached this process from an inherited Logger
    /// (POSIX). See `detachForFork()` for why that must happen.
    [[nodiscard]] static bool detachedByFork() noexcept
    {
        return sDetachedByFork_.load(std::memory_order_relaxed);
    }

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

    /** The threshold a call site in `subsystem` is checked against.
     *
     *  One relaxed load, the same as the process-wide threshold it
     *  replaced, because every slot always holds an answer: setThreshold()
     *  writes the whole table rather than leaving a sentinel for the read
     *  side to resolve. Resolving "unset" per call site would have cost a
     *  second load and a branch on R1.4's 0.62 ns path to serve a decision
     *  that is made once, from a control thread, by a human.
     *
     *  Subsystem ids at or above cSubsystemLevels share the process-wide
     *  threshold. That is a real limit and it is a fixed table rather than
     *  a hash for the same reason: the index has to be a constant offset
     *  for this to stay one load. Ids are consumer-assigned, so keeping the
     *  ones you want to steer independently below the ceiling costs
     *  nothing.
     */
    [[nodiscard]] Severity threshold(const SubsystemId subsystem) const noexcept
    {
        return subsystem.value_ < cSubsystemLevels
                   ? subsystemThreshold_[subsystem.value_].load(std::memory_order_relaxed)
                   : threshold_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] Severity threshold() const noexcept
    {
        return threshold_.load(std::memory_order_relaxed);
    }

    /// Sets the threshold for every subsystem, including those already
    /// steered individually. "Turn everything down to Warning" has to mean
    /// that, or the control is not a control.
    void setThreshold(const Severity severity) noexcept
    {
        threshold_.store(severity, std::memory_order_relaxed);
        for (std::atomic<Severity>& slot : subsystemThreshold_) {
            slot.store(severity, std::memory_order_relaxed);
        }
    }

    /// Steers one subsystem, leaving the rest where they are: debug for
    /// storage without inviting debug traffic from everything else, which
    /// at 72 ns a record is the difference between watching one component
    /// and drowning. A no-op for an id at or above cSubsystemLevels.
    void setThreshold(const SubsystemId subsystem, const Severity severity) noexcept
    {
        if (subsystem.value_ < cSubsystemLevels) {
            subsystemThreshold_[subsystem.value_].store(severity, std::memory_order_relaxed);
        }
    }

    /// Declares one subsystem's name after construction -- a plugin
    /// registering itself, typically -- and writes the record immediately.
    /// Control-thread work, exactly like create()'s declarations: nothing
    /// here is reachable from detail::emit, so it costs the emit path
    /// nothing. A drop is counted through the same reserveRecord() path as
    /// every other record (R9.1).
    void declareSubsystem(SubsystemId subsystem, std::string_view name) noexcept;

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

    /// The activity inherited through the environment (R5.4), stamped by
    /// the emit path whenever no CorrelationScope is active on the calling
    /// thread; 0 when the process was not spawned inside one. Per instance
    /// rather than a process-wide latch so tests reach it through the same
    /// scoped binding as everything else (R7).
    [[nodiscard]] std::uint64_t rootCorrelation() const noexcept
    {
        return rootCorrelation_;
    }

    void countDrop() noexcept;
    void countTruncation() noexcept;
    [[nodiscard]] Stats stats() const noexcept;

    /// The generation stamped on this instance's segment. The emit path
    /// compares a site's last-announced generation against this to decide
    /// whether this segment has been told what the site means.
    [[nodiscard]] std::uint64_t segmentGeneration() const noexcept
    {
        return segment_.generation();
    }

    [[nodiscard]] const std::string& segmentPath() const noexcept
    {
        return segment_.path();
    }

private:
    Logger() noexcept = default;

    /** Runs in the child after `fork()`, and unbinds whatever was bound.
     *
     *  A forked child inherits three things that together corrupt a
     *  segment in silence: the shared mapping (MAP_SHARED survives fork),
     *  the bound Logger, and this thread's writer cache -- pointing at the
     *  same chunk, at the same offset, as the parent. Both processes then
     *  write the same bytes to the same addresses and the last writer
     *  wins. Measured before this existed: 41 records emitted across a
     *  fork, 21 decodable afterwards, and every counter -- drops, damage,
     *  undecodable -- reporting zero. Losing records is bad; losing them
     *  while the mechanism reports perfect health is what R9.1 exists to
     *  forbid.
     *
     *  Unbinding is the honest response rather than a clever one. Claiming
     *  a fresh chunk in the parent's segment would preserve the records but
     *  attribute them to the parent's process id, and misleading
     *  diagnostics are worse than absent ones. R5.1 already says each
     *  process writes its own segment; a child that wants to log makes its
     *  own Logger, exactly as the two-process example does.
     */
    static void detachForFork() noexcept
    {
        sActive_.store(nullptr, std::memory_order_relaxed);
        sDetachedByFork_.store(true, std::memory_order_relaxed);
    }

    /// Registers detachForFork() with pthread_atfork, once per process.
    /// Called from create(), so a process that never logs never registers.
    static void registerForkHandlerOnce() noexcept
    {
#if !defined(_WIN32)
        static const bool once = [] {
            ::pthread_atfork(nullptr, nullptr, &Logger::detachForFork);
            return true;
        }();
        (void)once;
#endif
    }

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
    static inline std::atomic<bool> sDetachedByFork_{false};

    detail::Segment segment_{};
    std::uint64_t rootCorrelation_{0};
    std::atomic<Severity> threshold_{Severity::Trace};
    std::array<std::atomic<Severity>, cSubsystemLevels> subsystemThreshold_{};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> truncated_{0};
};

// ---------------------------------------------------------------------------
// Implementation

namespace detail {

/** The fit-retry-drop reservation, in one place.
 *
 *  Every record written anywhere in this library follows the same three
 *  steps: try this thread's current chunk, claim a fresh one if the record
 *  does not fit, and count a drop if even a new chunk cannot take it
 *  (R9.1). It was written out three times -- twice on the emit path, once
 *  in child capture -- and the copies had already drifted apart on who
 *  counts the drop, which is exactly how a counter starts lying.
 *
 *  On success `writer` names the chunk the reservation belongs to; the
 *  caller commits through it. On failure the drop is already counted and
 *  the returned reservation is invalid.
 */
[[nodiscard]] inline ChunkWriter::Reservation
reserveRecord(Logger& logger, const std::uint32_t payloadBytes, ChunkWriter*& writer) noexcept
{
    writer = logger.currentWriter();
    ChunkWriter::Reservation slot =
        (writer != nullptr) ? writer->reserve(payloadBytes) : ChunkWriter::Reservation{};
    if (slot.valid()) {
        return slot;
    }

    writer = logger.refillWriter();
    slot = (writer != nullptr) ? writer->reserve(payloadBytes) : ChunkWriter::Reservation{};
    if (!slot.valid()) {
        logger.countDrop();
    }
    return slot;
}

/** Writes one SubsystemDefinition record: the id, then the name capped and
 *  encoded exactly as SiteDefinition's format/file strings are (a u16
 *  length then bytes, wire::cInlineBytesCap ceiling, cFlagTruncated set and
 *  counted when the cap cuts real data -- R9.2, never a silent cut).
 *
 *  Goes through reserveRecord() rather than open-coding fit/retry/drop --
 *  that helper exists precisely because three copies of this sequence had
 *  already drifted apart on who counts the drop. This one runs from
 *  create() or declareSubsystem(), both control-thread callers, never from
 *  detail::emit -- so, unlike writeSiteDefinition, there is no need to
 *  shrink the record to fit a brand-new empty chunk: a name that does not
 *  fit is capped up front, and a record that still does not fit is simply
 *  dropped and counted like any other.
 *
 *  This does not bump wire::cFormatVersion. Verified against reader.hpp
 *  before relying on it: Decoder::decodeAll's first pass switches on
 *  RecordKind, and its `default` arm does `++skipped_` with the comment
 *  "intact, just not a shape this layer decodes" -- so an older decoder
 *  built before SubsystemDefinition existed reads a segment that has one,
 *  does not recognise kind 8, and counts it as skipped rather than
 *  undecodable. The record is unknown to that decoder, not damage.
 */
[[nodiscard]] inline bool writeSubsystemDefinition(Logger& logger, const SubsystemId subsystem,
                                                    std::string_view name) noexcept
{
    bool truncated = false;
    if (name.size() > wire::cInlineBytesCap) {
        name = name.substr(0, wire::cInlineBytesCap);
        truncated = true;
    }
    const auto nameLen = static_cast<std::uint16_t>(name.size());
    const auto payloadBytes =
        static_cast<std::uint32_t>(sizeof(wire::SubsystemDefinitionPayload) + nameLen);

    ChunkWriter* writer = nullptr;
    const ChunkWriter::Reservation slot = reserveRecord(logger, payloadBytes, writer);
    if (!slot.valid()) {
        return false; // reserveRecord() already counted the drop.
    }

    wire::SubsystemDefinitionPayload payload{};
    payload.subsystemId_ = subsystem.value_;
    payload.nameLen_ = nameLen;
    payload.reserved0_ = 0u;

    std::byte* p = slot.payload_;
    wire::storeUnaligned(p, payload);
    p += sizeof(payload);
    if (nameLen > 0u) {
        std::memcpy(p, name.data(), nameLen);
    }

    wire::RecordHead head{};
    head.payloadBytes_ = static_cast<std::uint16_t>(payloadBytes);
    head.kind_ = wire::RecordKind::SubsystemDefinition;
    head.flags_ = truncated ? static_cast<std::uint8_t>(wire::cFlagTruncated) : std::uint8_t{0};
    writer->commit(slot, head);

    if (truncated) {
        logger.countTruncation();
    }
    return true;
}

} // namespace detail

[[nodiscard]] inline Logger Logger::create(const Options& options) noexcept
{
    Logger result{};
    registerForkHandlerOnce();
    // A Logger created *after* a fork is this process's own, so the flag
    // stops describing the current state once one exists.
    sDetachedByFork_.store(false, std::memory_order_relaxed);
    result.segment_ = detail::Segment::create(options.directory_, options.stem_, options.segment_);
    result.rootCorrelation_ = detail::correlationFromEnvironment();
    result.setThreshold(options.threshold_);

    // Declared before anything else can reach this segment: the same
    // "definition precedes first use" discipline SiteDefinition already
    // follows, applied to the subsystem axis (docs/record-model.md).
    for (const auto& [id, name] : options.subsystemNames_) {
        (void)detail::writeSubsystemDefinition(result, id, name);
    }
    return result;
}

inline Logger::Logger(Logger&& other) noexcept
    : segment_{std::move(other.segment_)},
      rootCorrelation_{other.rootCorrelation_},
      threshold_{other.threshold_.load(std::memory_order_relaxed)},
      dropped_{other.dropped_.load(std::memory_order_relaxed)},
      truncated_{other.truncated_.load(std::memory_order_relaxed)}
{
    // std::atomic is neither copyable nor movable, so the table is carried
    // across element by element. Relaxed throughout: a Logger being moved
    // is not yet bound (the class comment above says why moving a bound one
    // is the caller's bug), so there is nobody to order against.
    for (std::size_t i = 0; i < cSubsystemLevels; ++i) {
        subsystemThreshold_[i].store(other.subsystemThreshold_[i].load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
    }
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

inline void Logger::declareSubsystem(const SubsystemId subsystem, const std::string_view name) noexcept
{
    (void)detail::writeSubsystemDefinition(*this, subsystem, name);
}

} // namespace sub0log
