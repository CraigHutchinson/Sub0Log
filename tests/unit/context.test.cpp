// context.hpp: thread-local correlation, its RAII scope, and reading the
// propagation environment variable (R5.4, R6).

#include <sub0log/context.hpp>

#include "support/fixtures.hpp"

#include <doctest/doctest.h>

#include <cstdlib>

using namespace sub0log;

TEST_CASE("CorrelationScope sets and restores this thread's id, nesting correctly")
{
    CHECK(currentCorrelation() == 0u);
    {
        const CorrelationScope outer;
        const std::uint64_t outerId = outer.id();
        CHECK(outerId != 0u);
        CHECK(currentCorrelation() == outerId);
        {
            const CorrelationScope inner;
            CHECK(currentCorrelation() == inner.id());
            CHECK(inner.id() != outerId); // ids differ
        }
        CHECK(currentCorrelation() == outerId); // restored on inner's exit
    }
    CHECK(currentCorrelation() == 0u); // restored on outer's exit
}

TEST_CASE("two default-constructed scopes never collide in a small sample")
{
    for (int i = 0; i < 64; ++i) {
        const CorrelationScope a;
        const CorrelationScope b;
        CHECK(a.id() != b.id());
    }
}

TEST_CASE("CorrelationScope accepts an explicit id, e.g. recovered from a parent process")
{
    const CorrelationScope scope{777u};
    CHECK(scope.id() == 777u);
    CHECK(currentCorrelation() == 777u);
}

TEST_CASE("correlationFromEnvironment parses, and is 0 on absence or garbage")
{
    test::unsetEnvVar(detail::cCorrelationEnvVar);
    CHECK(detail::correlationFromEnvironment() == 0u);

    test::setEnvVar(detail::cCorrelationEnvVar, "123456789");
    CHECK(detail::correlationFromEnvironment() == 123456789u);

    test::setEnvVar(detail::cCorrelationEnvVar, "not-a-number");
    CHECK(detail::correlationFromEnvironment() == 0u);

    test::setEnvVar(detail::cCorrelationEnvVar, "");
    CHECK(detail::correlationFromEnvironment() == 0u);

    test::unsetEnvVar(detail::cCorrelationEnvVar);
    CHECK(detail::correlationFromEnvironment() == 0u);
}
