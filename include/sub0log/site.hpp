#pragma once

/** @file site.hpp
 *  @brief The constant half of a call site (docs/record-model.md).
 */

#include "severity.hpp"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace sub0log {

/** One per call site, with static storage duration, materialised by the
 *  call-site macro (see STYLE_GUIDE.md for why a macro is required to get a
 *  *distinct* object per site).
 *
 *  Constant-initialisable on purpose: no constructor runs, no registration
 *  happens, nothing allocates. The site's identity in the stream is this
 *  object's address; the definition record written on first use is what makes
 *  that address meaningful to a reader (R4.3).
 */
struct SiteDescriptor {
    std::string_view format_;
    std::string_view file_;
    std::uint32_t line_{};
    SubsystemId subsystem_{};
    Severity severity_{Severity::Info};

    /// The segment generation this site's SiteDefinition was last written
    /// into; 0 until it has been written anywhere.
    ///
    /// A plain "announced" flag would be wrong, and was: it made announcing
    /// a once-per-process event, so a call site reached under a second
    /// Logger emitted a Message into a segment that had never been told what
    /// the site means -- an undecodable record, silently. That is ordinary
    /// consumer shape (a shared helper that logs, a test binding its own
    /// instance per case, a Logger recreated on reconfiguration), and R7.1
    /// promises exactly that binding works.
    ///
    /// Keying on the generation costs the same as the flag did: one relaxed
    /// load and a comparison on the emit path (R1.4). Two threads racing
    /// into a new generation may both write the definition, which is benign
    /// -- the decoder keeps the first, as it always did.
    mutable std::atomic<std::uint64_t> announcedGeneration_{0};

    [[nodiscard]] std::uint64_t id() const noexcept
    {
        return reinterpret_cast<std::uint64_t>(this);
    }
};

} // namespace sub0log
