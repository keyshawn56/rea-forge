cmake_minimum_required(VERSION 3.22.1)
project(rea_harness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(rea_harness SHARED
    elf_loader.cpp
    pak_archive.cpp
    sha256.cpp
    engine.cpp
    native_bridge.cpp)

target_compile_options(rea_harness PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility-inlines-hidden)

find_library(android-lib android)
find_library(log-lib log)

target_link_libraries(rea_harness PRIVATE ${android-lib} ${log-lib})

# JNI entry points must stay exported even though inlines are hidden.
set_target_properties(rea_harness PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
