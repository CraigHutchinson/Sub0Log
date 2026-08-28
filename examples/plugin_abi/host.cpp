// plugin_abi/host.cpp -- 12_plugin_abi: a host loading a plugin through
// R4's C ABI, the way an application with a plugin architecture actually
// would.
//
// plugin.cpp's own header comment is the half of this story about *why* a
// plugin cannot simply `#include <sub0log/log.hpp>` -- read that first if
// you have not. This file is the other half: what the host on the other
// end of that boundary looks like. Four steps, in order:
//
//   1. export the ABI getter (SUB0LOG_ABI_HOST_EXPORT(), once, below) and
//      bind a Logger the ordinary way -- the host is consumer code like
//      every other example, nothing about it is special;
//   2. resolve that export back through the dynamic loader itself, exactly
//      the way an unrelated plugin module would, rather than trusting that
//      the direct C++ call to the same function also proves it was
//      exported -- dlopen(nullptr, ...)/dlsym on POSIX,
//      GetModuleHandleW(nullptr)/GetProcAddress on Windows;
//   3. load the plugin module, hand it the table, let it log;
//   4. unload the plugin module -- *before* decoding anything. R4.3 says a
//      plugin's records must remain decodable after it is gone, and the
//      only way that claim means anything is to prove it with the module
//      actually unloaded first, not merely trusted to still work if it
//      were.
//
// Modeled on tests/system/abi.test.cpp, which pins this exact round trip
// for the test suite; this is the teaching version, generously commented
// per examples/README.md's own note on the departure.
//
// Requirements demonstrated: R4.1 (a plugin logs into the host without
// linking the library, and without a duplicated singleton), R4.2 (the
// boundary itself: a plain C function-pointer table, nothing else crosses
// it), R4.3 (decodable after the plugin is unloaded), R6.1 (correlation
// crosses the boundary too).

#include <sub0log/abi_host.hpp>
#include <sub0log/context.hpp>
#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::_getpid())
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::getpid())
#  include <dlfcn.h>
#endif

// Exactly one translation unit in the whole program may call this
// (abi_host.hpp's own contract) -- this executable is that unit, since it
// is the only place in this build that owns a Logger for a plugin to reach.
SUB0LOG_ABI_HOST_EXPORT()

namespace {

constexpr sub0log::SubsystemId cHost{1};

std::filesystem::path makeScratchDir()
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-12-plugin-abi-" + std::to_string(SUB0LOG_EXAMPLE_PID()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path onlySegment(const std::filesystem::path& dir)
{
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.path().extension() == ".s0l") {
            return entry.path();
        }
    }
    return {};
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

using PluginRunFn = void (*)(const Sub0LogAbiV1*);

#if defined(_WIN32)

[[nodiscard]] std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(needed > 0 ? needed : 0), L'\0');
    if (needed > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), needed);
    }
    return result;
}

using PluginHandle = HMODULE;

[[nodiscard]] PluginHandle loadPlugin(const std::string& path) { return ::LoadLibraryW(widen(path).c_str()); }
void unloadPlugin(PluginHandle handle) { ::FreeLibrary(handle); }

template <typename Fn>
[[nodiscard]] Fn resolve(PluginHandle handle, const char* name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(handle, name)));
}

// GetModuleHandleW(nullptr) is "this running executable" -- resolving the
// export back through it (rather than trusting the direct C++ call to
// sub0log::abi::hostTable() also proves it was exported) is what makes
// this the same lookup a genuinely separate plugin module would perform.
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

// dlopen(nullptr, ...) hands back a handle for the running program itself;
// resolving the export through *that*, rather than trusting the direct
// C++ call to sub0log::abi::hostTable() also proves it was exported, is
// what makes this the same lookup a genuinely separate plugin module would
// perform to find this host.
[[nodiscard]] Sub0LogAbiGetter resolveHostGetter()
{
    void* const self = ::dlopen(nullptr, RTLD_NOW);
    if (self == nullptr) {
        return nullptr;
    }
    auto* const getter = reinterpret_cast<Sub0LogAbiGetter>(::dlsym(self, SUB0LOG_ABI_GETTER_NAME));
    ::dlclose(self);
    return getter;
}

#endif

} // namespace

int main()
{
    const auto dir = makeScratchDir();
    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "pluginabi"});
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        return 1;
    }

    std::uint64_t correlationSeen = 0;
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope activity{};
        correlationSeen = activity.id();

        sub0log_info(cHost, "host: starting up, about to load a plugin");

        // --- step 2: resolve our own export the way a plugin would ------
        const Sub0LogAbiGetter getter = resolveHostGetter();
        if (getter == nullptr) {
            std::fprintf(stderr, "SUB0LOG_ABI_HOST_EXPORT() did not export a resolvable symbol\n");
            return 1;
        }
        const Sub0LogAbiV1* const table = getter();
        if (table == nullptr) {
            std::fprintf(stderr, "the ABI getter returned NULL -- no Logger bound? (there is one)\n");
            return 1;
        }
        std::printf("ABI table: size=%u version=%u (SUB0LOG_ABI_VERSION=%u)\n", table->size, table->version,
                    static_cast<unsigned>(SUB0LOG_ABI_VERSION));
        // R6.1 at the boundary: the plugin sees the same correlation id the
        // host set, through current_correlation() rather than a shared
        // global -- there is no shared global to have.
        if (table->current_correlation() != correlationSeen) {
            std::fprintf(stderr, "the ABI table's correlation id did not match the host's\n");
            return 1;
        }

        // --- step 3: load the plugin, hand it the table, let it log -----
        const PluginHandle plugin = loadPlugin(SUB0LOG_EXAMPLE_PLUGIN_PATH);
        if (plugin == nullptr) {
            std::fprintf(stderr, "could not load the plugin at " SUB0LOG_EXAMPLE_PLUGIN_PATH "\n");
            return 1;
        }
        const auto run = resolve<PluginRunFn>(plugin, "sub0log_example_plugin_run");
        if (run == nullptr) {
            std::fprintf(stderr, "the plugin did not export sub0log_example_plugin_run\n");
            return 1;
        }
        std::printf("plugin loaded from %s; handing it the table and letting it log\n",
                    SUB0LOG_EXAMPLE_PLUGIN_PATH);
        run(table);

        // --- step 4: unload BEFORE decoding anything (R4.3) --------------
        unloadPlugin(plugin);
        std::printf("plugin unloaded; everything after this line proves R4.3 -- decodable with the "
                    "plugin's module no longer mapped anywhere in this process.\n");

        sub0log_info(cHost, "host: plugin unloaded, wrapping up");
    }

    // --- decode: one stream, host records and plugin records together ---
    const auto image = slurp(onlySegment(dir));
    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "segment did not open\n");
        return 1;
    }
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    if (reader.unreadableBytes() != 0u || decoder.undecodableRecords() != 0u) {
        std::fprintf(stderr, "damage in a segment that should have none\n");
        return 1;
    }

    std::printf("\ndecoded %zu record(s):\n", records.size());
    for (const auto& record : records) {
        std::printf("  [subsystem=%u] %s\n", record.site_->subsystem_.value_,
                    sub0log::Decoder::format(record).c_str());
    }

    bool ok = true;
    ok = ok && (records.size() == 5u); // 2 host + 3 plugin
    ok = ok && (records[0].site_->format_ == "host: starting up, about to load a plugin");
    ok = ok && (records[4].site_->format_ == "host: plugin unloaded, wrapping up");
    for (const std::size_t i : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        const auto& record = records[i];
        ok = ok && (record.site_->format_ == "handled request {} for {}");
        ok = ok && (record.site_->file_ == "plugin.cpp");
        ok = ok && (record.site_->line_ == 78u);
        ok = ok && (record.site_->subsystem_ == sub0log::SubsystemId{99u});
        ok = ok && (record.correlationId_ == correlationSeen); // R6.1, again, on the written record
    }

    std::filesystem::remove_all(dir);
    if (!ok) {
        std::fprintf(stderr, "one or more expectations about the ABI round trip did not hold\n");
        return 1;
    }
    std::printf("\nexample 12 OK\n");
    return 0;
}
