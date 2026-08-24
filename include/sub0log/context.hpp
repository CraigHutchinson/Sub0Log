#pragma once

/** @file context.hpp
 *  @brief Correlation: a field, not a heuristic (R6), propagated to children
 *         through the environment because that survives exec (R5.4).
 */

#include <cstdint>

namespace sub0log {

/// The correlation id in scope on this thread; 0 when none. Stamped into
/// every message record by the emit path.
[[nodiscard]] std::uint64_t currentCorrelation() noexcept;

/** RAII: sets this thread's correlation id, restores the previous one on
 *  exit. Nesting works; joining related records is an equality test on the
 *  field (R6.2).
 *
 *  TODO(impl:producer): thread_local id; newId() from a splitmix64 over
 *  (process id, monotonic time, counter) -- unique enough per host, no
 *  coordination.
 */
class [[nodiscard]] CorrelationScope {
public:
    /// Enters a fresh id.
    CorrelationScope() noexcept;
    /// Enters an existing id (e.g. one recovered from a parent process).
    explicit CorrelationScope(std::uint64_t id) noexcept;
    ~CorrelationScope();
    CorrelationScope(const CorrelationScope&) = delete;
    CorrelationScope& operator=(const CorrelationScope&) = delete;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

private:
    std::uint64_t id_;
    std::uint64_t previous_;
};

namespace detail {

/// Name of the propagation variable (R5.4). A parent sets it in a child's
/// environment; a child's Logger seeds its root correlation from it.
inline constexpr const char* cCorrelationEnvVar = "SUB0LOG_CORRELATION";

/// Reads cCorrelationEnvVar; 0 when absent or malformed.
/// TODO(impl:producer).
[[nodiscard]] std::uint64_t correlationFromEnvironment() noexcept;

} // namespace detail

} // namespace sub0log
