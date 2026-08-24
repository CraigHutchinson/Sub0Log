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

    /// 0 until this process has written the SiteDefinition record. Checked
    /// with one relaxed load on the emit path; the definition write itself
    /// is idempotent per process, so a rare double-write under a race is
    /// benign (the decoder keeps the first).
    mutable std::atomic<std::uint8_t> announced_{0};

    [[nodiscard]] std::uint64_t id() const noexcept
    {
        return reinterpret_cast<std::uint64_t>(this);
    }
};

} // namespace sub0log
