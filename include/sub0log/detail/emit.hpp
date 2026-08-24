#pragma once

/** @file detail/emit.hpp
 *  @brief The producer emit path: definition-on-first-use, then the message
 *         record. Called only from the call-site macros in log.hpp.
 */

#include "../encode.hpp"
#include "../instance.hpp"
#include "../site.hpp"

namespace sub0log::detail {

/// The macro's enabled check: one relaxed load of the active instance and
/// one of its threshold (R1.4). False when nothing is bound.
[[nodiscard]] inline bool enabled(const Severity severity) noexcept
{
    Logger* const logger = Logger::active();
    return logger != nullptr && atLeast(severity, logger->threshold());
}

/** Writes, into the active logger's current chunk:
 *   - the site's SiteDefinition record, iff site.announced_ is 0 (relaxed
 *     load; set to 1 after the definition commits) -- the definition always
 *     precedes the site's first message in the stream;
 *   - the Message record: MessagePayload{site.id(), monotonicNowNs(),
 *     currentCorrelation()} then encodeArgs(args...).
 *
 *  Fit handling: a record that does not fit the current chunk's remainder
 *  claims a fresh chunk; if none, the drop is counted and the call returns
 *  (R9.1). Truncation by encodeArgs sets cFlagTruncated and is counted.
 *
 *  No allocation, no lock, no formatting, noexcept end to end (R1.1-R1.3).
 *
 *  TODO(impl:producer): implement per the above.
 */
template <Encodable... Args>
void emit(const SiteDescriptor& site, const Args&... args) noexcept;

} // namespace sub0log::detail
