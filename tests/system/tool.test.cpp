// sub0log-cat, end to end: the library writes a segment, the shipped tool
// reads it back as text.
//
// This is a system test rather than a unit one on purpose. The tool's value
// is precisely that it is the thing a person runs, so what is worth pinning
// is the whole chain -- produce, merge, render -- through the real binary
// and its real command line, not the functions inside it.

#include <sub0log/log.hpp>

#include "support/fixtures.hpp"

#include "support/test_framework.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#if defined(SUB0LOG_CAT_PATH)

namespace {

constexpr sub0log::SubsystemId cStorage{3};
constexpr sub0log::SubsystemId cNetwork{4};

/// Runs the tool with `arguments`, captures stdout to a file and returns it.
/// Quoting is deliberate: a test directory path contains a temp prefix that
/// is not guaranteed to be free of spaces.
[[nodiscard]] std::string runTool(const std::string& arguments,
                                  const std::filesystem::path& outputPath,
                                  int& exitCode)
{
    const std::string command = std::string{"\""} + SUB0LOG_CAT_PATH + "\" " + arguments
                              + " > \"" + outputPath.string() + "\" 2>&1";
    exitCode = std::system(command.c_str());
    std::ifstream in{outputPath};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("sub0log-cat renders a segment a producer just wrote")
{
    const auto directory = sub0log::test::freshDirectory("cattool");
    const auto output = directory / "out.txt";

    static constexpr std::array<std::pair<sub0log::SubsystemId, std::string_view>, 1>
        cNames{{{cStorage, "storage"}}};
    {
        sub0log::Logger::Options options{};
        options.directory_ = directory.string();
        options.subsystemNames_ = cNames;
        auto logger = sub0log::Logger::create(options);
        REQUIRE(logger.valid());
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope correlation{4242};

        sub0log_info(cStorage, "read {} at {}", std::uint64_t{42}, std::int32_t{-7});
        sub0log_warning(cNetwork, "retry {} of {}", std::uint32_t{2}, std::uint32_t{5});
        sub0log_error(cStorage, "gave up on {}", std::string_view{"blob.bin"});
    }

    SUBCASE("every record, rendered with its arguments substituted")
    {
        int exitCode = -1;
        const std::string text = runTool("\"" + directory.string() + "\"", output, exitCode);
        CAPTURE(text);
        CHECK(exitCode == 0);
        CHECK(text.find("read 42 at -7") != std::string::npos);
        CHECK(text.find("retry 2 of 5") != std::string::npos);
        CHECK(text.find("gave up on blob.bin") != std::string::npos);
        // The severity and subsystem are columns, not text to search for in
        // the message -- that is the whole R2.2 point, and the tool keeps it.
        CHECK(text.find("ERROR") != std::string::npos);
        // A declared subsystem prints by name, with the number it was
        // declared for still shown: the name is for the reader, the number
        // is what --subsystem takes and what the record carries.
        CHECK(text.find("storage(3)") != std::string::npos);
        // An undeclared one prints as the bare number rather than a guess.
        CHECK(text.find(" 4  retry 2 of 5") != std::string::npos);
    }

    SUBCASE("--level filters below the bar out")
    {
        int exitCode = -1;
        const std::string text =
            runTool("--level error \"" + directory.string() + "\"", output, exitCode);
        CAPTURE(text);
        CHECK(exitCode == 0);
        CHECK(text.find("gave up on blob.bin") != std::string::npos);
        CHECK(text.find("read 42 at -7") == std::string::npos);
    }

    SUBCASE("--subsystem filters on the field, not the text")
    {
        int exitCode = -1;
        const std::string text =
            runTool("--subsystem 4 \"" + directory.string() + "\"", output, exitCode);
        CAPTURE(text);
        CHECK(exitCode == 0);
        CHECK(text.find("retry 2 of 5") != std::string::npos);
        CHECK(text.find("read 42 at -7") == std::string::npos);
    }

    SUBCASE("--stats reports the mechanism's own counters")
    {
        int exitCode = -1;
        const std::string text =
            runTool("--stats \"" + directory.string() + "\"", output, exitCode);
        CAPTURE(text);
        CHECK(exitCode == 0);
        CHECK(text.find("unreadable 0 byte(s)") != std::string::npos);
        CHECK(text.find("undecodable 0 record(s)") != std::string::npos);
    }

    std::filesystem::remove_all(directory);
}

TEST_CASE("sub0log-cat says so rather than exiting quietly on a bad path")
{
    const auto directory = sub0log::test::freshDirectory("catbad");
    const auto output = directory / "out.txt";

    int exitCode = -1;
    const std::string text =
        runTool("\"" + (directory / "nothing-here.s0l").string() + "\"", output, exitCode);
    CAPTURE(text);
    CHECK(exitCode != 0);
    CHECK(text.find("nothing readable") != std::string::npos);

    std::filesystem::remove_all(directory);
}

#endif // SUB0LOG_CAT_PATH
