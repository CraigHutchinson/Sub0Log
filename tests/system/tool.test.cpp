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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef _WIN32
#  include <csignal>
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#if defined(SUB0LOG_CAT_PATH)

namespace {

constexpr sub0log::SubsystemId cStorage{3};
constexpr sub0log::SubsystemId cNetwork{4};

/// Runs the tool with `arguments`, captures stdout and stderr to a file and
/// returns what it wrote.
///
/// Two things here are Windows, and both cost a red CI run to find. The
/// path CMake hands over uses forward slashes, which `cmd.exe` reads as
/// switch introducers rather than separators ("The filename, directory name,
/// or volume label syntax is incorrect"), so it is put through
/// make_preferred() first. And `cmd.exe` strips the first and last quote of
/// a command line that begins with one, which mangles a quoted executable
/// path followed by quoted arguments -- the documented answer is an extra
/// enclosing pair for it to eat. Quoting at all is not optional either way:
/// a temp directory is not guaranteed to be free of spaces.
[[nodiscard]] std::string runTool(const std::string& arguments,
                                  const std::filesystem::path& outputPath,
                                  int& exitCode)
{
    const std::string exe = std::filesystem::path{SUB0LOG_CAT_PATH}.make_preferred().string();
    const std::string out = std::filesystem::path{outputPath}.make_preferred().string();

    std::string command = "\"" + exe + "\" " + arguments + " > \"" + out + "\" 2>&1";
#if defined(_WIN32)
    command = "\"" + command + "\"";
#endif

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


#ifndef _WIN32
namespace {

/// Reads `path` (which may not exist yet) and returns its contents.
[[nodiscard]] std::string readAll(const std::filesystem::path& path)
{
    std::ifstream in{path};
    if (!in) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

/** Waits until `path` contains `needle`, or the tailer dies, or time runs
 *  out. Returns the contents seen last.
 *
 *  Polling rather than sleeping a fixed amount, and that is the difference
 *  between this test and a flaky one: the first version slept 600 ms on
 *  each side and failed roughly one run in two on a loaded machine. A fixed
 *  wait encodes a guess about a shared runner's speed, where what the test
 *  actually needs is the *ordering* -- that the tailer has printed its
 *  first pass before the second record is written -- which only waiting for
 *  the evidence can establish.
 */
[[nodiscard]] std::string waitForSubstring(const std::filesystem::path& path,
                                            const std::string_view needle, const pid_t child)
{
    constexpr auto cLimit = std::chrono::seconds{10};
    const auto deadline = std::chrono::steady_clock::now() + cLimit;
    std::string text;
    while (std::chrono::steady_clock::now() < deadline) {
        text = readAll(path);
        if (text.find(needle) != std::string::npos) {
            return text;
        }
        // A tailer that has exited is never going to print it, so say that
        // rather than spending the whole timeout on a corpse.
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child) {
            return text;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return text;
}

} // namespace

// --follow is the tool's other half and the one a person leaves running, so
// it is worth an actual test rather than an assumption that the loop works.
//
// POSIX only, and for a reason rather than an oversight: testing it means
// starting the tool, letting it run while records appear underneath it, and
// then stopping it -- fork/exec/SIGTERM is how that is spelled here, as it
// already is for the fork and child-capture tests. The alternative,
// teaching the tool a bounded-follow option purely so a test could reach
// it, is exactly the test-only branch R7.2 forbids.
TEST_CASE("sub0log-cat --follow prints records written after it started")
{
    const auto directory = sub0log::test::freshDirectory("catfollow");
    const auto output = directory / "follow.txt";

    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());
    sub0log::Logger::ScopedBind bind{logger};

    sub0log_info(cStorage, "before the tailer");

    const pid_t tailer = ::fork();
    REQUIRE(tailer >= 0);
    if (tailer == 0) {
        // The child is the tool. Redirect its stdout to the capture file
        // and exec -- no library state of the parent's survives, so the
        // fork detach this repository added is irrelevant here.
        const int fd = ::open(output.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { ::_exit(2); }
        if (::dup2(fd, STDOUT_FILENO) < 0) { ::_exit(3); }
        ::execl(SUB0LOG_CAT_PATH, "sub0log-cat", "--follow", directory.string().c_str(),
                static_cast<char*>(nullptr));
        ::_exit(4); // execl only returns on failure
    }

    // Wait for the first pass before writing anything else, so that what
    // follows is genuinely "after the tailer started" rather than a race.
    const std::string firstPass = waitForSubstring(output, "before the tailer", tailer);
    CAPTURE(firstPass);
    REQUIRE(firstPass.find("before the tailer") != std::string::npos);

    sub0log_warning(cStorage, "after the tailer started");

    const std::string text = waitForSubstring(output, "after the tailer started", tailer);
    CAPTURE(text);

    ::kill(tailer, SIGTERM);
    int status = 0;
    ::waitpid(tailer, &status, 0);

    const auto first = text.find("before the tailer");
    const auto second = text.find("after the tailer started");
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    // Order matters as much as presence: the second record was written
    // after the tailer had already printed the first, so seeing it at all
    // means a later pass picked it up.
    CHECK(first < second);
    // And exactly once each -- a tailer that reprints what it has already
    // shown is worse than one that misses something.
    CHECK(text.find("before the tailer", first + 1) == std::string::npos);
    CHECK(text.find("after the tailer started", second + 1) == std::string::npos);

    std::filesystem::remove_all(directory);
}
#endif // !_WIN32

#endif // SUB0LOG_CAT_PATH
