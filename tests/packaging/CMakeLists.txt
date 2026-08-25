# Proves find_package(Sub0Log) actually works for an out-of-tree consumer
# (docs/adoption-friction.md 1.1) rather than trusting the root
# CMakeLists.txt install() rules by inspection. Driven as a `cmake -P`
# script rather than an ordinary add_subdirectory()'d target: the thing
# under test *is* a fresh `cmake --install` followed by a fresh
# find_package(), which cannot happen inside the build that is running the
# test.
#
# Labeled `packaging` (`ctest -L packaging`) and left in the default set:
# a full configure+install of the header-only library plus a one-file
# consumer build measured well under the ~30s budget this repo's other
# categories already keep to.
add_test(NAME "packaging::find_package"
         COMMAND ${CMAKE_COMMAND}
                 "-DSUB0LOG_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                 "-DSUB0LOG_CONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/consumer"
                 "-DSUB0LOG_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/work"
                 "-DSUB0LOG_GENERATOR=${CMAKE_GENERATOR}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/drive.cmake")
set_tests_properties("packaging::find_package" PROPERTIES
    LABELS "packaging"
    TIMEOUT 180)
