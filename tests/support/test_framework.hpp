#pragma once

/** @file tests/support/test_framework.hpp
 *  @brief doctest, plus the one include every test TU needs before it.
 *
 *  MSVC's <string_view> declares operator<<(basic_ostream&, basic_string_view)
 *  but leaves basic_ostream incomplete; doctest stringifies the values it
 *  compares, so any TU that CHECKs a string_view fails to compile there
 *  without <ostream>. libstdc++ and libc++ pull it in transitively, which is
 *  why this only ever showed up on the Windows job -- and why the include
 *  belongs in one place rather than in whichever test file happens to trip
 *  over it next.
 */

#include <ostream>

#include <doctest/doctest.h>
