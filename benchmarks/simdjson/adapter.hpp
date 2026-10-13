#pragma once

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include <simdjson.h>

namespace sub0log::simdjson_eval {

/// Experiment-only output buffer. The caller owns it; no input view is retained.
/// Fast selects the third refinement; false retains control scalar spelling code.
template <bool Fast>
struct Sink {
    simdjson::builder::string_builder builder_{};
    bool bulkControls_{false};

    void operator+=(const std::string_view text) { builder_.append_raw(text); }
    void operator+=(const char value) { builder_.append(value); }
    void append(const char* first, const char* last) {
        builder_.append_raw(first, static_cast<std::size_t>(last - first));
    }
    /// Preserve six-byte control escapes using bounded scratch and bulk appends.
    void appendControlText(const std::string_view text) {
        constexpr std::string_view hex{"0123456789abcdef"};
        std::array<char, 256> block{};
        std::size_t used = 0;
        builder_.append('"');
        for (const unsigned char byte : text) {
            if (block.size() - used < 6) {
                builder_.append_raw(block.data(), used);
                used = 0;
            }
            if (byte < 0x20u) {
                block[used++] = '\\'; block[used++] = 'u';
                block[used++] = '0'; block[used++] = '0';
                block[used++] = hex[byte >> 4u]; block[used++] = hex[byte & 0x0fu];
            } else {
                if (byte == '"' || byte == '\\') block[used++] = '\\';
                block[used++] = static_cast<char>(byte);
            }
        }
        builder_.append_raw(block.data(), used);
        builder_.append('"');
    }
    /// Copying is intentional: the production formatter promises owned output.
    [[nodiscard]] std::string owned() const {
        std::string_view view;
        const auto error = builder_.view().get(view);
        if (error == simdjson::MEMALLOC) { throw std::bad_alloc{}; }
        if (error) { throw std::runtime_error{"simdjson evaluation builder failed"}; }
        return std::string{view};
    }
};

} // namespace sub0log::simdjson_eval
