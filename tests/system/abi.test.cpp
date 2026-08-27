// R4, proved rather than argued: docs/adoption-friction.md 2.3 measured a
// plugin compiled -fvisibility=hidden losing every one of its own records
// because Logger::sActive_ is a separate object in its module. This test
// builds a plugin that exact way (tests/system/plugin/abi_test_plugin.cpp,
// which links nothing of ours and includes only sub0log_abi.h), dlopen()s
// it, hands it the host's table, lets it log, dlclose()s it, and only then
// decodes the host's segment -- R4.3's "decodable after the plugin has been
// unloaded" is not worth pinning any other way.

#include <sub0log/abi_host.hpp>
#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include "support/fixtures.hpp"

#include "support/test_framework.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#if defined(SUB0LOG_ABI_PLUGIN_PATH)

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

// Exactly one translation unit in this whole link calls this (the contract
// in abi_host.hpp): the system-test binary is that unit, since it is the
// only place in this build that owns a Logger for a plugin to reach.
SUB0LOG_ABI_HOST_EXPORT()

namespace {

constexpr sub0log::SubsystemId cHost{5};

using PluginRunFn = void (*)(const Sub0LogAbiV1*);

#if defined(_WIN32)

// Windows CI (windows-msvc) exercises this; it was written to be reasoned
// about against the documented Win32 contracts (LoadLibraryW/GetProcAddress/
// FreeLibrary, GetModuleHandleW(nullptr) for the running module) rather than
// run locally -- this environment has no Windows toolchain to verify it on.

[[nodiscard]] std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    REQUIRE(needed > 0);
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(),
                          needed);
    return result;
}

using PluginHandle = HMODULE;

[[nodiscard]] PluginHandle loadPlugin(const std::string& path)
{
    return ::LoadLibraryW(widen(path).c_str());
}

void unloadPlugin(PluginHandle handle) { ::FreeLibrary(handle); }

template <typename Fn>
[[nodiscard]] Fn resolve(PluginHandle handle, const char* name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(handle, name)));
}

[[nodiscard]] Sub0LogAbiGetter resolveHostGetter()
{
    const HMODULE self = ::GetModuleHandleW(nullptr);
    return reinterpret_cast<Sub0LogAbiGetter>(
        reinterpret_cast<void*>(::GetProcAddress(self, SUB0LOG_ABI_GETTER_NAME)));
}

#else

using PluginHandle = void*;

[[nodiscard]] PluginHandle loadPlugin(const std::string& path)
{
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void unloadPlugin(PluginHandle handle) { ::dlclose(handle); }

template <typename Fn>
[[nodiscard]] Fn resolve(PluginHandle handle, const char* name)
{
    return reinterpret_cast<Fn>(::dlsym(handle, name));
}

// dlopen(NULL, ...) hands back a handle for the running program itself; this
// is what proves SUB0LOG_ABI_HOST_EXPORT() actually placed a dynamically
// resolvable symbol (R4.1), the same way a plugin would resolve it, rather
// than trusting the direct C++ call sub0log::abi::hostTable() would also
// have given.
[[nodiscard]] Sub0LogAbiGetter resolveHostGetter()
{
    void* const self = ::dlopen(nullptr, RTLD_NOW);
    REQUIRE(self != nullptr);
    auto* const getter =
        reinterpret_cast<Sub0LogAbiGetter>(::dlsym(self, SUB0LOG_ABI_GETTER_NAME));
    ::dlclose(self);
    return getter;
}

#endif

} // namespace

TEST_CASE("a plugin built -fvisibility=hidden logs into the host through the C ABI (R4)")
{
    const auto directory = sub0log::test::freshDirectory("abiroundtrip");

    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());
    std::uint64_t correlationSeen = 0;
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope correlation{};
        correlationSeen = correlation.id();

        sub0log_info(cHost, "host before the plugin");

        const Sub0LogAbiGetter getter = resolveHostGetter();
        REQUIRE(getter != nullptr); // R4.1: the export macro really exported it
        const Sub0LogAbiV1* const table = getter();
        REQUIRE(table != nullptr); // a Logger is bound
        CHECK(table->size >= sizeof(Sub0LogAbiV1));
        CHECK(table->version == SUB0LOG_ABI_VERSION);
        CHECK(table->current_correlation() == correlationSeen); // R6.1 across the boundary

        const PluginHandle plugin = loadPlugin(SUB0LOG_ABI_PLUGIN_PATH);
        REQUIRE(plugin != nullptr);

        const auto run = resolve<PluginRunFn>(plugin, "sub0log_test_plugin_run");
        REQUIRE(run != nullptr);
        run(table);

        unloadPlugin(plugin);
        // R4.3 starts being tested from exactly this line: nothing below
        // may depend on the plugin's module still being mapped.

        sub0log_info(cHost, "host after the plugin");
    }

    const auto image = sub0log::test::slurp(sub0log::test::onlySegmentIn(directory));
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());

    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(reader.unreadableBytes() == 0u);
    CHECK(decoder.undecodableRecords() == 0u); // the plugin's records decode too

    // One stream: the host's two records and the plugin's two, in the order
    // they were written, exactly as if the plugin had linked the library.
    REQUIRE(records.size() == 4u);
    REQUIRE(records[0].site_ != nullptr);
    CHECK(records[0].site_->format_ == "host before the plugin");

    for (const std::uint64_t i : {std::uint64_t{1}, std::uint64_t{2}}) {
        CAPTURE(i);
        const auto& record = records[static_cast<std::size_t>(i)];
        REQUIRE(record.site_ != nullptr);
        CHECK(record.site_->format_ == "plugin emitted {} of {}");
        CHECK(record.site_->file_ == "abi_test_plugin.cpp");
        CHECK(record.site_->line_ == 7u);
        CHECK(record.site_->subsystem_ == sub0log::SubsystemId{42u});
        CHECK(record.site_->severity_ == sub0log::Severity::Info);
        CHECK(record.correlationId_ == correlationSeen); // R6.1, again, on the written record
        REQUIRE(record.args_.size() == 2u);
        CHECK(std::get<std::uint64_t>(record.args_[0]) == i);
        CHECK(std::get<std::string_view>(record.args_[1]) == "hello from the plugin");
    }

    REQUIRE(records[3].site_ != nullptr);
    CHECK(records[3].site_->format_ == "host after the plugin");

    std::filesystem::remove_all(directory);
}

#endif // SUB0LOG_ABI_PLUGIN_PATH
