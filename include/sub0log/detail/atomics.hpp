#pragma once

/** @file detail/atomics.hpp
 *  @brief The producer's atomic operations on segment words, in one place,
 *         for cores with and without lock-free 64-bit atomics.
 *
 *  R1.3 promises no lock on the producer path. Two words in a segment are
 *  shared across threads, and both are u64 in the wire format: a record's
 *  head word (commit-last, chunk.hpp) and the chunk-claim cursor
 *  (segment.hpp). Where the core has a 64-bit read-modify-write, both are
 *  single 64-bit atomics, exactly as they always were. Where it does not --
 *  ARMv7-M and ARMv8-M, i.e. every Cortex-M -- the standard library would
 *  quietly substitute a lock table, so they are done with 32-bit atomics
 *  instead, *without changing a byte of the format* (docs/embedded.md,
 *  "Cortex-M"):
 *
 *   - Commit. RecordHead::pack() keeps payloadBytes/kind/flags in the low
 *     32 bits and sequence/commit-tag in the high 32. On a little-endian
 *     core, storing the low half and then release-storing the high half
 *     leaves exactly the bytes a 64-bit store would; a reader that
 *     acquire-loads the high half (the tag) before the low half can never
 *     see a tag without its length (loadHeadWord below).
 *   - Claim. The cursor never needs to exceed the chunk count, already a
 *     u32. A bounded compare-exchange on its low half stops at the count
 *     rather than counting past it, so the high half stays zero -- the same
 *     bytes again -- and it cannot wrap the way a bare 32-bit fetch_add
 *     would after 2^32 refused claims.
 *
 *  Counters (drops, truncations, unbound emits) are a relaxed increment of
 *  the widest lock-free word: u64 where that is lock-free, u32 otherwise,
 *  where they wrap at 2^32 -- stated, since a counter is R9.1's whole
 *  promise.
 *
 *  SUB0LOG_SPLIT_ATOMICS=1 selects the split path on a core that does not
 *  need it. It is a configuration, not a test hook: it lets a host build
 *  run, benchmark and sanitise exactly the code a Cortex-M runs. CI builds
 *  and runs the whole suite that way (linux-gcc-split-atomics, with
 *  ASan/UBSan), and each operation is a template on the path so a unit
 *  test can hold both side by side and compare their bytes
 *  (tests/unit/atomics.test.cpp).
 */

#include "../wire.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sub0log::detail {

/** std::atomic_ref where the standard library has it, and the same
 *  operations over the GCC/Clang `__atomic` builtins where it does not.
 *
 *  libc++ gained atomic_ref only in LLVM 19, so every libc++ 18 toolchain
 *  -- the Android NDK through r27 among them -- has no std::atomic_ref at
 *  all, and this library could not build there. The builtins are what
 *  atomic_ref is implemented with on those compilers, so the fallback is
 *  the same instructions, not an approximation. Only the members the
 *  library uses are provided.
 */
#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
template <typename T>
using AtomicRef = std::atomic_ref<T>;
#elif defined(__GNUC__) || defined(__clang__)
[[nodiscard]] constexpr int builtinOrder(const std::memory_order order) noexcept
{
    switch (order) {
    case std::memory_order_relaxed: return __ATOMIC_RELAXED;
    case std::memory_order_consume: return __ATOMIC_CONSUME;
    case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
    case std::memory_order_release: return __ATOMIC_RELEASE;
    case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
    default: return __ATOMIC_SEQ_CST;
    }
}

template <typename T>
class AtomicRef {
public:
    static constexpr bool is_always_lock_free = __atomic_always_lock_free(sizeof(T), 0);
    static constexpr std::size_t required_alignment = alignof(T) > sizeof(T) ? alignof(T) : sizeof(T);

    explicit AtomicRef(T& object) noexcept : object_{&object} {}

    [[nodiscard]] T load(const std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
        return __atomic_load_n(object_, builtinOrder(order));
    }
    void store(const T value, const std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
        __atomic_store_n(object_, value, builtinOrder(order));
    }
    T fetch_add(const T value, const std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
        return __atomic_fetch_add(object_, value, builtinOrder(order));
    }
    bool compare_exchange_weak(T& expected, const T desired, const std::memory_order success,
                               const std::memory_order failure) const noexcept
    {
        return __atomic_compare_exchange_n(object_, &expected, desired, true,
                                           builtinOrder(success), builtinOrder(failure));
    }

private:
    T* object_;
};
#else
#  error "Sub0Log needs std::atomic_ref or the GCC/Clang __atomic builtins"
#endif

#if defined(SUB0LOG_SPLIT_ATOMICS) && SUB0LOG_SPLIT_ATOMICS
inline constexpr bool cSplitAtomics = true;
#else
inline constexpr bool cSplitAtomics = !AtomicRef<std::uint64_t>::is_always_lock_free;
#endif

static_assert(AtomicRef<std::uint32_t>::is_always_lock_free,
              "Sub0Log needs lock-free 32-bit atomics (R1.3: no lock on the producer "
              "path). A core without them -- ARMv6-M, Cortex-M0/M0+ -- has no "
              "lock-free read-modify-write at all; docs/embedded.md has the options.");
static_assert(!cSplitAtomics || std::endian::native == std::endian::little,
              "the split 32-bit protocol relies on the commit tag being in the "
              "high half of the head word's *storage*, which is little-endian "
              "only; a big-endian core needs the halves swapped (docs/embedded.md)");

/** What a call site records to remember it has been defined in the bound
 *  segment (SiteDescriptor::announcedKey_), loaded on every emit.
 *
 *  u64 -- the segment's random generation, exactly as it always was --
 *  wherever that load is lock-free. u32 on the split path, where it is a
 *  per-process counter (Segment::announceKey()): exact within one image,
 *  which on a microcontroller is the whole program. The generation stays
 *  the key on hosts because a header-only library can be instantiated once
 *  per shared object (hidden visibility, docs/adoption-friction.md 2.3),
 *  giving each its own counter; a Logger made in one and bound in another
 *  could then present a key a site had already seen, where 64 random bits
 *  make that practically impossible.
 */
using AnnounceWord = std::conditional_t<cSplitAtomics, std::uint32_t, std::uint64_t>;

/// Begins a u32's lifetime at `p`, for the split path's atomic_ref -- the
/// same reasoning, and the same fallback, as wire::startUint64LifetimeAt.
[[nodiscard]] inline std::uint32_t* startUint32LifetimeAt(std::byte* const p) noexcept
{
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
    return std::start_lifetime_as<std::uint32_t>(p);
#else
    return reinterpret_cast<std::uint32_t*>(p);
#endif
}

/// Publishes a packed head word: the record exists once this returns.
/// `word` must already carry the commit tag; the payload must be written.
template <bool Split = cSplitAtomics>
inline void storeHeadWord(std::byte* const slot, const std::uint64_t word) noexcept
{
    if constexpr (Split) {
        // Length/kind/flags first, relaxed -- nobody acts on them until the
        // tag is visible -- then the half holding the tag, release, which
        // orders both the payload and the low half before it.
        AtomicRef<std::uint32_t>{*startUint32LifetimeAt(slot)}.store(
            static_cast<std::uint32_t>(word), std::memory_order_relaxed);
        AtomicRef<std::uint32_t>{*startUint32LifetimeAt(slot + 4)}.store(
            static_cast<std::uint32_t>(word >> 32u), std::memory_order_release);
    } else {
        AtomicRef<std::uint64_t>{*wire::startUint64LifetimeAt(slot)}.store(
            word, std::memory_order_release);
    }
}

/** The reader's side of storeHeadWord: a head word read so that a tag seen
 *  implies the rest of the record is seen (the acquire pairs with the
 *  commit's release).
 *
 *  Plain loads plus a fence, as the reader always used: it reads images
 *  that may be const, unaligned, file-backed copies, or a live segment, and
 *  an image is not an atomic object. The halves are read tag first on
 *  *every* path, not only the split one: a plain 8-byte read is not
 *  single-copy atomic on a 32-bit core even when the writer's 64-bit store
 *  is (armv7-a's STREXD, Android armeabi-v7a), so reading the word whole
 *  can pair a new tag with a stale length -- a record that looks committed
 *  and is not. Found by running the publish test under qemu-arm. Reading
 *  the tag half first is correct for both writers: once the tag is visible
 *  the store that carried it has happened, so the length read after the
 *  acquire is that store's. `Split` is kept for symmetry with the other
 *  operations; both instantiations are the same code.
 */
template <bool Split = cSplitAtomics>
[[nodiscard]] inline std::uint64_t loadHeadWord(const std::byte* const slot) noexcept
{
    const std::uint32_t high = wire::loadUnaligned<std::uint32_t>(slot + 4);
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t low = wire::loadUnaligned<std::uint32_t>(slot);
    return (static_cast<std::uint64_t>(high) << 32u) | low;
}

/// Claims the next chunk index from the cursor at `cursor`, or returns
/// `chunkCount` when every chunk is already claimed (exhausted).
template <bool Split = cSplitAtomics>
[[nodiscard]] inline std::uint32_t claimChunkIndex(std::byte* const cursor,
                                                   const std::uint32_t chunkCount) noexcept
{
    if constexpr (Split) {
        // The low half, bounded: stop at chunkCount instead of counting
        // past it, so the high half is never touched and stays zero.
        AtomicRef<std::uint32_t> low{*startUint32LifetimeAt(cursor)};
        std::uint32_t index = low.load(std::memory_order_relaxed);
        while (index < chunkCount) {
            if (low.compare_exchange_weak(index, index + 1u, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
                return index;
            }
        }
        return chunkCount;
    } else {
        // Wait-free where the core has it: one fetch_add, which keeps
        // counting refused claims past the end (claimedChunks() clamps).
        AtomicRef<std::uint64_t> word{*wire::startUint64LifetimeAt(cursor)};
        const std::uint64_t index = word.fetch_add(1u, std::memory_order_relaxed);
        return index < chunkCount ? static_cast<std::uint32_t>(index) : chunkCount;
    }
}

/// Chunks claimed so far, clamped to `chunkCount`. One relaxed load.
template <bool Split = cSplitAtomics>
[[nodiscard]] inline std::uint32_t loadClaimedChunks(std::byte* const cursor,
                                                     const std::uint32_t chunkCount) noexcept
{
    std::uint64_t claimed = 0;
    if constexpr (Split) {
        claimed = AtomicRef<std::uint32_t>{*startUint32LifetimeAt(cursor)}.load(
            std::memory_order_relaxed);
    } else {
        claimed = AtomicRef<std::uint64_t>{*wire::startUint64LifetimeAt(cursor)}.load(
            std::memory_order_relaxed);
    }
    return claimed < chunkCount ? static_cast<std::uint32_t>(claimed) : chunkCount;
}

/** A relaxed event counter in the widest lock-free word. u64 where 64-bit
 *  atomics are lock-free; u32 on the split path, where it wraps at 2^32
 *  (docs/embedded.md). Reads widen to u64 either way, so Stats' types do
 *  not change per target.
 */
class RelaxedCounter {
public:
    using Word = std::conditional_t<cSplitAtomics, std::uint32_t, std::uint64_t>;

    constexpr RelaxedCounter() noexcept = default;
    constexpr explicit RelaxedCounter(const Word initial) noexcept : value_{initial} {}

    void increment() noexcept { value_.fetch_add(1u, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t load() const noexcept
    {
        return value_.load(std::memory_order_relaxed);
    }
    void store(const std::uint64_t value) noexcept
    {
        value_.store(static_cast<Word>(value), std::memory_order_relaxed);
    }

private:
    std::atomic<Word> value_{0};
};

} // namespace sub0log::detail
