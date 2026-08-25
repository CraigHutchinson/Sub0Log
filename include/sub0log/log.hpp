#pragma once

/** @file log.hpp
 *  @brief Call-site entry points. See REQUIREMENTS.md for the contract and
 *         STYLE_GUIDE.md for why these are macros and why they are lowercase.
 *
 *  Usage:
 *  @code
 *      constexpr sub0log::SubsystemId Storage{3};
 *      sub0log_debug(Storage, "read {} at {} for {} bytes", blobId, offset, length);
 *  @endcode
 *
 *  Nothing here formats, allocates, or locks; a disabled site costs a relaxed
 *  load and a comparison and does not evaluate its arguments (R1).
 */

#include "context.hpp"
#include "detail/emit.hpp"
#include "instance.hpp"
#include "severity.hpp"
#include "site.hpp"

/** Internal expansion helper (UPPER_SNAKE per the style guide: this name
 *  never appears in consumer code).
 *
 *  The descriptor is `static constinit`: a distinct object with static
 *  storage duration per call site, constant-initialised so no dynamic
 *  initialisation or latched-global behaviour exists (docs/record-model.md).
 *  Arguments are only evaluated on the enabled path.
 */
#define SUB0LOG_EMIT(severityValue, subsystemValue, formatText, ...)              \
    do {                                                                          \
        if (::sub0log::detail::enabled(                                           \
                (severityValue), ::sub0log::toSubsystemId(subsystemValue))) {          \
            static constinit ::sub0log::SiteDescriptor sub0logSite_{              \
                (formatText), __FILE__, __LINE__,                                 \
                ::sub0log::toSubsystemId(subsystemValue),                         \
                (severityValue)};                                                 \
            ::sub0log::detail::emit(sub0logSite_ __VA_OPT__(, ) __VA_ARGS__);     \
        }                                                                         \
    } while (false)

#define sub0log_trace(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Trace, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
#define sub0log_debug(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Debug, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
#define sub0log_info(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Info, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
#define sub0log_warning(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Warning, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
#define sub0log_error(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Error, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
/// The tier above Error for what could not be classified (R9.2).
#define sub0log_unclassified(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Unclassified, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
#define sub0log_fatal(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Fatal, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
