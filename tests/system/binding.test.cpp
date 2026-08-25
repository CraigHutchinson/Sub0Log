// R9.3: a call site that reaches no instance emits nothing, and that fact is
// discoverable.
//
// R9.1 already makes a drop visible, but only once a Logger has been found
// to count on. Everything failing before that point used to be invisible --
// the forked child that overwrote its parent, a plugin with its own copy of
// the active-instance pointer, and the ordinary window before a process
// binds. All three look identical from outside: records gone, drops zero,
// undecodable zero, exit status clean.
//
// The counter is a delta measurement everywhere below: it is module-scoped
// and monotonic by design, so a test asserts what its own calls added to it,
// never its absolute value.

#include <sub0log/log.hpp>

#include "support/fixtures.hpp"

#include "support/test_framework.hpp"

#include <cstdint>
#include <filesystem>

#ifndef _WIN32
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

constexpr sub0log::SubsystemId cUnbound{11};

} // namespace

TEST_CASE("emitting with nothing bound is counted (R9.3)")
{
    const std::uint64_t before = sub0log::unboundEmits();

    for (int i = 0; i < 7; ++i) {
        sub0log_info(cUnbound, "into nowhere {}", static_cast<std::uint64_t>(i));
    }

    CHECK(sub0log::unboundEmits() - before == 7u);
}

TEST_CASE("a bound instance never touches the unbound counter")
{
    const auto directory = sub0log::test::freshDirectory("bound");

    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    const std::uint64_t before = sub0log::unboundEmits();
    {
        sub0log::Logger::ScopedBind bind{logger};

        // Both halves of the bound path: a record that is written, and one
        // suppressed by the threshold. Neither is a call site emitting into
        // nowhere, so neither is R9.3's business -- and keeping the counter
        // off this branch is what leaves R1.4's disabled-site cost alone.
        logger.setThreshold(sub0log::Severity::Error);
        sub0log_error(cUnbound, "written");
        sub0log_debug(cUnbound, "below threshold");
    }
    CHECK(sub0log::unboundEmits() == before);

    // Leaving the scope unbinds, so the counter resumes moving.
    sub0log_error(cUnbound, "after the scope");
    CHECK(sub0log::unboundEmits() - before == 1u);

    std::filesystem::remove_all(directory);
}

#ifndef _WIN32
// The fork case is where this counter earns its keep: the child is detached
// from the parent's instance on purpose, so its call sites emit nothing.
// Before R9.3 that was indistinguishable from the child having logged
// successfully. Now the child can say how many records it lost, and does --
// through its exit status, which is the only channel a detached child has.
TEST_CASE("a detached child can account for what it did not log (R9.3)")
{
    const auto directory = sub0log::test::freshDirectory("forkcount");

    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cUnbound, "parent before the fork");

        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            const std::uint64_t before = sub0log::unboundEmits();
            for (int i = 0; i < 5; ++i) {
                sub0log_info(cUnbound, "child {}", static_cast<std::uint64_t>(i));
            }
            ::_exit(static_cast<int>(sub0log::unboundEmits() - before));
        }
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 5); // all five, accounted for
    }

    std::filesystem::remove_all(directory);
}
#endif // !_WIN32
