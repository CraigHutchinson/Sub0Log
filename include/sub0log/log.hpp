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
 *
 *  **From a signal handler:** a thread that has already emitted at least one
 *  record may emit from a handler. The store sequence itself is
 *  async-signal-safe by construction -- no allocation, no lock, no errno
 *  traffic, one release store to commit -- which is what
 *  `examples/04_crash_handler.cpp` relies on when it logs a Fatal record
 *  from inside a handler and then lets the signal kill the process. The
 *  qualification is the way in, not the path: the first emit on a thread
 *  reaches a `thread_local` writer cache, and `Logger::create` runs a
 *  function-local static for its fork handler. In an executable the cache
 *  is constant-initialised straight into `.tbss` -- no guard variable and
 *  no runtime call appear in the object file at all -- but built `-fPIC`
 *  into a shared library the same access goes through `__tls_get_addr`,
 *  which may allocate the first time a thread touches the block. That is
 *  the whole of the difference, and it is why the contract is "already
 *  emitted once" rather than "always": warm the thread and bind the Logger
 *  before installing the handler, which every real program does anyway.
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
/// The top of the ladder -- a record, nothing more: this does not abort,
/// terminate, or throw, unlike LOG(FATAL) in some other logging libraries.
/// Pair it with your own call to std::abort()/std::terminate() (as
/// examples/04_crash_handler.cpp does) if the process should not continue;
/// sub0log_fatal by itself only marks the record's severity.
#define sub0log_fatal(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Fatal, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
