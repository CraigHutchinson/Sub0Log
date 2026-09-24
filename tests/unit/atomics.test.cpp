// detail/atomics.hpp: the 64-bit and split 32-bit protocols side by side.
// The split path is what every Cortex-M runs; its whole claim is that it
// writes the *same bytes* as the 64-bit path with only 32-bit atomics, so
// that is what is checked first, before either path's concurrency.
//
// Every operation is a template on the path, so this one binary tests both
// regardless of how it was configured. CI additionally builds and runs the
// entire suite with SUB0LOG_SPLIT_ATOMICS=1 (and under ThreadSanitizer),
// which is what exercises the split path through the real Logger.

#include <sub0log/detail/atomics.hpp>
#include <sub0log/wire.hpp>

#include "support/test_framework.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using sub0log::detail::claimChunkIndex;
using sub0log::detail::loadClaimedChunks;
using sub0log::detail::loadHeadWord;
using sub0log::detail::storeHeadWord;
namespace wire = sub0log::wire;

std::uint64_t packed(const std::uint16_t payloadBytes, const wire::RecordKind kind,
                     const std::uint8_t flags, const std::uint16_t sequence)
{
    wire::RecordHead head{};
    head.payloadBytes_ = payloadBytes;
    head.kind_ = kind;
    head.flags_ = flags;
    head.sequence_ = sequence;
    return head.pack();
}

} // namespace

TEST_CASE("a split commit writes exactly the bytes a 64-bit commit writes")
{
    const std::uint64_t words[] = {
        packed(0, wire::RecordKind::Message, 0, 0),
        packed(40, wire::RecordKind::Message, 0, 1),
        packed(0xFFFF, wire::RecordKind::Continuation, 0xFF, 0xFFFF),
        packed(8, wire::RecordKind::SiteDefinition, wire::cFlagTruncated, 0x1234),
    };
    for (const std::uint64_t word : words) {
        CAPTURE(word);
        alignas(8) std::array<std::byte, 8> whole{};
        alignas(8) std::array<std::byte, 8> split{};
        storeHeadWord<false>(whole.data(), word);
        storeHeadWord<true>(split.data(), word);
        CHECK(std::memcmp(whole.data(), split.data(), 8) == 0);

        // ...and either reader recovers it from either writer's bytes.
        CHECK(loadHeadWord<false>(split.data()) == word);
        CHECK(loadHeadWord<true>(whole.data()) == word);
        CHECK(wire::RecordHead::isCommitted(loadHeadWord<true>(split.data())));
    }
}

TEST_CASE("the commit tag lives entirely in the half the split path stores last")
{
    // The split protocol's correctness rests on this layout fact: if any
    // tag bit were in the low 32 bits, a reader could see a partial tag
    // before the length. Pinned here so a RecordHead change cannot break
    // Cortex-M silently.
    const std::uint64_t tagOnly = static_cast<std::uint64_t>(wire::cCommitTag) << 48u;
    CHECK((tagOnly & 0xFFFFFFFFull) == 0u);
    CHECK_FALSE(wire::RecordHead::isCommitted(packed(64, wire::RecordKind::Message, 0, 7)
                                              & 0xFFFFFFFFull));
}

TEST_CASE("a bounded split claim stops at the chunk count and leaves the high half zero")
{
    constexpr std::uint32_t cCount = 5;

    alignas(8) std::array<std::byte, 8> splitCursor{};
    for (std::uint32_t expected = 0; expected < cCount; ++expected) {
        CHECK(claimChunkIndex<true>(splitCursor.data(), cCount) == expected);
    }
    for (int refused = 0; refused < 1000; ++refused) {
        CHECK(claimChunkIndex<true>(splitCursor.data(), cCount) == cCount);
    }
    // Identical to a cursor that was simply stored as the u64 count: the
    // format's view of the word, not just the low half, is unchanged.
    CHECK(wire::loadUnaligned<std::uint64_t>(splitCursor.data()) == cCount);
    CHECK(loadClaimedChunks<true>(splitCursor.data(), cCount) == cCount);

    // The 64-bit path counts refused claims past the end; the clamped read
    // is what keeps usage() identical across the two.
    alignas(8) std::array<std::byte, 8> wholeCursor{};
    for (std::uint32_t i = 0; i < cCount + 1000u; ++i) {
        (void)claimChunkIndex<false>(wholeCursor.data(), cCount);
    }
    CHECK(wire::loadUnaligned<std::uint64_t>(wholeCursor.data()) == cCount + 1000u);
    CHECK(loadClaimedChunks<false>(wholeCursor.data(), cCount) == cCount);
}

template <bool Split>
void checkConcurrentClaims()
{
    // Every index handed out exactly once, none past the count, however the
    // threads interleave. The split path is a CAS loop, so this is the case
    // where a lost update would show up as a duplicate.
    constexpr std::uint32_t cCount = 20000;
    constexpr int cThreads = 8;
    alignas(8) std::array<std::byte, 8> cursor{};
    std::vector<std::atomic<int>> seen(cCount);
    std::atomic<std::uint32_t> refused{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < cThreads; ++t) {
        threads.emplace_back([&] {
            for (;;) {
                const std::uint32_t index = claimChunkIndex<Split>(cursor.data(), cCount);
                if (index == cCount) {
                    refused.fetch_add(1u, std::memory_order_relaxed);
                    return;
                }
                seen[index].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    int duplicates = 0;
    int missing = 0;
    for (const std::atomic<int>& count : seen) {
        const int value = count.load();
        duplicates += value > 1 ? 1 : 0;
        missing += value == 0 ? 1 : 0;
    }
    CHECK(duplicates == 0);
    CHECK(missing == 0);
    CHECK(refused.load() == static_cast<std::uint32_t>(cThreads));
    CHECK(loadClaimedChunks<Split>(cursor.data(), cCount) == cCount);
}

TEST_CASE("concurrent claims hand out every chunk exactly once (split path)")
{
    checkConcurrentClaims<true>();
}

TEST_CASE("concurrent claims hand out every chunk exactly once (64-bit path)")
{
    checkConcurrentClaims<false>();
}

template <bool Split>
void checkPublishedRecordsAreWhole()
{
    // Message passing through the commit, the protocol's whole job: a
    // writer fills a payload, then commits a head word whose length and
    // sequence describe it; a reader spinning on the slot must never see a
    // committed tag with a length or payload from before the commit. One
    // slot reused many times, zeroed between rounds under a handshake, so
    // each round is a fresh publish the reader races.
    struct alignas(8) Slot {
        std::array<std::byte, 8> head{};
        std::array<std::uint32_t, 4> payload{};
    };
    Slot slot{};
    constexpr std::uint32_t cRounds = 20000;
    std::atomic<std::uint32_t> round{0};
    std::atomic<std::uint32_t> consumed{0};
    std::atomic<int> torn{0};

    std::thread reader([&] {
        for (std::uint32_t r = 1; r <= cRounds; ++r) {
            while (round.load(std::memory_order_acquire) != r) {
            }
            std::uint64_t word = 0;
            do {
                word = loadHeadWord<Split>(slot.head.data());
            } while (!wire::RecordHead::isCommitted(word));
            const wire::RecordHead head = wire::RecordHead::unpack(word);
            const auto expectedLength = static_cast<std::uint16_t>(r % 997u + 1u);
            if (head.payloadBytes_ != expectedLength
                || head.sequence_ != static_cast<std::uint16_t>(r)) {
                torn.fetch_add(1);
            }
            for (std::uint32_t& value : slot.payload) {
                // Relaxed, so the only ordering is the head word's acquire.
                if (std::atomic_ref<std::uint32_t>{value}.load(std::memory_order_relaxed) != r) {
                    torn.fetch_add(1);
                }
            }
            consumed.store(r, std::memory_order_release);
        }
    });

    for (std::uint32_t r = 1; r <= cRounds; ++r) {
        // Reset between rounds, while the reader is parked on `round`.
        std::atomic_ref<std::uint64_t>{*wire::startUint64LifetimeAt(slot.head.data())}.store(
            0, std::memory_order_relaxed);
        round.store(r, std::memory_order_release);
        // The payload is written *after* the reader is released, so the
        // only thing ordering it before the reader's checks is the commit's
        // release and the reader's acquire -- the property under test.
        for (std::uint32_t& value : slot.payload) {
            std::atomic_ref<std::uint32_t>{value}.store(r, std::memory_order_relaxed);
        }
        storeHeadWord<Split>(slot.head.data(),
                             packed(static_cast<std::uint16_t>(r % 997u + 1u),
                                    wire::RecordKind::Message, 0, static_cast<std::uint16_t>(r)));
        while (consumed.load(std::memory_order_acquire) != r) {
        }
    }
    reader.join();
    CHECK(torn.load() == 0);
}

TEST_CASE("a reader never sees a committed tag without its record (split path)")
{
    checkPublishedRecordsAreWhole<true>();
}

TEST_CASE("a reader never sees a committed tag without its record (64-bit path)")
{
    checkPublishedRecordsAreWhole<false>();
}

TEST_CASE("RelaxedCounter reads as u64 on every path, and wraps only where documented")
{
    sub0log::detail::RelaxedCounter counter;
    counter.increment();
    counter.increment();
    CHECK(counter.load() == 2u);

    counter.store(0xFFFFFFFFull);
    counter.increment();
    if constexpr (sub0log::detail::cSplitAtomics) {
        CHECK(counter.load() == 0u); // u32 on this path: docs/embedded.md says so
    } else {
        CHECK(counter.load() == 0x100000000ull);
    }
}
