// plugin_abi/plugin.cpp -- the plugin half of 12_plugin_abi, and the one
// file in this whole ladder that must NOT include <sub0log/log.hpp>.
//
// Why not, when every other example does? Because a plugin that included it
// would compile and would silently lose every one of its own records the
// moment it is built the way real plugins are built.
//
// Sub0Log's call-site macros reach the active Logger through a single
// process-wide pointer, `Logger::sActive_` -- a `static inline` data
// member. That is one object per process only when the whole program is
// one module. The instant this file becomes a shared library instead
// (which is the only way anything gets called "a plugin"), the linker's
// visibility rules decide whether that pointer is shared with the host or
// duplicated: built `-fvisibility=hidden` -- the recommended default for a
// shared library on POSIX, and the *only* behaviour a Windows DLL has,
// nothing is exported unless asked for -- this module gets its own,
// separate `sActive_`, permanently null, because ScopedBind ran in the
// host's copy. Every call site in this file would then be silently
// evaluating a null check and returning: not a crash, not a dropped-record
// counter incrementing, nothing. Building `-fvisibility=hidden` below (see
// CMakeLists.txt) is not a hardening flourish, it is what makes that real
// rather than theoretical, and it is exactly the shape
// docs/adoption-friction.md's finding 2.3 measured.
//
// R4 exists to give a plugin a way to reach the host's Logger that a
// process boundary cannot silently duplicate: a plain C function-pointer
// table (sub0log_abi.h), resolved by name and handed across, with no
// language-level global on either side of it. This file includes ONLY that
// header -- no other header of ours, and nothing is linked against the
// library (see CMakeLists.txt's target_include_directories: the raw
// include path, not Sub0Log::Sub0Log) -- because the whole point is to
// prove a plugin does not need to.
//
// Wire encoding below is written by hand rather than through encode.hpp,
// on purpose: a real plugin has no access to that header either, only to
// the type codes sub0log_abi.h's own comments document.

#include <sub0log/sub0log_abi.h>

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#  define SUB0LOG_EXAMPLE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define SUB0LOG_EXAMPLE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

// A site id only has to be a stable, distinct value for this plugin's
// lifetime -- the host never dereferences it, only carries it into the
// SiteDefinition record it writes. The address of a static object is a
// convenient way to get one without inventing an id-allocation scheme;
// STYLE_GUIDE.md's macro discussion is the C++-side version of the same
// requirement (a distinct object with static storage duration per call
// site), done here by hand because this file has no macro to do it for it.
struct PluginSite {
    int marker_;
};
PluginSite gRequestSite{0};

constexpr std::uint8_t cTypeU64 = 9;   // wire::TypeCode::U64
constexpr std::uint8_t cTypeBytes = 13; // wire::TypeCode::Bytes
constexpr std::uint8_t cArgTypes[2] = {cTypeU64, cTypeBytes};

constexpr char cRoute[] = "/status";

} // namespace

// The one thing this plugin exports: the host resolves this by name after
// loading the module and hands it the table it already fetched through
// sub0log_abi_v1(). Three "requests" logged, each through the host's
// producer path, running in the host's module the whole time -- there is
// no second sActive_ here for this to be silently separate from.
SUB0LOG_EXAMPLE_PLUGIN_EXPORT void sub0log_example_plugin_run(const Sub0LogAbiV1* table)
{
    if (table == nullptr || table->size < sizeof(Sub0LogAbiV1)) {
        return; // NULL means no Logger is bound in the host right now (R9.3's shape, at the boundary).
    }

    const auto siteId = reinterpret_cast<std::uint64_t>(&gRequestSite);
    constexpr std::uint32_t cSubsystem = 99u; // this plugin's own vocabulary (R2.2's story again, at the boundary).
    constexpr std::uint8_t cSeverityInfo = 2u; // sub0log::Severity::Info

    table->define_site(siteId, cSubsystem, cSeverityInfo, "handled request {} for {}", "plugin.cpp", 78u,
                       cArgTypes, 2u);

    for (std::uint64_t requestId = 1; requestId <= 3; ++requestId) {
        // u64 count, then u16 length + bytes -- exactly what a Bytes
        // argument looks like on the wire (wire.hpp's own TypeCode::Bytes
        // comment), assembled by hand because this file has no encode.hpp
        // to do it for it either.
        constexpr std::uint16_t cRouteLen = sizeof(cRoute) - 1;
        unsigned char payload[8 + 2 + sizeof(cRoute) - 1];
        std::memcpy(payload, &requestId, 8);
        std::memcpy(payload + 8, &cRouteLen, 2);
        std::memcpy(payload + 10, cRoute, cRouteLen);

        Sub0LogAbiRecord record{};
        record.site_id = siteId;
        record.subsystem_id = cSubsystem;
        record.severity = cSeverityInfo;
        record.payload = payload;
        record.payload_bytes = static_cast<std::uint32_t>(sizeof(payload));
        table->emit(&record); // Runs in the HOST's producer path. Never blocks; a drop is counted there.
    }
}
