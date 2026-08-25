#pragma once

/** @file support/mixed_records.hpp
 *  @brief Builds an in-memory segment image containing a mix of record
 *         shapes via the real producer path -- used as unmeasured setup by
 *         the "decode", "merge" and "format" KPI groups, which measure only
 *         the read side.
 *
 *  Called several times per run (decode's one fixture, merge's four,
 *  format's one) to build independent segments, all from the same call
 *  sites. That is now simply correct: a site announces itself into every
 *  segment it is used in, so each image carries its own definitions.
 *
 *  It was not always. This helper used to be a template on an `int Tag`
 *  purely to give each call a distinct set of call sites, because
 *  announcing was once per process and every image after the first came
 *  out undecodable. The library defect that forced that is fixed, and the
 *  workaround is gone with it.
 */

#include "temp_dir.hpp"

#include <sub0log/log.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sub0log::bench {

inline constexpr sub0log::SubsystemId cMixedRecordsSubsystem{9};

/// Emits `recordCount` records cycling through five call sites -- five
/// distinct SiteDefinitions written once each, then many Messages, the
/// "few sites, many messages" shape real logging has -- covering ints,
/// floats, bools and strings so decode/merge/format exercise every
/// argument path rather than just one.
inline void emitMixedRecords(std::uint64_t recordCount)
{
    static const std::string cName16(16, 'n');

    for (std::uint64_t i = 0; i < recordCount; ++i) {
        switch (i % 5) {
        case 0:
            sub0log_info(cMixedRecordsSubsystem, "int pair {} {}",
                        static_cast<std::uint64_t>(i),
                        static_cast<std::int32_t>(-static_cast<std::int64_t>(i)));
            break;
        case 1:
            sub0log_debug(cMixedRecordsSubsystem, "flag {} value {}",
                         (i % 2u) == 0u, static_cast<double>(i) * 1.5);
            break;
        case 2:
            sub0log_warning(cMixedRecordsSubsystem, "name {}", std::string_view{cName16});
            break;
        case 3:
            sub0log_error(cMixedRecordsSubsystem, "quad {} {} {} {}",
                          static_cast<std::uint64_t>(i), static_cast<std::int32_t>(i),
                          static_cast<double>(i) * 0.5, (i % 3u) == 0u);
            break;
        default:
            sub0log_trace(cMixedRecordsSubsystem, "byte {}", static_cast<std::uint8_t>(i & 0xFFu));
            break;
        }
    }
}

/// Builds one segment with `recordCount` mixed records under a fresh temp
/// directory, slurps it into memory once, and lets the producing Logger
/// (and its file) go away -- everything here runs outside any measured
/// lambda. Returns an empty vector on any setup failure; callers check for
/// that rather than throwing partway through a benchmark run.
[[nodiscard]] inline std::vector<std::byte>
buildMixedSegmentImage(const std::string& tagName, std::uint64_t recordCount)
{
    TempDir dir{tagName};

    // Generous per-record headroom (the largest shape here -- "quad" and
    // "name" -- lands around 60-70 bytes on the wire; 512 bytes/record
    // leaves ample margin) plus a floor so small fixtures still map a
    // sane minimum segment.
    const std::uint64_t segmentBytes =
        std::max<std::uint64_t>(16ull * 1024 * 1024, recordCount * 512ull + 4096ull);

    auto logger = sub0log::Logger::create(
        {.directory_ = dir.path().string(),
         .stem_ = "mixed",
         .segment_ = {.segmentBytes_ = segmentBytes}});
    if (!logger.valid()) {
        return {};
    }
    {
        sub0log::Logger::ScopedBind bind{logger};
        emitMixedRecords(recordCount);
    } // unbind before the file is read back

    const std::filesystem::path segmentPath = onlySegmentIn(dir.path());
    if (segmentPath.empty()) {
        return {};
    }
    return slurpFile(segmentPath);
}

} // namespace sub0log::bench
