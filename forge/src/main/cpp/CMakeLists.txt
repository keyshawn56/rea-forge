cmake_minimum_required(VERSION 3.22.1)
project(rea_forge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(rea_forge SHARED
    core/io.cpp
    core/sha256.cpp
    format/nso.cpp
    format/npdm.cpp
    format/pfs0.cpp
    format/romfs.cpp
    recomp/a64_scanner.cpp
    shader/shader_builder.cpp
    gfx/vk_cache_builder.cpp
    forge_jni.cpp)

target_include_directories(rea_forge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(rea_forge PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility-inlines-hidden)

find_library(android-lib android)
find_library(log-lib log)
find_library(vulkan-lib vulkan)

target_link_libraries(rea_forge PRIVATE
    ${android-lib}
    ${log-lib}
    ${vulkan-lib})

set_target_properties(rea_forge PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
