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
 *  The table is mutex-guarded rather than lock-free. R1.3's no-lock promise
 *  is about a call site's own uncoordinated claim on *its* chunk; this
 *  table is genuinely shared, cross-plugin, cross-thread state that no C++
 *  call site ever needed, and every call through this table already pays
 *  for an indirect call across a module boundary the compiler cannot
 *  inline through. That cost dwarfs a mutex uncontended, which is the
 *  common case (a plugin announces a site rarely; the table is read once
 *  per emit). Nothing has measured this in a plugin's own hot loop, so if
 *  it turns out to matter, a fixed-size lock-free open-addressed table
 *  (same shape as `cSubsystemLevels`'s bound in instance.hpp) is the next
 *  step, not a rewrite -- but it was not worth building on a guess.
 *
 *  **Everything below is noexcept and never lets an exception cross the
 *  boundary** (R4.2): the one place that could throw --
 *  `std::unordered_map::operator[]`'s allocation -- is caught and treated
 *  as the write it followed already being uncounted-for, which the next
 *  emit() answers safely (drop, counted) rather than by propagating.
 */

#include "detail/emit.hpp"
#include "instance.hpp"
#include "severity.hpp"
#include "sub0log_abi.h"
#include "wire.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>

namespace sub0log::abi {

namespace detail {

/// Tracks, per plugin site id, the segment generation the host last
/// confirmed writing that site's SiteDefinition into. See the file comment
/// above for why this exists and why it is locked rather than lock-free.
class SiteAnnounceTable {
public:
    /// 0 (never a valid segment generation in practice -- randomGeneration()
    /// draws from the full 64-bit space, the same assumption SiteDescriptor's
    /// own default of 0 already relies on in site.hpp) when siteId has never
    /// been recorded.
    [[nodiscard]] std::uint64_t generationFor(const std::uint64_t siteId) const noexcept
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        const auto it = generations_.find(siteId);
        return it != generations_.end() ? it->second : 0u;
    }

    /// Records that this segment (identified by its generation) now carries
    /// siteId's SiteDefinition. Swallows an allocation failure as a no-op:
    /// the record itself has already been written by the caller
    /// (defineSite), so the only consequence of losing the bookkeeping is
    /// that the next emit() for this site is refused and counted as a drop
    /// rather than trusted -- the safe direction to fail in, and the same
    /// direction every other failure on this path already fails in.
    void record(const std::uint64_t siteId, const std::uint64_t generation) noexcept
    {
        try {
            const std::lock_guard<std::mutex> lock{mutex_};
            generations_[siteId] = generation;
        } catch (...) {
            // See the doc comment above: this function must not let an
            // exception cross back into a noexcept ABI entry point.
        }
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::uint64_t> generations_;
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
