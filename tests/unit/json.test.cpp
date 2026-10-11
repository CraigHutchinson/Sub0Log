#include <sub0log/json.hpp>

#include "support/test_framework.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

TEST_CASE("JSON formatter validates borrowed record shape before export")
{
    sub0log::MergedRecord entry{};
    CHECK_THROWS_AS(sub0log::formatJson(entry), std::invalid_argument);
    sub0log::DecodedSite site{};
    entry.record_.site_ = &site;
    entry.record_.args_.emplace_back(true);
    CHECK_THROWS_AS(sub0log::formatJson(entry), std::invalid_argument);
    site.argTypes_.push_back(sub0log::wire::TypeCode::Bool);
    CHECK_NOTHROW(sub0log::formatJson(entry));
    site.argTypes_[0] = sub0log::wire::TypeCode::U64;
    CHECK_THROWS_AS(sub0log::formatJson(entry), std::bad_variant_access);
    site.argTypes_[0] = sub0log::wire::TypeCode::Invalid;
    CHECK_THROWS_AS(sub0log::formatJson(entry), std::invalid_argument);
}

TEST_CASE("JSON text fallback rejects invalid UTF8 instead of replacing bytes")
{
    const std::array<std::string_view, 8> invalid{{
        "\x80", "\xc0\x80", "\xc2", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80"}};
    sub0log::DecodedSite site{};
    sub0log::MergedRecord entry{};
    entry.record_.site_ = &site;
    for (const auto bytes : invalid) {
        site.format_ = bytes;
        site.file_ = bytes;
        const auto output = sub0log::formatJson(entry, bytes);
        CHECK(output.find("\"format\":{\"encoding\":\"hex\"") != std::string::npos);
        CHECK(output.find("\"file\":{\"encoding\":\"hex\"") != std::string::npos);
        CHECK(output.find("\"subsystem_name\":{\"encoding\":\"hex\"") != std::string::npos);
    }
    site.format_ = "\xc2\x80\xe0\xa0\x80\xf4\x8f\xbf\xbf";
    site.file_ = "\"\\\n\t";
    const auto output = sub0log::formatJson(entry);
    CHECK(output.find("\"format\":{\"encoding\":\"utf8\"") != std::string::npos);
    CHECK(output.find("\\\"\\\\\\u000a\\u0009") != std::string::npos);
}

TEST_CASE("JSON formatter result owns text after decoder-style borrows change")
{
    std::string format{"before {}"};
    std::string bytes{"old"};
    sub0log::DecodedSite site{};
    site.format_ = format;
    site.argTypes_.push_back(sub0log::wire::TypeCode::Bytes);
    sub0log::MergedRecord entry{};
    entry.record_.site_ = &site;
    entry.record_.args_.emplace_back(std::string_view{bytes});
    const auto output = sub0log::formatJson(entry);
    format.assign("after");
    bytes.assign("new");
    CHECK(output.find("before {}") != std::string::npos);
    CHECK(output.find("6f6c64") != std::string::npos);
}
