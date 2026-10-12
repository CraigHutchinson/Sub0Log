#pragma once

/** @file sub0log/json.hpp
 *  Cold JSON snapshot formatting for machine consumers of decoded messages.
 */

#include "merge.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace sub0log {
namespace json_detail {

[[nodiscard]] constexpr bool validUtf8(const std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80u) {
            continue;
        }
        unsigned count{};
        std::uint32_t code{};
        std::uint32_t minimum{};
        if (first >= 0xc2u && first <= 0xdfu) {
            count = 1; code = first & 0x1fu; minimum = 0x80u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            count = 2; code = first & 0x0fu; minimum = 0x800u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            count = 3; code = first & 0x07u; minimum = 0x10000u;
        } else {
            return false;
        }
        if (text.size() - i < count) {
            return false;
        }
        for (unsigned j = 0; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0u) != 0x80u) {
                return false;
            }
            code = (code << 6u) | (next & 0x3fu);
        }
        if (code < minimum || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

inline constexpr std::string_view cHexDigits = "0123456789abcdef";

inline void appendQuoted(std::string& out, const std::string_view text)
{
    out += '"';
    for (const unsigned char byte : text) {
        if (byte == '"' || byte == '\\') {
            out += '\\';
            out += static_cast<char>(byte);
        } else if (byte < 0x20u) {
            out += "\\u00";
            out += cHexDigits[byte >> 4u];
            out += cHexDigits[byte & 0x0fu];
        } else {
            out += static_cast<char>(byte);
        }
    }
    out += '"';
}

inline void appendHex(std::string& out, const std::string_view bytes)
{
    out += "{\"encoding\":\"hex\",\"value\":\"";
    for (const unsigned char byte : bytes) {
        out += cHexDigits[byte >> 4u];
        out += cHexDigits[byte & 0x0fu];
    }
    out += "\"}";
}

inline void appendText(std::string& out, const std::string_view bytes)
{
    if (!validUtf8(bytes)) {
        appendHex(out, bytes);
        return;
    }
    out += "{\"encoding\":\"utf8\",\"value\":";
    appendQuoted(out, bytes);
    out += '}';
}

/// Appends decimal digits directly; the buffer covers every supported integer,
/// including the sign of the smallest signed value.
template <std::integral Integer>
    requires (!std::same_as<Integer, bool>)
inline void appendInteger(std::string& out, const Integer value)
{
    std::array<char, std::numeric_limits<Integer>::digits10 + 3> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        throw std::runtime_error{"JSON integer conversion failed"};
    }
    out.append(buffer.data(), result.ptr);
}

inline void appendFloat(std::string& out, const double value)
{
    if (std::isnan(value)) {
        out += "\"nan\"";
    } else if (std::isinf(value)) {
        out += std::signbit(value) ? "\"-inf\"" : "\"inf\"";
    } else if (value == 0.0 && std::signbit(value)) {
        out += "-0.0";
    } else {
        std::array<char, 64> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                          std::chars_format::general,
                                          std::numeric_limits<double>::max_digits10);
        if (result.ec != std::errc{}) {
            throw std::runtime_error{"JSON float conversion failed"};
        }
        out.append(buffer.data(), result.ptr);
    }
}

inline void appendArg(std::string& out, const wire::TypeCode code, const DecodedArg& arg)
{
    out += "{\"type\":";
    appendInteger(out, static_cast<unsigned>(code));
    out += ",\"value\":";
    switch (code) {
    case wire::TypeCode::Bool:
        out += std::get<bool>(arg) ? "true" : "false";
        break;
    case wire::TypeCode::I8: case wire::TypeCode::I16:
    case wire::TypeCode::I32: case wire::TypeCode::I64:
        out += '"';
        appendInteger(out, std::get<std::int64_t>(arg));
        out += '"';
        break;
    case wire::TypeCode::U8: case wire::TypeCode::U16:
    case wire::TypeCode::U32: case wire::TypeCode::U64: case wire::TypeCode::Pointer:
        out += '"';
        appendInteger(out, std::get<std::uint64_t>(arg));
        out += '"';
        break;
    case wire::TypeCode::F32: case wire::TypeCode::F64:
        appendFloat(out, std::get<double>(arg));
        break;
    case wire::TypeCode::Char: {
        const char value = std::get<char>(arg);
        appendHex(out, std::string_view{&value, 1});
        break;
    }
    case wire::TypeCode::Bytes:
        appendHex(out, std::get<std::string_view>(arg));
        break;
    case wire::TypeCode::Invalid:
    default:
        throw std::invalid_argument{"JSON argument has invalid wire type"};
    }
    out += '}';
}

} // namespace json_detail

/** Formats one merged message using the version-1 JSON schema in docs/json-export.md.
 *
 * @param[in] entry Decoded message and real merge metadata; all its borrows must remain live.
 * @param[in] subsystemName Name resolved by the caller, empty when no declaration is available.
 * @return An owned JSON object without a trailing newline; retains no input views.
 * @note Cold path: allocates and may throw allocation/conversion exceptions. A null site,
 *       mismatched type count or invalid type throws std::invalid_argument; mismatched
 *       variant alternatives throw std::bad_variant_access. Not a producer or live-reader API.
 */
[[nodiscard]] inline std::string formatJson(const MergedRecord& entry,
                                           const std::string_view subsystemName = {})
{
    const DecodedRecord& record = entry.record_;
    if (record.site_ == nullptr || record.site_->argTypes_.size() != record.args_.size()) {
        throw std::invalid_argument{"JSON message needs a site and matching argument types"};
    }
    const DecodedSite& site = *record.site_;
    std::string out = "{\"schema\":1,\"kind\":\"message\"";
    out += ",\"process_id\":\"";
    json_detail::appendInteger(out, entry.processId_);
    out += '"';
    out += ",\"thread_id\":\"";
    json_detail::appendInteger(out, record.ownerThread_);
    out += '"';
    out += ",\"site_id\":\"";
    json_detail::appendInteger(out, site.siteId_);
    out += '"';
    out += ",\"correlation_id\":\"";
    json_detail::appendInteger(out, record.correlationId_);
    out += '"';
    out += ",\"mono_ns\":\"";
    json_detail::appendInteger(out, record.monoNs_);
    out += '"';
    out += ",\"aligned_ns\":\"";
    json_detail::appendInteger(out, entry.alignedNs_);
    out += '"';
    out += ",\"severity\":";
    json_detail::appendInteger(out, static_cast<unsigned>(site.severity_));
    out += ",\"subsystem_id\":";
    json_detail::appendInteger(out, site.subsystem_.value_);
    out += ",\"source_line\":";
    json_detail::appendInteger(out, site.line_);
    out += ",\"truncated\":";
    out += record.truncated_ ? "true" : "false";
    out += ",\"subsystem_name\":";
    json_detail::appendText(out, subsystemName);
    out += ",\"format\":";
    json_detail::appendText(out, site.format_);
    out += ",\"file\":";
    json_detail::appendText(out, site.file_);
    out += ",\"message\":";
    json_detail::appendText(out, Decoder::format(record));
    out += ",\"segment_generation\":\"";
    json_detail::appendInteger(out, record.segmentGeneration_);
    out += "\",\"record_offset\":\"";
    json_detail::appendInteger(out, record.recordOffset_);
    out += '"';
    out += ",\"args\":[";
    for (std::size_t i = 0; i < record.args_.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        json_detail::appendArg(out, site.argTypes_[i], record.args_[i]);
    }
    out += "]}";
    return out;
}

} // namespace sub0log
