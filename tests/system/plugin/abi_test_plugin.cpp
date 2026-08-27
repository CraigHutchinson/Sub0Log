// The plugin side of the R4 round trip (docs/adoption-friction.md 2.3): this
// file includes ONLY sub0log_abi.h -- no other header of ours, nothing
// linked -- and is built -fvisibility=hidden on POSIX (see CMakeLists.txt),
// the exact configuration the probe in 2.3 measured losing every plugin
// record under the header-only path. Proving the ABI fixes that means
// proving this build, not a friendlier one.
//
// Wire encoding is written by hand rather than through encode.hpp, on
// purpose: a real plugin has no access to that header either. Two
// arguments, matching the type codes handed to define_site below --
// wire::TypeCode::U64 (9) then wire::TypeCode::Bytes (13), values copied
// from wire.hpp rather than included from it, exactly as a real plugin's
// build would have to.

#include <sub0log/sub0log_abi.h>

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#  define SUB0LOG_TEST_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define SUB0LOG_TEST_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

// One static object, its address the site id -- by hand, the shape
// STYLE_GUIDE.md requires a macro to get right in the C++ path (a distinct
// object with static storage duration per call site). A real plugin might
// derive this any number of ways; all the host cares about is that it is
// stable for the plugin's lifetime.
struct PluginSite {
    int marker_;
};

PluginSite gSite{0};

constexpr std::uint8_t cTypeU64 = 9u;   // wire::TypeCode::U64
constexpr std::uint8_t cTypeBytes = 13u; // wire::TypeCode::Bytes
constexpr std::uint8_t cArgTypes[2] = {cTypeU64, cTypeBytes};

constexpr char cText[] = "hello from the plugin";

} // namespace

// The one thing this plugin exports: the test harness resolves this by name
// after dlopen(), hands it the table the host already fetched through
// sub0log_abi_v1(), and this defines one site and emits two records through
// it -- never touching Sub0Log's own headers to do so.
SUB0LOG_TEST_PLUGIN_EXPORT void sub0log_test_plugin_run(const Sub0LogAbiV1* table)
{
    if (table == nullptr || table->size < sizeof(Sub0LogAbiV1)) {
        return;
    }

    const auto siteId = reinterpret_cast<std::uint64_t>(&gSite);
    constexpr std::uint32_t cSubsystem = 42u;
    constexpr std::uint8_t cSeverityInfo = 2u; // sub0log::Severity::Info

    table->define_site(siteId, cSubsystem, cSeverityInfo, "plugin emitted {} of {}",
                       "abi_test_plugin.cpp", 7u, cArgTypes, 2u);

    for (std::uint64_t i = 1; i <= 2; ++i) {
        // u64 count, then u16 length + bytes -- exactly what encode.hpp
        // would have produced for {std::uint64_t, std::string_view}.
        constexpr std::uint16_t cTextLen = sizeof(cText) - 1;
        unsigned char payload[8 + 2 + sizeof(cText) - 1];
        std::memcpy(payload, &i, 8);
        std::memcpy(payload + 8, &cTextLen, 2);
        std::memcpy(payload + 10, cText, cTextLen);

        Sub0LogAbiRecord record{};
        record.site_id = siteId;
        record.subsystem_id = cSubsystem;
        record.severity = cSeverityInfo;
        record.payload = payload;
        record.payload_bytes = static_cast<std::uint32_t>(sizeof(payload));
        table->emit(&record);
    }
}
