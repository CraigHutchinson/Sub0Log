#pragma once

/** @file support/live_reader.hpp
 *  @brief One read pass over a live, still-growing segment file --
 *         map read-only, decode whatever is currently committed, report
 *         what happened. Shared by every scenario that hammers a segment
 *         with a concurrent reader thread (`live_tail`, `soak`); both used
 *         to carry their own copy of this exact body.
 *
 *  This goes through `detail::FileMapping` rather than the copy-then-
 *  `SegmentReader::open` pattern the rest of the codebase uses for a live
 *  tail (`examples/07_live_tail.cpp`, `tools/sub0log_cat.cpp --follow`):
 *  those poll at human timescales, where a full-file copy per pass is
 *  free, while these two scenarios run as many passes per second as the
 *  CPU allows under sustained producer load, where a copy every pass would
 *  tax the harness more than the library it exists to stress.
 */

#include <sub0log/detail/platform.hpp>
#include <sub0log/reader.hpp>

#include <cstdint>
#include <string>

namespace sub0log::stress {

/// How one attempted pass over the live file went.
enum class LivePassOutcome {
    MappingUnavailable, ///< the file could not be mapped this pass (a transient open race)
    ReaderInvalid,       ///< the mapping opened but SegmentReader::open() rejected it
    Completed,            ///< mapped, opened, and decoded
};

struct LivePassResult {
    LivePassOutcome outcome_{LivePassOutcome::MappingUnavailable};
    std::uint64_t decodedCount_{0};
    bool undecodableFound_{false};
};

/// Maps `segmentPath` read-only, opens a SegmentReader over it, and decodes
/// everything currently committed. Fresh FileMapping/SegmentReader/Decoder
/// every call: there is nothing to reuse across passes, since each pass
/// reads whatever the producer(s) have written since the last one.
[[nodiscard]] inline LivePassResult readOneLivePass(const std::string& segmentPath)
{
    detail::FileMapping mapping = detail::FileMapping::openReadOnly(segmentPath);
    if (!mapping.valid()) {
        return {LivePassOutcome::MappingUnavailable, 0, false};
    }
    SegmentReader reader = SegmentReader::open(mapping.bytes());
    if (!reader.valid()) {
        return {LivePassOutcome::ReaderInvalid, 0, false};
    }
    Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    return {LivePassOutcome::Completed, records.size(), decoder.undecodableRecords() != 0u};
}

} // namespace sub0log::stress
