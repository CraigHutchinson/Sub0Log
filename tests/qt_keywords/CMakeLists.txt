# Issue #1: Qt's keyword macros (`emit`, `slots`, `signals`, `foreach`,
# `forever`) must not break a consumer, whichever order it includes Qt and
# Sub0Log in. One source, keyword_consumer.cpp, built per variant and run
# as a real write-then-decode round trip, not a header-only compile check.
#
#   qt_keywords::shim_first / shim_after
#       Qt's macro definitions reproduced without Qt (qt_keyword_shim.hpp).
#       Always built, so every CI configuration -- MSVC included -- checks
#       the guarantee whether or not Qt is installed there.
#   qt_keywords::qt6_first / qt6_after
#       The same test against a real <QtCore/QObject>, when Qt6::Core is
#       found. SUB0LOG_TEST_QT=ON turns "not found" into a configure error,
#       which is how CI makes sure the real-Qt legs never skip silently.
#
# Labeled `qt` as well as `integration` (one process, real files).

set(SUB0LOG_TEST_QT "AUTO" CACHE STRING
    "Build the real-Qt consumer variants: ON (require Qt6::Core), OFF, or AUTO")
set_property(CACHE SUB0LOG_TEST_QT PROPERTY STRINGS AUTO ON OFF)

function(sub0log_add_qt_keywords_variant NAME REAL_QT FIRST)
    set(target "Sub0LogQtKeywords_${NAME}")
    add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/keyword_consumer.cpp")
    target_link_libraries(${target} PRIVATE Sub0Log::Sub0Log)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${target} PRIVATE
        "SUB0LOG_KEYWORDS_REAL_QT=${REAL_QT}"
        "SUB0LOG_KEYWORDS_FIRST=${FIRST}"
        "SUB0LOG_KEYWORDS_VARIANT=\"${NAME}\"")
    # The host export in keyword_consumer.cpp (SUB0LOG_ABI_HOST_EXPORT) wants
    # the same treatment Sub0LogSystemTests gets -- see tests/CMakeLists.txt.
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        ENABLE_EXPORTS ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(REAL_QT)
        target_link_libraries(${target} PRIVATE Qt6::Core)
    endif()
    add_test(NAME "qt_keywords::${NAME}" COMMAND ${target})
    set_tests_properties("qt_keywords::${NAME}" PROPERTIES LABELS "integration;qt")
    # Windows finds Qt6Core.dll through PATH, not an rpath.
    if(WIN32 AND REAL_QT)
        set_tests_properties("qt_keywords::${NAME}" PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endfunction()

sub0log_add_qt_keywords_variant(shim_first 0 1)
sub0log_add_qt_keywords_variant(shim_after 0 0)

if(NOT SUB0LOG_TEST_QT STREQUAL "OFF")
    if(SUB0LOG_TEST_QT STREQUAL "ON")
        find_package(Qt6 REQUIRED COMPONENTS Core)
    else()
        find_package(Qt6 QUIET COMPONENTS Core)
    endif()
    if(TARGET Qt6::Core)
        sub0log_add_qt_keywords_variant(qt6_first 1 1)
        sub0log_add_qt_keywords_variant(qt6_after 1 0)
    else()
        message(STATUS "Sub0Log: Qt6::Core not found -- real-Qt keyword tests skipped "
                       "(the shim variants still run; -DSUB0LOG_TEST_QT=ON to require Qt)")
    endif()
endif()
