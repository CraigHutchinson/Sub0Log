// SUB0LOG_PLATFORM_CUSTOM (issue #2): the producer with no OS underneath --
// no OS header included, the platform supplied by the four C hooks below,
// the segment in a static array. Two uses:
//
//   - the host suite builds and runs it (tests/embedded/CMakeLists.txt), so
//     the custom arm keeps compiling and working on every CI leg;
//   - CI's embedded-arm job cross-compiles and links it for a bare-metal
//     32-bit ARM core with arm-none-eabi-g++ and prints its size -- the
//     numbers docs/embedded.md quotes come from exactly this file.
//
// Ten call sites of assorted argument shapes, so the per-site cost is
// visible against the one-off cost of the Logger itself.

#include <sub0log/log.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

// A real port wires these to its RTOS: a monotonic tick counter scaled to
// ns, an RTC, a node/image id, and the current task handle. With no RTC,
// the wall hook returns the *monotonic* reading, not 0: readers align each
// segment through its (monotonic, wall) anchor pair, and a constant wall
// clock would restart every segment's timeline at the same instant
// (docs/embedded.md, "Ordering across rotated segments").
namespace {
std::uint64_t gTicks = 0;
}
extern "C" std::uint64_t sub0log_platform_monotonic_ns(void) noexcept
{
    gTicks += 1000u;
    return gTicks;
}
extern "C" std::uint64_t sub0log_platform_wall_ns(void) noexcept { return gTicks; }
extern "C" std::uint64_t sub0log_platform_process_id(void) noexcept { return 1u; }
extern "C" std::uint64_t sub0log_platform_thread_id(void) noexcept { return 1u; }

alignas(8) std::byte gStorage[16u * 1024u];

int main()
{
    // The default chunk (wire::cDefaultChunkBytes, 64 KiB) is sized for a
    // desktop segment and is larger than this whole buffer; an embedded
    // caller always picks its own.
    sub0log::Logger::Options options{};
    options.segment_.chunkBytes_ = 1024u;
    auto logger = sub0log::Logger::createInMemory(gStorage, options);
    if (!logger.valid()) {
        return 1;
    }
    {
        sub0log::Logger::ScopedBind bind{logger};
        constexpr sub0log::SubsystemId s{1};
        sub0log_info(s, "v {}", std::uint32_t{7});
        sub0log_info(s, "a {}", std::uint32_t{1});
        sub0log_info(s, "b {} {}", std::uint32_t{1}, std::int64_t{2});
        sub0log_warning(s, "c {}", 1.5);
        sub0log_error(s, "d {}", std::string_view{"x"});
        sub0log_debug(s, "e");
        sub0log_info(s, "f {}", std::uint16_t{3});
        sub0log_info(s, "g {} {}", std::uint32_t{1}, std::uint32_t{2});
        sub0log_info(s, "h {}", true);
        sub0log_info(s, "i {}", std::int32_t{-1});
    }
    // Something observable from the buffer, or an optimiser is entitled to
    // delete every store into it and the size numbers mean nothing.
    const bool claimed = gStorage[sub0log::wire::cNextChunkOffset] != std::byte{0};
    return (claimed && logger.stats().droppedRecords_ == 0u) ? 0 : 1;
}
