// Real mapped-file producer for the standard-library JSON receiver.
#include <sub0log/log.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

int main(const int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    constexpr sub0log::SubsystemId cPrimary{7};
    constexpr sub0log::SubsystemId cWarnings{8};
    constexpr std::array<std::pair<sub0log::SubsystemId, std::string_view>, 2> cNames{{
        {cPrimary, "receiver\n\"test"}, {cWarnings, "warning"}}};
    sub0log::Logger::Options options{};
    options.directory_ = argv[1];
    options.subsystemNames_ = cNames;
    auto logger = sub0log::Logger::create(options);
    if (!logger.valid()) {
        return 1;
    }
    const sub0log::Logger::ScopedBind bind{logger};
    const sub0log::CorrelationScope correlation{9007199254740993ull};
    sub0log_info(cPrimary, "integers {} {} {} {} {} {} {} {}",
        std::int8_t{-128}, std::uint8_t{255}, std::int16_t{-32768}, std::uint16_t{65535},
        std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::uint64_t>::max());
    sub0log_info(cPrimary, "scalars {} {} {}", true, '\0', &options);
    sub0log_info(cPrimary, "floats {} {} {} {} {} {} {} {}",
        1.25f, 2.5, std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
        -0.0, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
    constexpr char cBytes[] = {'\0', '\n', '\r', '\t', '"', '\\', '\x1b', '\x80', '\xff'};
    sub0log_info(cPrimary, "bytes {}", std::string_view{cBytes, sizeof(cBytes)});
    sub0log_info(cPrimary, "unicode {}", std::string_view{"\xc3\xa9\xf0\x9f\x98\x80"});
    sub0log_info(cPrimary, "\xff" "invalid-format");
    const std::string large(9000, 'z');
    sub0log_info(cPrimary, "truncated {}", std::string_view{large});
    sub0log_warning(cWarnings, "warning fixture");
    return logger.stats().droppedRecords_ == 0 ? 0 : 1;
}
