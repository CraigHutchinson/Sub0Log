# Explicit experiment only: never a transitive dependency of Sub0Log.
CPMAddPackage(NAME simdjson GITHUB_REPOSITORY simdjson/simdjson
    GIT_TAG 1bcf71bd85059ab6574ea1159de9298dcc1212c5 EXCLUDE_FROM_ALL YES
    OPTIONS "SIMDJSON_DEVELOPER_MODE OFF" "SIMDJSON_INSTALL OFF")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/sub0log/json.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/generate_adapter.py" "${CMAKE_CURRENT_SOURCE_DIR}/adapter.hpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_adapter.py"
    "${PROJECT_SOURCE_DIR}/include/sub0log/json.hpp" "${CMAKE_CURRENT_BINARY_DIR}/formatter.hpp"
    COMMAND_ERROR_IS_FATAL ANY)
add_library(sub0log_simdjson_evaluation INTERFACE)
target_include_directories(sub0log_simdjson_evaluation INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(sub0log_simdjson_evaluation INTERFACE Sub0Log::Sub0Log simdjson::simdjson)
add_executable(Sub0LogJsonEvaluation evaluate.cpp)
target_link_libraries(Sub0LogJsonEvaluation PRIVATE sub0log_simdjson_evaluation nanobench Threads::Threads)
target_include_directories(Sub0LogJsonEvaluation PRIVATE "${PROJECT_SOURCE_DIR}/benchmarks")
set_target_properties(Sub0LogJsonEvaluation PROPERTIES CXX_STANDARD 23 CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(Sub0LogJsonEvaluation PRIVATE /W4)
else()
    target_compile_options(Sub0LogJsonEvaluation PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/sub0log_cat.cpp")
foreach(arm fresh reuse stdlib)
    add_executable(sub0log-cat-simdjson-${arm} "${CMAKE_CURRENT_BINARY_DIR}/cat-${arm}.cpp")
    if(arm STREQUAL "stdlib")
        target_link_libraries(sub0log-cat-simdjson-${arm} PRIVATE Sub0Log::Sub0Log Threads::Threads)
        target_include_directories(sub0log-cat-simdjson-${arm} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    else()
        target_link_libraries(sub0log-cat-simdjson-${arm} PRIVATE sub0log_simdjson_evaluation Threads::Threads)
    endif()
    set_target_properties(sub0log-cat-simdjson-${arm} PROPERTIES CXX_STANDARD 23 CXX_EXTENSIONS OFF)
    if(SUB0LOG_BUILD_TESTING)
        add_test(NAME "simdjson::jsonl_${arm}" COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/jsonl/receive.py"
            $<TARGET_FILE:sub0log-cat-simdjson-${arm}> $<TARGET_FILE:Sub0LogJsonlFixture>)
        add_test(NAME "simdjson::follow_${arm}" COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/jsonl/follow.py"
            $<TARGET_FILE:sub0log-cat-simdjson-${arm}> $<TARGET_FILE:Sub0LogFollowFixture>)
        set_tests_properties("simdjson::jsonl_${arm}" "simdjson::follow_${arm}" PROPERTIES TIMEOUT 90)
    endif()
endforeach()
if(SUB0LOG_BUILD_TESTING)
    add_test(NAME "simdjson::equivalence" COMMAND Sub0LogJsonEvaluation control
        "${CMAKE_CURRENT_BINARY_DIR}/ctest-equivalence.json")
    set_tests_properties("simdjson::equivalence" PROPERTIES TIMEOUT 120)
endif()
