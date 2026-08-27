// SiteAnnounceTable's own contract (abi_host.hpp), tested in isolation from
// Logger/emit/wire entirely: every site it successfully records is found
// again with the same generation, a site never recorded reads as
// unannounced rather than colliding with something else, and running past
// capacity is counted rather than lost silently -- the "add a dedicated
// counter" half of the exhaustion decision, made real rather than argued.

#include <sub0log/abi_host.hpp>

#include "support/test_framework.hpp"

#include <cstdint>

TEST_CASE("SiteAnnounceTable finds every site it successfully recorded")
{
    sub0log::abi::detail::SiteAnnounceTable table;

    for (std::uint64_t i = 1; i <= 100; ++i) {
        table.record(i, i * 7u);
    }
    for (std::uint64_t i = 1; i <= 100; ++i) {
        CAPTURE(i);
        CHECK(table.generationFor(i) == i * 7u);
    }

    // Re-recording under a new generation is what a fresh Logger's
    // announcement looks like (R7.1): same site id, different generation,
    // and the table forgets nothing about who it is.
    table.record(1u, 999u);
    CHECK(table.generationFor(1u) == 999u);
}

TEST_CASE("a site never recorded reads as generation 0, not a stale collision")
{
    sub0log::abi::detail::SiteAnnounceTable table;
    table.record(42u, 5u);
    CHECK(table.generationFor(43u) == 0u); // distinct id, never recorded
}

TEST_CASE("more distinct sites than the table holds is counted, not lost silently")
{
    const std::uint64_t before = sub0log::abi::siteTableExhausted();

    sub0log::abi::detail::SiteAnnounceTable table;
    constexpr std::uint64_t cSlots = sub0log::abi::detail::SiteAnnounceTable::cSlots;
    constexpr std::uint64_t cAttempts = cSlots + 500u;

    for (std::uint64_t i = 1; i <= cAttempts; ++i) {
        // Multiplying by an odd 64-bit constant is a bijection mod 2^64, so
        // distinct i give distinct ids -- deterministic uniqueness without
        // needing the table's own probeStart() to be visible from here.
        table.record(i * 0x9E3779B97F4A7C15ull, i);
    }

    // Pigeonhole: at most cSlots of the cAttempts distinct ids can ever be
    // resident in a cSlots-slot table, so at least (cAttempts - cSlots) of
    // these record() calls must have exhausted their probe run --
    // deterministic, not a hash-luck probabilistic hope.
    constexpr std::uint64_t cMinExhausted = cAttempts - cSlots;
    CHECK(sub0log::abi::siteTableExhausted() - before >= cMinExhausted);
}
