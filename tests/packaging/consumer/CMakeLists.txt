cmake_minimum_required(VERSION 3.21)
project(Sub0LogPackagingConsumer LANGUAGES CXX)

# The point of this project: it knows nothing about Sub0Log's source tree,
# CPM, or doctest -- only find_package(), exactly like a distro package, a
# vendored SDK, a superbuild, Conan or vcpkg would. REQUIRED means a broken
# install() fails *here*, at configure time, instead of link time or never.
find_package(Sub0Log REQUIRED)

add_executable(sub0log_packaging_consumer main.cpp)
target_link_libraries(sub0log_packaging_consumer PRIVATE Sub0Log::Sub0Log)

# Deliberately no CXX_STANDARD/CXX_EXTENSIONS/target_compile_options here:
# this project sets nothing that Sub0Log::Sub0Log is supposed to bring with
# it. C++23 and (on MSVC) /Zc:preprocessor must arrive purely as usage
# requirements carried by the exported target, exactly as they do for an
# in-tree consumer -- if install(EXPORT) ever stopped carrying them, this
# is where a default-standard compile would fail instead of the gap
# passing unnoticed. main.cpp's static_assert on std::to_underlying is the
# other half of that check.
