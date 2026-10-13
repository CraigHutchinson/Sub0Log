"""Build-only experiment: adapt the current schema, fail closed if its shape changes."""
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:])
s = source.read_text()
def replace(old, new):
    global s
    assert s.count(old) == 1, (old, s.count(old))
    s = s.replace(old, new)
replace('#include "merge.hpp"', '#include <sub0log/json.hpp>\n#include "adapter.hpp"')
replace('namespace sub0log {', 'namespace sub0log::simdjson_eval {')
for name in ('appendQuoted', 'appendHex', 'appendText', 'appendFloat', 'appendArg'):
    replace(f'inline void {name}(std::string& out,',
            f'template <bool Fast>\ninline void {name}(Sink<Fast>& out,')
replace('template <std::integral Integer>', 'template <bool Fast, std::integral Integer>')
replace('inline void appendInteger(std::string& out,', 'inline void appendInteger(Sink<Fast>& out,')
replace('    if (!validUtf8(bytes)) {', '    if (!(Fast ? simdjson::validate_utf8(bytes.data(), bytes.size()) : validUtf8(bytes))) {')
replace('    out += \'"\';\n    for (const unsigned char byte : text) {', '''    if constexpr (Fast) {
        if (std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20u; })) {
            out.builder_.escape_and_append_with_quotes(text);
            return;
        }
        if (out.bulkControls_) {
            out.appendControlText(text);
            return;
        }
    }
    out += '"';
    for (const unsigned char byte : text) {''')
replace('    for (const unsigned char byte : bytes) {\n        out += cHexDigits[byte >> 4u];\n        out += cHexDigits[byte & 0x0fu];\n    }', '''    if constexpr (Fast) {
        std::array<char, 256> block{};
        while (!bytes.empty()) {
            const auto count = std::min(bytes.size(), block.size() / 2);
            for (std::size_t i = 0; i < count; ++i) {
                const auto byte = static_cast<unsigned char>(bytes[i]);
                block[2 * i] = cHexDigits[byte >> 4u];
                block[2 * i + 1] = cHexDigits[byte & 0x0fu];
            }
            out += std::string_view{block.data(), 2 * count};
            bytes.remove_prefix(count);
        }
    } else {
        for (const unsigned char byte : bytes) {
            out += cHexDigits[byte >> 4u];
            out += cHexDigits[byte & 0x0fu];
        }
    }''')
replace('inline void appendHex(Sink<Fast>& out, const std::string_view bytes)', 'inline void appendHex(Sink<Fast>& out, std::string_view bytes)')
replace('    std::array<char, std::numeric_limits<Integer>::digits10 + 3> buffer{};', '''    if constexpr (Fast) {
        out.builder_.append(value);
        return;
    }
    std::array<char, std::numeric_limits<Integer>::digits10 + 3> buffer{};''')
replace('[[nodiscard]] inline std::string formatJson(const MergedRecord& entry,\n                                           const std::string_view subsystemName = {})', '''template <bool Fast>
[[nodiscard]] inline std::string formatJson(const MergedRecord& entry,
                                           const std::string_view subsystemName, Sink<Fast>& out)''')
replace('    std::string out = ', '    out.builder_.clear();\n    out += ')
replace('    return out;', '    return out.owned();')
output.write_text('// Generated evaluation adapter; not an installed/public header.\n' + s)

# Optional CLI copies receive the actual argument/filter/budget/IO path.
# Only the formatter call and its run-local storage differ from the control.
cli = source.parents[2] / 'tools/sub0log_cat.cpp'
text = cli.read_text()
call = 'sub0log::formatJson(entry, nameFor(entry.record_.site_->subsystem_))'
assert text.count(call) == 1
for arm in ('fresh', 'reuse'):
    generated = '#include "formatter.hpp"\n' + text
    if arm == 'reuse':
        start = 'int run(const Options& options, Health& health)\n{'
        assert generated.count(start) == 1
        generated = generated.replace(start, start + '\n    sub0log::simdjson_eval::Sink<true> jsonSink;\n    jsonSink.bulkControls_ = true;')
        replacement = 'sub0log::simdjson_eval::formatJson(entry, nameFor(entry.record_.site_->subsystem_), jsonSink)'
    else:
        replacement = 'sub0log::simdjson_eval::formatFresh(entry, nameFor(entry.record_.site_->subsystem_))'
    (output.parent / f'cat-{arm}.cpp').write_text(generated.replace(call, replacement))
with output.open('a') as generated:
    generated.write('''
namespace sub0log::simdjson_eval {
[[nodiscard]] inline std::string formatFresh(const MergedRecord& entry, std::string_view name = {}) {
    Sink<true> out;
    out.bulkControls_ = true;
    return formatJson(entry, name, out);
}
}
''')

# Matched no-dependency alternative: bulk appends and a comparable initial buffer.
# This separates buffer strategy from SIMD/library attribution.
portable = source.read_text().replace('#include "merge.hpp"', '#include <sub0log/json.hpp>\n#include <algorithm>')
portable = portable.replace('namespace sub0log {', 'namespace sub0log::stdlib_eval {')
portable = portable.replace('    out += \'"\';\n    for (const unsigned char byte : text) {', '''    if (std::none_of(text.begin(), text.end(), [](unsigned char c) {
            return c < 0x20u || c == '"' || c == '\\\\';
        })) {
        out += '"'; out += text; out += '"';
        return;
    }
    out += '"';
    for (const unsigned char byte : text) {''')
portable = portable.replace('    const DecodedSite& site = *record.site_;', '    const DecodedSite& site = *record.site_;')
portable = portable.replace('    out += ",\\"process_id\\":\\"";', '    out.reserve(1024);\n    out += ",\\"process_id\\":\\"";')
old_hex = '''    for (const unsigned char byte : bytes) {
        out += cHexDigits[byte >> 4u];
        out += cHexDigits[byte & 0x0fu];
    }'''
assert portable.count(old_hex) == 1
portable = portable.replace(old_hex, '''    std::array<char, 256> block{};
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min(bytes.size() - offset, block.size() / 2);
        for (std::size_t i = 0; i < count; ++i) {
            const auto byte = static_cast<unsigned char>(bytes[offset + i]);
            block[2 * i] = cHexDigits[byte >> 4u];
            block[2 * i + 1] = cHexDigits[byte & 0x0fu];
        }
        out.append(block.data(), 2 * count);
        offset += count;
    }''')
# Share the exact buffered escape implementation; only the output API differs.
import textwrap
adapter = pathlib.Path(__file__).with_name('adapter.hpp').read_text()
body = adapter.split('    void appendControlText(const std::string_view text) {', 1)[1]
body = body.split('    /// Copying is intentional:', 1)[0]
assert body.rstrip().endswith('}')
body = textwrap.dedent(body.rstrip()[:-1]).strip()
body = body.replace('builder_.append_raw(block.data(), used)', 'out.append(block.data(), used)')
body = body.replace("builder_.append('\"')", "out += '\"'")
start = portable.index('inline void appendQuoted(')
end = portable.index('inline void appendHex(', start)
fast_path = portable[start:portable.index("    out += '\"';\n    for", start)]
portable = portable[:start] + fast_path + textwrap.indent(body, '    ') + '\n}\n\n' + portable[end:]
(output.parent / 'formatter-stdlib.hpp').write_text('// Generated no-dependency evaluation arm.\n' + portable)
(output.parent / 'cat-stdlib.cpp').write_text('#include "formatter-stdlib.hpp"\n' + cli.read_text().replace(
    call, 'sub0log::stdlib_eval::formatJson(entry, nameFor(entry.record_.site_->subsystem_))'))
