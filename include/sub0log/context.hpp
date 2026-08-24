#pragma once

/** @file context.hpp
 *  @brief Correlation: a field, not a heuristic (R6), propagated to children
 *         through the environment because that survives exec (R5.4).
 */

#include "detail/platform.hpp"

#include <cstdint>
#include <cstdlib>

namespace sub0log {

namespace detail {

/// splitmix64: a fast, well-mixed generator, used here only to turn a few
/// weakly-random inputs (pid, a clock reading, a per-thread counter) into an
/// id that is unique enough per host without any coordination -- not a
/// cryptographic primitive, and not trying to be one.
[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept
{
    x += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = x;
    z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31u);
}

/// A fresh correlation id: splitmix64 over (process id, monotonic time, a
/// thread-local counter) -- unique enough per host, no coordination needed.
[[nodiscard]] inline std::uint64_t newCorrelationId() noexcept
{
    thread_local std::uint64_t tCounter{0};
    ++tCounter;
    std::uint64_t seed = splitmix64(currentProcessId());
    seed = splitmix64(seed ^ monotonicNowNs());
    seed = splitmix64(seed ^ tCounter);
    if (seed == 0u) {
        seed = 1u; // 0 is reserved for "no correlation in scope".
    }
    return seed;
}

/// The id in scope on this thread; 0 means none. Every CorrelationScope
/// reads and restores this on entry/exit.
inline thread_local std::uint64_t tCurrentCorrelationId{0};

} // namespace detail

/// The correlation id in scope on this thread; 0 when none. The emit path
/// stamps this into every message record, falling back to the Logger's
/// root correlation (seeded from the environment, R5.4) when no scope is
/// active -- the fallback lives on the instance, not in a process-wide
/// latch, so a test can exercise it through the same scoped binding as
/// everything else (R7).
[[nodiscard]] inline std::uint64_t currentCorrelation() noexcept
{
    return detail::tCurrentCorrelationId;
}

/** RAII: sets this thread's correlation id, restores the previous one on
 *  exit. Nesting works; joining related records is an equality test on the
 *  field (R6.2).
 */
class [[nodiscard]] CorrelationScope {
public:
    /// Enters a fresh id.
    CorrelationScope() noexcept
        : id_{detail::newCorrelationId()}, previous_{detail::tCurrentCorrelationId}
    {
        detail::tCurrentCorrelationId = id_;
    }

    /// Enters an existing id (e.g. one recovered from a parent process).
    explicit CorrelationScope(std::uint64_t id) noexcept
        : id_{id}, previous_{detail::tCurrentCorrelationId}
    {
        detail::tCurrentCorrelationId = id_;
    }

    ~CorrelationScope() { detail::tCurrentCorrelationId = previous_; }

    CorrelationScope(const CorrelationScope&) = delete;
    CorrelationScope& operator=(const CorrelationScope&) = delete;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

private:
    std::uint64_t id_;
    std::uint64_t previous_;
};

namespace detail {

/// Name of the propagation variable (R5.4). A parent sets it in a child's
/// environment; Logger::create reads it as the instance's root correlation,
/// so a cooperating child needs no wiring code to join.
inline constexpr const char* cCorrelationEnvVar = "SUB0LOG_CORRELATION";

/// Reads cCorrelationEnvVar; 0 when absent or malformed. Not on the emit
/// hot path -- called once at Logger creation, so getenv's documented lack
/// of thread-safety with concurrent setenv is the caller's concern, same
/// as for any other use of the environment.
[[nodiscard]] inline std::uint64_t correlationFromEnvironment() noexcept
{
    // MSVC deprecates getenv in favour of _dupenv_s, whose difference is that
    // it hands back an allocation to free. What it protects against is
    // retaining the returned pointer across a setenv; this call parses the
    // value immediately and retains nothing, so the deprecation does not
    // apply and a library header must not emit a warning into every consumer
    // that includes it.
#if defined(_MSC_VER)
#  pragma warning(suppress : 4996)
#endif
    const char* const value = std::getenv(cCorrelationEnvVar);
    if (value == nullptr || *value == '\0') {
        return 0u;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value) {
        return 0u; // no digits parsed at all: garbage, not a number.
    }
    return static_cast<std::uint64_t>(parsed);
}

} // namespace detail

} // namespace sub0log
