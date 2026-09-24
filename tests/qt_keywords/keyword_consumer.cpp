// A Qt-style consumer of Sub0Log, compiled four ways by CMakeLists.txt:
// {Qt's keyword macros, or a shim of them} x {defined before, or after,
// Sub0Log's headers}. Issue #1: Qt `#define`s `emit` and `slots` to
// nothing, and any Sub0Log identifier with either name was deleted from the
// token stream -- including at a sub0log_info() *call site*, which is why
// "include Sub0Log first" was never a real workaround and why the
// keywords-after variant exists at all.
//
// Compiling is half the claim. The other half is that the call sites the
// macros would have mangled still produce records a reader decodes, typed,
// so this writes through every renamed path and reads it all back:
//   - a fixed-argument site          (log.hpp's SUB0LOG_EMIT -> detail::emitRecord)
//   - a continuation-chained site    (detail::emitChained, `contSlots`)
//   - the plugin ABI table           (Sub0LogAbiV1::emit_record)

#ifndef SUB0LOG_KEYWORDS_FIRST
#  error "CMakeLists.txt passes SUB0LOG_KEYWORDS_FIRST=0 or 1"
#endif

#if SUB0LOG_KEYWORDS_REAL_QT
#  define SUB0LOG_KEYWORDS_HEADER <QtCore/QObject>
#else
#  define SUB0LOG_KEYWORDS_HEADER "qt_keyword_shim.hpp"
#endif

#if SUB0LOG_KEYWORDS_FIRST
#  include SUB0LOG_KEYWORDS_HEADER
#endif

// Every public header, not only the ones the round trip below needs: a
// header that reintroduces an `emit`/`slots` identifier fails this test
// whether or not anything here calls into it.
#include <sub0log/abi_host.hpp>
#include <sub0log/child.hpp>
#include <sub0log/chunk.hpp>
#include <sub0log/context.hpp>
#include <sub0log/encode.hpp>
#include <sub0log/instance.hpp>
#include <sub0log/log.hpp>
#include <sub0log/merge.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/segment.hpp>
#include <sub0log/severity.hpp>
#include <sub0log/site.hpp>
#include <sub0log/sub0log_abi.h>
#include <sub0log/version.hpp>
#include <sub0log/wire.hpp>

#if !SUB0LOG_KEYWORDS_FIRST
#  include SUB0LOG_KEYWORDS_HEADER
#endif

#if SUB0LOG_KEYWORDS_REAL_QT
#  include <QtCore/QString>
#endif

// If these are not macros here the test proves nothing: a consumer (or a
// Qt build) with QT_NO_KEYWORDS set would pass it vacuously.
#if !defined(emit) || !defined(slots) || !defined(signals)
#  error "Qt's keyword macros are not active -- this test would be vacuous"
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

// Plugin hosts write this once; the test is one, so it gets the real export.
SUB0LOG_ABI_HOST_EXPORT()

namespace {

constexpr sub0log::SubsystemId cQtConsumer{7};

int gFailures = 0;

void check(const bool ok, const char* const what)
{
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++gFailures;
    }
}

struct PluginSite {
    int marker_;
};
PluginSite gPluginSite{0};

// Emits one record through the C ABI table exactly as a plugin would --
// a {u64} payload, the site defined first.
void emitThroughAbi(const Sub0LogAbiV1& table)
{
    constexpr std::uint8_t cTypeU64 = 9u; // wire::TypeCode::U64
    constexpr std::uint8_t cArgTypes[1] = {cTypeU64};
    constexpr std::uint8_t cSeverityInfo = 2u;
    const auto siteId = reinterpret_cast<std::uint64_t>(&gPluginSite);

    table.define_site(siteId, 42u, cSeverityInfo, "abi value {}", __FILE__,
                      static_cast<std::uint32_t>(__LINE__), cArgTypes, 1u);

    const std::uint64_t value = 99u;
    unsigned char payload[sizeof(value)];
    std::memcpy(payload, &value, sizeof(value));

    Sub0LogAbiRecord record{};
    record.site_id = siteId;
    record.subsystem_id = 42u;
    record.severity = cSeverityInfo;
    record.payload = payload;
    record.payload_bytes = static_cast<std::uint32_t>(sizeof(payload));
    table.emit_record(&record);
}

} // namespace

int main()
{
    const auto directory = std::filesystem::temp_directory_path()
        / ("sub0log-qt-keywords-" SUB0LOG_KEYWORDS_VARIANT "-"
           + std::to_string(sub0log::detail::currentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);

    // Long enough to force the continuation chain (cInlineBytesCap is the
    // most one record carries inline).
    const std::string longText(sub0log::wire::cInlineBytesCap * 2u + 17u, 'q');

    {
        auto logger = sub0log::Logger::create({.directory_ = directory.string()});
        check(logger.valid(), "Logger::create");
        if (!logger.valid()) {
            return 1;
        }
        sub0log::Logger::ScopedBind bind{logger};

        sub0log_info(cQtConsumer, "fixed {} {}", std::uint64_t{42}, std::int32_t{-7});
        sub0log_warning(cQtConsumer, "chained {}", std::string_view{longText});

        const Sub0LogAbiV1* table = sub0log::abi::hostTable();
        check(table != nullptr, "hostTable() with a Logger bound");
        if (table != nullptr) {
            emitThroughAbi(*table);
        }
    }

#if SUB0LOG_KEYWORDS_REAL_QT
    // Uses QtCore for real, so the link against Qt6::Core is not optimised
    // into a no-op and a broken Qt setup fails here rather than passing.
    check(!QString::fromUtf8(qVersion()).isEmpty(), "qVersion()");
#endif

    // Read back: exactly one segment file, three records, typed.
    std::vector<std::filesystem::path> segments;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file()) {
            segments.push_back(entry.path());
        }
    }
    check(segments.size() == 1u, "exactly one segment written");
    if (segments.size() != 1u) {
        return 1;
    }
    std::ifstream in(segments[0], std::ios::binary);
    const std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    const std::vector<std::byte> image(reinterpret_cast<const std::byte*>(raw.data()),
                                       reinterpret_cast<const std::byte*>(raw.data()) + raw.size());

    auto reader = sub0log::SegmentReader::open(image);
    check(reader.valid(), "SegmentReader::open");
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    check(decoder.undecodableRecords() == 0u, "no undecodable records");
    check(records.size() == 3u, "three records decoded");

    if (records.size() == 3u) {
        check(sub0log::Decoder::format(records[0]) == "fixed 42 -7", "fixed-argument record");

        check(records[1].args_.size() == 1u
                  && std::get<std::string_view>(records[1].args_[0]) == longText,
              "continuation-chained record reassembled");

        check(records[2].site_ != nullptr && records[2].site_->format_ == "abi value {}"
                  && records[2].args_.size() == 1u
                  && std::get<std::uint64_t>(records[2].args_[0]) == 99u,
              "ABI emit_record record");
    }

    std::filesystem::remove_all(directory, ec);
    if (gFailures != 0) {
        return 1;
    }
    std::printf("qt keywords (" SUB0LOG_KEYWORDS_VARIANT "): OK\n");
    return 0;
}
