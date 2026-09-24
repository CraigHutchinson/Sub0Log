// The embedded producer path (issue #2), held to the constraints a firmware
// or NDK consumer actually has, by measurement rather than by reading:
//
//   - compiled -fno-exceptions -fno-rtti (CMakeLists.txt; GCC/Clang), and
//     including only the producer's own header;
//   - no heap allocation from Logger::createInMemory through every record
//     written: operator new is replaced below and counted across that
//     window, and a non-zero count fails the test;
//   - no file and no mapping: the storage is a static array, and the
//     segment path is empty;
//   - a full buffer counts its drops.
//
// The buffer is then dumped to argv[1], where the *desktop* tool,
// sub0log-cat, has to decode it (the embedded::decode ctest) -- the
// compatibility claim is that nothing downstream can tell.

#include <sub0log/log.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>

namespace {

std::atomic<std::uint64_t> gAllocations{0};

void* countedAllocate(const std::size_t bytes) noexcept
{
    gAllocations.fetch_add(1u, std::memory_order_relaxed);
    return std::malloc(bytes == 0u ? 1u : bytes);
}

constexpr sub0log::SubsystemId cSensor{3};

// Deliberately modest: the embedded path has to be usable at the sizes a
// microcontroller's retained RAM actually offers.
constexpr std::size_t cStorageBytes = 16u * 1024u;
alignas(8) std::byte gStorage[cStorageBytes];

} // namespace

// Replaced for the whole program so nothing -- the library, the standard
// library underneath it -- can allocate on the path under test unseen.
// No exceptions here, so a failed allocation aborts rather than throwing.
void* operator new(const std::size_t bytes)
{
    void* const p = countedAllocate(bytes);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}
void* operator new[](const std::size_t bytes)
{
    return ::operator new(bytes);
}
void* operator new(const std::size_t bytes, const std::nothrow_t&) noexcept
{
    return countedAllocate(bytes);
}
void* operator new[](const std::size_t bytes, const std::nothrow_t&) noexcept
{
    return countedAllocate(bytes);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(const int argc, char** const argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <dump-path>\n", argv[0]);
        return 2;
    }

    // Two windows, not one: creation and the emit path. The emit path must
    // never allocate, anywhere. Creation must not either -- except under
    // MSVC's iterator debugging (_ITERATOR_DEBUG_LEVEL > 0, the Debug
    // default), where every std::string constructed or moved heap-allocates
    // a debug "container proxy". Logger::Options and the segment's path are
    // std::string (docs/embedded.md, item 2), so a Debug MSVC build counts
    // those proxies there; they are the STL's checking, not the producer,
    // and a Release build has none. Reported either way, failed only where
    // it is the library's own allocation.
    const std::uint64_t before = gAllocations.load(std::memory_order_relaxed);
    std::uint64_t createAllocations = 0;
    std::uint64_t emitAllocations = 0;
    sub0log::Logger::Stats stats{};
    bool valid = false;
    bool pathEmpty = false;
    {
        const std::pair<sub0log::SubsystemId, std::string_view> names[] = {{cSensor, "sensor"}};
        sub0log::Logger::Options options{};
        options.segment_.chunkBytes_ = sub0log::wire::cChunkSizeUnit;
        options.subsystemNames_ = names;

        auto logger = sub0log::Logger::createInMemory(gStorage, options);
        valid = logger.valid();
        pathEmpty = logger.segmentPath().empty();
        const std::uint64_t afterCreate = gAllocations.load(std::memory_order_relaxed);
        createAllocations = afterCreate - before;
        {
            sub0log::Logger::ScopedBind bind{logger};
            sub0log_info(cSensor, "embedded sample {} of {}", std::uint32_t{3},
                         std::string_view{"imu"});
            // Enough to exhaust 16 KiB, so the drop path runs too.
            for (std::uint32_t i = 0; i < 2000u; ++i) {
                sub0log_debug(cSensor, "tick {}", i);
            }
        }
        stats = logger.stats();
        emitAllocations = gAllocations.load(std::memory_order_relaxed) - afterCreate;
    }

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
    constexpr bool cStlDebugProxies = true;
#else
    constexpr bool cStlDebugProxies = false;
#endif

    std::printf("embedded producer: valid=%d path-empty=%d heap-allocations: create=%llu%s "
                "emit=%llu dropped=%llu sizeof(Logger)=%zu storage=%zu\n",
                valid ? 1 : 0, pathEmpty ? 1 : 0,
                static_cast<unsigned long long>(createAllocations),
                cStlDebugProxies ? " (MSVC debug-iterator proxies)" : "",
                static_cast<unsigned long long>(emitAllocations),
                static_cast<unsigned long long>(stats.droppedRecords_), sizeof(sub0log::Logger),
                cStorageBytes);

    int failures = 0;
    if (!valid) {
        std::fprintf(stderr, "FAILED: createInMemory over a static buffer\n");
        ++failures;
    }
    if (!pathEmpty) {
        std::fprintf(stderr, "FAILED: an in-memory Logger reported a file path\n");
        ++failures;
    }
    if (emitAllocations != 0u) {
        std::fprintf(stderr, "FAILED: the in-memory emit path allocated %llu times\n",
                     static_cast<unsigned long long>(emitAllocations));
        ++failures;
    }
    if (createAllocations != 0u && !cStlDebugProxies) {
        std::fprintf(stderr, "FAILED: createInMemory allocated %llu times\n",
                     static_cast<unsigned long long>(createAllocations));
        ++failures;
    }
    if (stats.droppedRecords_ == 0u) {
        std::fprintf(stderr, "FAILED: a full buffer counted no drops\n");
        ++failures;
    }

    std::FILE* const out = std::fopen(argv[1], "wb");
    if (out == nullptr || std::fwrite(gStorage, 1, cStorageBytes, out) != cStorageBytes) {
        std::fprintf(stderr, "FAILED: could not write %s\n", argv[1]);
        ++failures;
    }
    if (out != nullptr) {
        std::fclose(out);
    }
    return failures == 0 ? 0 : 1;
}
