#pragma once

/** @file tests/qt_keywords/qt_keyword_shim.hpp
 *  @brief Qt's keyword macros, reproduced without Qt (issue #1).
 *
 *  These are the definitions QtCore/qtmetamacros.h (Qt 6; qobjectdefs.h in
 *  Qt 5) makes whenever a consumer has not defined QT_NO_KEYWORDS, spelled
 *  the way Qt spells them: `emit` and `slots` expand to *nothing*,
 *  `signals` to an access specifier, `foreach`/`forever` to loop headers.
 *  The empty ones are the dangerous ones -- the preprocessor deletes the
 *  name from the token stream before the compiler ever sees a namespace.
 *
 *  This exists so the include-order guarantee is checked on every CI
 *  configuration, MSVC included, whether or not Qt is installed there; the
 *  real-Qt variants of the same test (CMakeLists.txt, SUB0LOG_TEST_QT)
 *  prove the shim has not drifted from what Qt actually does.
 */

#define Q_EMIT
#define Q_SLOTS
#define Q_SIGNALS public
#define Q_FOREACH(variable, container) for (variable : container)
#define Q_FOREVER for (;;)

#define emit Q_EMIT
#define slots Q_SLOTS
#define signals Q_SIGNALS
#define foreach Q_FOREACH
#define forever Q_FOREVER
