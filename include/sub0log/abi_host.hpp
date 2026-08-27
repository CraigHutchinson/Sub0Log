#pragma once

/** @file abi_host.hpp
 *  @brief Host-side implementation of the plugin C ABI (sub0log_abi.h, R4).
 *
 *  This is the fix for docs/adoption-friction.md 2.3: a call site inside a
 *  module with its own copy of `Logger::sActive_` -- any shared library
 *  built `-fvisibility=hidden`, which is the recommended default and the
 *  only behaviour Windows offers -- cannot see a Logger the host bound, so
 *  every call site in it silently emits nothing. The header-only path
 *  cannot fix that; a C ABI crossing the module boundary can, because the
 *  functions below run *in the host's module*, where `Logger::active()` is
 *  the real one. `abi_test_plugin.cpp` (tests/system/plugin) proves it: the
 *  same probe as 2.3, this time through the table, with the plugin still
 *  built `-fvisibility=hidden`.
 *
 *  Include this from exactly the one translation unit that calls
 *  SUB0LOG_ABI_HOST_EXPORT() -- everything else here is safe to see from
 *  many TUs (it is all `inline`), but the export macro defines the
 *  `sub0log_abi_v1` symbol itself, and a second definition anywhere in the
 *  link is an ordinary duplicate-symbol error, by design (see the macro).
 *
 *  **Announce-once, across the boundary (R4.3).** A C++ call site's
 *  "already announced into this generation?" flag is free: SiteDescriptor
 *  (site.hpp) is a distinct static object per call site, so its
 *  `announcedGeneration_` atomic belongs to nobody else. A plugin's call
 *  site is only a `uint64_t` crossing the boundary -- there is no host-owned
 *  object per site to hang that atomic off, so the equivalent state has to
 *  live in a table the host owns instead, shared by every site any plugin
 *  announces (`detail::SiteAnnounceTable`, below).
 *
 *  The alternative considered and rejected: trust that a plugin which calls
 *  `define_site` once, at load time, will call it again whenever the host
 *  rebinds to a fresh Logger (R7.1 says exactly that happens -- a test
 *  binding its own instance, a service recreating its Logger on
 *  reconfiguration). The ABI has no way to tell a plugin "the segment
 *  changed, re-announce" -- `sub0log_abi.h` is frozen and carries no such
 *  callback -- so trusting the plugin reproduces the exact bug
 *  `roundtrip.test.cpp`'s "announced into segment too" test pins for the
 *  C++ path: a Message written into a segment that was never told what the
 *  site means. `emit()` below refuses instead: a Message whose site was not
 *  confirmed announced *into this segment's generation* is dropped and
 *  counted (R9.1), never written undecodably (R4.3's forbidding clause,
 *  taken literally).
 *
 *  The table is a fixed-size, lock-free, open-addressed array, not a
 *  mutex-guarded `std::unordered_map` -- it was the latter first, and
 *  measuring it in a plugin's own hot loop is what changed that. Two
 *  independent measurements, same harness shape (200k records/thread, a
 *  512 MiB segment so nothing drops), same C++ path logged alongside the
 *  ABI one for a same-machine baseline:
 *
 *      reviewer's machine, mutex+map:
 *        threads=1   abi=88.0 ns/record    cpp=66.7 ns/record
 *        threads=4   abi=892.4 ns/record   cpp=384.2 ns/record
 *
 *      this scratchpad, mutex+map (before) vs. the lock-free table (after),
 *      median of five runs -- absolute numbers differ from the reviewer's
 *      (a shared, virtualised sandbox, not dedicated hardware; the C++
 *      column's own thread=4 number moving is the tell), so the shape is
 *      the claim, not the nanoseconds:
 *        threads=1   before: abi=88.5  cpp=79.6    after: abi=67.3  cpp=72.8
 *        threads=4   before: abi=194.7 cpp=23.5    after: abi=21.4  cpp=46.0
 *
 *  Both runs agree on the shape that matters: at one thread the ABI and the
 *  C++ path are within the boundary's own cost of each other (an indirect
 *  call the compiler cannot inline through), locked or not. At four threads
 *  the locked table falls far behind the C++ path it should be tracking;
 *  the lock-free table tracks it. That four-thread gap was the same
 *  contention cliff `sUnboundEmits` hit before it went lock-free
 *  (instance.hpp) -- every plugin emit serialising on one lock, in a
 *  process that may have several plugins each running their own threads.
 *
 *  The replacement (`SiteAnnounceTable` below) is `cSlots` fixed slots of
 *  two relaxed atomics apiece, indexed by a splitmix64 mix of the site id
 *  with bounded linear probing (`cProbeLimit`). Every load on the read
 *  path (`generationFor`, called from every `emit()`) is
 *  `memory_order_relaxed`, which is sound rather than merely fast: the only
 *  thing the caller does with the result is one equality compare against
 *  the current segment generation, and a stale-but-eventually-consistent
 *  read here has exactly one failure mode -- reading an older generation
 *  than the one `record()` most recently stored, which makes `emit()`
 *  treat an actually-announced site as unannounced. That triggers a
 *  refusal (dropped, counted, R9.1), never an undecodable Message: the
 *  danger this table exists to prevent is a Message referencing a
 *  definition the segment never got, and a stale relaxed read can only
 *  push the answer *more* cautious, never less. There is no ordering
 *  requirement between this table and the wire write it is remembering
 *  either -- `writeSiteDefinitionCore` has already committed the
 *  SiteDefinition record (with its own release store, chunk.hpp) before
 *  `record()` is ever called, so nothing downstream of the generation
 *  compare depends on this table's memory order at all.
 *
 *  Fixed capacity means a full table (or a probe run that exceeds
 *  `cProbeLimit`) is a real, if unlikely, outcome: a plugin (or several)
 *  declaring more distinct call sites than `cSlots` holds. That is not
 *  silent either -- see `siteTableExhausted()`.
 *
 *  **Why this table remembers only a generation, not the whole
 *  definition.** It would be more robust for `emit()` to re-announce
 *  automatically after a Logger rebind (R7.1) by caching `define_site`'s
 *  full arguments -- format, file, line, subsystem, severity, type codes --
 *  keyed by site id, rather than dropping (counted) until the plugin
 *  happens to call `define_site` again. That was considered and left out,
 *  deliberately:
 *
 *   - It is genuinely safe to do, which is worth stating precisely rather
 *     than assuming: `format`/`file`/`argTypeCodes` are pointers into the
 *     *plugin's* memory (its own string literals and static data), and the
 *     only thing that can ever trigger a re-announce is the plugin itself
 *     calling `emit()` again -- which requires it to still be loaded, which
 *     is exactly the condition under which those pointers are still valid.
 *     There is no dangling-after-unload hazard here, unlike caching a
 *     pointer for later use by someone *else*.
 *   - But it does not fit this table's shape without giving up the
 *     property that makes it lock-free. Two atomics per slot admits one
 *     relaxed compare on the read path with no ordering to reason about
 *     beyond "the generation might be stale, and stale only means
 *     over-cautious" (above). A cached format/file/type-codes tail is
 *     plain data, written by `define_site` and read by a concurrent
 *     `emit()` on another thread -- that is a data race unless every field
 *     is synchronised, which is either a second lock (defeating the point
 *     of this change) or a materially more intricate lock-free record than
 *     "two integers, CAS the first".
 *   - The contract this leaves behind is asymmetric with the C++ path (a
 *     `SiteDescriptor` re-announces itself silently on every rebind; a
 *     plugin site does not) but it is not a silent one: a dropped Message
 *     is counted (R9.1), and the fix is one call the plugin already knows
 *     how to make. A host that rebinds its Logger while plugins are loaded
 *     and wants their sites to survive it can call back into each plugin's
 *     own re-init entry point (exactly the shape `abi_test_plugin.cpp` /
 *     `abi.test.cpp` already use to hand a plugin the table in the first
 *     place) and have it call `define_site` again -- the table pointer
 *     itself does not change across a rebind (`gHostAbiTable` is
 *     `constexpr`), only the definitions need repeating. A future ABI
 *     version could add a rebind notification to make this automatic
 *     (`sub0log_abi.h` is `size`-versioned exactly so it can grow); v1
 *     does not have one, and drop-and-count is the honest contract for the
 *     table v1 actually has.
 *
 *  **Everything below is noexcept and never lets an exception cross the
 *  boundary** (R4.2). That is easier to see now than it was with the
 *  locked map: nothing in `SiteAnnounceTable` allocates or throws, so there
 *  is no swallowed-exception path left to reason about at all.
 */

#include "detail/emit.hpp"
#include "instance.hpp"
#include "severity.hpp"
#include "sub0log_abi.h"
#include "wire.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace sub0log::abi {

namespace detail {

/// Counts every time SiteAnnounceTable could not resolve or record a
/// plugin site id within its probe bound. Distinct from Logger::countDrop()
/// on purpose: a define_site collision has not lost a wire record (the
/// SiteDefinition was already written by writeSiteDefinitionCore before
/// record() is ever called) -- it has lost the host's *memory* that it did,
/// which only costs a record once some later emit() for the same site finds
/// itself "unannounced" and is dropped (and counted, separately, through
/// the ordinary per-segment path). This counter is what tells an operator
/// *why*: too many distinct plugin call sites for the table, not a full
/// segment. See siteTableExhausted() for the public reader.
inline std::atomic<std::uint64_t> sSiteTableExhausted{0};

/// Tracks, per plugin site id, the segment generation the host last
/// confirmed writing that site's SiteDefinition into. Fixed-size and
/// lock-free -- see the file comment above for the measurement that
/// replaced the mutex-guarded map this used to be, and for why every load
/// on the read path is safely relaxed.
class SiteAnnounceTable {
public:
    /// Power of two so `hash & (cSlots - 1)` is a mask, not a modulo. 4096
    /// slots costs 64 KiB (two 8-byte atomics per slot) -- generous for how
    /// many distinct call sites one plugin process declares, bounded rather
    /// than unbounded because this table lives on emit()'s path and must
    /// not allocate (R1.2) or grow.
    static constexpr std::size_t cSlots = 4096u;
    static_assert((cSlots & (cSlots - 1u)) == 0u, "cSlots must be a power of two");

    /// How many slots generationFor()/record() probe past a site's ideal
    /// slot before giving up and counting the table exhausted, rather than
    /// walking the whole array on every emit() once it is even lightly
    /// loaded. Bounds both functions at O(cProbeLimit) regardless of how
    /// full the table is -- the same trade cSubsystemLevels makes in
    /// instance.hpp (a fixed table over an unbounded one, because the read
    /// side must stay cheap).
    static constexpr std::size_t cProbeLimit = 32u;

    /// A site id of 0 is never produced by a real call site (site ids are
    /// addresses of static objects -- see sub0log_abi.h's own comment on
    /// Sub0LogAbiRecord::site_id -- and an address is never null), so 0 is
    /// free to mean "this slot has never been claimed", the same trade
    /// site.hpp's announcedGeneration_ already makes for generation 0.

    /// The generation last recorded for siteId, or 0 when siteId has never
    /// been recorded -- including when the probe bound was exhausted
    /// without finding it (counted; see detail::sSiteTableExhausted).
    ///
    /// Every load here is relaxed, and that is deliberate rather than
    /// convenient: see the file comment above for why a stale read has no
    /// path to an undecodable Message, only to an unnecessary re-announce.
    [[nodiscard]] std::uint64_t generationFor(const std::uint64_t siteId) const noexcept
    {
        std::size_t slot = probeStart(siteId);
        for (std::size_t i = 0; i < cProbeLimit; ++i) {
            const std::uint64_t key = slots_[slot].siteId_.load(std::memory_order_relaxed);
            if (key == siteId) {
                return slots_[slot].generation_.load(std::memory_order_relaxed);
            }
            if (key == 0u) {
                return 0u; // empty slot inside the probe bound: genuinely never announced.
            }
            slot = nextSlot(slot);
        }
        sSiteTableExhausted.fetch_add(1u, std::memory_order_relaxed);
        return 0u; // undetermined within the bound; "not announced" is the safe answer.
    }

    /// Claims (or finds) siteId's slot and records generation, relaxed
    /// throughout: the only synchronisation this table owes a reader is
    /// the generation compare in emit(), and that compare tolerates a
    /// stale read by construction (file comment above).
    ///
    /// A claim that cannot find a slot within the probe bound does not
    /// call Logger::countDrop() itself: the SiteDefinition this claim would
    /// remember has already been written to the wire by the caller
    /// (defineSite). What is lost is only the host's memory of that fact --
    /// which costs a record the next time emit() for this site finds
    /// itself "unannounced" and is dropped through the ordinary path.
    /// There is no way to un-claim a slot to make room, so a plugin whose
    /// distinct site count exceeds cSlots sees every one of its excess
    /// sites' Messages dropped for the life of the process -- a real,
    /// documented limit (siteTableExhausted()), not a silent one.
    void record(const std::uint64_t siteId, const std::uint64_t generation) noexcept
    {
        std::size_t slot = probeStart(siteId);
        for (std::size_t i = 0; i < cProbeLimit; ++i) {
            std::uint64_t expected = 0u;
            const bool claimed = slots_[slot].siteId_.compare_exchange_strong(
                expected, siteId, std::memory_order_relaxed, std::memory_order_relaxed);
            if (claimed || expected == siteId) {
                slots_[slot].generation_.store(generation, std::memory_order_relaxed);
                return;
            }
            slot = nextSlot(slot);
        }
        sSiteTableExhausted.fetch_add(1u, std::memory_order_relaxed);
    }

private:
    struct Slot {
        std::atomic<std::uint64_t> siteId_{0};
        std::atomic<std::uint64_t> generation_{0};
    };

    /// splitmix64's mixing step over siteId. Two call sites at nearby
    /// addresses -- the ordinary case for static objects declared next to
    /// each other in one plugin translation unit -- must not land in
    /// adjacent slots, or the second site a small plugin declares walks
    /// straight into cProbeLimit.
    [[nodiscard]] static std::size_t probeStart(const std::uint64_t siteId) noexcept
    {
        std::uint64_t x = siteId + 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30u)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27u)) * 0x94D049BB133111EBull;
        x ^= x >> 31u;
        return static_cast<std::size_t>(x) & (cSlots - 1u);
    }

    [[nodiscard]] static std::size_t nextSlot(const std::size_t slot) noexcept
    {
        return (slot + 1u) & (cSlots - 1u);
    }

    std::array<Slot, cSlots> slots_{};
};

/// One table, shared by every plugin this host loads -- module-scoped like
/// `sUnboundEmits` (instance.hpp), and for the same reason: this state
/// belongs to the host, not to any one plugin.
[[nodiscard]] inline SiteAnnounceTable& siteAnnounceTable() noexcept
{
    static SiteAnnounceTable table; // thread-safe magic static (C++11).
    return table;
}

} // namespace detail

/// How many times SiteAnnounceTable could not claim or find a slot within
/// its probe bound (detail::sSiteTableExhausted) -- too many distinct
/// plugin call sites for the fixed table, not a full segment. Module-scoped
/// like sub0log::unboundEmits(), for the same reason: the table is shared
/// by every plugin the host loads, not owned by any one Logger.
[[nodiscard]] inline std::uint64_t siteTableExhausted() noexcept
{
    return detail::sSiteTableExhausted.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The table's three functions. extern "C" (not just callable from C): the
// struct's function-pointer members were declared inside sub0log_abi.h's
// `extern "C" { ... }` block, which gives the *pointer types themselves* C
// language linkage ([dcl.link]) -- assigning the address of an ordinary
// C++-linkage function there is exactly the mismatch that language linkage
// exists to catch, even where every mainstream ABI would happen to accept
// it. `inline` alongside `extern "C"` is what keeps this a header: multiple
// translation units may include this file (unlike the one that may call
// SUB0LOG_ABI_HOST_EXPORT()), and ODR-inline is what makes that legal for a
// function whose link-name has no C++ mangling to keep two definitions apart.

/// define_site: writes the SiteDefinition record for a plugin's call site
/// into whatever segment is currently bound, and records this segment's
/// generation against siteId on success -- see the announce-once discussion
/// above for why that second step exists and what it protects.
extern "C" inline void sub0logAbiHostDefineSite(std::uint64_t siteId, std::uint32_t subsystemId,
                                                std::uint8_t severity, const char* formatText,
                                                const char* file, std::uint32_t line,
                                                const std::uint8_t* argTypeCodes,
                                                std::uint8_t argCount) noexcept
{
    Logger* const logger = Logger::active();
    if (logger == nullptr) {
        // Same observation R9.3 already names for a C++ call site: nothing
        // is reachable from here, and that fact must not be free to miss.
        (void)sub0log::detail::countUnboundEmit();
        return;
    }

    const std::string_view format{formatText != nullptr ? formatText : ""};
    const std::string_view fileView{file != nullptr ? file : ""};
    const std::span<const std::uint8_t> typeCodes{
        argTypeCodes, argTypeCodes != nullptr ? static_cast<std::size_t>(argCount) : std::size_t{0}};

    if (sub0log::detail::writeSiteDefinitionCore(*logger, siteId, toSubsystemId(subsystemId),
                                                 static_cast<Severity>(severity), format, fileView,
                                                 line, typeCodes)) {
        detail::siteAnnounceTable().record(siteId, logger->segmentGeneration());
    }
    // A failed write already counted its own drop inside
    // writeSiteDefinitionCore (via reserveRecord). Leaving the table
    // unrecorded here is what keeps the next emit() for this site refused
    // rather than undecodable -- the same invariant detail::emit keeps for
    // a C++ call site whose definition failed to write.
}

/// emit: writes one Message record from a plugin's pre-encoded payload.
extern "C" inline void sub0logAbiHostEmit(const Sub0LogAbiRecord* record) noexcept
{
    if (record == nullptr) {
        return; // malformed call; nothing to attribute a count to.
    }

    Logger* const logger = Logger::active();
    if (logger == nullptr) {
        (void)sub0log::detail::countUnboundEmit();
        return;
    }

    // The boundary's answer to R1.4: a plugin has no macro to skip
    // evaluating its arguments below threshold (sub0log_abi.h carries no
    // cheaper "would this even be kept" query to call first -- named in the
    // report as something worth adding), but the record itself must still
    // never be written below threshold, and the check must run on every
    // call because Logger::setThreshold can change between them.
    const Severity severity = static_cast<Severity>(record->severity);
    const SubsystemId subsystem = toSubsystemId(record->subsystem_id);
    if (!atLeast(severity, logger->threshold(subsystem))) {
        return;
    }

    // R4.3: refuse a Message this segment was never told how to decode
    // rather than trust the plugin re-announced recently enough. See
    // detail::SiteAnnounceTable's comment for why the host, not the site,
    // owns this state for a plugin call site.
    if (detail::siteAnnounceTable().generationFor(record->site_id) != logger->segmentGeneration()) {
        logger->countDrop();
        return;
    }

    // The payload is already encoded bytes, not arguments to encode, so the
    // only cap here is the wire's own: the head word's 16-bit payload
    // length (wire.hpp). Cut and flagged rather than dropped outright,
    // matching how a single oversized argument is handled on the C++ path
    // (encode.hpp's per-argument cInlineBytesCap) -- visible, never silent
    // (R9.2).
    constexpr std::uint32_t cMaxPayload =
        0xFFFFu - static_cast<std::uint32_t>(sizeof(wire::MessagePayload));
    const bool payloadTruncated = record->payload_bytes > cMaxPayload;
    const std::uint32_t payloadBytes = payloadTruncated ? cMaxPayload : record->payload_bytes;
    const std::uint32_t totalBytes =
        static_cast<std::uint32_t>(sizeof(wire::MessagePayload)) + payloadBytes;

    // Fully qualified rather than the bare `ChunkWriter` detail::emit uses:
    // this file's own `sub0log::abi::detail` namespace (just above) shadows
    // the library's `sub0log::detail` for unqualified lookup of `detail::`,
    // so every reference to the library's internals stays fully qualified
    // throughout this file to keep that shadow from being a trap.
    sub0log::detail::ChunkWriter* writer = nullptr;
    const sub0log::detail::ChunkWriter::Reservation slot =
        sub0log::detail::reserveRecord(*logger, totalBytes, writer);
    if (!slot.valid()) {
        return; // reserveRecord already counted the drop (R9.1).
    }

    // Same stamping detail::emit uses: thread scope first, falling back to
    // the instance root (R5.4) -- correlation crosses the plugin boundary
    // exactly as it crosses a thread boundary, because it is read from the
    // same thread_local.
    const std::uint64_t scoped = currentCorrelation();
    const std::uint64_t correlation = scoped != 0u ? scoped : logger->rootCorrelation();
    const wire::MessagePayload messagePayload{record->site_id, sub0log::detail::monotonicNowNs(),
                                              correlation};
    wire::storeUnaligned(slot.payload_, messagePayload);

    if (payloadBytes > 0u && record->payload != nullptr) {
        std::memcpy(slot.payload_ + sizeof(wire::MessagePayload), record->payload, payloadBytes);
    }

    wire::RecordHead head{};
    head.payloadBytes_ = static_cast<std::uint16_t>(totalBytes);
    head.kind_ = wire::RecordKind::Message;
    head.flags_ = payloadTruncated ? static_cast<std::uint8_t>(wire::cFlagTruncated) : std::uint8_t{0};
    writer->commit(slot, head);

    if (payloadTruncated) {
        logger->countTruncation();
    }
}

/// current_correlation: the thread's scope, or the instance root, 0 with
/// nothing bound (matching sub0log::currentCorrelation()'s own fallback,
/// but answerable with no Logger reachable -- unlike emit/define_site this
/// is a query, not an attempt to log, so R9.3 has nothing to count here).
extern "C" inline std::uint64_t sub0logAbiHostCurrentCorrelation() noexcept
{
    const std::uint64_t scoped = currentCorrelation();
    if (scoped != 0u) {
        return scoped;
    }
    Logger* const logger = Logger::active();
    return logger != nullptr ? logger->rootCorrelation() : 0u;
}

/// The table itself: one instance, its three function pointers constant
/// (address-of a function is a constant expression), so building it is not
/// something the getter needs to redo per call -- only the "is a Logger
/// bound" check below varies.
inline constexpr Sub0LogAbiV1 gHostAbiTable = {
    static_cast<std::uint32_t>(sizeof(Sub0LogAbiV1)),
    static_cast<std::uint32_t>(SUB0LOG_ABI_VERSION),
    &sub0logAbiHostDefineSite,
    &sub0logAbiHostEmit,
    &sub0logAbiHostCurrentCorrelation,
};

/// The getter's real implementation, called through SUB0LOG_ABI_HOST_EXPORT.
/// NULL exactly when sub0log_abi.h documents it should be: no Logger bound.
[[nodiscard]] inline const Sub0LogAbiV1* hostTable() noexcept
{
    return Logger::active() != nullptr ? &gHostAbiTable : nullptr;
}

} // namespace sub0log::abi

/** Defines and exports the plugin-facing getter (sub0log_abi.h,
 *  `SUB0LOG_ABI_GETTER_NAME`). Write this once, at namespace scope, in
 *  exactly one translation unit the host links -- like `main()`, this macro
 *  generates a definition, so a second invocation anywhere in the program
 *  is a duplicate-symbol error at link time. That is the enforcement
 *  mechanism for "exactly one": the linker, not the macro.
 *
 *  Visibility is requested explicitly (`__attribute__((visibility("default")))`
 *  on GCC/Clang, `__declspec(dllexport)` on MSVC) because a host built
 *  `-fvisibility=hidden` -- or any host on Windows, where nothing is
 *  exported unless asked -- is precisely the situation this ABI exists to
 *  answer (R4.1). On POSIX the executable itself must additionally be
 *  linked with its defined symbols pushed into the dynamic symbol table
 *  (`-rdynamic`) for `dlsym` to find this from a loaded plugin or from the
 *  host's own `dlopen(NULL, ...)`; CMake's `ENABLE_EXPORTS` target property
 *  is the portable spelling for that (and for the import library MSVC needs
 *  on the same path) -- see tests/CMakeLists.txt.
 */
#if defined(_WIN32)
#  define SUB0LOG_ABI_HOST_VISIBILITY __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define SUB0LOG_ABI_HOST_VISIBILITY __attribute__((visibility("default")))
#else
#  define SUB0LOG_ABI_HOST_VISIBILITY
#endif

#define SUB0LOG_ABI_HOST_EXPORT()                                                                \
    extern "C" SUB0LOG_ABI_HOST_VISIBILITY const Sub0LogAbiV1* sub0log_abi_v1() noexcept         \
    {                                                                                            \
        return ::sub0log::abi::hostTable();                                                      \
    }
