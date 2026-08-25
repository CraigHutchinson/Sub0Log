# sub0log-cat: the reader that ships, so that "look at the records" is a
# command rather than forty lines every consumer writes for themselves
# (docs/adoption-friction.md 2.1). It is an ordinary consumer of
# Sub0Log::Sub0Log, exactly like the examples, and links nothing else.
find_package(Threads REQUIRED)

add_executable(sub0log-cat "${CMAKE_CURRENT_SOURCE_DIR}/sub0log_cat.cpp")
target_link_libraries(sub0log-cat PRIVATE Sub0Log::Sub0Log Threads::Threads)

set_target_properties(sub0log-cat PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(sub0log-cat PRIVATE /W4)
else()
    target_compile_options(sub0log-cat PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Installed alongside the headers when the library is: a tool nobody can run
# is the state this fixed, and a packaged Sub0Log that ships only headers
# leaves a consumer back where they started.
if(SUB0LOG_INSTALL)
    install(TARGETS sub0log-cat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
